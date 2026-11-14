use std::fmt::Write as _;
use std::io::Write;

use crate::pager::{PAGE, Pager, Wire};
use crate::wire::{Reg, State, parse_after, parse_state};

/// How a reference run ended.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Status {
    /// The function returned to the stop address with the entry stack
    /// pointer.
    Returned,
    /// The run reached something the engine cannot run, such as a system
    /// call or a runtime-provided function; the text says what and where.
    Left(String),
    /// The engine failed: a lift error, a fault or the step limit.
    Failed(String),
}

/// What an engine returns.
#[derive(Clone, Debug)]
pub struct Outcome {
    /// How the run ended.
    pub status: Status,
    /// Instructions run.
    pub steps: u64,
    /// The registers at the end, in the order of the state's.
    pub regs: Vec<Reg>,
}

/// A reference machine: runs one guest function from a state, reading and
/// writing guest memory through the pager.
pub trait Engine {
    /// Runs from `state.pc` until the function returns to `state.stop`,
    /// at most `limit` instructions.
    fn run(&mut self, state: &State, pager: &mut Pager<'_>, limit: u64) -> Outcome;
}

/// The step limit when a state names none.
pub const DEFAULT_LIMIT: u64 = 5_000_000;

/// The verdict on one function call.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Verdict {
    /// Registers and touched pages agree.
    Match {
        /// Instructions the engine ran.
        steps: u64,
        /// Pages the run touched.
        pages: usize,
    },
    /// They differ; the text is the report.
    Mismatch(String),
    /// The engine stopped before the end, so nothing was compared.
    Stopped(String),
}

impl Verdict {
    /// The word on the protocol's `RESULT` line.
    pub fn word(&self) -> &'static str {
        match self {
            Verdict::Match { .. } => "match",
            Verdict::Mismatch(_) => "mismatch",
            Verdict::Stopped(_) => "stopped",
        }
    }
}

fn hex128(v: u128) -> String {
    if v > u128::from(u64::MAX) {
        format!("{:#x}:{:016x}", v >> 64, v as u64)
    } else {
        format!("{v:#x}")
    }
}

fn compare_regs(engine: &[Reg], lifted: &[Reg], out: &mut String) -> bool {
    let mut same = true;
    for l in lifted {
        let Some(e) = engine.iter().find(|r| r.name == l.name) else {
            continue;
        };
        if e.value != l.value {
            same = false;
            let _ = writeln!(
                out,
                "register {}: lifted {}, engine {}",
                l.name,
                hex128(l.value),
                hex128(e.value)
            );
        }
    }
    same
}

fn compare_pages(
    pager: &mut Pager<'_>,
    sp: Option<u64>,
    out: &mut String,
) -> std::io::Result<bool> {
    let ours: Vec<(u64, Vec<u8>)> = pager.touched().map(|(p, d)| (p, d.to_vec())).collect();
    let mut same = true;
    for (page, want) in ours {
        let Some(have) = pager.wire().fetch(page)? else {
            continue;
        };
        let live = |k: usize| sp.is_none_or(|sp| page + k as u64 >= sp);
        let diffs: Vec<usize> = (0..PAGE as usize)
            .filter(|&k| have[k] != want[k] && live(k))
            .collect();
        let Some(&k) = diffs.first() else {
            continue;
        };
        same = false;
        let end = (k + 8).min(PAGE as usize);
        let _ = writeln!(
            out,
            "page {page:#x}: {} bytes differ, first at {:#x}: lifted {}, engine {}",
            diffs.len(),
            page + k as u64,
            hex_bytes(&have[k..end]),
            hex_bytes(&want[k..end])
        );
    }
    Ok(same)
}

fn hex_bytes(b: &[u8]) -> String {
    b.iter().fold(String::new(), |mut s, x| {
        let _ = write!(s, "{x:02x}");
        s
    })
}

fn io_err(msg: String) -> std::io::Error {
    std::io::Error::new(std::io::ErrorKind::InvalidData, msg)
}

/// Runs one case: the engine from `state`, then, when it returned, the
/// driver lets the lifted code run and the two are compared.
///
/// # Errors
///
/// A broken pipe or a line out of the protocol.
pub fn case(
    wire: &mut Wire,
    engine: &mut dyn Engine,
    state: &State,
    limit: u64,
) -> std::io::Result<Verdict> {
    let mut pager = Pager::new(wire);
    let run = engine.run(state, &mut pager, limit);
    let verdict = match &run.status {
        Status::Returned => {
            pager.wire().send(&format!("RAN ok steps={}", run.steps))?;
            let line = pager
                .wire()
                .recv()?
                .ok_or_else(|| io_err("the driver closed before AFTER".into()))?;
            let after = parse_after(&line).map_err(io_err)?;
            let mut report = String::new();
            let regs = compare_regs(&run.regs, &after, &mut report);
            let sp = after
                .iter()
                .find(|r| r.name == "sp")
                .map(|r| r.value as u64);
            let pages = pager.touched().count();
            let mem = compare_pages(&mut pager, sp, &mut report)?;
            if regs && mem {
                Verdict::Match {
                    steps: run.steps,
                    pages,
                }
            } else {
                Verdict::Mismatch(report)
            }
        }
        Status::Left(why) => {
            pager
                .wire()
                .send(&format!("RAN left steps={}", run.steps))?;
            Verdict::Stopped(why.clone())
        }
        Status::Failed(why) => {
            pager
                .wire()
                .send(&format!("RAN failed steps={}", run.steps))?;
            Verdict::Stopped(why.clone())
        }
    };
    let detail = match &verdict {
        Verdict::Match { steps, pages } => format!("steps={steps} pages={pages}"),
        Verdict::Mismatch(r) => format!("lines={}", r.lines().count()),
        Verdict::Stopped(why) => why.replace('\n', " "),
    };
    wire.send(&format!("RESULT {} {detail}", verdict.word()))?;
    Ok(verdict)
}

/// Serves the driver until it closes the pipe: each `STATE` line is one
/// case. Each verdict's report goes to `log`.
///
/// # Errors
///
/// A broken pipe or a line out of the protocol.
pub fn serve(
    wire: &mut Wire,
    engine: &mut dyn Engine,
    log: &mut dyn Write,
) -> std::io::Result<Vec<Verdict>> {
    let mut all = Vec::new();
    while let Some(line) = wire.recv()? {
        if line.is_empty() {
            continue;
        }
        let state = parse_state(&line).map_err(io_err)?;
        let limit = state
            .get("limit")
            .map_or(DEFAULT_LIMIT, |v| u64::try_from(v).unwrap_or(DEFAULT_LIMIT));
        let v = case(wire, engine, &state, limit)?;
        match &v {
            Verdict::Match { steps, pages } => {
                writeln!(
                    log,
                    "{:#x}: match, {steps} instructions, {pages} pages",
                    state.pc
                )?;
            }
            Verdict::Mismatch(r) => {
                writeln!(log, "{:#x}: MISMATCH", state.pc)?;
                for l in r.lines() {
                    writeln!(log, "  {l}")?;
                }
            }
            Verdict::Stopped(why) => writeln!(log, "{:#x}: stopped: {why}", state.pc)?,
        }
        all.push(v);
    }
    Ok(all)
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::io::Cursor;
    use std::rc::Rc;

    use super::*;

    struct Sink(Rc<RefCell<Vec<u8>>>);

    impl Write for Sink {
        fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
            self.0.borrow_mut().extend_from_slice(b);
            Ok(b.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    struct Adder;

    impl Engine for Adder {
        fn run(&mut self, state: &State, pager: &mut Pager<'_>, _limit: u64) -> Outcome {
            let mut b = [0u8; 1];
            pager.read(0x1000, &mut b).expect("page 0x1000 is mapped");
            pager
                .write(0x1008, &[b[0] + 1])
                .expect("the page is mapped");
            let x0 = state.get("x0").unwrap_or(0) + 1;
            Outcome {
                status: Status::Returned,
                steps: 3,
                regs: vec![Reg {
                    name: "x0".into(),
                    value: x0,
                }],
            }
        }
    }

    struct Stops;

    impl Engine for Stops {
        fn run(&mut self, _state: &State, _pager: &mut Pager<'_>, _limit: u64) -> Outcome {
            Outcome {
                status: Status::Left("system call svc #7 at 0x8004010".into()),
                steps: 1,
                regs: Vec::new(),
            }
        }
    }

    fn page(first: u8, at8: u8) -> Vec<u8> {
        let mut p = vec![0u8; PAGE as usize];
        p[0] = first;
        p[8] = at8;
        p
    }

    fn drive(engine: &mut dyn Engine, script: &[&[u8]]) -> (Vec<Verdict>, String, String) {
        let input: Vec<u8> = script.concat();
        let out = Rc::new(RefCell::new(Vec::new()));
        let mut wire = Wire::new(
            Box::new(std::io::BufReader::new(Cursor::new(input))),
            Box::new(Sink(out.clone())),
        );
        let mut log = Vec::new();
        let v = serve(&mut wire, engine, &mut log).expect("the protocol runs");
        let sent = String::from_utf8(out.borrow().clone()).expect("text");
        (v, sent, String::from_utf8(log).expect("text"))
    }

    fn ok(p: &[u8]) -> Vec<u8> {
        [b"OK\n".as_slice(), p].concat()
    }

    #[test]
    fn a_case_that_agrees_matches() {
        let live = page(4, 5);
        let (v, sent, log) = drive(
            &mut Adder,
            &[
                b"STATE pc=100 stop=200 x0=9\n",
                &ok(&page(4, 0)),
                b"AFTER x0=a\n",
                &ok(&live),
            ],
        );
        assert_eq!(v, vec![Verdict::Match { steps: 3, pages: 1 }]);
        assert!(sent.contains("RAN ok steps=3"), "{sent}");
        assert!(sent.contains("RESULT match steps=3 pages=1"), "{sent}");
        assert!(log.contains("0x100: match"), "{log}");
    }

    #[test]
    fn a_wrong_register_and_a_wrong_page_are_named() {
        let (v, sent, _) = drive(
            &mut Adder,
            &[
                b"STATE pc=100 stop=200 x0=9\n",
                &ok(&page(4, 0)),
                b"AFTER x0=b\n",
                &ok(&page(4, 6)),
            ],
        );
        let Verdict::Mismatch(r) = &v[0] else {
            panic!("{v:?}")
        };
        assert!(r.contains("register x0: lifted 0xb, engine 0xa"), "{r}");
        assert!(
            r.contains("page 0x1000: 1 bytes differ, first at 0x1008: lifted 0600000000000000, engine 0500000000000000"),
            "{r}"
        );
        assert!(sent.contains("RESULT mismatch"), "{sent}");
    }

    #[test]
    fn bytes_below_the_final_stack_pointer_are_not_compared() {
        let (v, _, _) = drive(
            &mut Adder,
            &[
                b"STATE pc=100 stop=200 x0=9\n",
                &ok(&page(4, 0)),
                b"AFTER x0=a sp=1010\n",
                &ok(&page(4, 6)),
            ],
        );
        assert!(matches!(v[0], Verdict::Match { .. }), "{v:?}");
    }

    #[test]
    fn a_run_that_leaves_the_guest_is_reported_not_compared() {
        let (v, sent, log) = drive(&mut Stops, &[b"STATE pc=100 stop=200\n"]);
        assert!(
            matches!(&v[0], Verdict::Stopped(w) if w.contains("svc #7")),
            "{v:?}"
        );
        assert!(sent.starts_with("RAN left"), "{sent}");
        assert!(log.contains("stopped: system call svc #7"), "{log}");
    }
}

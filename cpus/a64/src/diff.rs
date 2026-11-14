//! The ARM64 side of `relift diff`: the register names the protocol uses and
//! an engine that runs a function in the `relift-ir` interpreter.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::collections::btree_map::Entry;

use relift_diff::{Engine, Outcome, Pager, Reg, State as Case, Status};
use relift_ir::interp::{self, Fault, GuestMemory, Intrinsics, State};
use relift_ir::{Block, Flow};

use crate::decode::Features;
use crate::lift;
use crate::lift::system::{self, TRAP_BRK};
use crate::regs;

/// The registers a state carries, in report order: `x0` to `x30`, `sp`,
/// `nzcv` (the MRS layout), `fpcr`, `tpidr_el0`, `tpidrro_el0`, then `v0`
/// to `v31`.
pub fn names() -> Vec<String> {
    let mut n: Vec<String> = (0..31).map(|i| format!("x{i}")).collect();
    for s in ["sp", "nzcv", "fpcr", "tpidr_el0", "tpidrro_el0"] {
        n.push(s.to_string());
    }
    n.extend((0..32).map(|i| format!("v{i}")));
    n
}

/// The registers a lifted function leaves in its context when it returns:
/// everything but the flags (docs/abi.md stores NZCV only where a callee
/// reads it) and the thread registers, which a function does not change.
pub fn context_names() -> Vec<String> {
    names()
        .into_iter()
        .filter(|n| !matches!(n.as_str(), "nzcv" | "tpidr_el0" | "tpidrro_el0"))
        .collect()
}

struct Mem<'p, 'w> {
    pager: &'p RefCell<&'p mut Pager<'w>>,
    reads: u64,
    flip: u64,
}

impl GuestMemory for Mem<'_, '_> {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault> {
        self.reads += 1;
        let flip = self.reads == self.flip;
        self.pager.borrow_mut().read(addr, buf).map_err(|a| Fault {
            addr: a,
            size: buf.len() as u32,
            write: false,
        })?;
        if flip && let Some(b) = buf.first_mut() {
            *b ^= 1;
        }
        Ok(())
    }

    fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), Fault> {
        self.pager
            .borrow_mut()
            .write(addr, data)
            .map_err(|a| Fault {
                addr: a,
                size: data.len() as u32,
                write: true,
            })
    }
}

struct Intr<'p, 'w> {
    pager: &'p RefCell<&'p mut Pager<'w>>,
}

impl Intrinsics for Intr<'_, '_> {
    fn call(&mut self, name: &'static str, args: &[u128]) -> Result<u128, interp::Error> {
        match name {
            system::FP_RAISE | system::MSR_FPSR => Ok(0),
            system::MRS_FPSR => Ok(args[0]),
            system::DCZID => Ok(4),
            system::DC_ZVA => {
                let a = (args[0] as u64) & !63;
                self.pager
                    .borrow_mut()
                    .write(a, &[0u8; 64])
                    .map_err(|addr| {
                        interp::Error::Fault(Fault {
                            addr,
                            size: 64,
                            write: true,
                        })
                    })?;
                Ok(0)
            }
            _ => system::InterpIntrinsics.call(name, args),
        }
    }
}

/// Runs a function in the `relift-ir` interpreter, one instruction at a
/// time, lifting each from the guest memory the pager serves.
#[derive(Debug)]
pub struct IrEngine {
    features: Features,
    flip_read: u64,
}

impl IrEngine {
    /// An engine that lifts for a CPU with `features`.
    pub fn new(features: Features) -> IrEngine {
        IrEngine {
            features,
            flip_read: 0,
        }
    }

    /// Flips the low bit of the first byte of the `n`th memory read, a
    /// positive control: a run with it must report a mismatch.
    #[must_use]
    pub fn flip_read(mut self, n: u64) -> IrEngine {
        self.flip_read = n;
        self
    }
}

fn load(state: &Case) -> State {
    let file = regs::regs();
    let mut st = State::new(file.len());
    for i in 0..31 {
        st.regs[regs::x(i).index()] = state.get(&format!("x{i}")).unwrap_or(0);
    }
    let get = |n: &str| state.get(n).unwrap_or(0);
    st.regs[regs::SP.index()] = get("sp");
    for i in 0..32 {
        st.regs[regs::vreg(i).index()] = get(&format!("v{i}"));
    }
    for (i, f) in [regs::N, regs::Z, regs::C, regs::V].iter().enumerate() {
        st.regs[f.index()] = (get("nzcv") >> (31 - i)) & 1;
    }
    st.regs[regs::FPCR.index()] = get("fpcr");
    st.regs[regs::TPIDR_EL0.index()] = get("tpidr_el0");
    st.regs[regs::TPIDRRO_EL0.index()] = get("tpidrro_el0");
    st.regs[regs::FP_FLAGS.index()] = 0;
    st
}

fn store(st: &State) -> Vec<Reg> {
    let reg = |name: String, value: u128| Reg { name, value };
    let mut out: Vec<Reg> = (0..31)
        .map(|i| {
            reg(
                format!("x{i}"),
                st.regs[regs::x(i).index()] & u128::from(u64::MAX),
            )
        })
        .collect();
    out.push(reg(
        "sp".into(),
        st.regs[regs::SP.index()] & u128::from(u64::MAX),
    ));
    let mut nzcv = 0u128;
    for (i, f) in [regs::N, regs::Z, regs::C, regs::V].iter().enumerate() {
        nzcv |= (st.regs[f.index()] & 1) << (31 - i);
    }
    out.push(reg("nzcv".into(), nzcv));
    out.push(reg("fpcr".into(), st.regs[regs::FPCR.index()]));
    out.push(reg("tpidr_el0".into(), st.regs[regs::TPIDR_EL0.index()]));
    out.push(reg(
        "tpidrro_el0".into(),
        st.regs[regs::TPIDRRO_EL0.index()],
    ));
    out.extend((0..32).map(|i| reg(format!("v{i}"), st.regs[regs::vreg(i).index()])));
    out
}

impl IrEngine {
    fn fetch(&self, mem: &mut Mem<'_, '_>, pc: u64) -> Result<Block, Status> {
        let mut w = [0u8; 4];
        if mem.read(pc, &mut w).is_err() {
            return Err(Status::Left(format!(
                "pc {pc:#x} is not guest memory: a runtime-provided function"
            )));
        }
        let word = u32::from_le_bytes(w);
        lift::lift(word, pc, &self.features).map_err(|e| {
            if word == 0 {
                Status::Left(format!(
                    "pc {pc:#x} holds no guest code: a runtime-provided function"
                ))
            } else {
                Status::Failed(format!("lift {word:08x} at {pc:#x}: {e}"))
            }
        })
    }
}

impl Engine for IrEngine {
    fn run(&mut self, state: &Case, pager: &mut Pager<'_>, limit: u64) -> Outcome {
        let mut st = load(state);
        let sp0 = st.regs[regs::SP.index()];
        let pager = RefCell::new(pager);
        let mut mem = Mem {
            pager: &pager,
            reads: 0,
            flip: self.flip_read,
        };
        let mut intr = Intr { pager: &pager };
        let mut cache: BTreeMap<u64, Block> = BTreeMap::new();
        let mut pc = state.pc;
        let mut steps = 0u64;
        let mut low = sp0;
        let status = loop {
            if pc == state.stop && st.regs[regs::SP.index()] == sp0 {
                break Status::Returned;
            }
            if steps >= limit {
                break Status::Failed(format!("step limit {limit} reached at {pc:#x}"));
            }
            if let Entry::Vacant(slot) = cache.entry(pc) {
                match self.fetch(&mut mem, pc) {
                    Ok(b) => {
                        slot.insert(b);
                    }
                    Err(status) => break status,
                }
            }
            steps += 1;
            match interp::run(&cache[&pc], &mut st, &mut mem, &mut intr) {
                Ok(o) => {
                    if let Flow::Trap(code) = o.flow {
                        break if code & TRAP_BRK == 0 {
                            Status::Left(format!(
                                "system call svc #{code:#x} at {pc:#x}: runtime-provided"
                            ))
                        } else {
                            Status::Failed(format!("brk #{:#x} at {pc:#x}", code & !TRAP_BRK))
                        };
                    }
                    pc = o.next;
                    low = low.min(st.regs[regs::SP.index()]);
                }
                Err(e) => break Status::Failed(format!("{e} at {pc:#x}")),
            }
        };
        Outcome {
            status,
            steps,
            scratch: (low < sp0).then_some((low as u64, sp0 as u64)),
            regs: store(&st),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::io::Cursor;

    use relift_diff::Wire;

    use super::*;

    const MOVZ_X1_17: u32 = 0xd280_0221;
    const ADD_X0_X0_X1: u32 = 0x8b01_0000;
    const RET: u32 = 0xd65f_03c0;
    const SVC_7: u32 = 0xd400_00e1;

    fn code_page(words: &[u32]) -> Vec<u8> {
        let mut p = vec![0u8; 4096];
        for (i, w) in words.iter().enumerate() {
            p[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
        }
        [b"OK\n".as_slice(), &p].concat()
    }

    fn run(words: &[u32], flip: u64) -> Outcome {
        let mut wire = Wire::new(
            Box::new(std::io::BufReader::new(Cursor::new(code_page(words)))),
            Box::new(std::io::sink()),
        );
        let mut pager = Pager::new(&mut wire);
        let state = Case {
            pc: 0x1000,
            stop: 0x2000,
            regs: vec![
                Reg {
                    name: "x0".into(),
                    value: 5,
                },
                Reg {
                    name: "x30".into(),
                    value: 0x2000,
                },
            ],
        };
        IrEngine::new(Features::ALL)
            .flip_read(flip)
            .run(&state, &mut pager, 100)
    }

    fn x(o: &Outcome, name: &str) -> u128 {
        o.regs
            .iter()
            .find(|r| r.name == name)
            .expect("the register is reported")
            .value
    }

    #[test]
    fn a_function_runs_to_its_return() {
        let o = run(&[MOVZ_X1_17, ADD_X0_X0_X1, RET], 0);
        assert_eq!(o.status, Status::Returned);
        assert_eq!((o.steps, x(&o, "x0"), x(&o, "x1")), (3, 22, 17));
    }

    #[test]
    fn a_system_call_is_reported_as_the_runtime() {
        let o = run(&[MOVZ_X1_17, SVC_7, RET], 0);
        assert!(
            matches!(&o.status, Status::Left(w) if w.contains("svc #0x7 at 0x1004")),
            "{:?}",
            o.status
        );
    }

    #[test]
    fn a_jump_to_zeros_is_a_runtime_function() {
        let o = run(&[0x1400_0100], 0);
        assert!(
            matches!(&o.status, Status::Left(w) if w.contains("runtime-provided")),
            "{:?}",
            o.status
        );
    }

    #[test]
    fn the_names_cover_the_state() {
        assert_eq!(names().len(), 31 + 5 + 32);
        assert_eq!(context_names().len(), 31 + 2 + 32);
    }
}

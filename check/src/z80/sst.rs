//! `--oracle sst-z80 run DIR [PREFIX]`: SingleStepTests/z80 v1. Every case
//! compares all registers (WZ, Q, R, IFF, IM, EI and P included), the RAM
//! the case lists, the T-state count, the port list and the T-state, address
//! and kind of every bus access.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use super::cpu::Cpu;
use super::json::{self, J};
use super::mutate::Mutation;

pub const PAGES: [&str; 7] = ["base", "CB", "ED", "DD", "FD", "DDCB", "FDCB"];

/// The page of a test file by its name.
pub fn page_of_file(name: &str) -> usize {
    if name.starts_with("dd cb") {
        5
    } else if name.starts_with("fd cb") {
        6
    } else if name.starts_with("cb") {
        1
    } else if name.starts_with("ed") {
        2
    } else if name.starts_with("dd") {
        3
    } else if name.starts_with("fd") {
        4
    } else {
        0
    }
}

/// The access cycles SST lists: the T-state with the read or write strobe,
/// T2 of a memory cycle and T3 of an I/O cycle.
fn wanted_accesses(cycles: &[J]) -> Vec<(u64, u16, u8)> {
    let mut want = Vec::new();
    for (i, c) in cycles.iter().enumerate() {
        let pins = c.idx(2).str().as_bytes();
        if pins.len() < 4 {
            continue;
        }
        let (rd, wr, m, io) = (
            pins[0] == b'r',
            pins[1] == b'w',
            pins[2] == b'm',
            pins[3] == b'i',
        );
        if rd || wr {
            let kind = match (rd, m, io) {
                (true, true, _) => b'r',
                (false, true, _) => b'w',
                (true, _, true) => b'i',
                _ => b'o',
            };
            want.push((i as u64, c.idx(0).num() as u16, kind));
        }
    }
    want
}

/// Runs one SST case; gives the names of the parts that differ and a
/// description of the first.
pub fn run_case(cpu: &mut Cpu, t: &J) -> (Vec<String>, String) {
    let mut detail = String::new();
    let (Some(ini), Some(fin)) = (t.get("initial"), t.get("final")) else {
        return (vec!["malformed".into()], detail);
    };
    cpu.load(ini);
    let mut ports = Vec::new();
    if let Some(J::A(ps)) = t.get("ports") {
        for p in ps {
            if p.idx(2).str() == "r" {
                ports.push((p.idx(0).num() as u16, p.idx(1).num() as u8, false));
            }
        }
    }
    cpu.reset_bus(&ports);
    let mut bad = Vec::new();
    if let Err(e) = cpu.step() {
        return (vec![format!("error:{e}")], detail);
    }
    for (k, v) in cpu.dump() {
        if let Some(e) = fin.get(k)
            && e.num() != v
        {
            bad.push(k.to_string());
            let _ = write!(detail, " {k}={v:#x}/{:#x}", e.num());
        }
    }
    let b = cpu.bus.borrow();
    if let Some(J::A(ram)) = fin.get("ram") {
        for e in ram {
            let a = e.idx(0).num() as usize & 0xFFFF;
            if u64::from(b.mem[a]) != e.idx(1).num() {
                bad.push("ram".into());
                let _ = write!(detail, " ram[{a:#x}]={:#x}/{:#x}", b.mem[a], e.idx(1).num());
                break;
            }
        }
    }
    let Some(J::A(cycles)) = t.get("cycles") else {
        return (bad, detail);
    };
    if cycles.len() as u64 != b.clock {
        bad.push("tstates".into());
        let _ = write!(detail, " T={}/{}", b.clock, cycles.len());
    }
    let want = wanted_accesses(cycles);
    let mut got: Vec<(u64, u16, u8)> = b
        .log
        .iter()
        .map(|a| match a.kind {
            b'f' | b'r' => (a.t + 1, a.addr, b'r'),
            b'w' => (a.t + 1, a.addr, b'w'),
            k => (a.t + 2, a.addr, k),
        })
        .collect();
    got.sort_unstable();
    if got != want {
        bad.push("access".into());
        let _ = write!(detail, " acc={got:?}/{want:?}");
    }
    let mut pw: Vec<(u16, u8, u8)> = Vec::new();
    if let Some(J::A(ps)) = t.get("ports") {
        for p in ps {
            pw.push((
                p.idx(0).num() as u16,
                p.idx(1).num() as u8,
                p.idx(2).str().as_bytes().first().copied().unwrap_or(0),
            ));
        }
    }
    let gp: Vec<(u16, u8, u8)> = b
        .log
        .iter()
        .filter(|a| a.kind == b'i' || a.kind == b'o')
        .map(|a| (a.addr, a.val, if a.kind == b'i' { b'r' } else { b'w' }))
        .collect();
    if gp != pw {
        bad.push("ports".into());
        let _ = write!(detail, " ports={gp:?}/{pw:?}");
    }
    (bad, detail)
}

/// What one file gave.
#[derive(Default)]
struct FileResult {
    name: String,
    page: usize,
    cases: u64,
    pass: u64,
    fields: BTreeMap<String, u64>,
    first: Option<String>,
}

fn run_file(cpu: &mut Cpu, dir: &str, name: &str) -> Result<FileResult, String> {
    let text = std::fs::read(format!("{dir}/{name}")).map_err(|e| format!("{name}: {e}"))?;
    let J::A(tests) = json::parse(&text) else {
        return Err(format!("{name}: not an array"));
    };
    let mut r = FileResult {
        name: name.trim_end_matches(".json").to_string(),
        page: page_of_file(name),
        ..FileResult::default()
    };
    for t in &tests {
        r.cases += 1;
        let (bad, detail) = run_case(cpu, t);
        if bad.is_empty() {
            r.pass += 1;
            continue;
        }
        for b in &bad {
            let b = if b.starts_with("error") { "error" } else { b };
            *r.fields.entry(b.to_string()).or_default() += 1;
        }
        r.first.get_or_insert_with(|| {
            format!(
                "{}: {}{detail}",
                t.get("name").map_or("", J::str),
                bad.join(",")
            )
        });
    }
    Ok(r)
}

/// Runs the files of `dir` whose names start with `prefix`, on `jobs`
/// threads, and prints the table. Gives whether every case passed (with a
/// mutation: whether at least 95% of the files caught it; some mutants are
/// equivalent, like dropping the write of `LD B,B`).
pub fn run(
    dir: &str,
    prefix: Option<&str>,
    jobs: usize,
    mutation: Option<Mutation>,
) -> Result<bool, String> {
    let mut files: Vec<String> = std::fs::read_dir(dir)
        .map_err(|e| format!("{dir}: {e}"))?
        .filter_map(|e| e.ok()?.file_name().into_string().ok())
        .filter(|n| n.ends_with(".json"))
        .filter(|n| prefix.is_none_or(|p| n.starts_with(p)))
        .collect();
    files.sort();
    if files.is_empty() {
        return Err(format!("no test files in {dir}"));
    }
    let next = AtomicUsize::new(0);
    let results = Mutex::new(Vec::new());
    let failed = Mutex::new(Vec::new());
    std::thread::scope(|s| {
        for _ in 0..jobs.clamp(1, files.len()) {
            s.spawn(|| {
                let mut cpu = Cpu::new();
                cpu.mutation = mutation;
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some(f) = files.get(i) else { break };
                    match run_file(&mut cpu, dir, f) {
                        Ok(r) => results.lock().expect("lock").push(r),
                        Err(e) => failed.lock().expect("lock").push(e),
                    }
                }
            });
        }
    });
    if let Some(e) = failed.into_inner().expect("lock").first() {
        return Err(e.clone());
    }
    let mut results = results.into_inner().expect("lock");
    results.sort_by(|a, b| a.name.cmp(&b.name));

    let mut per_page = [[0u64; 4]; 7];
    let mut fields: BTreeMap<String, u64> = BTreeMap::new();
    for r in &results {
        let p = &mut per_page[r.page];
        p[0] += r.cases;
        p[1] += r.pass;
        p[2] += 1;
        p[3] += u64::from(r.pass == r.cases);
        for (k, v) in &r.fields {
            *fields.entry(format!("{}:{k}", PAGES[r.page])).or_default() += v;
        }
    }
    println!("page      cases     pass   files  files-all-pass");
    let mut tot = [0u64; 4];
    for (i, p) in per_page.iter().enumerate() {
        if p[0] == 0 {
            continue;
        }
        println!("{:6} {:8} {:8} {:7} {:7}", PAGES[i], p[0], p[1], p[2], p[3]);
        for (t, v) in tot.iter_mut().zip(p) {
            *t += v;
        }
    }
    println!("total  {:8} {:8} {:7} {:7}", tot[0], tot[1], tot[2], tot[3]);
    if !fields.is_empty() && mutation.is_none() {
        println!("\nmismatching parts (page:part cases):");
        for (k, v) in &fields {
            println!("  {k} {v}");
        }
    }
    let failing: Vec<&FileResult> = results.iter().filter(|r| r.pass != r.cases).collect();
    if let Some(m) = mutation {
        let missed: Vec<&str> = results
            .iter()
            .filter(|r| r.pass == r.cases)
            .map(|r| r.name.as_str())
            .collect();
        println!(
            "\nmutation {m:?}: caught in {} of {} files",
            results.len() - missed.len(),
            results.len()
        );
        for n in &missed {
            println!("  missed: {n}");
        }
        if let Some(f) = failing.first() {
            println!(
                "first mismatch: {}: {}",
                f.name,
                f.first.as_deref().unwrap_or("")
            );
        }
        return Ok(missed.len() * 20 <= results.len());
    }
    for f in failing.iter().take(20) {
        println!("\n{}: {}/{} {:?}", f.name, f.pass, f.cases, f.fields);
        println!("  first: {}", f.first.as_deref().unwrap_or(""));
    }
    if failing.len() > 20 {
        println!("\n... and {} more files with failures", failing.len() - 20);
    }
    Ok(failing.is_empty())
}

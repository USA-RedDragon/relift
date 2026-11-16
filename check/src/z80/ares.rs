//! `--oracle ares-z80`: random states for the Z80 core of ares (a separate
//! process, `check/oracle/ares`), and the comparison of its results with the
//! lifted templates on the IR interpreter.
//!
//! `z80-step` reads one state per line in SingleStepTests shape, runs one
//! instruction (or, with an `irq` field, takes that interrupt with 0xFF on
//! the data bus: 0 NMI, 1 IM 0, 2 IM 1, 3 IM 2) and prints the final state,
//! the bus accesses with their time and the T-state count.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::io::Write as _;
use std::path::PathBuf;
use std::process::Stdio;

use relift_cpu_z80::Entry;

use super::cpu::Cpu;
use super::json::{self, J};

const PAGE_NAMES: [&str; 8] = ["base", "CB", "ED", "DD", "FD", "DDCB", "FDCB", "IRQ"];

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    fn byte(&mut self) -> u64 {
        self.next() & 0xFF
    }

    fn word(&mut self) -> u64 {
        self.next() & 0xFFFF
    }

    fn bit(&mut self) -> u64 {
        self.next() & 1
    }
}

fn ram_json(ram: &BTreeMap<u16, u8>) -> String {
    let mut s = String::from("[");
    for (i, (a, v)) in ram.iter().enumerate() {
        let _ = write!(s, "{}[{a},{v}]", if i > 0 { "," } else { "" });
    }
    s.push(']');
    s
}

/// The templates have no prefix bytes as opcodes on the pages whose
/// prefixes are the other pages.
fn has_template(page: u8, code: u8) -> bool {
    !(matches!(page, 0 | 3 | 4) && matches!(code, 0xCB | 0xDD | 0xED | 0xFD))
}

/// `n` random states per template, one JSON line each: random registers
/// (WZ, Q, IM, IFF, EI and P included), random operand bytes, RAM at every
/// address the instruction can touch, and short block counts now and then.
pub fn states(n: usize, seed: u64) -> Vec<String> {
    let mut rng = Rng(seed | 1);
    let mut lines = Vec::new();
    for page in 0..7u8 {
        for code in 0..=255u8 {
            if !has_template(page, code) {
                continue;
            }
            for k in 0..n {
                lines.push(one_state(&mut rng, page, code, k));
            }
        }
    }
    lines
}

fn one_state(rng: &mut Rng, page: u8, code: u8, k: usize) -> String {
    let pc = rng.word() as u16;
    let mut bytes = [
        rng.byte() as u8,
        rng.byte() as u8,
        rng.byte() as u8,
        rng.byte() as u8,
    ];
    match page {
        0 => bytes[0] = code,
        1 => bytes[..2].copy_from_slice(&[0xCB, code]),
        2 => bytes[..2].copy_from_slice(&[0xED, code]),
        3 => bytes[..2].copy_from_slice(&[0xDD, code]),
        4 => bytes[..2].copy_from_slice(&[0xFD, code]),
        5 => {
            bytes[..2].copy_from_slice(&[0xDD, 0xCB]);
            bytes[3] = code;
        }
        _ => {
            bytes[..2].copy_from_slice(&[0xFD, 0xCB]);
            bytes[3] = code;
        }
    }
    let f = rng.byte();
    let q = if rng.bit() == 0 { 0 } else { f };
    let mut regs: Vec<(&str, u64)> = vec![
        ("pc", u64::from(pc)),
        ("sp", rng.word()),
        ("a", rng.byte()),
        ("b", rng.byte()),
        ("c", rng.byte()),
        ("d", rng.byte()),
        ("e", rng.byte()),
        ("f", f),
        ("h", rng.byte()),
        ("l", rng.byte()),
        ("i", rng.byte()),
        ("r", rng.byte()),
        ("ei", rng.bit()),
        ("wz", rng.word()),
        ("ix", rng.word()),
        ("iy", rng.word()),
        ("af_", rng.word()),
        ("bc_", rng.word()),
        ("de_", rng.word()),
        ("hl_", rng.word()),
        ("im", rng.next() % 3),
        ("p", rng.bit()),
        ("q", q),
        ("iff1", rng.bit()),
        ("iff2", rng.bit()),
    ];
    if rng.next().is_multiple_of(4) {
        regs[3].1 = rng.next() % 3;
        regs[4].1 = rng.next() % 3;
    }
    let g = |k: &str| regs.iter().find(|r| r.0 == k).map_or(0, |r| r.1);
    let pair = |h: &str, l: &str| (g(h) << 8 | g(l)) as u16;
    let disp = |base: u64| (base as u16).wrapping_add(bytes[2] as i8 as u16);
    let nn = |o: usize| u16::from(bytes[o]) | u16::from(bytes[o + 1]) << 8;
    let hl = pair("h", "l");
    let sp = g("sp") as u16;
    let mut ram: BTreeMap<u16, u8> = BTreeMap::new();
    for a in [
        hl,
        hl.wrapping_add(1),
        hl.wrapping_sub(1),
        pair("b", "c"),
        pair("d", "e"),
        sp,
        sp.wrapping_add(1),
        sp.wrapping_sub(1),
        sp.wrapping_sub(2),
        disp(g("ix")),
        disp(g("iy")),
        nn(1),
        nn(1).wrapping_add(1),
        nn(2),
        nn(2).wrapping_add(1),
    ] {
        ram.insert(a, rng.byte() as u8);
    }
    for (i, b) in bytes.iter().enumerate() {
        ram.insert(pc.wrapping_add(i as u16), *b);
    }
    let mut o = format!("{{\"name\":\"{page} {code:02x} {k}\",\"initial\":{{");
    for (k, v) in &regs {
        let _ = write!(o, "\"{k}\":{v},");
    }
    let _ = write!(o, "\"ram\":{}}},\"ports\":[]}}", ram_json(&ram));
    o
}

/// `n * 10` random interrupt states per entry: IFF1 set, no `EI` just
/// before, IM as the entry says (any for an NMI), 0xFF on the data bus.
pub fn irq_states(n: usize, seed: u64) -> Vec<String> {
    let mut rng = Rng(seed | 1);
    let mut lines = Vec::new();
    for kind in 0..4u8 {
        for k in 0..n * 10 {
            let pc = rng.word();
            let f = rng.byte();
            let q = if rng.bit() == 0 { 0 } else { f };
            let im = if kind == 0 {
                rng.next() % 3
            } else {
                u64::from(kind) - 1
            };
            let sp = rng.word() as u16;
            let i = rng.byte();
            let vector = (i << 8 | 0xFF) as u16;
            let mut ram: BTreeMap<u16, u8> = BTreeMap::new();
            for a in [
                sp.wrapping_sub(1),
                sp.wrapping_sub(2),
                vector,
                vector.wrapping_add(1),
            ] {
                ram.insert(a, rng.byte() as u8);
            }
            let (a, b, c, d, e) = (rng.byte(), rng.byte(), rng.byte(), rng.byte(), rng.byte());
            let (h, l, r) = (rng.byte(), rng.byte(), rng.byte());
            let (wz, ix, iy) = (rng.word(), rng.word(), rng.word());
            let (af_, bc_, de_, hl_) = (rng.word(), rng.word(), rng.word(), rng.word());
            let p = rng.bit();
            let iff1 = if kind == 0 { rng.bit() } else { 1 };
            let iff2 = rng.bit();
            lines.push(format!(
                "{{\"name\":\"7 {kind:02x} {k}\",\"irq\":{kind},\"initial\":{{\"pc\":{pc},\"sp\":{sp},\"a\":{a},\"b\":{b},\"c\":{c},\"d\":{d},\"e\":{e},\"f\":{f},\"h\":{h},\"l\":{l},\"i\":{i},\"r\":{r},\"ei\":0,\"wz\":{wz},\"ix\":{ix},\"iy\":{iy},\"af_\":{af_},\"bc_\":{bc_},\"de_\":{de_},\"hl_\":{hl_},\"im\":{im},\"p\":{p},\"q\":{q},\"iff1\":{iff1},\"iff2\":{iff2},\"ram\":{}}},\"ports\":[]}}",
                ram_json(&ram)
            ));
        }
    }
    lines
}

/// A departure of ares from SingleStepTests and the real chip that the
/// comparison knows: the page, the opcode and the parts that differ.
fn known_departure(page: usize, code: u8, bad: &[String]) -> Option<&'static str> {
    let only = |parts: &[&str]| bad.iter().all(|b| parts.contains(&b.as_str()));
    match (page, code) {
        (2, 0x40 | 0x48) if only(&["wz"]) => {
            Some("ED 40/48: WZ taken from BC after IN wrote B or C")
        }
        (3 | 4, 0x37 | 0x3F) if only(&["f", "q"]) => Some("DD/FD 37/3F: Q kept across the prefix"),
        (0 | 3 | 4, 0xE3) if only(&["access"]) => Some("EX (SP),HL/IX/IY: low byte written first"),
        (7, 0 | 3) if only(&["access"]) => Some("NMI and IM 2: acknowledge and push order"),
        _ => None,
    }
}

/// What a comparison found.
#[derive(Default)]
pub struct Report {
    pub states: [u64; 8],
    pub equal: [u64; 8],
    pub known: BTreeMap<&'static str, u64>,
    pub unexplained: u64,
    pub first: Vec<String>,
}

fn compare_one(cpu: &mut Cpu, t: &J, r: &J, detail: &mut String) -> Vec<String> {
    let (Some(ini), Some(fin)) = (t.get("initial"), r.get("final")) else {
        return vec!["malformed".into()];
    };
    cpu.load(ini);
    cpu.reset_bus(&[]);
    let run = match t.get("irq") {
        Some(k) => cpu.accept(match k.num() {
            0 => Entry::Nmi,
            1 => Entry::Im0,
            2 => Entry::Im1,
            _ => Entry::Im2,
        }),
        None => cpu.step(),
    };
    if let Err(e) = run {
        return vec![format!("error {e}")];
    }
    let mut bad = Vec::new();
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
                let _ = write!(detail, " ram[{a:#x}]");
                break;
            }
        }
    }
    let want_t = r.get("tstates").map_or(0, J::num);
    if want_t != b.clock {
        bad.push("tstates".into());
        let _ = write!(detail, " T={}/{want_t}", b.clock);
    }
    // ares logs an access after its clocks: M1 fetches and I/O after 4
    // T-states, other memory cycles after 3.
    let mut got: Vec<(u64, u16, u8, u8)> = b
        .log
        .iter()
        .map(|a| {
            let (k, d) = match a.kind {
                b'f' if a.m1 => (b'f', 4),
                b'f' | b'r' => (b'r', 3),
                b'w' => (b'w', 3),
                k => (k, 4),
            };
            (a.t + d, a.addr, a.val, k)
        })
        .collect();
    got.sort_unstable();
    let mut want: Vec<(u64, u16, u8, u8)> = Vec::new();
    if let Some(J::A(cs)) = r.get("cycles") {
        for c in cs {
            let kind = c.idx(2).str().as_bytes().first().copied().unwrap_or(0);
            want.push((
                c.idx(3).num(),
                c.idx(0).num() as u16,
                c.idx(1).num() as u8,
                kind,
            ));
        }
    }
    want.sort_unstable();
    if got != want {
        bad.push("access".into());
        let _ = write!(detail, " acc={got:?}/{want:?}");
    }
    bad
}

/// Compares the states `ins` (JSON lines) with the oracle's results `outs`.
pub fn compare(ins: &str, outs: &str) -> Report {
    let mut cpu = Cpu::new();
    let mut rep = Report::default();
    let mut by_op: BTreeMap<String, (u64, u64)> = BTreeMap::new();
    for (li, lo) in ins.lines().zip(outs.lines()) {
        let t = json::parse(li.as_bytes());
        let r = json::parse(lo.as_bytes());
        let name = t.get("name").map_or("", J::str).to_string();
        let mut it = name.split(' ');
        let page: usize = it.next().and_then(|p| p.parse().ok()).unwrap_or(0).min(7);
        let code = it
            .next()
            .and_then(|c| u8::from_str_radix(c, 16).ok())
            .unwrap_or(0);
        let mut detail = String::new();
        let bad = compare_one(&mut cpu, &t, &r, &mut detail);
        rep.states[page] += 1;
        let e = by_op
            .entry(format!("{} {code:02x}", PAGE_NAMES[page]))
            .or_default();
        e.0 += 1;
        if bad.is_empty() {
            rep.equal[page] += 1;
            e.1 += 1;
        } else if let Some(why) = known_departure(page, code, &bad) {
            *rep.known.entry(why).or_default() += 1;
        } else {
            rep.unexplained += 1;
            if rep.first.len() < 10 {
                rep.first.push(format!("{name}: {}{detail}", bad.join(",")));
            }
        }
    }
    rep
}

impl Report {
    pub fn print(&self, what: &str) {
        println!("{what}\npage     states    equal");
        let (mut s, mut e) = (0, 0);
        for (i, name) in PAGE_NAMES.iter().enumerate() {
            if self.states[i] == 0 {
                continue;
            }
            println!("{name:6} {:8} {:8}", self.states[i], self.equal[i]);
            s += self.states[i];
            e += self.equal[i];
        }
        println!("total  {s:8} {e:8}");
        println!("known ares departures (see check/oracle/ares/README.md):");
        for (why, n) in &self.known {
            println!("  {n:6} {why}");
        }
        println!("unexplained differences: {}", self.unexplained);
        for f in &self.first {
            println!("  {f}");
        }
    }
}

/// Where an oracle binary is: the override variable, else the default
/// cache of `check/oracle/ares/build.sh`.
pub fn binary(var: &str, name: &str) -> PathBuf {
    std::env::var_os(var).map_or_else(
        || {
            let home = std::env::var_os("HOME")
                .map(PathBuf::from)
                .unwrap_or_default();
            home.join(".cache/relift/ares-oracle/bin").join(name)
        },
        PathBuf::from,
    )
}

/// Runs the oracle binary `bin` on `input` and gives what it prints.
pub fn run_oracle(bin: &PathBuf, input: String) -> Result<String, String> {
    let mut child = relift_proc::command(bin)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .map_err(|e| format!("{}: {e}", bin.display()))?;
    let mut stdin = child.stdin.take().ok_or("no stdin")?;
    let feeder = std::thread::spawn(move || stdin.write_all(input.as_bytes()));
    let out = child.wait_with_output().map_err(|e| e.to_string())?;
    feeder
        .join()
        .map_err(|_| "the writer panicked".to_string())?
        .map_err(|e| e.to_string())?;
    if !out.status.success() {
        return Err(format!("{} failed: {}", bin.display(), out.status));
    }
    String::from_utf8(out.stdout).map_err(|e| e.to_string())
}

/// Generates the states, runs them on the oracle binary `bin` and compares:
/// the instructions, then the interrupt entries. Gives whether every
/// difference is a known departure.
pub fn check(n: usize, seed: u64, bin: &PathBuf) -> Result<bool, String> {
    let mut ok = true;
    for (what, lines) in [
        (
            format!("instructions: {n} states per template, seed {seed}"),
            states(n, seed),
        ),
        (
            format!(
                "interrupt entries: {} states per entry, seed {seed}",
                n * 10
            ),
            irq_states(n, seed),
        ),
    ] {
        let mut input = lines.join("\n");
        input.push('\n');
        let out = run_oracle(bin, input.clone())?;
        let rep = compare(&input, &out);
        rep.print(&what);
        ok &= rep.unexplained == 0;
    }
    Ok(ok)
}

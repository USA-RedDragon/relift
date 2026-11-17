//! The places a suspended thread resumes at.
//!
//! The SDK switches threads by saving the running thread's registers into
//! its context and loading another's. The save stores every register but
//! the PC with one `STM Rn,{R0-R14}`, then the address the thread resumes
//! at, made by `ADD Rd,PC,#imm` and stored right after with
//! `STR Rd,[Rn,#imm]` into the same block. The load returns there, and the
//! thread then returns from every call up its stack that led to the save.
//! A CPU that waits for an interrupt (the CP15 wait or a BIOS halt), or
//! that may take one at once (an `MSR` that writes the CPSR's control
//! bits), leaves its host frames the same way: the interrupt may switch
//! threads. Dispatch reaches each of those return
//! addresses with no host frame to return into, so each is a place the
//! thread resumes at: the instruction after a save or a wait, and the
//! return address of every call that may reach one. A call whose target is
//! not known may reach one.

use std::collections::{BTreeMap, BTreeSet};

use relift_cpu_arm32_analyze::{EdgeKind, Function, Loc, Report};

/// How far before the PC store the `STM` of the other registers may be,
/// in instructions.
const SAVE_WINDOW: u32 = 8;

/// BIOS calls that wait for an interrupt: `IntrWait`, `VBlankIntrWait`,
/// `Halt`.
const WAIT_SWIS: [u8; 3] = [0x04, 0x05, 0x06];

/// The value of an ARM data-processing immediate: 8 bits rotated right by
/// twice the rotate field.
fn arm_imm(w: u32) -> u32 {
    (w & 0xFF).rotate_right(((w >> 8) & 0xF) * 2)
}

/// The resume points of a context save whose PC store is the ARM
/// instruction at `at`, `word` reading the code: the instruction the
/// stored address names and the one before it (the load returns with
/// `SUBS PC,LR,#4` or jumps to the address as is).
fn saved_pc(at: Loc, word: &dyn Fn(u32) -> Option<u32>) -> Vec<Loc> {
    let (Some(add), Some(store)) = (word(at.addr), word(at.addr.wrapping_add(4))) else {
        return Vec::new();
    };
    let rd = (add >> 12) & 0xF;
    let is_add_pc = add & 0xFFFF_0000 == 0xE28F_0000;
    let is_store = store & 0xFFF0_0000 == 0xE580_0000 && (store >> 12) & 0xF == rd;
    if !is_add_pc || !is_store {
        return Vec::new();
    }
    let rn = (store >> 16) & 0xF;
    let saves = (1..=SAVE_WINDOW).any(|k| {
        word(at.addr.wrapping_sub(4 * k))
            .is_some_and(|s| s & 0xFFB0_FFFF == 0xE880_7FFF && (s >> 16) & 0xF == rn)
    });
    if !saves {
        return Vec::new();
    }
    let target = at.addr.wrapping_add(8).wrapping_add(arm_imm(add));
    vec![
        Loc::rom(target.wrapping_sub(4), false),
        Loc::rom(target, false),
    ]
}

/// Whether the ARM instruction `w` is the CP15 wait for interrupt
/// (`MCR p15,0,Rd,c7,c0,4`).
fn is_cp15_wait(w: u32) -> bool {
    w & 0xFFFF_0FFF == 0xEE07_0F90
}

/// Whether the ARM instruction `w` writes the CPSR's control field (`MSR
/// CPSR_c`, from a register or an immediate), which may enable interrupts.
fn is_cpsr_control_write(w: u32) -> bool {
    let imm = w & 0x0FB0_F000 == 0x0320_F000;
    let reg = w & 0x0FB0_FFF0 == 0x0120_F000;
    (imm || reg) && w & (1 << 22) == 0 && w & (1 << 16) != 0 && w >> 28 != 0xF
}

/// Whether the instruction `w` at `at` calls through a register
/// (`BLX Rm`).
fn is_register_call(at: Loc, w: u32) -> bool {
    if at.thumb {
        w & 0xFF87 == 0x4780
    } else {
        w & 0x0FFF_FFF0 == 0x012F_FF30 && w >> 28 != 0xF
    }
}

/// The constant an ARM `LDR LR,[PC,#imm]` at `at` loads, `word` reading
/// the code.
fn lr_literal(at: Loc, w: u32, word: &dyn Fn(u32) -> Option<u32>) -> Option<u32> {
    if at.thumb || w & 0x0F7F_F000 != 0x051F_E000 {
        return None;
    }
    let off = w & 0xFFF;
    let base = at.addr.wrapping_add(8);
    let lit = if w & (1 << 23) != 0 {
        base.wrapping_add(off)
    } else {
        base.wrapping_sub(off)
    };
    word(lit)
}

/// What one function contributes: whether it waits or saves a context
/// itself, the instructions after those, the return addresses of its
/// calls with the edge targets each call reaches (none for a call whose
/// target is not known), and the functions it jumps to.
struct Calls {
    own: bool,
    points: Vec<Loc>,
    calls: Vec<(Loc, Vec<(u32, bool)>)>,
    tails: Vec<(u32, bool)>,
}

fn scan(f: &Function, swis: &BTreeSet<Loc>, word: &dyn Fn(u32) -> Option<u32>) -> Calls {
    let mut out = Calls {
        own: false,
        points: Vec::new(),
        calls: Vec::new(),
        tails: Vec::new(),
    };
    let mut targets: BTreeMap<Loc, Vec<(u32, bool)>> = BTreeMap::new();
    for e in &f.edges {
        if matches!(
            e.kind,
            EdgeKind::Call | EdgeKind::LinkedJump | EdgeKind::Via
        ) || (e.kind == EdgeKind::Pointer && f.linked.contains(&e.site))
        {
            targets
                .entry(e.site)
                .or_default()
                .push((e.target.addr, e.target.thumb));
        } else {
            out.tails.push((e.target.addr, e.target.thumb));
        }
    }
    let mut lr_set: Option<u32> = None;
    for (&l, &n) in &f.instructions {
        if l.copy.is_some() {
            continue;
        }
        let next = Loc::rom(l.addr.wrapping_add(u32::from(n)), l.thumb);
        let Some(raw) = word(l.addr) else {
            continue;
        };
        let w = if l.thumb { raw & 0xFFFF } else { raw };
        if swis.contains(&l) || (!l.thumb && (is_cp15_wait(w) || is_cpsr_control_write(w))) {
            out.own = true;
            out.points.push(next);
        }
        if !l.thumb {
            let found = saved_pc(l, word);
            if !found.is_empty() {
                out.own = true;
                out.points.extend(found);
            }
        }
        if let Some(t) = targets.remove(&l) {
            out.calls.push((next, t));
        } else if is_register_call(l, w) || f.linked.contains(&l) {
            out.calls.push((next, Vec::new()));
        } else if let Some(r) = lr_set.take()
            && !l.thumb
            && w & 0x0FFF_FFF0 == 0x012F_FF10
        {
            out.calls.push((Loc::rom(r & !1, r & 1 != 0), Vec::new()));
        }
        if let Some(r) = lr_literal(l, w, word) {
            lr_set = Some(r);
        }
    }
    out
}

/// The resume points in the functions of one processor's reports, one map
/// per report from each resume point to the entry of the function of that
/// report it is an instruction of; `word` reads the word at an address in
/// the code of a report's run, by the report's index. A resume point that
/// is a function entry of its report, or no instruction of its function,
/// is left out.
#[must_use]
pub fn resumes(
    reports: &[&Report],
    word: &dyn Fn(usize, u32) -> Option<u32>,
) -> Vec<BTreeMap<Loc, Loc>> {
    let key = |l: Loc| (l.addr, l.thumb);
    let mut scans: Vec<(usize, &Function, Calls)> = Vec::new();
    for (k, r) in reports.iter().enumerate() {
        let swis: BTreeSet<Loc> = r
            .swis
            .iter()
            .filter(|s| WAIT_SWIS.contains(&s.number))
            .map(|s| s.site)
            .collect();
        for f in r.functions.values() {
            scans.push((k, f, scan(f, &swis, &|a| word(k, a))));
        }
    }
    let mut switching: BTreeSet<(u32, bool)> = scans
        .iter()
        .filter(|(_, _, c)| c.own)
        .map(|(_, f, _)| key(f.entry))
        .collect();
    let mut out = vec![BTreeMap::new(); reports.len()];
    for (k, f, c) in &scans {
        for p in &c.points {
            out[*k].insert(*p, f.entry);
        }
    }
    loop {
        let before = switching.len();
        for (k, f, c) in &scans {
            if c.tails.iter().any(|t| switching.contains(t)) {
                switching.insert(key(f.entry));
            }
            for (back, ts) in &c.calls {
                if ts.is_empty() || ts.iter().any(|t| switching.contains(t)) {
                    switching.insert(key(f.entry));
                    out[*k].insert(*back, f.entry);
                }
            }
        }
        if switching.len() == before {
            break;
        }
    }
    for (k, m) in out.iter_mut().enumerate() {
        m.retain(|l, f| {
            !reports[k].functions.contains_key(l)
                && reports[k]
                    .functions
                    .get(f)
                    .is_some_and(|f| f.instructions.contains_key(l))
        });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_cpu_arm32_analyze::{Edge, Origin};

    fn function(entry: u32, words: &[u32], edges: &[(u32, u32)]) -> Function {
        Function {
            entry: Loc::rom(entry, false),
            origin: Origin::Called,
            instructions: (0..words.len() as u32)
                .map(|i| (Loc::rom(entry + 4 * i, false), 4))
                .collect(),
            edges: edges
                .iter()
                .map(|&(site, target)| Edge {
                    site: Loc::rom(site, false),
                    target: Loc::rom(target, false),
                    kind: EdgeKind::Call,
                })
                .collect(),
            tables: Vec::new(),
            returns: true,
            linked: BTreeSet::new(),
        }
    }

    #[test]
    fn a_context_save_and_its_callers_resume_after_the_save_and_each_call() {
        let code: BTreeMap<u32, Vec<u32>> = BTreeMap::from([
            (
                0x100,
                vec![
                    0xE881_7FFF,
                    0xE28F_0008,
                    0xE581_003C,
                    0xE3A0_0000,
                    0xE12F_FF1E,
                    0xE3A0_0001,
                ],
            ),
            (0x200, vec![0xEB00_0000, 0xE12F_FF1E]),
            (0x300, vec![0xEB00_0000, 0xE12F_FF1E]),
            (0x400, vec![0xE12F_FF33, 0xE12F_FF1E]),
            (0x500, vec![0xEB00_0000, 0xE12F_FF1E]),
        ]);
        let edges: BTreeMap<u32, Vec<(u32, u32)>> = BTreeMap::from([
            (0x200, vec![(0x200, 0x100)]),
            (0x300, vec![(0x300, 0x200)]),
            (0x500, vec![(0x500, 0x600)]),
        ]);
        let mut report = Report::default();
        for (&at, words) in &code {
            let e = edges.get(&at).cloned().unwrap_or_default();
            report
                .functions
                .insert(Loc::rom(at, false), function(at, words, &e));
        }
        let word = |_: usize, a: u32| {
            code.iter()
                .find_map(|(&b, w)| w.get(a.wrapping_sub(b) as usize / 4).filter(|_| a >= b))
                .copied()
        };
        let found = resumes(&[&report], &word);
        let got: Vec<(u32, u32)> = found[0].iter().map(|(l, f)| (l.addr, f.addr)).collect();
        assert_eq!(
            got,
            [
                (0x110, 0x100),
                (0x114, 0x100),
                (0x204, 0x200),
                (0x304, 0x300),
                (0x404, 0x400)
            ]
        );
    }
}

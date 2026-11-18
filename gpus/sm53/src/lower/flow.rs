//! The control-flow pass: Maxwell's stack of reconvergence points
//! (`SSY`/`SYNC`, `PBK`/`BRK`, `PCNT`/`CONT`) resolved to plain branches.
//!
//! `SSY`, `PBK` and `PCNT` push a code offset and its kind on a per-thread
//! stack; `SYNC`, `BRK` and `CONT` pop entries until one of their own kind
//! and jump to it (a `BRK` unwinds the `SSY` entries above its `PBK`). For
//! one thread that is only a jump, so the pass walks every path from the
//! first instruction with its stack, as Ryujinx's decoder (MIT) propagates
//! the pushes into their pops, and gives each pop the one target every path
//! agrees on. The pop lowers to an SM86 `BRA` with the same guard; the push
//! to a `BSSY` on the barrier of its stack depth with the same target, which
//! tells the SM86 translator where the guest's lanes come together again.
//!
//! A pop that more than one push reaches, a push at two stack depths, a
//! condition code test, and any call, return or indirect jump stop with
//! their reason. An unguarded `KIL` ends its path, as Ryujinx's decoder and
//! the program walk take it; the SM86 `KILL` it lowers to demotes and goes
//! on, so the pass marks the instruction after it, when nothing else
//! reaches that, to end the invocation (see [`Flow::kil_exits`]).

use std::collections::{BTreeMap, BTreeSet, HashSet};

use crate::bundle::{BUNDLE, is_sched, slots};
use crate::form::{Op, Src};
use crate::inst::Inst;

use super::Stop;

/// The most stack states the walk visits.
const MAX_STATES: usize = 1 << 16;
/// Convergence barriers SM86 has (B0 to B15).
const BARRIERS: usize = 16;
/// The condition code test that always holds.
const CC_T: u64 = 15;

/// The kind of a stack entry: which pop consumes it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum Kind {
    Sync,
    Brk,
    Cont,
}

/// What the pass found: which instructions run, where each pop goes and
/// which barrier each push uses.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Flow {
    /// Per instruction index: whether control reaches it.
    pub reached: Vec<bool>,
    /// The target instruction index of each `SYNC`, `BRK` and `CONT`.
    pub pops: BTreeMap<usize, usize>,
    /// The barrier (stack depth) and target index of each `SSY`, `PBK` and
    /// `PCNT`.
    pub pushes: BTreeMap<usize, (u8, usize)>,
    /// Indices after an unguarded `KIL` that nothing else reaches: each
    /// lowers to an unguarded `EXIT`, so that the demoted invocation ends
    /// where the Maxwell thread did. An index may be one past the code.
    pub kil_exits: BTreeSet<usize>,
}

/// The instruction index of code offset `at` (scheduling words skipped).
pub fn index(at: u64) -> usize {
    let b = BUNDLE as u64;
    ((at / b) * 3 + (at % b) / 8 - 1) as usize
}

/// The code offset of instruction index `k`.
pub fn offset(k: usize) -> u64 {
    (k / 3 * BUNDLE + 8 + 8 * (k % 3)) as u64
}

/// The instruction index a relative branch at `at` with word `w` reaches:
/// the signed 24-bit offset (bits 20 to 43) after the instruction; a target
/// on a scheduling word runs the bundle's first instruction. `None` for a
/// negative or unaligned target.
pub fn branch_target(at: u64, w: u64) -> Option<usize> {
    let imm = (((w >> 20) & 0xff_ffff) as i64) << 40 >> 40;
    let t = at as i64 + 8 + imm;
    if t < 0 || t & 7 != 0 {
        return None;
    }
    let t = t as u64;
    Some(index(if is_sched(t) { t + 8 } else { t }))
}

fn stop(i: &Inst, reason: &'static str) -> Stop {
    Stop {
        at: i.at,
        op: i.op(),
        src: i.enc.src,
        reason,
    }
}

/// The pass over `code` (whole bundles, from the first instruction).
///
/// # Errors
///
/// The first construct the pass does not resolve, with its reason.
pub fn flow(code: &[u8]) -> Result<Flow, Stop> {
    let insts: Vec<Option<Inst>> = slots(code).map(|s| Inst::decode(s.word, s.at)).collect();
    let n = insts.len();
    let mut f = Flow {
        reached: vec![false; n],
        ..Flow::default()
    };
    let mut kil_ends = Vec::new();
    let mut seen: HashSet<(usize, Vec<(usize, Kind)>)> = HashSet::new();
    let mut work: Vec<(usize, Vec<(usize, Kind)>)> = vec![(0, Vec::new())];
    let past = |k: usize| Stop {
        at: offset(k),
        op: Op::Nop,
        src: Src::None,
        reason: "control runs past the code",
    };
    while let Some((k, stack)) = work.pop() {
        if !seen.insert((k, stack.clone())) {
            continue;
        }
        if seen.len() > MAX_STATES {
            return Err(Stop {
                at: offset(k),
                op: Op::Nop,
                src: Src::None,
                reason: "more control stack states than the pass follows",
            });
        }
        let Some(i) = insts.get(k).ok_or_else(|| past(k))? else {
            return Err(Stop {
                at: offset(k),
                op: Op::Nop,
                src: Src::None,
                reason: "no Maxwell form matches a reachable word",
            });
        };
        f.reached[k] = true;
        let (p, inv) = i.guard().unwrap_or((7, false));
        let never = p == 7 && inv;
        let guarded = p != 7;
        let cc_t = i.get("Ccc").is_none_or(|c| c == CC_T);
        let mut next = vec![(k + 1, stack.clone())];
        if never {
            work.extend(next);
            continue;
        }
        match i.op() {
            Op::Ssy | Op::Pbk | Op::Pcnt => {
                if i.get("Ca") == Some(1) {
                    return Err(stop(i, "push of a target in a constant bank"));
                }
                let t = branch_target(i.at, i.word)
                    .ok_or_else(|| stop(i, "push target outside the code"))?;
                let depth = stack.len();
                if depth >= BARRIERS {
                    return Err(stop(i, "control stack deeper than the SM86 barriers"));
                }
                match f.pushes.insert(k, (depth as u8, t)) {
                    Some((d, _)) if d as usize != depth => {
                        return Err(stop(i, "push at two control stack depths"));
                    }
                    _ => {}
                }
                let kind = match i.op() {
                    Op::Ssy => Kind::Sync,
                    Op::Pbk => Kind::Brk,
                    _ => Kind::Cont,
                };
                next[0].1.push((t, kind));
            }
            Op::Sync | Op::Brk | Op::Cont => {
                if !cc_t {
                    return Err(stop(i, "pop on a condition code test"));
                }
                let want = match i.op() {
                    Op::Sync => Kind::Sync,
                    Op::Brk => Kind::Brk,
                    _ => Kind::Cont,
                };
                let mut rest = stack.clone();
                let t = loop {
                    match rest.pop() {
                        Some((t, kind)) if kind == want => break t,
                        Some(_) => {}
                        None => return Err(stop(i, "pop with no push of its kind")),
                    }
                };
                if f.pops.insert(k, t).is_some_and(|x| x != t) {
                    return Err(stop(i, "pop that more than one push reaches"));
                }
                if !guarded {
                    next.clear();
                }
                next.push((t, rest));
            }
            Op::Bra => {
                if !cc_t {
                    return Err(stop(i, "branch on a condition code test"));
                }
                if i.get("Ca") == Some(1) || i.get("Lmt") == Some(1) || i.get("U") == Some(1) {
                    return Err(stop(i, "BRA with .CA, .LMT or .U"));
                }
                let t = branch_target(i.at, i.word)
                    .ok_or_else(|| stop(i, "branch target outside the code"))?;
                if !guarded {
                    next.clear();
                }
                next.push((t, stack.clone()));
            }
            Op::Exit => {
                if !cc_t {
                    return Err(stop(i, "EXIT on a condition code test"));
                }
                if i.get("KeepRefCnt") == Some(1) {
                    return Err(stop(i, "EXIT.KEEPREFCOUNT"));
                }
                if !guarded {
                    next.clear();
                }
            }
            Op::Kil => {
                if !cc_t {
                    return Err(stop(i, "KIL on a condition code test"));
                }
                if !guarded {
                    next.clear();
                    kil_ends.push(k);
                }
            }
            Op::Cal | Op::Ret | Op::Pret | Op::Jcal => {
                return Err(stop(i, "subroutine call or return"));
            }
            Op::Brx | Op::Jmp | Op::Jmx => {
                return Err(stop(
                    i,
                    "indirect jump (its targets are in a constant bank)",
                ));
            }
            Op::Longjmp | Op::Plongjmp | Op::Pexit | Op::Sam | Op::Ram | Op::Rtt | Op::Bpt => {
                return Err(stop(i, "control instruction with no counterpart"));
            }
            _ => {}
        }
        for (t, s) in next {
            if t >= n {
                return Err(past(t));
            }
            work.push((t, s));
        }
    }
    for k in kil_ends {
        if !f.reached.get(k + 1).copied().unwrap_or(false) {
            f.kil_exits.insert(k + 1);
        }
    }
    Ok(f)
}

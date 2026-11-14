//! The FPSR census: whether a program can observe FPSR's cumulative flags.
//!
//! Producing the flags costs time, so a build produces them only for a
//! program that reads them: one with an `MRS Xt, FPSR` whose result can be
//! used. A read whose register is overwritten on every path before any use
//! (libgcc's soft-float `__sfp_handle_exceptions` raises exceptions this way)
//! does not count. A result left in `x0` at a return counts only when a
//! caller uses `x0` after the call, found by following call sites.
//!
//! Register use is conservative: any IR `Get` of the register is a use, a
//! call uses `x0` to `x7` (and preserves `x19` to `x29`, so tracking goes on
//! after it), a computed jump uses everything, and a return uses `x0`.

use std::collections::{BTreeMap, BTreeSet};

use relift_ir::{Block, Flow, Next, Op};

use crate::regs;

/// One function as the census sees it: lifted blocks by address (`None` for
/// an instruction that did not lift) and the raw instruction words.
pub struct Function<'a> {
    /// The entry address.
    pub entry: u64,
    /// The instructions by address.
    pub insts: BTreeMap<u64, Option<&'a Block>>,
    /// The raw word at an address of the function.
    pub raw: &'a dyn Fn(u64) -> u32,
}

/// Whether `w` is `MRS Xt, FPSR`; gives `t`.
pub fn mrs_fpsr(w: u32) -> Option<u32> {
    (w & 0xffff_ffe0 == 0xd53b_4420).then_some(w & 31)
}

/// What a forward walk of one register found.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Use {
    /// Some path uses the value.
    Used,
    /// The value reaches a return in `x0` and is otherwise dead.
    Returned,
    /// Every path overwrites it first.
    Dead,
}

fn reads(b: &Block, r: relift_ir::Reg) -> bool {
    b.insts.iter().any(|i| matches!(i.op, Op::Get(g) if g == r))
}

fn writes(b: &Block, r: relift_ir::Reg) -> bool {
    b.insts
        .iter()
        .any(|i| matches!(i.op, Op::Set(g, _) if g == r))
}

/// Follows register `t` from the instruction after `from` in `f`.
fn walk(f: &Function<'_>, t: u32, from: u64) -> Use {
    if t == 31 {
        return Use::Dead;
    }
    let r = regs::x(t);
    let mut seen = BTreeSet::new();
    let mut todo = vec![from];
    let mut out = Use::Dead;
    while let Some(pc) = todo.pop() {
        if !seen.insert(pc) {
            continue;
        }
        let Some(slot) = f.insts.get(&pc) else {
            // Off the function: a tail call or fall into another function.
            if t < 8 {
                return Use::Used;
            }
            continue;
        };
        let Some(b) = slot else {
            return Use::Used;
        };
        if reads(b, r) {
            return Use::Used;
        }
        if writes(b, r) {
            continue;
        }
        let next = |n: Next| -> Vec<u64> {
            match n {
                Next::Const(a) => vec![a],
                Next::Select {
                    taken, not_taken, ..
                } => vec![taken, not_taken],
                Next::Dynamic(_) => Vec::new(),
            }
        };
        match b.exit.flow {
            Flow::Return => {
                if t == 0 {
                    out = Use::Returned;
                }
            }
            Flow::Call { ret } => {
                if t < 19 {
                    if t < 8 {
                        return Use::Used;
                    }
                    continue;
                }
                todo.push(ret);
            }
            Flow::Trap(_) => {
                if t < 8 {
                    return Use::Used;
                }
                todo.extend(next(b.exit.next));
            }
            Flow::Jump => {
                if let Next::Dynamic(_) = b.exit.next {
                    // A jump table inside the function lands on known
                    // addresses, but stay conservative.
                    return Use::Used;
                }
                todo.extend(next(b.exit.next));
            }
        }
    }
    out
}

/// The addresses of the `MRS FPSR` instructions whose result a program can
/// use. Empty means the program never observes FPSR's cumulative flags.
pub fn live_fpsr_reads(fns: &[Function<'_>]) -> Vec<u64> {
    // Call sites by callee.
    let mut callers: BTreeMap<u64, Vec<(usize, u64)>> = BTreeMap::new();
    for (i, f) in fns.iter().enumerate() {
        for b in f.insts.values().flatten() {
            if let (Flow::Call { ret }, Next::Const(t)) = (b.exit.flow, b.exit.next) {
                callers.entry(t).or_default().push((i, ret));
            }
        }
    }
    let mut live = Vec::new();
    for f in fns {
        for (&pc, b) in &f.insts {
            if b.is_none() {
                continue;
            }
            let Some(t) = mrs_fpsr((f.raw)(pc)) else {
                continue;
            };
            match walk(f, t, pc + 4) {
                Use::Used => live.push(pc),
                Use::Dead => {}
                Use::Returned => {
                    if returned_is_used(fns, &callers, f.entry) {
                        live.push(pc);
                    }
                }
            }
        }
    }
    live
}

/// Whether a caller of `entry` uses `x0` after the call, following callers
/// that return it in turn.
fn returned_is_used(
    fns: &[Function<'_>],
    callers: &BTreeMap<u64, Vec<(usize, u64)>>,
    entry: u64,
) -> bool {
    let mut seen = BTreeSet::new();
    let mut todo = vec![entry];
    while let Some(e) = todo.pop() {
        if !seen.insert(e) {
            continue;
        }
        let Some(sites) = callers.get(&e) else {
            // Reached only indirectly or as a program entry: unknown callers.
            return true;
        };
        for &(i, ret) in sites {
            match walk(&fns[i], 0, ret) {
                Use::Used => return true,
                Use::Returned => todo.push(fns[i].entry),
                Use::Dead => {}
            }
        }
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::lift::lift;

    fn func<'a>(
        entry: u64,
        words: &'a [u32],
        blocks: &'a [Block],
        raw: &'a dyn Fn(u64) -> u32,
    ) -> Function<'a> {
        let insts = (0..words.len())
            .map(|i| (entry + 4 * i as u64, Some(&blocks[i])))
            .collect();
        Function { entry, insts, raw }
    }

    fn lift_all(base: u64, words: &[u32]) -> Vec<Block> {
        words
            .iter()
            .enumerate()
            .map(|(i, w)| lift(*w, base + 4 * i as u64).expect("lifts"))
            .collect()
    }

    const RET: u32 = 0xd65f_03c0;

    #[test]
    fn a_read_overwritten_before_use_is_dead() {
        // mrs x1, fpsr; mov x1, #0; ret
        let w = [0xd53b_4421, 0xd280_0001, RET];
        let b = lift_all(0x1000, &w);
        let raw = |pc: u64| w[((pc - 0x1000) / 4) as usize];
        assert!(live_fpsr_reads(&[func(0x1000, &w, &b, &raw)]).is_empty());
    }

    #[test]
    fn a_read_that_is_stored_is_live() {
        // mrs x1, fpsr; str x1, [x0]; ret
        let w = [0xd53b_4421, 0xf900_0001, RET];
        let b = lift_all(0x1000, &w);
        let raw = |pc: u64| w[((pc - 0x1000) / 4) as usize];
        assert_eq!(live_fpsr_reads(&[func(0x1000, &w, &b, &raw)]), vec![0x1000]);
    }

    #[test]
    fn a_returned_read_counts_only_when_a_caller_uses_it() {
        // callee at 0x2000: mrs x0, fpsr; ret
        let cw = [0xd53b_4420, RET];
        let cb = lift_all(0x2000, &cw);
        // caller A at 0x1000: bl 0x2000; mov x0, #0; ret (drops it)
        let aw = [0x9400_0400, 0xd280_0000, RET];
        let ab = lift_all(0x1000, &aw);
        // caller B at 0x3000: bl 0x2000; str x0, [x1]; ret (uses it)
        let bw = [0x97ff_fc00, 0xf900_0020, RET];
        let bb = lift_all(0x3000, &bw);
        let raw = |pc: u64| match pc {
            0x1000..0x2000 => aw[((pc - 0x1000) / 4) as usize],
            0x2000..0x3000 => cw[((pc - 0x2000) / 4) as usize],
            _ => bw[((pc - 0x3000) / 4) as usize],
        };
        let callee = func(0x2000, &cw, &cb, &raw);
        let a = func(0x1000, &aw, &ab, &raw);
        assert!(live_fpsr_reads(&[a, callee]).is_empty());
        let callee = func(0x2000, &cw, &cb, &raw);
        let a = func(0x1000, &aw, &ab, &raw);
        let b = func(0x3000, &bw, &bb, &raw);
        assert_eq!(live_fpsr_reads(&[a, callee, b]), vec![0x2000]);
    }
}

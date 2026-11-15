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

pub mod region;

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

/// Whether `w` is `MSR FPSR, Xt`.
pub fn msr_fpsr(w: u32) -> bool {
    w & 0xffff_ffe0 == 0xd51b_4420
}

/// Whether block `b` may raise FP exception flags: an FP operation other
/// than negation and absolute value (sign-bit operations), flags an IR-built
/// operation raises, or an FP guard whose slow path runs the exact layer.
pub fn raises(b: &Block) -> bool {
    use relift_ir::FpOp;
    b.insts.iter().any(|i| match &i.op {
        Op::Fp(op, _) => !matches!(op, FpOp::Neg | FpOp::Abs),
        Op::Intrinsic(call) => call.name == crate::lift::system::FP_RAISE,
        Op::Guard(_, code) => {
            *code == crate::lift::sail::SLOW_FPCR || *code == crate::lift::sail::SLOW_FLAGS
        }
        _ => false,
    })
}

/// What [`summarize`] needs to know about the rest of the program.
pub struct Context<'a> {
    /// Whether an address is a function entry.
    pub is_entry: &'a dyn Fn(u64) -> bool,
    /// The targets of the computed call or jump at an address, when the
    /// build resolved them (a transfer through a loader-filled slot).
    pub resolved: &'a dyn Fn(u64) -> Option<Vec<u64>>,
    /// Whether a call to an entry may come back to its return point from
    /// elsewhere (`setjmp`).
    pub resume_point: &'a dyn Fn(u64) -> bool,
    /// The program start, if it is in this module: its entry and the
    /// registers the runtime sets before it enters there.
    pub start: Option<(u64, &'a [(relift_ir::Reg, u128)])>,
}

fn sext(v: u128, w: relift_ir::Width) -> u128 {
    let s = 128 - w.bits();
    (((v << s) as i128) >> s) as u128
}

/// The value of instruction `i` of `b` from the known values `vals` of
/// earlier ones and the known registers `regs`; `None` when unknown.
fn fold(
    b: &Block,
    i: usize,
    vals: &[Option<u128>],
    regs: &BTreeMap<relift_ir::Reg, u128>,
) -> Option<u128> {
    use relift_ir::interp::{eval_bin, eval_cmp, eval_un};
    use relift_ir::{ExtKind, Width};
    let inst = &b.insts[i];
    let v = |x: relift_ir::Value| vals.get(x.index()).copied().flatten();
    let wd = |x: relift_ir::Value| b.width_of(x).unwrap_or(Width::W1);
    let w = inst.width?;
    let r = match inst.op {
        Op::Const(c) => c.get(),
        Op::Get(r) => *regs.get(&r)?,
        Op::Un(op, a) => eval_un(op, v(a)?, w),
        Op::Bin(op, a, c) => eval_bin(op, v(a)?, v(c)?, w),
        Op::Cmp(op, a, c) => u128::from(eval_cmp(op, v(a)?, v(c)?, wd(a))),
        Op::Select(c, a, d) => {
            if v(c)? != 0 {
                v(a)?
            } else {
                v(d)?
            }
        }
        Op::Ext(ExtKind::Zero, a) => v(a)?,
        Op::Ext(ExtKind::Sign, a) => sext(v(a)?, wd(a)),
        Op::Extract(a, lo) => v(a)? >> lo,
        Op::Concat(h, l) => (v(h)? << wd(l).bits()) | v(l)?,
        _ => return None,
    };
    Some(r & w.mask())
}

/// The instructions of `f` that can run when it is entered with the
/// registers `known` holding the given values and from nowhere else: a
/// forward propagation of known register values, which follows only the
/// taken side of a branch whose condition it knows. A call, a trap or a
/// merge of paths that disagree forgets the values it cannot keep. An
/// instruction that no other one of `f` can reach (a landing pad) is
/// entered with nothing known; one that failed to lift ends the thread.
pub fn reachable(
    f: &Function<'_>,
    tables: &BTreeMap<u64, Vec<u64>>,
    known: &[(relift_ir::Reg, u128)],
) -> BTreeSet<u64> {
    let mut ins: BTreeMap<u64, BTreeMap<relift_ir::Reg, u128>> = BTreeMap::new();
    ins.insert(f.entry, known.iter().copied().collect());
    let mut work = vec![f.entry];
    let mut reached = BTreeSet::new();
    for (&pc, b) in &f.insts {
        let Some(b) = b else { continue };
        match (b.exit.flow, b.exit.next) {
            (Flow::Return, _) => {}
            (Flow::Call { ret }, n) => {
                reached.insert(ret);
                if let Next::Const(t) = n {
                    reached.insert(t);
                }
            }
            (_, Next::Const(t)) => {
                reached.insert(t);
            }
            (
                _,
                Next::Select {
                    taken, not_taken, ..
                },
            ) => {
                reached.extend([taken, not_taken]);
            }
            (_, Next::Dynamic(_)) => {
                reached.extend(tables.get(&pc).into_iter().flatten().copied());
            }
        }
    }
    for &pc in f.insts.keys() {
        if pc != f.entry && !reached.contains(&pc) {
            ins.insert(pc, BTreeMap::new());
            work.push(pc);
        }
    }
    while let Some(pc) = work.pop() {
        let Some(Some(b)) = f.insts.get(&pc) else {
            continue;
        };
        let regs = ins[&pc].clone();
        let mut vals = Vec::with_capacity(b.insts.len());
        for i in 0..b.insts.len() {
            let v = fold(b, i, &vals, &regs);
            vals.push(v);
        }
        let mut out = regs.clone();
        for inst in &b.insts {
            if let Op::Set(r, v) = inst.op {
                match vals.get(v.index()).copied().flatten() {
                    Some(c) => out.insert(r, c),
                    None => out.remove(&r),
                };
            }
        }
        let mut succs = Vec::new();
        match (b.exit.flow, b.exit.next) {
            (Flow::Return, _) => {}
            (Flow::Call { ret }, _) => {
                out.clear();
                succs.push(ret);
            }
            (flow, Next::Const(t)) => {
                if flow != Flow::Jump {
                    out.clear();
                }
                succs.push(t);
            }
            (
                flow,
                Next::Select {
                    cond,
                    taken,
                    not_taken,
                },
            ) => {
                let c = vals.get(cond.index()).copied().flatten();
                if flow != Flow::Jump {
                    out.clear();
                }
                match c {
                    Some(0) => succs.push(not_taken),
                    Some(_) => succs.push(taken),
                    None => succs.extend([taken, not_taken]),
                }
            }
            (_, Next::Dynamic(_)) => match tables.get(&pc) {
                Some(ts) => succs.extend(ts.iter().copied()),
                None => return f.insts.keys().copied().collect(),
            },
        }
        for s in succs {
            if !f.insts.contains_key(&s) {
                continue;
            }
            match ins.get_mut(&s) {
                None => {
                    ins.insert(s, out.clone());
                    work.push(s);
                }
                Some(e) => {
                    let n = e.len();
                    e.retain(|r, v| out.get(r) == Some(v));
                    if e.len() != n {
                        work.push(s);
                    }
                }
            }
        }
    }
    ins.into_keys().collect()
}

/// One instruction's edges: successors in the function and what leaves it.
fn edges(
    f: &Function<'_>,
    tables: &BTreeMap<u64, Vec<u64>>,
    cx: &Context<'_>,
    pc: u64,
    b: &Block,
) -> (Vec<u64>, region::End) {
    use region::{End, Targets};
    let inside = |t: u64| f.insts.contains_key(&t);
    let mut succs = Vec::new();
    let mut out = Vec::new();
    let mut unknown = false;
    let mut to = |t: u64, succs: &mut Vec<u64>| {
        if inside(t) {
            succs.push(t);
        } else if (cx.is_entry)(t) {
            out.push(t);
        } else {
            unknown = true;
        }
    };
    let end = match b.exit.flow {
        Flow::Return => End::Return,
        Flow::Call { ret } => {
            if inside(ret) {
                succs.push(ret);
            }
            let t = match b.exit.next {
                Next::Const(t) => Targets::Known(vec![t]),
                _ => (cx.resolved)(pc).map_or(Targets::Unknown, Targets::Known),
            };
            return (succs, End::Call(t));
        }
        Flow::Jump | Flow::Trap(_) => {
            match b.exit.next {
                Next::Const(t) => to(t, &mut succs),
                Next::Select {
                    taken, not_taken, ..
                } => {
                    to(taken, &mut succs);
                    to(not_taken, &mut succs);
                }
                Next::Dynamic(_) => match tables.get(&pc) {
                    Some(ts) => ts.iter().for_each(|&t| to(t, &mut succs)),
                    None => match (cx.resolved)(pc) {
                        Some(ts) => out.extend(ts),
                        None => unknown = true,
                    },
                },
            }
            End::Fall
        }
    };
    let end = if unknown {
        End::Exit(Targets::Unknown)
    } else if !out.is_empty() {
        End::Exit(Targets::Known(out))
    } else {
        end
    };
    (succs, end)
}

/// The region analysis view of `f`, with jump tables `tables`, whose live
/// reads are `reads`.
pub fn summarize(
    f: &Function<'_>,
    tables: &BTreeMap<u64, Vec<u64>>,
    reads: &BTreeSet<u64>,
    cx: &Context<'_>,
) -> region::Func {
    use region::{Bb, End, Func, Targets};
    struct I {
        pc: u64,
        succs: Vec<u64>,
        end: End,
        fp: bool,
        writes: bool,
    }
    let mut insts = Vec::with_capacity(f.insts.len());
    let mut foreign = BTreeSet::new();
    for (&pc, b) in &f.insts {
        let Some(b) = b else {
            // It reports itself as undefined and ends the thread.
            insts.push(I {
                pc,
                succs: Vec::new(),
                end: End::Fall,
                fp: false,
                writes: false,
            });
            continue;
        };
        let (succs, end) = edges(f, tables, cx, pc, b);
        if let (End::Call(Targets::Known(ts)), Flow::Call { ret }) = (&end, b.exit.flow)
            && ts.iter().any(|&t| (cx.resume_point)(t))
        {
            foreign.insert(ret);
        }
        let w = (f.raw)(pc);
        insts.push(I {
            pc,
            succs,
            end,
            fp: raises(b),
            writes: msr_fpsr(w),
        });
    }
    let mut npreds: BTreeMap<u64, usize> = BTreeMap::new();
    for i in &insts {
        for &s in &i.succs {
            *npreds.entry(s).or_default() += 1;
        }
    }
    // A block starts at the entry, a read, a foreign point, a join, a
    // branch target, or after an instruction that does more than fall
    // through to the next one.
    let mut leaders = BTreeSet::from([f.entry]);
    let mut prev_falls: Option<u64> = None;
    for (k, i) in insts.iter().enumerate() {
        let n = npreds.get(&i.pc).copied().unwrap_or(0);
        if prev_falls != Some(i.pc) || n != 1 || reads.contains(&i.pc) || foreign.contains(&i.pc) {
            leaders.insert(i.pc);
        }
        let next = insts.get(k + 1).map(|j| j.pc);
        prev_falls = (i.end == End::Fall && i.succs.len() == 1 && Some(i.succs[0]) == next)
            .then(|| i.succs[0]);
    }
    // The entry block comes first, even when code below the entry belongs
    // to the function.
    let order: Vec<u64> = std::iter::once(f.entry)
        .chain(leaders.iter().copied().filter(|&a| a != f.entry))
        .collect();
    let index: BTreeMap<u64, usize> = order.iter().enumerate().map(|(k, &a)| (a, k)).collect();
    let live = match cx.start {
        Some((e, regs)) if e == f.entry => Some(reachable(f, tables, regs)),
        _ => None,
    };
    let mut blocks: Vec<Bb> = order
        .iter()
        .map(|&a| Bb {
            start: a,
            fp: false,
            fp_tail: false,
            writes: false,
            succs: Vec::new(),
            end: End::Fall,
            foreign: foreign.contains(&a),
            start_dead: live.as_ref().is_some_and(|l| !l.contains(&a)),
        })
        .collect();
    let mut cur = 0;
    for i in insts {
        if let Some(&k) = index.get(&i.pc) {
            cur = k;
        }
        let blk = &mut blocks[cur];
        blk.fp |= i.fp;
        if i.writes {
            blk.writes = true;
            blk.fp_tail = false;
        }
        blk.fp_tail |= i.fp;
        blk.succs = i
            .succs
            .iter()
            .filter_map(|s| index.get(s).copied())
            .collect();
        blk.end = i.end;
    }
    let reads = reads
        .iter()
        .filter_map(|r| index.get(r).map(|&k| (*r, k)))
        .collect();
    Func {
        entry: f.entry,
        blocks,
        reads,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cpu::{Cpu, Va};
    use crate::decode::Features;
    use crate::lift::lift;

    const ALL: Cpu = Cpu::new(Features::ALL, Va::BITS39);

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
            .map(|(i, w)| lift(*w, base + 4 * i as u64, &ALL).expect("lifts"))
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

    /// rtld's entry: cmp x0, #0; b.ne exc; nop; nop; ret; exc: br x2.
    const START: [u32; 6] = [
        0xf100_001f,
        0x5400_0081,
        0xd503_201f,
        0xd503_201f,
        RET,
        0xd61f_0040,
    ];

    #[test]
    fn a_known_register_rules_out_the_branch_it_decides() {
        let b = lift_all(0x1000, &START);
        let raw = |pc: u64| START[((pc - 0x1000) / 4) as usize];
        let f = func(0x1000, &START, &b, &raw);
        let t = BTreeMap::new();
        let r = reachable(&f, &t, &[(regs::x(0), 0)]);
        assert_eq!(
            r.into_iter().collect::<Vec<_>>(),
            vec![0x1000, 0x1004, 0x1008, 0x100c, 0x1010]
        );
        // The other side's computed jump may land anywhere in the function.
        assert_eq!(reachable(&f, &t, &[(regs::x(0), 1)]).len(), START.len());
    }

    #[test]
    fn code_nothing_in_the_function_reaches_is_entered_from_outside() {
        let w = [
            START[0],
            START[1],
            START[2],
            START[3],
            RET,
            START[5],
            0xd503_201f,
            RET,
        ];
        let b = lift_all(0x1000, &w);
        let raw = |pc: u64| w[((pc - 0x1000) / 4) as usize];
        let f = func(0x1000, &w, &b, &raw);
        let r = reachable(&f, &BTreeMap::new(), &[(regs::x(0), 0)]);
        assert!(!r.contains(&0x1014));
        assert!(r.contains(&0x1018) && r.contains(&0x101c));
    }

    #[test]
    fn an_unknown_register_keeps_every_path() {
        let b = lift_all(0x1000, &START);
        let raw = |pc: u64| START[((pc - 0x1000) / 4) as usize];
        let f = func(0x1000, &START, &b, &raw);
        let t = BTreeMap::new();
        assert_eq!(reachable(&f, &t, &[]).len(), START.len());
        // A value the code overwrites before the branch decides nothing.
        let w = [0xaa01_03e0, START[0], 0x5400_0061, START[2], RET, START[5]];
        let b = lift_all(0x1000, &w);
        let raw = |pc: u64| w[((pc - 0x1000) / 4) as usize];
        let f = func(0x1000, &w, &b, &raw);
        assert_eq!(reachable(&f, &t, &[(regs::x(0), 0)]).len(), w.len());
    }
}

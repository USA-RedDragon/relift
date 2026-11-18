//! Control flow: basic blocks from the branches, loops from the dominator
//! tree, and structured SPIR-V from both.
//!
//! Each selection merges at its immediate post-dominator inside the
//! innermost loop; back edges become continues and loop exits become
//! breaks. A loop with several exits stores which one it took in a variable
//! and breaks; the code after the loop dispatches on it. EXIT is not a
//! block: every edge to it returns (after the epilogue) in place. KILL is
//! not control flow at all (see the io module).
//!
//! BSSY/BSYNC mark where the guest's lanes come together again. They emit
//! nothing: where a program asks for maximal reconvergence (see the lanes
//! module), lanes come together at the merges this structure picks, so
//! each BSSY's target must lie on the chain of merges that starts at its
//! block or no warp operation may run between the target and the next
//! merge on that chain, no BREAK may let lanes run warp operations before
//! their BSYNC,
//! and no BMOV may change a barrier that is read before it is set again;
//! anything else is rejected. Elsewhere
//! which lanes run together cannot be observed. A BSSY whose target is no
//! merge, with a warp operation after it, has its region funneled: the
//! edges from the region into the target's block and the edges that leave
//! the region for elsewhere (its BREAK paths) all go to one new block
//! through a selector variable, as with split joins below. That block is
//! the merge of the region, and the lanes it sends to the target enter it
//! together, as the guest's do after their BSYNC.
//!
//! `CALL.REL` subroutines are inlined at every call site: a block is a pair
//! of the call stack and its first instruction, and `RET` continues after
//! the innermost call. The callee returns through the address its caller
//! put in a register with `MOV`, which with the RET's offset must name the
//! instruction after the `CALL`.
//!
//! A CALL whose callee never reaches a RET is a plain jump. The absolute
//! forms (`CALL.ABS`, `RET.ABS`, `JMP`, indirect calls) are described in
//! the calls module; `BPT.TRAP` ends the invocation like EXIT.
//!
//! `BRX` and `BRXU` jump to a register plus a base, through a table in a
//! constant bank the program cannot see. Their targets are the instruction
//! after the jump and the code nothing else reaches: each instruction after
//! an unconditional transfer that is unreachable without the jump belongs
//! to the nearest jump before it. The jump becomes an OpSwitch over those
//! addresses. Its default, an address outside the cases, ends the
//! invocation: a fragment is discarded, any other stage returns without
//! its outputs, so a missed case shows instead of running the wrong one.
//!
//! BAR waits for every thread of the CTA that has not exited, and a lane
//! that exited counts as arrived (measured on SM86); lanes of one warp at
//! two BAR sites hang. OpControlBarrier instead needs every invocation of
//! the workgroup at the same instruction. So each BAR must lie where the
//! whole workgroup runs: not in a block that a branch with a condition
//! that may differ between invocations can make some of them skip (the
//! blocks between the branch and its post-dominator), not under such a
//! guard, and not after a varying EXIT. Which values are uniform comes
//! from the `uniform` module; anything else is rejected with the reason.
//!
//! One shape is converted instead: in a compute program, a branch with a
//! varying condition whose one arm skips straight to its post-dominator
//! and whose other arm (the region) holds a BAR. Every invocation runs the
//! region; one that would have skipped it carries out only its arithmetic,
//! and gets back at the post-dominator the registers and predicates the
//! region wrote. The region's own branches must then be uniform with its
//! memory and texture results taken as varying. The guest's skipping
//! threads arrive at none of the region's BARs and, if no BAR follows the
//! post-dominator, at no BAR at all, so the region's threads go on once
//! they have exited; nothing orders the skipping threads' memory accesses
//! after the post-dominator with the region's then. The conversion keeps
//! that only where it cannot be seen: the region has no warp operation,
//! no uniform register write, no EXIT, and reads no global or surface
//! memory, and the code from the post-dominator on has no BAR and no
//! shared memory access (and no surface write if the region samples
//! textures). A varying EXIT before a BAR is converted the same way, its
//! region the rest of the program: the guest counts an exited thread as
//! arrived at every BAR, and an invocation that runs on with no effects
//! arrives at each with the others.
//!
//! A varying exit from a loop with a BAR, to EXIT or to a block that only
//! exits, is converted alike: an invocation that takes it stops carrying
//! out the loop and the code after it (its memory and texture accesses are
//! skipped) but keeps going round with the others, and the loop is left,
//! to the exit, once no invocation of the workgroup carries it out any
//! more, which they find out together through a shared word between
//! workgroup barriers. The loop's other branches, with its skipped
//! accesses taken as varying, must leave every BAR and the exit itself
//! where the whole workgroup runs, and the code from the exit's other arm
//! on must have no warp operation.
//!
//! A selection whose arms meet again before its merge (an arm that leaves
//! early to code the other arm skips) copies the shared code into each arm.
//! When that copying grows too much, the program is structured again with
//! the joins split: the edges into the first shared block and the early
//! leaving edges of the region go to a new block through a selector
//! variable, and the new block switches on it, so the selection merges
//! there and nothing is copied.
//!
//! A graph this cannot express (irreducible, recursive calls) is rejected
//! with the reason.

mod masks;
mod uniform;

pub(crate) use uniform::arithmetic;

use super::calls::{self, Callee};
use super::{Code, Ctx, Res, bad};
use crate::Operand;
use crate::spirv::op;
use std::collections::{HashMap, HashSet};

pub(crate) const EXIT: usize = usize::MAX;
const WORKGROUP: u32 = 4;
const SCOPE_WORKGROUP: u32 = 2;
const SEM_ACQUIRE_RELEASE: u32 = 0x8;
const SEM_WORKGROUP: u32 = 0x100;

/// A BSSY whose target the structure does not reconverge at: the block
/// that holds the BSSY, the target instruction and the reason to give if
/// the region cannot be funneled there.
pub(crate) struct OffMerge {
    pub(crate) block: usize,
    pub(crate) target: usize,
    pub(crate) why: String,
}
const MAX_CALL_DEPTH: usize = 8;
pub(crate) const TOO_MUCH_DUPLICATION: &str = "control flow needs too much duplication";
const MAX_BLOCKS: usize = 1 << 16;

#[derive(Clone, Copy, Debug)]
pub(crate) enum Sel {
    Reg(u8),
    UReg(u8),
}

#[derive(Clone, Debug)]
pub(crate) enum Term {
    Jump(usize),
    Branch(Vec<Operand>, usize, usize),
    /// Jump to `selector + base`: (address, block) cases.
    Switch(Sel, u32, Vec<(u32, usize)>),
    /// Set selector variable `var` to `value` and jump.
    Set(usize, u32, usize),
    /// Jump to the target the value of selector variable `var` names.
    Dispatch(usize, Vec<usize>),
}

impl Term {
    fn targets(&self) -> Vec<usize> {
        match self {
            Term::Jump(t) | Term::Set(_, _, t) => vec![*t],
            Term::Branch(_, t, f) => vec![*t, *f],
            Term::Switch(_, _, cases) => cases.iter().map(|c| c.1).collect(),
            Term::Dispatch(_, ts) => ts.clone(),
        }
    }

    fn retarget(&mut self, f: impl Fn(usize) -> usize) {
        match self {
            Term::Jump(t) | Term::Set(_, _, t) => *t = f(*t),
            Term::Branch(_, t, e) => {
                *t = f(*t);
                *e = f(*e);
            }
            Term::Switch(_, _, cases) => {
                for c in cases {
                    c.1 = f(c.1);
                }
            }
            Term::Dispatch(_, ts) => {
                for t in ts {
                    *t = f(*t);
                }
            }
        }
    }
}

#[derive(Clone, Debug)]
pub(crate) struct Block {
    pub first: usize,
    pub end: usize,
    pub term: Term,
}

fn terminal(b: usize) -> bool {
    b == EXIT
}

struct Lp {
    header: usize,
    exits: Vec<usize>,
    cont: u32,
    brk: u32,
    body: Vec<bool>,
    var: u32,
}

pub(crate) struct Graph {
    pub blocks: Vec<Block>,
    succs: Vec<Vec<usize>>,
    pub headers: HashMap<usize, (Vec<bool>, Vec<usize>)>,
    pdoms: HashMap<Option<usize>, Vec<Option<usize>>>,
    /// Blocks on a path to `__assertfail` (see `asserts_on_branch`), left
    /// out of post-dominance: the guest's grid stops there.
    asserting: Vec<bool>,
    /// Branches converted so that every invocation runs their region (see
    /// the module documentation).
    conv: Vec<Conv>,
    /// The innermost conversion whose region holds each block.
    gate: Vec<Option<usize>>,
    /// Shared memory loads every invocation reads the same word with (see
    /// [`Graph::broadcast_loads`]).
    broadcast: HashSet<usize>,
}

/// A converted branch.
#[derive(Clone, Debug)]
struct Conv {
    branch: usize,
    /// The first block of the region.
    region: usize,
    /// The post-dominator, where skipping invocations get their values back;
    /// `None` for a varying EXIT, whose exited invocations stay to the end.
    merge: Option<usize>,
    regs: Vec<u8>,
    preds: Vec<u8>,
    /// For a varying exit from a loop to a block that only ends the
    /// invocation: that block. The loop is left when no invocation still
    /// carries it out.
    stub: Option<usize>,
    /// The conversion whose region held the branch before this one.
    outer: Option<usize>,
}

/// The branch condition: the guard and the predicates the instruction
/// itself names, all of which must hold.
fn conditions(i: &crate::Inst, w: crate::Word) -> Res<Vec<Operand>> {
    let mut c = Vec::new();
    if let Some(g) = &i.guard {
        c.push(g.clone());
    }
    if i.mnemonic == "BRA" || i.mnemonic == "EXIT" {
        for o in &i.ops {
            match o {
                Operand::Pred(n, not) => c.push(Operand::Pred(*n, *not)),
                Operand::UReg(_, m) if i.mnemonic == "BRA" && w.bit(91) => {
                    let n = w.get(24, 3) as u8;
                    c.push(Operand::UPred(n, m.not));
                }
                Operand::Target(_) => {}
                o => return bad(format!("{} operand {o}", i.mnemonic)),
            }
        }
    }
    c.retain(|p| !matches!(p, Operand::Pred(7, false) | Operand::UPred(7, false)));
    Ok(c)
}

/// Signed 48-bit branch offset of a control word, in bytes.
fn raw_target(w: crate::Word) -> i64 {
    ((w.get64(34, 48) as i64) << 16 >> 16) * 4
}

/// Whether `i` ends a run of straight code (a transfer or a label target
/// is not known here, so every control instruction counts).
pub(super) fn ends_straight(i: &crate::Inst) -> bool {
    is_transfer(i) || matches!(i.mnemonic, "BSSY" | "BSYNC" | "WARPSYNC" | "BAR")
}

fn is_transfer(i: &crate::Inst) -> bool {
    matches!(
        i.mnemonic,
        "BRA" | "JMP" | "EXIT" | "CALL" | "RET" | "BRX" | "BRXU"
    ) || calls::is_trap(i)
}

/// Ends a block with no fall-through: an unguarded BRA, JMP, EXIT, BRX,
/// RET or BPT.TRAP.
fn no_fall(i: &crate::Inst, w: crate::Word) -> bool {
    (matches!(i.mnemonic, "BRA" | "JMP" | "EXIT" | "BRX" | "BRXU" | "RET") || calls::is_trap(i))
        && conditions(i, w).is_ok_and(|c| c.is_empty())
}

fn bra_target(i: &crate::Inst, end: usize) -> Res<usize> {
    let Some(Operand::Target(t)) = i.ops.last() else {
        return bad(format!("{} without a target", i.mnemonic));
    };
    usize::try_from(*t / 16)
        .ok()
        .filter(|k| *k < end && *t % 16 == 0)
        .map_or_else(|| bad("branch target outside the program"), Ok)
}

/// The instruction a direct CALL goes to.
fn callee_of(i: &crate::Inst, w: crate::Word, end: usize) -> Res<usize> {
    match calls::callee(i, w, end)? {
        Callee::To(t) => Ok(t),
        Callee::Reg(_) => bad("indirect CALL"),
    }
}

/// Instruction-level successors, with each BRX going only to the
/// instruction after it and each CALL to its target and its return point.
fn plain_succs(code: &Code, end: usize, k: usize) -> Vec<usize> {
    let i = &code.insts[k];
    let w = code.words[k];
    let mut v = Vec::new();
    let t = match i.mnemonic {
        "BRA" => bra_target(i, end).ok(),
        "JMP" => calls::jmp_target(i, end).ok(),
        "CALL" => match calls::callee(i, w, end) {
            Ok(Callee::To(t)) => Some(t),
            _ => None,
        },
        _ => None,
    };
    v.extend(t);
    if i.mnemonic == "BRX" || !no_fall(i, w) {
        v.push(k + 1);
    }
    v.retain(|t| *t < end);
    v
}

/// The cases of every BRX: the instruction after it and each instruction
/// after a transfer that nothing reaches without the BRX, given to the
/// nearest BRX before it. Heads are taken one at a time, lowest first, so
/// that a subroutine the cases call is reached through its call and not
/// taken for a case; code that reaches a RET is a subroutine (called or
/// dead), never a case.
fn brx_targets(code: &Code, end: usize) -> HashMap<usize, Vec<usize>> {
    let brxs: Vec<usize> = (0..end)
        .filter(|k| matches!(code.insts[*k].mnemonic, "BRX" | "BRXU"))
        .collect();
    let mut cases: HashMap<usize, Vec<usize>> = brxs.iter().map(|k| (*k, vec![k + 1])).collect();
    let mut seen = vec![false; end];
    let mut work = vec![0usize];
    loop {
        while let Some(k) = work.pop() {
            if k >= end || seen[k] {
                continue;
            }
            seen[k] = true;
            work.extend(plain_succs(code, end, k));
            if let Some(cs) = cases.get(&k) {
                work.extend(cs.iter().copied());
            }
        }
        let head = (1..end).find_map(|m| {
            if seen[m] || !no_fall(&code.insts[m - 1], code.words[m - 1]) || returns(code, end, m) {
                return None;
            }
            brxs.iter()
                .rev()
                .find(|k| **k < m && seen[**k])
                .map(|k| (*k, m))
        });
        let Some((k, m)) = head else {
            return cases;
        };
        if let Some(cs) = cases.get_mut(&k) {
            cs.push(m);
        }
        work.push(m);
    }
}

/// Whether code from `t` reaches a RET (not counting the subroutines it
/// calls); a CALL to code that never returns is a plain jump.
fn returns(code: &Code, end: usize, t: usize) -> bool {
    let mut seen = vec![false; end];
    let mut work = vec![t];
    while let Some(k) = work.pop() {
        if k >= end || seen[k] {
            continue;
        }
        seen[k] = true;
        let i = &code.insts[k];
        if i.mnemonic == "RET" {
            return true;
        }
        if i.mnemonic == "CALL" {
            work.push(k + 1);
            continue;
        }
        work.extend(plain_succs(code, end, k));
    }
    false
}

/// Block numbers of (call stack, first instruction) pairs, and the pairs
/// still to build.
#[derive(Default)]
struct Ids {
    ids: HashMap<(Vec<usize>, usize), usize>,
    work: Vec<(Vec<usize>, usize, usize)>,
}

impl Ids {
    fn get(&mut self, key: (Vec<usize>, usize), blocks: &mut Vec<Option<Block>>) -> usize {
        if let Some(&b) = self.ids.get(&key) {
            return b;
        }
        let b = blocks.len();
        blocks.push(None);
        self.work.push((key.0.clone(), key.1, b));
        self.ids.insert(key, b);
        b
    }

    fn next(&mut self) -> Option<(Vec<usize>, usize, usize)> {
        self.work.pop()
    }
}

/// The end of the program's code: the instruction count without the
/// trailing BRA self loops.
fn code_end(code: &Code) -> usize {
    let mut end = code.insts.len();
    while end > 0 && code.insts[end - 1].mnemonic == "BRA" {
        let i = &code.insts[end - 1];
        if !matches!(i.ops.as_slice(), [Operand::Target(t)] if *t == 16 * (end as i64 - 1)) {
            break;
        }
        end -= 1;
    }
    end
}

/// The cases the translation gives each BRX (see [`brx_targets`]).
pub(crate) fn brx_cases(code: &Code) -> HashMap<usize, Vec<usize>> {
    brx_targets(code, code_end(code))
}

pub(crate) fn build(code: &Code) -> Res<Vec<Block>> {
    let end = code_end(code);
    let mut leaders = vec![false; end + 1];
    leaders[0] = true;
    leaders[end] = true;
    for (k, i) in code.insts[..end].iter().enumerate() {
        match i.mnemonic {
            "BRA" => {
                if i.mods.iter().any(|m| *m != "U") {
                    return bad(format!("BRA.{}", i.mods.join(".")));
                }
                let t = bra_target(i, end)?;
                leaders[t] = true;
                leaders[k + 1] = true;
            }
            "CALL" => {
                if let Callee::To(t) = calls::callee(i, code.words[k], end)? {
                    leaders[t] = true;
                }
                leaders[k + 1] = true;
            }
            "JMP" => {
                leaders[calls::jmp_target(i, end)?] = true;
                leaders[k + 1] = true;
            }
            "RET" | "BPT" => {
                calls::check(i)?;
                leaders[k + 1] = true;
            }
            "EXIT" | "BRX" | "BRXU" => leaders[k + 1] = true,
            "JMX" | "RTT" => {
                return bad(format!("control {}", i.mnemonic));
            }
            "WARPSYNC" if i.has("EXCLUSIVE") => return bad("WARPSYNC.EXCLUSIVE"),
            _ => {}
        }
    }
    for i in &code.insts[..end] {
        if matches!(i.mnemonic, "BRX" | "BRXU") && (i.guard.is_some() || !i.mods.is_empty()) {
            return bad(format!("{} form {i}", i.mnemonic));
        }
    }
    let brx = brx_targets(code, end);
    for t in brx.values().flatten() {
        leaders[*t] = true;
    }
    let mut calls: HashMap<usize, bool> = HashMap::new();
    let mut next = vec![end; end + 1];
    for k in (0..end).rev() {
        next[k] = if leaders[k + 1] { k + 1 } else { next[k + 1] };
    }
    let mut functions: Option<Vec<usize>> = None;
    let mut ids = Ids::default();
    let mut blocks: Vec<Option<Block>> = Vec::new();
    ids.get((Vec::new(), 0), &mut blocks);
    while let Some((stack, s, me)) = ids.next() {
        if blocks.len() > MAX_BLOCKS {
            return bad("control flow needs too much duplication");
        }
        let stop = next[s];
        let last = &code.insts[stop - 1];
        let w = code.words[stop - 1];
        let fall = (stop < end).then(|| (stack.clone(), stop));
        let term_inst = is_transfer(last);
        let to = |stack: &[usize], t: usize| (stack.to_vec(), t);
        let target: Option<(Vec<usize>, usize)>;
        let mut term = None;
        let indirect = match last.mnemonic {
            "CALL" => match calls::callee(last, w, end)? {
                Callee::Reg(r) => Some(r),
                Callee::To(_) => None,
            },
            _ => None,
        };
        match last.mnemonic {
            "BRA" => target = Some(to(&stack, bra_target(last, end)?)),
            "JMP" => target = Some(to(&stack, calls::jmp_target(last, end)?)),
            "EXIT" => target = None,
            "CALL" if let Some(r) = indirect => {
                if last.guard.is_some() {
                    return bad("guarded indirect CALL");
                }
                if stack.len() >= MAX_CALL_DEPTH || stack.contains(&(stop - 1)) {
                    return bad("recursive CALL");
                }
                let fs = functions
                    .get_or_insert_with(|| calls::functions(code, end, |t| returns(code, end, t)));
                if fs.is_empty() {
                    return bad("indirect CALL with no function in the program");
                }
                let mut st = stack.clone();
                st.push(stop - 1);
                if fall.is_none() {
                    return bad("CALL falls off the end");
                }
                let mut cases = Vec::new();
                for &t in fs.iter() {
                    let b = ids.get((st.clone(), t), &mut blocks);
                    cases.push((16 * t as u32, b));
                }
                term = Some(Term::Switch(Sel::Reg(r), 0, cases));
                target = None;
            }
            "CALL"
                if !*calls.entry(stop - 1).or_insert_with(|| {
                    callee_of(last, w, end).is_ok_and(|t| returns(code, end, t))
                }) =>
            {
                target = Some(to(&stack, callee_of(last, w, end)?));
            }
            "CALL" => {
                let t = callee_of(last, w, end)?;
                if stack.len() >= MAX_CALL_DEPTH
                    || stack.iter().any(|&c| {
                        callee_of(&code.insts[c], code.words[c], end).is_ok_and(|x| x == t)
                    })
                {
                    return bad("recursive CALL");
                }
                let mut st = stack.clone();
                st.push(stop - 1);
                target = Some((st, t));
            }
            "RET" => {
                let Some((&site, rest)) = stack.split_last() else {
                    return bad("RET outside a call");
                };
                if !calls::return_address_set(code, site, stop - 1) {
                    return bad("CALL without its return address");
                }
                target = Some(to(rest, site + 1));
            }
            "BRX" | "BRXU" => {
                let sel = if w.bit(91) || last.mnemonic == "BRXU" {
                    Sel::UReg(w.get(24, 6) as u8)
                } else {
                    Sel::Reg(w.get(24, 8) as u8)
                };
                let base = (16 * stop as i64 + raw_target(w)) as u32;
                let mut cases = Vec::new();
                for &t in &brx[&(stop - 1)] {
                    let b = ids.get(to(&stack, t), &mut blocks);
                    cases.push((16 * t as u32, b));
                }
                term = Some(Term::Switch(sel, base, cases));
                target = None;
            }
            _ => target = None,
        }
        let term = match term {
            Some(t) => t,
            None if term_inst => {
                let conds = conditions(last, w)?;
                let to = match last.mnemonic {
                    "EXIT" | "BPT" => EXIT,
                    _ => match target {
                        Some(k) => ids.get(k, &mut blocks),
                        None => return bad(format!("{} without a target", last.mnemonic)),
                    },
                };
                if conds.is_empty() {
                    Term::Jump(to)
                } else {
                    let Some(f) = fall else {
                        return bad("conditional branch falls off the end");
                    };
                    Term::Branch(conds, to, ids.get(f, &mut blocks))
                }
            }
            None => match fall {
                Some(f) => Term::Jump(ids.get(f, &mut blocks)),
                None => return bad("code falls off the end"),
            },
        };
        blocks[me] = Some(Block {
            first: s,
            end: if term_inst { stop - 1 } else { stop },
            term,
        });
    }
    blocks
        .into_iter()
        .map(|b| b.map_or_else(|| bad("block never built"), Ok))
        .collect()
}

/// One basic block for analyses: the instructions it runs (without the
/// transfer that ends it), its successors (`None` is the exit), and what
/// picks among them: branch conditions or a switch selector.
pub(crate) struct FlowBlock {
    pub insts: std::ops::Range<usize>,
    pub succs: Vec<Option<usize>>,
    pub conds: Vec<Operand>,
    pub selector: Option<Operand>,
}

/// The blocks the translation emits, subroutines inlined, entry first.
pub(crate) fn flow(code: &Code) -> Res<Vec<FlowBlock>> {
    let reg = |s: Sel| match s {
        Sel::Reg(n) => Operand::Reg(n, Default::default()),
        Sel::UReg(n) => Operand::UReg(n, Default::default()),
    };
    let blk = |b: usize| (!terminal(b)).then_some(b);
    build(code)?
        .into_iter()
        .map(|b| {
            let (succs, conds, selector) = match b.term {
                Term::Jump(t) => (vec![blk(t)], Vec::new(), None),
                Term::Branch(c, t, f) => (vec![blk(t), blk(f)], c, None),
                Term::Switch(sel, _, cases) => (
                    cases
                        .iter()
                        .map(|&(_, t)| blk(t))
                        .chain(std::iter::once(None))
                        .collect(),
                    Vec::new(),
                    Some(reg(sel)),
                ),
                Term::Set(..) | Term::Dispatch(..) => return bad("shared join before analysis"),
            };
            Ok(FlowBlock {
                insts: b.first..b.end,
                succs,
                conds,
                selector,
            })
        })
        .collect()
}

/// Immediate dominators of a graph given by successor lists, by Cooper,
/// Harvey and Kennedy's iteration; `None` for nodes `entry` cannot reach.
fn idoms(succ: &[Vec<usize>], entry: usize) -> Vec<Option<usize>> {
    let n = succ.len();
    let mut post = vec![usize::MAX; n];
    let mut order = Vec::with_capacity(n);
    let mut seen = vec![false; n];
    let mut stack = vec![(entry, 0usize)];
    seen[entry] = true;
    while let Some(&mut (u, ref mut k)) = stack.last_mut() {
        if *k < succ[u].len() {
            let v = succ[u][*k];
            *k += 1;
            if !seen[v] {
                seen[v] = true;
                stack.push((v, 0));
            }
        } else {
            post[u] = order.len();
            order.push(u);
            stack.pop();
        }
    }
    let mut preds = vec![Vec::new(); n];
    for (u, ss) in succ.iter().enumerate() {
        if seen[u] {
            for &v in ss {
                preds[v].push(u);
            }
        }
    }
    let mut idom: Vec<Option<usize>> = vec![None; n];
    idom[entry] = Some(entry);
    let mut changed = true;
    while changed {
        changed = false;
        for &b in order.iter().rev() {
            if b == entry {
                continue;
            }
            let mut new: Option<usize> = None;
            for &p in &preds[b] {
                if idom[p].is_none() {
                    continue;
                }
                new = Some(match new {
                    None => p,
                    Some(mut a) => {
                        let mut q = p;
                        while a != q {
                            while post[a] < post[q] {
                                a = idom[a].unwrap_or(entry);
                            }
                            while post[q] < post[a] {
                                q = idom[q].unwrap_or(entry);
                            }
                        }
                        a
                    }
                });
            }
            if new.is_some() && idom[b] != new {
                idom[b] = new;
                changed = true;
            }
        }
    }
    idom
}

fn dominates(idom: &[Option<usize>], a: usize, mut b: usize) -> bool {
    loop {
        if a == b {
            return true;
        }
        match idom[b] {
            Some(p) if p != b => b = p,
            _ => return false,
        }
    }
}

impl Graph {
    pub(crate) fn new(blocks: Vec<Block>) -> Res<Graph> {
        let n = blocks.len();
        let succs: Vec<Vec<usize>> = blocks
            .iter()
            .map(|b| {
                let mut v = b.term.targets();
                v.retain(|t| !terminal(*t));
                let mut seen = HashSet::new();
                v.retain(|t| seen.insert(*t));
                v
            })
            .collect();
        let idom = idoms(&succs, 0);
        let mut latches: HashMap<usize, Vec<usize>> = HashMap::new();
        let mut state = vec![0u8; n];
        let mut stack = vec![(0usize, 0usize)];
        state[0] = 1;
        while let Some(&mut (u, ref mut k)) = stack.last_mut() {
            if *k < succs[u].len() {
                let v = succs[u][*k];
                *k += 1;
                match state[v] {
                    0 => {
                        state[v] = 1;
                        stack.push((v, 0));
                    }
                    1 => {
                        if !dominates(&idom, v, u) {
                            return bad("irreducible control flow");
                        }
                        latches.entry(v).or_default().push(u);
                    }
                    _ => {}
                }
            } else {
                state[u] = 2;
                stack.pop();
            }
        }
        for (u, ss) in succs.iter().enumerate() {
            for &v in ss {
                if idom[u].is_some()
                    && dominates(&idom, v, u)
                    && !latches.get(&v).is_some_and(|l| l.contains(&u))
                {
                    latches.entry(v).or_default().push(u);
                }
            }
        }
        let mut preds = vec![Vec::new(); n];
        for (u, ss) in succs.iter().enumerate() {
            for &v in ss {
                preds[v].push(u);
            }
        }
        let mut headers = HashMap::new();
        for (&h, ls) in &latches {
            let mut body = vec![false; n];
            body[h] = true;
            let mut work: Vec<usize> = ls.clone();
            while let Some(u) = work.pop() {
                if body[u] {
                    continue;
                }
                body[u] = true;
                work.extend(preds[u].iter().copied());
            }
            let mut exits: Vec<usize> = Vec::new();
            for u in (0..n).filter(|u| body[*u]) {
                for &s in &succs[u] {
                    if !body[s] && !exits.contains(&s) {
                        exits.push(s);
                    }
                }
            }
            headers.insert(h, (body, exits));
        }
        Ok(Graph {
            gate: vec![None; blocks.len()],
            blocks,
            succs,
            headers,
            pdoms: HashMap::new(),
            asserting: Vec::new(),
            conv: Vec::new(),
            broadcast: HashSet::new(),
        })
    }

    /// Whether block `b` may end the invocation (EXIT among its targets).
    fn exits(&self, b: usize) -> bool {
        self.blocks[b].term.targets().into_iter().any(terminal)
    }

    /// Immediate post-dominator of `b` among the blocks of loop `ctx` (the
    /// whole program for `None`), where leaving the loop or going round it
    /// ends every path; `None` if paths from `b` share no block. Paths that
    /// end the invocation are left out, except that a block that may end it
    /// and has no other way to the end (a loop left only through EXIT)
    /// ends its paths there.
    pub(crate) fn ipdom(&mut self, ctx: Option<usize>, b: usize) -> Option<usize> {
        if !self.pdoms.contains_key(&ctx) {
            let n = self.blocks.len();
            let end = n;
            let mut rev = vec![Vec::new(); n + 1];
            let body = ctx.map(|h| self.headers[&h].0.clone());
            let gone = |u: usize| self.asserting.get(u).copied().unwrap_or(false);
            for u in 0..n {
                if body.as_ref().is_some_and(|bd| !bd[u]) || gone(u) {
                    continue;
                }
                let mut any = false;
                for &s in self.succs[u].iter().filter(|&&s| !gone(s)) {
                    let sink = Some(s) == ctx || body.as_ref().is_some_and(|bd| !bd[s]);
                    any = true;
                    if sink {
                        rev[end].push(u);
                    } else {
                        rev[s].push(u);
                    }
                }
                if !any {
                    rev[end].push(u);
                }
            }
            let mut reach = vec![false; n + 1];
            let mut work = vec![end];
            while let Some(u) = work.pop() {
                if !std::mem::replace(&mut reach[u], true) {
                    work.extend(rev[u].iter().copied());
                }
            }
            for u in 0..n {
                if !reach[u] && body.as_ref().is_none_or(|bd| bd[u]) && self.exits(u) {
                    rev[end].push(u);
                }
            }
            let id = idoms(&rev, end);
            let v = id
                .into_iter()
                .take(n)
                .map(|p| p.filter(|p| *p != end))
                .collect();
            self.pdoms.insert(ctx, v);
        }
        self.pdoms[&ctx][b]
    }
}

enum Dest {
    Label(u32),
    Ret,
    /// Leave the loop through its exit with this index.
    Break(u32, u32, u32),
    Inline(usize),
}

/// How a block with several successors picks one.
enum Head {
    Cond(u32),
    /// Selector and one literal per target; the first target is the default.
    Switch(u32, Vec<u32>),
    /// Selector and one literal per target; any other value ends the
    /// invocation.
    Cases(u32, Vec<u32>),
}

struct Emit<'a> {
    g: Graph,
    code: &'a Code,
    used: HashSet<u32>,
    emitted: usize,
    selectors: HashMap<usize, u32>,
    /// Per conversion: whether the invocation carries out the region, and
    /// the saved registers and predicates.
    conv_vars: HashMap<usize, ConvVars>,
    /// The shared word loop exit conversions vote in.
    any_word: Option<u32>,
}

struct ConvVars {
    on: u32,
    pending: u32,
    regs: Vec<u32>,
    preds: Vec<u32>,
}

impl Emit<'_> {
    fn resolve(&self, t: usize, follow: Option<(usize, u32)>, lp: Option<&Lp>) -> Res<Dest> {
        if t == EXIT {
            return Ok(Dest::Ret);
        }
        if let Some(l) = lp {
            if t == l.header {
                return Ok(Dest::Label(l.cont));
            }
            if let Some(k) = l.exits.iter().position(|e| *e == t) {
                if l.exits.len() == 1 {
                    return Ok(Dest::Label(l.brk));
                }
                return Ok(Dest::Break(l.var, k as u32, l.brk));
            }
        }
        if let Some((f, fl)) = follow
            && f == t
        {
            return Ok(Dest::Label(fl));
        }
        if let Some(l) = lp
            && !l.body[t]
        {
            return bad("branch out of more than one loop");
        }
        Ok(Dest::Inline(t))
    }

    fn selector_var(&mut self, c: &mut Ctx, var: usize) -> u32 {
        *self.selectors.entry(var).or_insert_with(|| {
            let v = c.m.id();
            c.locals.push(v);
            v
        })
    }

    fn br(&mut self, c: &mut Ctx, l: u32) {
        self.used.insert(l);
        c.m.emit_void(op::BRANCH, &[l]);
    }

    /// Ends the current block at a destination that is not code to inline.
    fn leave(&mut self, c: &mut Ctx, d: &Dest) -> Res<()> {
        match *d {
            Dest::Ret => {
                c.epilogue();
                c.m.emit_void(op::RETURN, &[]);
            }
            Dest::Break(var, k, brk) => {
                let v = c.m.c_u32(k);
                c.m.emit_void(op::STORE, &[var, v]);
                self.br(c, brk);
            }
            Dest::Label(l) => self.br(c, l),
            Dest::Inline(_) => unreachable!(),
        }
        Ok(())
    }

    fn content(&mut self, c: &mut Ctx, b: usize) -> Res<()> {
        let restores: Vec<usize> = (0..self.g.conv.len())
            .filter(|&k| self.g.conv[k].merge == Some(b))
            .collect();
        for k in restores {
            self.restore(c, k);
        }
        let gate = self.g.gate[b];
        let (first, end) = (self.g.blocks[b].first, self.g.blocks[b].end);
        for k in first..end {
            let i = &self.code.insts[k];
            let (w, pc) = (self.code.words[k], 16 * k as u64);
            match gate {
                Some(g) if uniform::gated(i) => {
                    let on = self.on(c, g);
                    let then = c.m.id();
                    let merge = c.m.id();
                    c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
                    c.m.emit_void(op::BRANCH_CONDITIONAL, &[on, then, merge]);
                    c.m.emit_void(op::LABEL, &[then]);
                    c.guarded(i, w, pc)?;
                    c.m.emit_void(op::BRANCH, &[merge]);
                    c.m.emit_void(op::LABEL, &[merge]);
                }
                _ => c.guarded(i, w, pc)?,
            }
        }
        if self.code.asserts.contains(&end) {
            c.assert_record(end);
        }
        Ok(())
    }

    fn vars(&mut self, c: &mut Ctx, k: usize) -> &ConvVars {
        let conv = &self.g.conv[k];
        let (nr, np) = (conv.regs.len(), conv.preds.len());
        self.conv_vars.entry(k).or_insert_with(|| {
            let mut new = || {
                let v = c.m.id();
                c.locals.push(v);
                v
            };
            ConvVars {
                on: new(),
                pending: new(),
                regs: (0..nr).map(|_| new()).collect(),
                preds: (0..np).map(|_| new()).collect(),
            }
        })
    }

    /// Whether the invocation carries out the region of conversion `k`.
    fn on(&mut self, c: &mut Ctx, k: usize) -> u32 {
        let v = self.vars(c, k).on;
        let u = c.u32t();
        let b = c.boolt();
        let x = c.m.emit(op::LOAD, u, &[v]);
        let z = c.m.c_u32(0);
        let on = c.m.emit(op::I_NOT_EQUAL, b, &[x, z]);
        match (self.g.conv[k].stub, self.g.conv[k].outer) {
            (Some(_), Some(o)) => {
                let o = self.on(c, o);
                c.m.emit(op::LOGICAL_AND, b, &[on, o])
            }
            _ => on,
        }
    }

    /// Sets each loop exit conversion's invocation as carrying out its
    /// region, at the start of the program.
    fn start(&mut self, c: &mut Ctx) {
        let one = c.m.c_u32(1);
        for k in 0..self.g.conv.len() {
            if self.g.conv[k].stub.is_some() {
                let v = self.vars(c, k).on;
                c.m.emit_void(op::STORE, &[v, one]);
            }
        }
    }

    /// At the varying exit of loop exit conversion `k`, `stay` saying
    /// whether this invocation goes on in the loop: notes that, and gives
    /// whether any invocation of the workgroup still carries out the loop.
    /// Every invocation runs this together, so its barriers are uniform.
    fn leave_loop(&mut self, c: &mut Ctx, k: usize, stay: u32) -> u32 {
        let b = c.boolt();
        let u = c.u32t();
        let on = self.on(c, k);
        let now = c.m.emit(op::LOGICAL_AND, b, &[on, stay]);
        let one = c.m.c_u32(1);
        let z = c.m.c_u32(0);
        let x = c.select(u, now, one, z);
        let v = self.vars(c, k).on;
        c.m.emit_void(op::STORE, &[v, x]);
        let word = match self.any_word {
            Some(w) => w,
            None => {
                let p = c.m.t_ptr(WORKGROUP, u);
                let w = c.m.global(p, WORKGROUP);
                c.globals.push(w);
                self.any_word = Some(w);
                w
            }
        };
        let exec = c.m.c_u32(SCOPE_WORKGROUP);
        let sem = c.m.c_u32(SEM_ACQUIRE_RELEASE | SEM_WORKGROUP);
        let barrier = |c: &mut Ctx| c.m.emit_void(op::CONTROL_BARRIER, &[exec, exec, sem]);
        let when = |c: &mut Ctx, cond: u32, f: &dyn Fn(&mut Ctx)| {
            let then = c.m.id();
            let merge = c.m.id();
            c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
            c.m.emit_void(op::BRANCH_CONDITIONAL, &[cond, then, merge]);
            c.m.emit_void(op::LABEL, &[then]);
            f(c);
            c.m.emit_void(op::BRANCH, &[merge]);
            c.m.emit_void(op::LABEL, &[merge]);
        };
        barrier(c);
        let i = c.local_index();
        let first = c.m.emit(op::I_EQUAL, b, &[i, z]);
        when(c, first, &|c| c.m.emit_void(op::STORE, &[word, z]));
        barrier(c);
        let on = self.on(c, k);
        when(c, on, &|c| {
            let u = c.u32t();
            c.m.emit(op::ATOMIC_OR, u, &[word, exec, z, one]);
        });
        barrier(c);
        let any = c.m.emit(op::LOAD, u, &[word]);
        c.m.emit(op::I_NOT_EQUAL, b, &[any, z])
    }

    /// Enters the region of conversion `k` from its branch, `take` saying
    /// whether this invocation takes it: notes that and saves what the
    /// region writes.
    fn enter(&mut self, c: &mut Ctx, k: usize, take: u32) {
        let b = c.boolt();
        let take = match self.g.gate[self.g.conv[k].branch] {
            Some(outer) => {
                let o = self.on(c, outer);
                c.m.emit(op::LOGICAL_AND, b, &[take, o])
            }
            None => take,
        };
        let u = c.u32t();
        let one = c.m.c_u32(1);
        let z = c.m.c_u32(0);
        let t = c.select(u, take, one, z);
        let conv = self.g.conv[k].clone();
        let vars = self.vars(c, k);
        let (on, pending) = (vars.on, vars.pending);
        let (rv, pv) = (vars.regs.clone(), vars.preds.clone());
        c.m.emit_void(op::STORE, &[on, t]);
        if conv.merge.is_none() {
            return;
        }
        c.m.emit_void(op::STORE, &[pending, one]);
        for (&r, &v) in conv.regs.iter().zip(&rv) {
            let x = c.load_reg(r);
            c.m.emit_void(op::STORE, &[v, x]);
        }
        for (&p, &v) in conv.preds.iter().zip(&pv) {
            let x = c.load_pred(p, false);
            let x = c.select(u, x, one, z);
            c.m.emit_void(op::STORE, &[v, x]);
        }
    }

    /// At the merge of conversion `k`: an invocation that did not take the
    /// region gets back what it wrote.
    fn restore(&mut self, c: &mut Ctx, k: usize) {
        let conv = self.g.conv[k].clone();
        let vars = self.vars(c, k);
        let (pending, rv, pv) = (vars.pending, vars.regs.clone(), vars.preds.clone());
        let u = c.u32t();
        let b = c.boolt();
        let z = c.m.c_u32(0);
        let p = c.m.emit(op::LOAD, u, &[pending]);
        let p = c.m.emit(op::I_NOT_EQUAL, b, &[p, z]);
        let on = self.on(c, k);
        let off = c.m.emit(op::LOGICAL_NOT, b, &[on]);
        let back = c.m.emit(op::LOGICAL_AND, b, &[p, off]);
        let then = c.m.id();
        let merge = c.m.id();
        c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        c.m.emit_void(op::BRANCH_CONDITIONAL, &[back, then, merge]);
        c.m.emit_void(op::LABEL, &[then]);
        for (&r, &v) in conv.regs.iter().zip(&rv) {
            let x = c.m.emit(op::LOAD, u, &[v]);
            c.store_reg(r, x);
        }
        for (&pr, &v) in conv.preds.iter().zip(&pv) {
            let x = c.m.emit(op::LOAD, u, &[v]);
            let x = c.m.emit(op::I_NOT_EQUAL, b, &[x, z]);
            c.store_pred(pr, x);
        }
        c.m.emit_void(op::BRANCH, &[merge]);
        c.m.emit_void(op::LABEL, &[merge]);
        c.m.emit_void(op::STORE, &[pending, z]);
    }

    fn cond(&mut self, c: &mut Ctx, conds: &[Operand]) -> Res<u32> {
        let mut acc: Option<u32> = None;
        for p in conds {
            let v = c.pred_operand(p)?;
            acc = Some(match acc {
                None => v,
                Some(a) => {
                    let t = c.boolt();
                    c.m.emit(op::LOGICAL_AND, t, &[a, v])
                }
            });
        }
        Ok(acc.unwrap_or_else(|| c.m.c_bool(true)))
    }

    /// The first post-dominator of `b` in loop context `ctx` that is not
    /// in `skip` (the body of a loop headed by `b`).
    fn merge_after(
        &mut self,
        ctx: Option<usize>,
        b: usize,
        skip: Option<&[bool]>,
    ) -> Option<usize> {
        let mut p = self.g.ipdom(ctx, b);
        let mut steps = 0;
        while let Some(x) = p
            && skip.is_some_and(|s| s[x])
        {
            steps += 1;
            if steps > self.g.blocks.len() {
                return None;
            }
            p = self.g.ipdom(ctx, x);
        }
        p
    }

    /// Emits a selection that picks one of `targets` and merges at `pd`;
    /// returns where the region goes on, or `None` if it ended.
    fn fork(
        &mut self,
        c: &mut Ctx,
        head: Head,
        targets: &[usize],
        pd: Option<usize>,
        follow: Option<(usize, u32)>,
        lp: Option<&Lp>,
    ) -> Res<Option<(usize, u32)>> {
        let real = pd.filter(|&m| {
            follow.is_none_or(|(f, _)| f != m)
                && lp.is_none_or(|l| l.header != m && !l.exits.contains(&m))
        });
        let merge = c.m.id();
        let arm_follow = match real {
            Some(r) => Some((r, merge)),
            None => follow.map(|(f, _)| (f, merge)),
        };
        let switch = matches!(head, Head::Switch(..) | Head::Cases(..));
        let mut arms: Vec<(u32, Dest)> = Vec::new();
        let mut by_target: HashMap<usize, u32> = HashMap::new();
        let mut labels = Vec::new();
        for &tg in targets {
            if let Some(&l) = by_target.get(&tg) {
                labels.push(l);
                continue;
            }
            let d = if Some(tg) == real {
                Dest::Label(merge)
            } else {
                self.resolve(tg, arm_follow, lp)?
            };
            let l = match d {
                Dest::Label(x) if !switch || x == merge => x,
                d => {
                    let l = c.m.id();
                    arms.push((l, d));
                    l
                }
            };
            self.used.insert(l);
            by_target.insert(tg, l);
            labels.push(l);
        }
        c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        match head {
            Head::Cond(cv) => {
                c.m.emit_void(op::BRANCH_CONDITIONAL, &[cv, labels[0], labels[1]]);
            }
            Head::Switch(sel, lits) => {
                let mut a = vec![sel, labels[0]];
                for (k, &l) in labels.iter().enumerate().skip(1) {
                    a.push(lits[k]);
                    a.push(l);
                }
                c.m.emit_void(op::SWITCH, &a);
            }
            Head::Cases(sel, lits) => {
                let trap = c.m.id();
                let mut a = vec![sel, trap];
                for (k, &l) in labels.iter().enumerate() {
                    a.push(lits[k]);
                    a.push(l);
                }
                c.m.emit_void(op::SWITCH, &a);
                c.m.emit_void(op::LABEL, &[trap]);
                if c.stage == crate::program::Stage::Fragment {
                    c.m.emit_void(op::KILL, &[]);
                } else {
                    c.m.emit_void(op::RETURN, &[]);
                }
            }
        }
        for (l, d) in arms {
            match d {
                Dest::Inline(b) => self.region(c, b, l, arm_follow, lp)?,
                d => {
                    c.m.emit_void(op::LABEL, &[l]);
                    self.leave(c, &d)?;
                }
            }
        }
        match real {
            Some(r) => Ok(Some((r, merge))),
            None => {
                c.m.emit_void(op::LABEL, &[merge]);
                match follow {
                    Some((_, fl)) if self.used.contains(&merge) => self.br(c, fl),
                    _ => c.m.emit_void(op::UNREACHABLE, &[]),
                }
                Ok(None)
            }
        }
    }

    fn selector(&mut self, c: &mut Ctx, sel: Sel, base: u32) -> u32 {
        let r = match sel {
            Sel::Reg(n) => c.load_reg(n),
            Sel::UReg(n) => c.load_ureg(n),
        };
        let u = c.u32t();
        let b = c.m.c_u32(base);
        c.m.emit(op::I_ADD, u, &[r, b])
    }

    fn region(
        &mut self,
        c: &mut Ctx,
        mut cur: usize,
        mut label: u32,
        follow: Option<(usize, u32)>,
        lp: Option<&Lp>,
    ) -> Res<()> {
        loop {
            self.emitted += 1;
            if self.emitted > 4 * self.g.blocks.len() + 64 {
                return bad(TOO_MUCH_DUPLICATION);
            }
            if let Some((body, exits)) = self.g.headers.get(&cur).cloned()
                && lp.is_none_or(|l| l.header != cur)
            {
                let h = c.m.id();
                let merge = c.m.id();
                let cont = c.m.id();
                let bodyl = c.m.id();
                let var = if exits.len() > 1 {
                    let v = c.m.id();
                    c.locals.push(v);
                    v
                } else {
                    0
                };
                c.m.emit_void(op::LABEL, &[label]);
                self.br(c, h);
                c.m.emit_void(op::LABEL, &[h]);
                c.m.emit_void(op::LOOP_MERGE, &[merge, cont, 0]);
                self.br(c, bodyl);
                let inner = Lp {
                    header: cur,
                    exits: exits.clone(),
                    cont,
                    brk: merge,
                    body,
                    var,
                };
                self.region(c, cur, bodyl, None, Some(&inner))?;
                c.m.emit_void(op::LABEL, &[cont]);
                self.br(c, h);
                match exits.as_slice() {
                    [] => {
                        c.m.emit_void(op::LABEL, &[merge]);
                        c.m.emit_void(op::UNREACHABLE, &[]);
                        return Ok(());
                    }
                    [e] => match self.resolve(*e, follow, lp)? {
                        Dest::Inline(e) => {
                            cur = e;
                            label = merge;
                            continue;
                        }
                        d => {
                            c.m.emit_void(op::LABEL, &[merge]);
                            return self.leave(c, &d);
                        }
                    },
                    _ => {
                        c.m.emit_void(op::LABEL, &[merge]);
                        let u = c.u32t();
                        let sel = c.m.emit(op::LOAD, u, &[var]);
                        let lits = (0..exits.len() as u32).collect();
                        let pd = self.merge_after(lp.map(|l| l.header), cur, Some(&inner.body));
                        match self.fork(c, Head::Switch(sel, lits), &exits, pd, follow, lp)? {
                            Some((r, l)) => {
                                cur = r;
                                label = l;
                                continue;
                            }
                            None => return Ok(()),
                        }
                    }
                }
            }
            c.m.emit_void(op::LABEL, &[label]);
            self.content(c, cur)?;
            let mut term = self.g.blocks[cur].term.clone();
            let mut uniform_exit = None;
            if let Some(k) = self.g.conv.iter().position(|x| x.branch == cur)
                && let Term::Branch(conds, t, _) = &term
            {
                let region = self.g.conv[k].region;
                let cv = self.cond(c, conds)?;
                let take = if *t == region {
                    cv
                } else {
                    let b = c.boolt();
                    c.m.emit(op::LOGICAL_NOT, b, &[cv])
                };
                match self.g.conv[k].stub {
                    Some(stub) => {
                        let any = self.leave_loop(c, k, take);
                        uniform_exit = Some((Head::Cond(any), vec![region, stub]));
                    }
                    None => {
                        self.enter(c, k, take);
                        term = Term::Jump(region);
                    }
                }
            }
            let (head, targets) = if let Some(x) = uniform_exit {
                x
            } else {
                match term {
                    Term::Set(var, value, t) => {
                        let v = self.selector_var(c, var);
                        let k = c.m.c_u32(value);
                        c.m.emit_void(op::STORE, &[v, k]);
                        match self.resolve(t, follow, lp)? {
                            Dest::Inline(t) => {
                                let l = c.m.id();
                                self.br(c, l);
                                cur = t;
                                label = l;
                                continue;
                            }
                            d => return self.leave(c, &d),
                        }
                    }
                    Term::Dispatch(var, ts) => {
                        let v = self.selector_var(c, var);
                        let u = c.u32t();
                        let s = c.m.emit(op::LOAD, u, &[v]);
                        (Head::Switch(s, (0..ts.len() as u32).collect()), ts)
                    }
                    Term::Jump(t) => match self.resolve(t, follow, lp)? {
                        Dest::Inline(t) => {
                            let l = c.m.id();
                            self.br(c, l);
                            cur = t;
                            label = l;
                            continue;
                        }
                        d => return self.leave(c, &d),
                    },
                    Term::Branch(conds, t, f) => (Head::Cond(self.cond(c, &conds)?), vec![t, f]),
                    Term::Switch(sel, base, cases) => {
                        let s = self.selector(c, sel, base);
                        let lits = cases.iter().map(|x| x.0).collect();
                        (Head::Cases(s, lits), cases.iter().map(|x| x.1).collect())
                    }
                }
            };
            let pd = self.g.ipdom(lp.map(|l| l.header), cur);
            match self.fork(c, head, &targets, pd, follow, lp)? {
                Some((r, l)) => {
                    cur = r;
                    label = l;
                }
                None => return Ok(()),
            }
        }
    }
}

impl Graph {
    /// The innermost loop that holds `b`.
    fn innermost(&self, b: usize) -> Option<usize> {
        self.headers
            .iter()
            .filter(|(_, (body, _))| body[b])
            .min_by_key(|(_, (body, _))| body.iter().filter(|x| **x).count())
            .map(|(h, _)| *h)
    }

    /// Where the lanes that run loop `h` come together after it.
    fn after_loop(&mut self, h: usize) -> Option<usize> {
        let (body, exits) = self.headers[&h].clone();
        match exits.as_slice() {
            [] => None,
            [e] => Some(*e),
            _ => {
                let outer = self
                    .headers
                    .iter()
                    .filter(|(o, (b, _))| **o != h && b[h])
                    .min_by_key(|(_, (b, _))| b.iter().filter(|x| **x).count())
                    .map(|(o, _)| *o);
                let mut p = self.ipdom(outer, h);
                for _ in 0..self.blocks.len() {
                    match p {
                        Some(x) if body[x] => p = self.ipdom(outer, x),
                        _ => break,
                    }
                }
                // Exits that meet nowhere inside the enclosing loop: the
                // lanes that stay in it are together again at its header.
                p.or(outer)
            }
        }
    }

    /// The next block where the lanes that run `b` are together again.
    fn next_merge(&mut self, b: usize, entering: bool) -> Option<usize> {
        if entering && self.headers.contains_key(&b) {
            return self.after_loop(b);
        }
        match &self.blocks[b].term {
            Term::Jump(t) => (!terminal(*t)).then_some(*t),
            _ => {
                let ctx = self.innermost(b);
                match self.ipdom(ctx, b) {
                    Some(p) => Some(p),
                    None => ctx,
                }
            }
        }
    }

    fn holds(&self, b: usize, k: usize) -> bool {
        self.blocks[b].first <= k && k <= self.blocks[b].end
    }

    /// The instructions run after block `b` and before instruction `t`:
    /// the blocks reached from the successors of `b` without passing `t`,
    /// as ranges of instructions.
    fn reach_before(&self, b: usize, t: usize) -> Vec<std::ops::Range<usize>> {
        self.reach_before_any(b, &[t])
    }

    /// As `reach_before`, stopping at any of `stops`.
    fn reach_before_any(&self, b: usize, stops: &[usize]) -> Vec<std::ops::Range<usize>> {
        let mut seen = vec![false; self.blocks.len()];
        let mut work = self.succs[b].clone();
        let mut out = Vec::new();
        while let Some(u) = work.pop() {
            if seen[u] {
                continue;
            }
            seen[u] = true;
            let blk = &self.blocks[u];
            if let Some(t) = stops.iter().copied().filter(|t| self.holds(u, *t)).min() {
                out.push(blk.first..t);
                continue;
            }
            out.push(blk.first..blk.end);
            work.extend(self.succs[u].iter().copied());
        }
        out
    }

    /// Whether every convergence barrier the BMOV at `k` (in block `b`)
    /// writes is written again, by a BSSY or another BMOV, on every path
    /// before anything reads it: the reset of the barriers at a program's
    /// start, or a clear right before the BSSY that sets the barrier. A
    /// `BMOV MACTIVE, Bn` is no such read: it is translated only where Bn
    /// holds the active lanes saved there (see `mem::lanes::mactive_from_saved`).
    fn dead_barrier_writes(&self, code: &Code, b: usize, k: usize) -> bool {
        use super::mem::lanes::barrier_name;
        let insts = &code.insts;
        let i = &insts[k];
        let mut written = Vec::new();
        written.extend(i.ops.first().and_then(barrier_name));
        if i.has("CLEAR") {
            written.extend(i.ops.get(1).and_then(barrier_name));
        }
        let names =
            |j: &crate::Inst, n: u8, at: usize| j.ops.get(at).and_then(barrier_name) == Some(n);
        written.into_iter().all(|n| {
            let step = |j: &crate::Inst| -> Option<bool> {
                if j.mnemonic == "BSSY" && j.guard.is_none() && names(j, n, 0) {
                    return Some(true);
                }
                if j.mnemonic == "BMOV" && j.guard.is_none() {
                    let to_active =
                        matches!(j.ops.first(), Some(Operand::Sym(s)) if s == "MACTIVE");
                    if to_active && names(j, n, 1) {
                        return None;
                    }
                    let to_barrier = j.ops.first().and_then(barrier_name).is_some();
                    let discarded = matches!(j.ops.first(), Some(Operand::Reg(255, _)));
                    if names(j, n, 1) && !to_barrier && !discarded {
                        return Some(false);
                    }
                    if names(j, n, 0) || (j.has("CLEAR") && names(j, n, 1)) {
                        return Some(true);
                    }
                    return None;
                }
                j.ops
                    .iter()
                    .any(|o| barrier_name(o) == Some(n))
                    .then_some(false)
            };
            let scan = |range: std::ops::Range<usize>| {
                range
                    .map(|j| step(&insts[j]))
                    .find(Option::is_some)
                    .flatten()
            };
            let blk = &self.blocks[b];
            let mut work = Vec::new();
            match scan(k + 1..blk.end) {
                Some(ok) => return ok,
                None => work.extend(self.succs[b].iter().copied()),
            }
            let mut seen = vec![false; self.blocks.len()];
            while let Some(u) = work.pop() {
                if std::mem::replace(&mut seen[u], true) {
                    continue;
                }
                let ub = &self.blocks[u];
                match scan(ub.first..ub.end) {
                    Some(true) => {}
                    Some(false) => return false,
                    None => work.extend(self.succs[u].iter().copied()),
                }
            }
            true
        })
    }

    /// Checks that the structure reconverges the guest's lanes where its
    /// BSSY/BSYNC regions do (see the module documentation).
    pub(crate) fn check_reconvergence(&mut self, code: &Code) -> Res<Option<OffMerge>> {
        let insts = &code.insts;
        for b in 0..self.blocks.len() {
            let blk = self.blocks[b].clone();
            for k in blk.first..blk.end {
                let i = &insts[k];
                if super::mem::lanes::writes_barrier(i) && !self.dead_barrier_writes(code, b, k) {
                    return bad(format!(
                        "BMOV changes a convergence barrier at {:#x}",
                        16 * k
                    ));
                }
                match i.mnemonic {
                    "BSSY" => {
                        let Some(Operand::Target(t)) = i.ops.last() else {
                            return bad("BSSY without a target");
                        };
                        let t = (*t / 16) as usize;
                        if !self.merges_at(b, t)
                            && let Some(j) = self.unmerged_warp_op(b, t, insts)
                        {
                            return Ok(Some(OffMerge {
                                block: b,
                                target: t,
                                why: format!(
                                    "BSSY at {:#x} reconverges at {:#x}, not at a merge, before the warp operation at {:#x}",
                                    16 * k,
                                    16 * t,
                                    16 * j
                                ),
                            }));
                        }
                    }
                    "BREAK" => {
                        let Some(n) = i.ops.last().and_then(super::mem::lanes::barrier_name) else {
                            return bad("BREAK without a barrier");
                        };
                        let bssy = (0..k).rev().find(|&j| {
                            insts[j].mnemonic == "BSSY"
                                && insts[j]
                                    .ops
                                    .first()
                                    .and_then(super::mem::lanes::barrier_name)
                                    == Some(n)
                        });
                        let Some(Operand::Target(t)) = bssy.and_then(|j| insts[j].ops.last())
                        else {
                            return bad("BREAK without its BSSY");
                        };
                        let t = (*t / 16) as usize;
                        let mut stops = vec![t];
                        stops.extend(enclosing_bsyncs(insts, bssy.unwrap_or(0), k));
                        let after = (k + 1..blk.end)
                            .chain(self.reach_before_any(b, &stops).into_iter().flatten());
                        let warp = after
                            .into_iter()
                            .find(|&j| super::mem::lanes::reads_lanes(&insts[j]));
                        if let Some(j) = warp {
                            return bad(format!(
                                "warp operation at {:#x} between BREAK and its BSYNC",
                                16 * j
                            ));
                        }
                    }
                    _ => {}
                }
            }
        }
        Ok(None)
    }

    /// The blocks only some invocations of the workgroup run (a branch
    /// with a condition that may differ between them can make the others
    /// skip them) and the blocks after an EXIT only some take, with the
    /// varying branches converted where `convert` allows (see the module
    /// documentation).
    fn partial(&mut self, code: &Code, convert: bool) -> (Vec<bool>, Vec<bool>) {
        let insts = &code.insts;
        let n = self.blocks.len();
        let mut partial = vec![false; n];
        let mut exited = vec![false; n];
        loop {
            let entry = self.varying(code, &partial);
            let mut p = partial.clone();
            let mut e = exited.clone();
            let mut restart = false;
            for b in 0..n {
                let Some(mut v) = entry[b] else { continue };
                let blk = &self.blocks[b];
                for (k, i) in insts.iter().enumerate().take(blk.end).skip(blk.first) {
                    let gated = self.gate[b].is_some() && uniform::gated(i);
                    self.step(k, i, partial[b] || gated, &mut v);
                }
                let (uniform, exits) = match &blk.term {
                    Term::Jump(_) | Term::Set(..) => (true, false),
                    _ if self.asserts_on_branch(code, b) => (true, false),
                    Term::Dispatch(_, ts) => (false, ts.iter().any(|t| terminal(*t))),
                    Term::Branch(conds, t, f) => {
                        (v.uniform_all(conds), terminal(*t) || terminal(*f))
                    }
                    Term::Switch(Sel::Reg(r), ..) => (v.uniform_reg(*r, false), false),
                    Term::Switch(Sel::UReg(r), ..) => (v.uniform_reg(*r, true), false),
                };
                if uniform || self.conv.iter().any(|k| k.branch == b) {
                    continue;
                }
                if convert && self.convert(code, b) {
                    restart = true;
                    break;
                }
                let stop = if exits { None } else { self.ipdom(None, b) };
                let mut work = self.succs[b].clone();
                let mut seen = vec![false; n];
                while let Some(u) = work.pop() {
                    if seen[u] || Some(u) == stop {
                        continue;
                    }
                    seen[u] = true;
                    let marks = if exits { &mut e } else { &mut p };
                    marks[u] = true;
                    work.extend(self.succs[u].iter().copied());
                }
            }
            if restart {
                partial = vec![false; n];
                exited = vec![false; n];
                continue;
            }
            if p == partial && e == exited {
                return (partial, exited);
            }
            partial = p;
            exited = e;
        }
    }

    /// Checks that every BAR runs where all the workgroup's invocations
    /// run it (see the module documentation).
    pub(crate) fn check_barriers(&mut self, code: &Code, convert: bool) -> Res<()> {
        let insts = &code.insts;
        self.broadcast = broadcast_loads(self, code);
        let asserting: Vec<bool> = (0..self.blocks.len())
            .map(|u| self.asserting_block(code, u))
            .collect();
        if asserting.iter().any(|&a| a) {
            self.asserting = asserting;
            self.pdoms.clear();
        }
        let (partial, exited) = self.partial(code, convert);
        for k in self.conv.iter().filter(|k| k.stub.is_some()) {
            if partial[k.branch] || exited[k.branch] {
                let blk = &self.blocks[k.branch];
                return bad(format!(
                    "loop exit at {:#x} in non-uniform control flow",
                    16 * (blk.end - 1)
                ));
            }
        }
        let entry = self.varying(code, &partial);
        for b in 0..self.blocks.len() {
            let Some(mut v) = entry[b] else { continue };
            let blk = &self.blocks[b];
            for (k, i) in insts.iter().enumerate().take(blk.end).skip(blk.first) {
                if i.mnemonic == "BAR" {
                    let guard = i.guard.iter().cloned().collect::<Vec<_>>();
                    if partial[b] || !v.uniform_all(&guard) {
                        return bad(format!("BAR at {:#x} in non-uniform control flow", 16 * k));
                    }
                    if exited[b] {
                        return bad(format!(
                            "BAR at {:#x} after an EXIT only some invocations take",
                            16 * k
                        ));
                    }
                }
                let gated = self.gate[b].is_some() && uniform::gated(i);
                self.step(k, i, partial[b] || gated, &mut v);
            }
        }
        Ok(())
    }

    /// The WARPSYNCs that the lanes of their mask reach together on the
    /// host, by instruction index, where the structure decides which lanes
    /// run together: an unguarded WARPSYNC of every lane in a block all
    /// the workgroup's invocations that have not exited run (exited lanes
    /// count as arrived), or one whose mask holds the lanes running it (see
    /// the masks module), outside the regions converted for BAR. Such a wait
    /// holds the same lanes as the host's invocations running together
    /// there, so a subgroup barrier among those is the guest's.
    pub(crate) fn synced_warpsyncs(&mut self, code: &Code) -> HashSet<usize> {
        let insts = &code.insts;
        let (partial, _) = self.partial(code, false);
        let active = self.active_mask_warpsyncs(code);
        let mut out = HashSet::new();
        let mut not = HashSet::new();
        for (b, &part) in partial.iter().enumerate() {
            let blk = &self.blocks[b];
            for (k, i) in insts.iter().enumerate().take(blk.end).skip(blk.first) {
                if i.mnemonic != "WARPSYNC" {
                    continue;
                }
                let all = match i.ops.first() {
                    Some(Operand::Hex(m)) => *m as u32 == u32::MAX,
                    Some(Operand::Int(m)) => *m as u32 == u32::MAX,
                    _ => false,
                };
                let ok = self.gate[b].is_none()
                    && ((all && !part && i.guard.is_none()) || active.contains(&k));
                if ok {
                    out.insert(k);
                } else {
                    not.insert(k);
                }
            }
        }
        out.retain(|k| !not.contains(k));
        out
    }

    /// Converts the varying branch `b` if it has the shape and the region
    /// the module documentation describes; gives whether it did.
    fn convert(&mut self, code: &Code, b: usize) -> bool {
        let Term::Branch(_, t, f) = self.blocks[b].term.clone() else {
            return false;
        };
        let done = match (self.exit_stub(code, t), self.exit_stub(code, f)) {
            (true, false) => self.convert_loop_exit(code, b, f, t),
            (false, true) => self.convert_loop_exit(code, b, t, f),
            _ => false,
        };
        if done {
            return true;
        }
        let merge = if terminal(t) || terminal(f) {
            None
        } else {
            match self.ipdom(None, b) {
                Some(m) => Some(m),
                None => return false,
            }
        };
        let region = match (merge, terminal(t), terminal(f)) {
            (None, true, false) => f,
            (None, false, true) => t,
            (Some(m), _, _) if t == m && f != m => f,
            (Some(m), _, _) if f == m && t != m => t,
            _ => return false,
        };
        let n = self.blocks.len();
        let mut stop = vec![false; n];
        if let Some(m) = merge {
            stop[m] = true;
        }
        let inside = reach(&self.succs, region, &stop);
        let post = match merge {
            Some(m) => reach(&self.succs, m, &vec![false; n]),
            None => vec![false; n],
        };
        if inside[b] {
            return false;
        }
        let idom = idoms(&self.succs, 0);
        let insts = &code.insts;
        let blocks = |m: &[bool]| (0..n).filter(|&u| m[u]).collect::<Vec<_>>();
        let body = blocks(&inside);
        let has = |us: &[usize], f: &dyn Fn(&crate::Inst) -> bool| {
            us.iter().any(|&u| {
                let blk = &self.blocks[u];
                insts[blk.first..blk.end].iter().any(f)
            })
        };
        if !has(&body, &|i| i.mnemonic == "BAR") {
            return false;
        }
        for &u in &body {
            if !dominates(&idom, b, u)
                || (merge.is_some() && self.exits(u))
                || !matches!(self.blocks[u].term, Term::Jump(_) | Term::Branch(..))
            {
                return false;
            }
        }
        let after = blocks(&post);
        let unseen = !has(&body, &|i| {
            super::simt::cross(i)
                || i.mnemonic == "R2UR"
                || (merge.is_some() && super::simt::writes_uniform(i))
                || matches!(
                    i.mnemonic,
                    "KILL" | "CALL" | "RET" | "BRX" | "BRXU" | "JMP" | "JMX" | "BPT"
                )
                || (merge.is_some()
                    && matches!(
                        i.mnemonic,
                        "LDG"
                            | "LD"
                            | "ATOM"
                            | "ATOMG"
                            | "RED"
                            | "SULD"
                            | "SUST"
                            | "SUATOM"
                            | "SURED"
                            | "LDGSTS"
                            | "EXIT"
                    ))
        }) && !has(&after, &|i| {
            matches!(
                i.mnemonic,
                "BAR" | "LDS" | "STS" | "ATOMS" | "LDSM" | "LDGSTS" | "LD" | "ST" | "ATOM"
            )
        }) && (!has(&body, &|i| {
            matches!(i.mnemonic, "TEX" | "TLD" | "TLD4" | "TXD" | "TMML")
        }) || !has(&after, &|i| {
            matches!(i.mnemonic, "SUST" | "SUATOM" | "SURED")
        }));
        if !unseen {
            return false;
        }
        let mut regs = Vec::new();
        let mut preds = Vec::new();
        for &u in &body {
            let blk = &self.blocks[u];
            for i in &insts[blk.first..blk.end] {
                regs.extend(super::uniform::written_regs(i));
                preds.extend(super::uniform::written_preds(i));
            }
        }
        regs.sort_unstable();
        regs.dedup();
        preds.sort_unstable();
        preds.dedup();
        let k = self.conv.len();
        self.conv.push(Conv {
            branch: b,
            region,
            merge,
            regs,
            preds,
            stub: None,
            outer: None,
        });
        for &u in &body {
            if self.gate[u].is_none_or(|j| dominates(&idom, self.conv[j].branch, b)) {
                self.gate[u] = Some(k);
            }
        }
        true
    }

    /// One instruction of the uniformity analysis, with the loads in
    /// `broadcast` uniform where their address is and every invocation
    /// runs them.
    fn step(&self, k: usize, i: &crate::Inst, partial: bool, v: &mut uniform::Varying) {
        if !partial && self.broadcast.contains(&k) && uniform::broadcast(i, v) {
            return;
        }
        uniform::step(i, partial, v);
    }

    /// Whether one side of branch `b` is a call to `__assertfail`: it stops
    /// the whole dispatch on the guest (the grid traps), so what the other
    /// invocations do after it does not matter and the branch counts as
    /// uniform.
    fn asserts_on_branch(&self, code: &Code, b: usize) -> bool {
        let targets = match &self.blocks[b].term {
            Term::Branch(_, t, f) => vec![*t, *f],
            Term::Dispatch(_, ts) => ts.clone(),
            _ => return false,
        };
        let (fail, rest): (Vec<usize>, Vec<usize>) = targets
            .into_iter()
            .partition(|&u| self.asserting_block(code, u));
        !fail.is_empty() && rest.len() <= 1
    }

    /// Whether block `start` only leads (through empty blocks) to a call
    /// to `__assertfail`.
    fn asserting_block(&self, code: &Code, start: usize) -> bool {
        {
            let mut u = start;
            for _ in 0..4 {
                if terminal(u) {
                    return false;
                }
                let blk = &self.blocks[u];
                match blk.term {
                    Term::Jump(EXIT) => return code.asserts.contains(&blk.end),
                    Term::Jump(n) | Term::Set(_, _, n) if blk.first == blk.end => u = n,
                    _ => return false,
                }
            }
            false
        }
    }

    /// Whether `t` ends the invocation: EXIT, or a block that does
    /// nothing else.
    fn exit_stub(&self, code: &Code, t: usize) -> bool {
        if terminal(t) {
            return true;
        }
        let blk = &self.blocks[t];
        matches!(blk.term, Term::Jump(EXIT))
            && code.insts[blk.first..blk.end]
                .iter()
                .all(|i| i.guard.is_none() && matches!(i.mnemonic, "EXIT" | "NOP" | "BSYNC"))
    }

    /// Converts the varying branch `b` of a loop from which invocations
    /// leave to `stub`, a block that only ends them, and otherwise go on at
    /// `stay` (see the module documentation); gives whether it did.
    fn convert_loop_exit(&mut self, code: &Code, b: usize, stay: usize, stub: usize) -> bool {
        let Some(h) = self.innermost(b) else {
            return false;
        };
        let body = &self.headers[&h].0;
        if terminal(stay) || (!terminal(stub) && body[stub]) || !body[stay] {
            return false;
        }
        let n = self.blocks.len();
        let mut stop = vec![false; n];
        if !terminal(stub) {
            stop[stub] = true;
        }
        let inside = reach(&self.succs, stay, &stop);
        let region: Vec<usize> = (0..n).filter(|&u| inside[u]).collect();
        let insts = &code.insts;
        let has = |f: &dyn Fn(&crate::Inst) -> bool| {
            region.iter().any(|&u| {
                let blk = &self.blocks[u];
                insts[blk.first..blk.end].iter().any(f)
            })
        };
        if !has(&|i| i.mnemonic == "BAR")
            || has(&|i| {
                super::simt::cross(i)
                    || i.mnemonic == "R2UR"
                    || matches!(i.mnemonic, "KILL" | "BRX" | "BRXU" | "JMP" | "JMX" | "BPT")
            })
            || region
                .iter()
                .any(|&u| !matches!(self.blocks[u].term, Term::Jump(_) | Term::Branch(..)))
        {
            return false;
        }
        let idom = idoms(&self.succs, 0);
        let outer = self.gate[b];
        let k = self.conv.len();
        self.conv.push(Conv {
            branch: b,
            region: stay,
            merge: None,
            regs: Vec::new(),
            preds: Vec::new(),
            stub: Some(stub),
            outer,
        });
        for &u in &region {
            if self.gate[u].is_none_or(|j| j != k && dominates(&idom, self.conv[j].branch, b)) {
                self.gate[u] = Some(k);
            }
        }
        true
    }

    /// What may vary between invocations at the start of each block
    /// (`None` where nothing reaches it), with the blocks in `partial` run
    /// by only some invocations.
    fn varying(&self, code: &Code, partial: &[bool]) -> Vec<Option<uniform::Varying>> {
        let n = self.blocks.len();
        let mut entry: Vec<Option<uniform::Varying>> = vec![None; n];
        entry[0] = Some(uniform::Varying::default());
        let mut work = vec![0];
        while let Some(b) = work.pop() {
            let Some(mut v) = entry[b] else { continue };
            let blk = &self.blocks[b];
            for k in blk.first..blk.end {
                let i = &code.insts[k];
                let gated = self.gate[b].is_some() && uniform::gated(i);
                self.step(k, i, partial[b] || gated, &mut v);
            }
            for &s in &self.succs[b] {
                let changed = match &mut entry[s] {
                    Some(old) => old.join(&v),
                    slot @ None => {
                        *slot = Some(v);
                        true
                    }
                };
                if changed {
                    work.push(s);
                }
            }
        }
        entry
    }

    /// The first instruction that sees which lanes run together (a warp
    /// operation that reads other lanes or the active mask, or WARPSYNC)
    /// from `t` on and before the lanes of `b` reach a merge of the
    /// structure: where the guest's lanes are back together and the host's
    /// may not be yet.
    fn unmerged_warp_op(&mut self, b: usize, t: usize, insts: &[crate::Inst]) -> Option<usize> {
        let mut chain = vec![false; self.blocks.len()];
        let mut cur = Some(b);
        let mut entering = false;
        for _ in 0..2 * self.blocks.len() + 2 {
            let Some(x) = cur else { break };
            if x != b {
                chain[x] = true;
            }
            cur = self.next_merge(x, entering);
            entering = true;
        }
        let sees = |i: &crate::Inst| {
            super::mem::lanes::reads_lanes(i)
                || super::mem::lanes::reads_active(i)
                || i.mnemonic == "WARPSYNC"
        };
        let mut seen = vec![false; self.blocks.len()];
        let mut work: Vec<(usize, usize)> = (0..self.blocks.len())
            .filter(|&u| self.holds(u, t))
            .map(|u| (u, t))
            .collect();
        while let Some((u, from)) = work.pop() {
            if seen[u] || chain[u] {
                continue;
            }
            seen[u] = true;
            let blk = &self.blocks[u];
            if let Some(j) = (from.max(blk.first)..blk.end).find(|&j| sees(&insts[j])) {
                return Some(j);
            }
            work.extend(self.succs[u].iter().map(|&s| (s, 0)));
        }
        None
    }

    /// Instruction `t` lies on the chain of merges that starts at `b`, or
    /// in a block that only a dispatch on that chain enters (every lane
    /// that goes there takes the same case together), or the lanes of `b`
    /// never reach it.
    fn merges_at(&mut self, b: usize, t: usize) -> bool {
        let mut cur = Some(b);
        let mut entering = false;
        for _ in 0..2 * self.blocks.len() + 2 {
            let Some(x) = cur else { break };
            if self.holds(x, t) {
                return true;
            }
            if let Term::Dispatch(_, ts) = &self.blocks[x].term
                && ts.iter().any(|&j| {
                    !terminal(j)
                        && self.holds(j, t)
                        && (0..self.blocks.len()).all(|u| u == x || !self.succs[u].contains(&j))
                })
            {
                return true;
            }
            cur = self.next_merge(x, entering);
            entering = true;
        }
        !self
            .reach_before(b, usize::MAX)
            .iter()
            .any(|r| r.contains(&t) || r.end == t)
    }
}

/// The BSYNCs of the regions around the BSSY at `inner` that hold the
/// BREAK at `k` and are never left by a BREAK of their own: lanes that
/// break out of the inner region still wait there for the rest.
fn enclosing_bsyncs(insts: &[crate::Inst], inner: usize, k: usize) -> Vec<usize> {
    use super::mem::lanes::barrier_name;
    let mut out = Vec::new();
    for (j, i) in insts.iter().enumerate().take(inner) {
        if i.mnemonic != "BSSY" {
            continue;
        }
        let (Some(m), Some(Operand::Target(t))) =
            (i.ops.first().and_then(barrier_name), i.ops.last())
        else {
            continue;
        };
        let t = (*t / 16) as usize;
        let Some(sync) = t.checked_sub(1).filter(|&s| s > k) else {
            continue;
        };
        let is_sync = |x: &crate::Inst, name: &str| {
            x.mnemonic == name && x.ops.last().and_then(barrier_name) == Some(m)
        };
        if !insts.get(sync).is_some_and(|x| is_sync(x, "BSYNC")) {
            continue;
        }
        if insts[j + 1..sync]
            .iter()
            .any(|x| is_sync(x, "BREAK") || is_sync(x, "BSSY"))
        {
            continue;
        }
        out.push(sync);
    }
    out
}

/// Blocks reachable from `from` over `succ`, not entering `stop`.
fn reach(succ: &[Vec<usize>], from: usize, stop: &[bool]) -> Vec<bool> {
    let mut seen = vec![false; succ.len()];
    let mut work = vec![from];
    while let Some(u) = work.pop() {
        if seen[u] || stop[u] {
            continue;
        }
        seen[u] = true;
        work.extend(succ[u].iter().copied());
    }
    seen
}

/// Whether `i` may write shared memory.
fn writes_shared(i: &crate::Inst) -> bool {
    matches!(
        i.mnemonic,
        "STS" | "ATOMS" | "LDGSTS" | "ST" | "ATOM" | "RED" | "CALL" | "RET"
    )
}

/// The unguarded `LDS` instructions that follow a BAR in their block, with
/// no shared memory write between, and from which no path writes shared
/// memory before the next BAR: no invocation can write shared memory while
/// another is between that BAR and the next, so a load of one address
/// reads the same word in every invocation that runs it.
fn broadcast_loads(g: &Graph, code: &Code) -> HashSet<usize> {
    let insts = &code.insts;
    let bar = |i: &crate::Inst| i.mnemonic == "BAR" && i.guard.is_none();
    let mut out = HashSet::new();
    for (b, blk) in g.blocks.iter().enumerate() {
        for k in blk.first..blk.end {
            let i = &insts[k];
            if i.mnemonic != "LDS" || i.guard.is_some() {
                continue;
            }
            let after = insts[blk.first..k]
                .iter()
                .rev()
                .take_while(|j| !writes_shared(j))
                .any(&bar);
            if !after {
                continue;
            }
            let quiet = |range: &[crate::Inst]| -> Option<bool> {
                for j in range {
                    if bar(j) {
                        return Some(true);
                    }
                    if writes_shared(j) {
                        return Some(false);
                    }
                }
                None
            };
            let mut ok = true;
            let mut seen = vec![false; g.blocks.len()];
            let mut work = Vec::new();
            match quiet(&insts[k + 1..blk.end]) {
                Some(q) => ok = q,
                None => work.extend(g.succs[b].iter().copied()),
            }
            while ok && let Some(u) = work.pop() {
                if std::mem::replace(&mut seen[u], true) {
                    continue;
                }
                let ub = &g.blocks[u];
                match quiet(&insts[ub.first..ub.end]) {
                    Some(q) => ok = q,
                    None => work.extend(g.succs[u].iter().copied()),
                }
            }
            if ok {
                out.insert(k);
            }
        }
    }
    out
}

/// What splitting a join needs to know about the graph.
struct Shape {
    idom: Vec<Option<usize>>,
    fwd: Vec<Vec<usize>>,
    pdom: Vec<Option<usize>>,
    loops: Vec<Vec<usize>>,
    end: usize,
}

impl Shape {
    fn new(blocks: &[Block]) -> Shape {
        let n = blocks.len();
        let all: Vec<Vec<usize>> = blocks
            .iter()
            .map(|b| {
                let mut v = b.term.targets();
                v.retain(|t| !terminal(*t));
                v
            })
            .collect();
        let idom = idoms(&all, 0);
        let fwd: Vec<Vec<usize>> = all
            .iter()
            .enumerate()
            .map(|(u, ss)| {
                ss.iter()
                    .copied()
                    .filter(|&v| idom[u].is_none() || !dominates(&idom, v, u))
                    .collect()
            })
            .collect();
        let end = n;
        let mut rev = vec![Vec::new(); n + 1];
        for (u, b) in blocks.iter().enumerate() {
            let ts = b.term.targets();
            if ts.iter().any(|t| terminal(*t)) || ts.is_empty() {
                rev[end].push(u);
            }
            for &v in &all[u] {
                rev[v].push(u);
            }
        }
        let pdom = idoms(&rev, end);
        let mut preds = vec![Vec::new(); n];
        for (u, ss) in all.iter().enumerate() {
            for &v in ss {
                preds[v].push(u);
            }
        }
        let mut loops: Vec<Vec<usize>> = vec![Vec::new(); n];
        for (u, ss) in all.iter().enumerate() {
            for &h in ss {
                if idom[u].is_none() || !dominates(&idom, h, u) {
                    continue;
                }
                let mut body = vec![false; n];
                body[h] = true;
                let mut work = vec![u];
                while let Some(x) = work.pop() {
                    if !body[x] {
                        body[x] = true;
                        work.extend(preds[x].iter().copied());
                    }
                }
                for (x, inl) in body.into_iter().enumerate() {
                    if inl && !loops[x].contains(&h) {
                        loops[x].push(h);
                    }
                }
            }
        }
        Shape {
            idom,
            fwd,
            pdom,
            loops,
            end,
        }
    }

    fn within(&self, a: usize, b: usize) -> bool {
        terminal(a) || self.loops[a].iter().all(|h| self.loops[b].contains(h))
    }

    /// The blocks from the post-dominator of `b` on.
    fn beyond(&self, b: usize) -> Vec<bool> {
        let none = vec![false; self.fwd.len()];
        match self.pdom[b].filter(|&p| p != self.end) {
            Some(p) => reach(&self.fwd, p, &none),
            None => none,
        }
    }
}

/// Splits one shared join (see the module notes); `false` if there is
/// none left.
fn split_one(blocks: &mut Vec<Block>, vars: &mut usize) -> bool {
    let n = blocks.len();
    let s = Shape::new(blocks);
    let mut order = Vec::new();
    let mut seen = vec![false; n];
    let mut stack = vec![(0usize, 0usize)];
    seen[0] = true;
    while let Some(&mut (u, ref mut k)) = stack.last_mut() {
        if *k < s.fwd[u].len() {
            let v = s.fwd[u][*k];
            *k += 1;
            if !seen[v] {
                seen[v] = true;
                stack.push((v, 0));
            }
        } else {
            order.push(u);
            stack.pop();
        }
    }
    order.reverse();
    let mut rpo = vec![usize::MAX; n];
    for (k, &u) in order.iter().enumerate() {
        rpo[u] = k;
    }
    for &b in &order {
        let mut arms = s.fwd[b].clone();
        arms.dedup();
        if arms.len() < 2 {
            continue;
        }
        let beyond = s.beyond(b);
        let mut hits = vec![0usize; n];
        for &a in &arms {
            for (u, r) in reach(&s.fwd, a, &beyond).into_iter().enumerate() {
                hits[u] += usize::from(r);
            }
        }
        let Some(j) = (0..n)
            .filter(|&u| hits[u] >= 2 && u != b && !matches!(blocks[u].term, Term::Set(..)))
            .min_by_key(|&u| rpo[u])
        else {
            continue;
        };
        if split_at(blocks, vars, &s, b, j) {
            return true;
        }
    }
    false
}

/// Sends the lanes of `b` that go to `j`, and those that leave the code
/// between them for elsewhere, through one new block that dispatches on a
/// selector variable, so the selection of `b` merges there and its lanes
/// for `j` enter it together. `false` if the region between them does not
/// allow it (`b` does not dominate it, a back edge leaves it, or loops
/// cross its edges).
fn split_at(blocks: &mut Vec<Block>, vars: &mut usize, s: &Shape, b: usize, j: usize) -> bool {
    let n = blocks.len();
    if !dominates(&s.idom, b, j) {
        return false;
    }
    let none = vec![false; n];
    let mut stop = s.beyond(b);
    stop[j] = true;
    let from_b = reach(&s.fwd, b, &stop);
    let mut back = vec![Vec::new(); n];
    for u in (0..n).filter(|&u| from_b[u]) {
        for &v in &s.fwd[u] {
            back[v].push(u);
        }
    }
    let to_j = reach(&back, j, &none);
    let region: Vec<usize> = (0..n).filter(|&u| from_b[u] && to_j[u]).collect();
    if region.iter().any(|&u| !dominates(&s.idom, b, u)) {
        return false;
    }
    let inside = |t: usize| !terminal(t) && t != j && region.contains(&t);
    let mut escapes: Vec<usize> = Vec::new();
    let mut back_edge = false;
    for &u in &region {
        for t in blocks[u].term.targets() {
            if inside(t) || t == j {
                continue;
            }
            if !terminal(t) && dominates(&s.idom, t, u) {
                back_edge = true;
            }
            if !escapes.contains(&t) {
                escapes.push(t);
            }
        }
    }
    if back_edge
        || escapes.is_empty()
        || region.iter().any(|&u| !s.within(u, j) || !s.within(j, u))
        || escapes.iter().any(|&t| !s.within(t, j))
    {
        return false;
    }
    let var = *vars;
    *vars += 1;
    let d = n;
    let mut targets = vec![j];
    targets.extend(&escapes);
    blocks.push(Block {
        first: 0,
        end: 0,
        term: Term::Dispatch(var, targets.clone()),
    });
    for (k, _) in targets.iter().enumerate() {
        blocks.push(Block {
            first: 0,
            end: 0,
            term: Term::Set(var, k as u32, d),
        });
    }
    for &u in &region {
        blocks[u]
            .term
            .retarget(|t| match targets.iter().position(|x| *x == t) {
                Some(k) if t == j || !inside(t) => d + 1 + k,
                _ => t,
            });
    }
    true
}

/// Splits the shared joins, a bounded number of times; gives the number
/// of selector variables used.
fn split_joins(blocks: &mut Vec<Block>) -> usize {
    let mut vars = 0;
    let limit = 2 * blocks.len() + 16;
    for _ in 0..limit {
        if !split_one(blocks, &mut vars) {
            break;
        }
    }
    vars
}

/// Funnels the BSSY region of `m` (see [`split_at`]) to the block that
/// holds its target (the block it ends, if it starts none); `false` if
/// there is none or it cannot.
fn funnel(blocks: &mut Vec<Block>, vars: &mut usize, m: &OffMerge) -> bool {
    let s = Shape::new(blocks);
    let t = m.target;
    let ok = |u: usize| u != m.block && dominates(&s.idom, m.block, u);
    let Some(j) = (0..blocks.len())
        .find(|&u| ok(u) && blocks[u].first <= t && t < blocks[u].end)
        .or_else(|| {
            (0..blocks.len()).find(|&u| ok(u) && blocks[u].first < t && t == blocks[u].end)
        })
    else {
        return false;
    };
    split_at(blocks, vars, &s, m.block, j)
}

/// The program's graph; `split` splits the shared joins first. With
/// `reconverge` (the structure decides which lanes run together), every
/// BSSY region must reconverge where the guest's does, and one whose
/// target is no merge is funneled there (see [`split_at`]).
pub(crate) fn structure(code: &Code, split: bool, reconverge: bool) -> Res<Graph> {
    let mut blocks = build(code)?;
    let mut vars = if split { split_joins(&mut blocks) } else { 0 };
    let limit = blocks.len() + 16;
    let mut g = Graph::new(blocks.clone())?;
    if reconverge {
        for k in 0.. {
            let Some(m) = g.check_reconvergence(code)? else {
                break;
            };
            if k == limit || !funnel(&mut blocks, &mut vars, &m) {
                return bad(m.why);
            }
            g = Graph::new(blocks.clone())?;
        }
    }
    Ok(g)
}

/// Emits the program's body, starting with label `first`; `split` splits
/// the shared joins first.
pub(crate) fn emit(c: &mut Ctx, code: &Code, first: u32, split: bool) -> Res<()> {
    super::io::prescan(c, &code.insts);
    let mut g = structure(code, split, c.iface.host.max_reconvergence)?;
    if code.insts.iter().any(|i| i.mnemonic == "BAR") {
        g.check_barriers(code, c.stage == crate::program::Stage::Compute)?;
    }
    if c.iface.host.max_reconvergence && code.insts.iter().any(|i| i.mnemonic == "WARPSYNC") {
        c.synced = g.synced_warpsyncs(code);
    }
    let mut e = Emit {
        g,
        code,
        used: HashSet::new(),
        emitted: 0,
        selectors: HashMap::new(),
        conv_vars: HashMap::new(),
        any_word: None,
    };
    c.m.keep_vars();
    let r = if e.g.conv.iter().any(|k| k.stub.is_some()) {
        let body = c.m.id();
        c.m.emit_void(op::LABEL, &[first]);
        e.start(c);
        c.m.emit_void(op::BRANCH, &[body]);
        e.region(c, 0, body, None, None)
    } else {
        e.region(c, 0, first, None, None)
    };
    c.m.restore_vars(None);
    r
}

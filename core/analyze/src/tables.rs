//! Jump tables: the targets of a computed jump found by running the code
//! before it in the IR interpreter, once per index value.
//!
//! The shape is a bounds check (a comparison and a conditional branch that
//! falls through or branches to the body), then straight-line code that loads from the table and computes the target, and
//! the jump. Walking back from the jump over instructions that fall through
//! into each other finds the conditional branch; the code between them is
//! the body. It may store only to stack slots; those, and every other block
//! the jump target does not depend on, are left out when it runs.
//! Each register the body reads before writing it, and that the
//! constant propagation does not know, is a candidate index. For a
//! candidate, the check (the instructions before the branch back to the one
//! that last writes the candidate, at most [`CMP_BACK`]) runs with the
//! candidate set to 0, 1, 2 and so on; while the branch keeps to the body,
//! the body runs with the same value over the module's own bytes and its
//! jump target is one table entry. Of the candidates for which every
//! in-bound index gives a code address, the one with the most distinct
//! targets wins.

use std::collections::{BTreeSet, HashMap};
use std::sync::Arc;

use relift_guest::{Frontend, Module};
use relift_ir::interp::{self, Fault, GuestMemory, NoDevices, NoIntrinsics, State};
use relift_ir::{Block, Flow, Next, Op, Reg, Value};

/// The most instructions between the conditional branch and the jump.
const RUN_BACK: usize = 16;
/// The most instructions before the conditional branch in the check.
const CMP_BACK: usize = 4;
/// The most instructions the check skips over because they do not touch
/// what it reads (the stores a compiler schedules between a compare and its
/// branch).
const SKIP_BACK: usize = 24;
/// The largest table.
const MAX_ENTRIES: u64 = 4096;

fn falls_into(b: &Block, next: u64) -> bool {
    match (b.exit.flow, b.exit.next) {
        (Flow::Jump, Next::Const(t)) => t == next,
        (
            Flow::Jump,
            Next::Select {
                taken, not_taken, ..
            },
        ) => taken == next || not_taken == next,
        _ => false,
    }
}

fn writes(b: &Block, r: Reg) -> bool {
    b.insts
        .iter()
        .any(|i| matches!(i.op, Op::Set(x, _) if x == r))
}

fn has_memory(b: &Block) -> bool {
    b.insts.iter().any(|i| {
        matches!(
            i.op,
            Op::Store { .. } | Op::StoreExclusive { .. } | Op::LoadExclusive { .. }
        )
    })
}

/// The module's bytes, read in place: table code only loads (the body is
/// refused when it stores), so a write faults rather than copying the image.
struct Image<'a> {
    base: u64,
    bytes: &'a [u8],
}

impl GuestMemory for Image<'_> {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault> {
        let fault = Fault {
            addr,
            size: u32::try_from(buf.len()).unwrap_or(u32::MAX),
            write: false,
        };
        let off = addr
            .checked_sub(self.base)
            .and_then(|o| usize::try_from(o).ok())
            .ok_or(fault)?;
        let src = off
            .checked_add(buf.len())
            .and_then(|end| self.bytes.get(off..end))
            .ok_or(fault)?;
        buf.copy_from_slice(src);
        Ok(())
    }

    fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), Fault> {
        Err(Fault {
            addr,
            size: u32::try_from(data.len()).unwrap_or(u32::MAX),
            write: true,
        })
    }
}

/// Runs `blocks` in order from `state` whether or not they follow each
/// other (the check, with independent instructions left out); the address
/// after the last one, or `None` when an instruction faults.
fn run_seq(blocks: &[Arc<Block>], state: &mut State, mem: &mut Image<'_>) -> Option<u64> {
    let mut next = None;
    for b in blocks {
        next = Some(
            interp::run_verified(b, state, mem, &mut NoIntrinsics, &mut NoDevices)
                .ok()?
                .next,
        );
    }
    next
}

/// The registers `blocks`, run in order, read before writing them.
fn reads(blocks: &[Arc<Block>]) -> BTreeSet<Reg> {
    let mut read = BTreeSet::new();
    let mut written = BTreeSet::new();
    for b in blocks {
        for i in &b.insts {
            if let Op::Get(x) = i.op
                && !written.contains(&x)
            {
                read.insert(x);
            }
        }
        for i in &b.insts {
            if let Op::Set(x, _) = i.op {
                written.insert(x);
            }
        }
    }
    read
}

/// Whether every store in `b` writes at an address made only of the stack
/// pointer `sp` and constants: a stack slot, never the table.
fn stores_to_stack(b: &Block, sp: Option<Reg>) -> bool {
    fn from_sp(b: &Block, v: Value, sp: Reg) -> bool {
        match &b.insts[v.index()].op {
            Op::Get(r) => *r == sp,
            Op::Const(_) => true,
            Op::Bin(_, x, y) => from_sp(b, *x, sp) && from_sp(b, *y, sp),
            Op::Ext(_, x) | Op::Extract(x, _) => from_sp(b, *x, sp),
            _ => false,
        }
    }
    b.insts.iter().all(|i| match i.op {
        Op::Store { addr, .. } => sp.is_some_and(|sp| from_sp(b, addr, sp)),
        _ => true,
    })
}

/// The blocks of `body` the jump at its end depends on, in order: a block
/// whose writes no later kept block reads (a load the compiler scheduled
/// into the body for later code) is left out, so it cannot fault the run.
fn slice(body: &[Arc<Block>]) -> Vec<Arc<Block>> {
    let Some((jump, rest)) = body.split_last() else {
        return Vec::new();
    };
    let mut needed = reads(std::slice::from_ref(jump));
    let mut keep = vec![jump.clone()];
    for b in rest.iter().rev() {
        let sets: BTreeSet<Reg> = b
            .insts
            .iter()
            .filter_map(|i| match i.op {
                Op::Set(x, _) => Some(x),
                _ => None,
            })
            .collect();
        if sets.iter().any(|x| needed.contains(x)) {
            needed.retain(|x| !sets.contains(x));
            needed.extend(reads(std::slice::from_ref(b)));
            keep.push(b.clone());
        }
    }
    keep.reverse();
    keep
}

/// The targets of the computed jump `jump` at `pc`, or `None` when it is not
/// a jump table this recognizes. `block_at` lifts the instruction at an
/// address; `entering` gives the conditional branch of the function that
/// jumps to an address; `known` gives the registers known to be constant at
/// an address. The body may store to stack slots of `fe`'s stack pointer.
pub(crate) fn resolve(
    module: &Module,
    pc: u64,
    jump: &Block,
    fe: &dyn Frontend,
    block_at: &mut dyn FnMut(u64) -> Option<Arc<Block>>,
    entering: &dyn Fn(u64) -> Option<Arc<Block>>,
    known: &dyn Fn(u64) -> HashMap<Reg, u128>,
) -> Option<Vec<u64>> {
    if !matches!(
        (jump.exit.flow, jump.exit.next),
        (Flow::Jump, Next::Dynamic(_))
    ) {
        return None;
    }
    let granule = fe.granule();
    let sp = fe.stack_pointer();
    let mut body = vec![Arc::new(jump.clone())];
    let mut at = pc;
    let branch = loop {
        if body.len() > RUN_BACK {
            return None;
        }
        let prev = at
            .checked_sub(granule)
            .and_then(|a| block_at(a).map(|b| (a, b)));
        match prev {
            Some((a, b)) if falls_into(&b, at) && !matches!(b.exit.next, Next::Select { .. }) => {
                if !stores_to_stack(&b, sp) {
                    return None;
                }
                body.insert(0, b);
                at = a;
            }
            Some((_, b)) if falls_into(&b, at) => break b,
            _ => break entering(at)?,
        }
    };
    let start = body[0].pc;
    let seeds = known(start);
    let body = slice(&body);
    if body
        .iter()
        .any(|b| b.insts.iter().any(|i| matches!(i.op, Op::Store { .. })))
    {
        return None;
    }
    let mut read = Vec::new();
    let mut written = BTreeSet::new();
    for b in &body {
        for i in &b.insts {
            if let Op::Get(r) = i.op
                && !written.contains(&r)
                && !read.contains(&r)
                && !seeds.contains_key(&r)
                && b.regs.width(r).bits() > 1
            {
                read.push(r);
            }
        }
        for i in &b.insts {
            if let Op::Set(r, _) = i.op {
                written.insert(r);
            }
        }
    }
    let mut mem = Image {
        base: module.base,
        bytes: &module.image,
    };
    let fresh = |seeds: &HashMap<Reg, u128>, r: Reg, idx: u64| {
        let mut st = State::new(body[0].regs.len());
        for (s, v) in seeds {
            st.regs[s.index()] = *v;
        }
        st.regs[r.index()] = u128::from(idx);
        st
    };
    let distinct = |t: &[u64]| t.iter().collect::<BTreeSet<_>>().len();
    let mut best: Option<Vec<u64>> = None;
    'cand: for r in read {
        let mut check = vec![branch.clone()];
        let mut back = branch.pc;
        let mut skipped = 0;
        while check.len() <= CMP_BACK && skipped <= SKIP_BACK {
            let Some(prev) = back.checked_sub(granule) else {
                break;
            };
            let Some(b) = block_at(prev) else {
                break;
            };
            if !falls_into(&b, back) || writes(&b, r) {
                break;
            }
            let needed = reads(&check);
            let feeds = b
                .insts
                .iter()
                .any(|i| matches!(i.op, Op::Set(x, _) if needed.contains(&x)));
            if feeds {
                if has_memory(&b) || b.insts.iter().any(|i| matches!(i.op, Op::Load { .. })) {
                    break;
                }
                check.insert(0, b);
            } else {
                skipped += 1;
            }
            back = prev;
        }
        // A check that never reads the candidate decides the same way for
        // every index: it leaves at index 0 (no targets) or never (more
        // entries than the largest table). Either way the candidate fails,
        // so skip the 4097 runs.
        if !check
            .iter()
            .any(|b| b.insts.iter().any(|i| matches!(i.op, Op::Get(x) if x == r)))
        {
            continue;
        }
        let check_seeds = known(check[0].pc);
        let mut targets = Vec::new();
        for idx in 0..=MAX_ENTRIES {
            let mut st = fresh(&check_seeds, r, idx);
            let Some(after) = run_seq(&check, &mut st, &mut mem) else {
                continue 'cand;
            };
            if after != start {
                break;
            }
            let mut st = fresh(&seeds, r, idx);
            let Some(t) = run_seq(&body, &mut st, &mut mem) else {
                continue 'cand;
            };
            if !module.is_code(t) || t % granule != 0 {
                continue 'cand;
            }
            targets.push(t);
        }
        if targets.is_empty() || targets.len() as u64 > MAX_ENTRIES {
            continue;
        }
        if best
            .as_ref()
            .is_none_or(|b| distinct(&targets) > distinct(b))
        {
            best = Some(targets);
        }
    }
    best
}

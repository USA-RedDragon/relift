//! Jump tables: the targets of a computed jump found by running the code
//! before it in the IR interpreter, once per index value.
//!
//! The shape is a bounds check (a comparison and a conditional branch that
//! falls through or branches to the body), then straight-line code that loads from the table and computes the target, and
//! the jump. Walking back from the jump over instructions that fall through
//! into each other finds the conditional branch; the code between them is
//! the body. Each register the body reads before writing it, and that the
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

use relift_guest::Module;
use relift_ir::interp::{self, Fault, GuestMemory, NoDevices, NoIntrinsics, State};
use relift_ir::{Block, Flow, Next, Op, Reg};

/// The most instructions between the conditional branch and the jump.
const RUN_BACK: usize = 16;
/// The most instructions before the conditional branch in the check.
const CMP_BACK: usize = 4;
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

/// Runs `blocks` (lifted, so verified when built) in order from `state`; the address after the last one, or
/// `None` when the path leaves the sequence or an instruction faults.
fn run(blocks: &[Arc<Block>], state: &mut State, mem: &mut Image<'_>) -> Option<u64> {
    let mut next = blocks.first()?.pc;
    for b in blocks {
        if b.pc != next {
            return None;
        }
        next = interp::run_verified(b, state, mem, &mut NoIntrinsics, &mut NoDevices)
            .ok()?
            .next;
    }
    Some(next)
}

/// The targets of the computed jump `jump` at `pc`, or `None` when it is not
/// a jump table this recognizes. `block_at` lifts the instruction at an
/// address; `entering` gives the conditional branch of the function that
/// jumps to an address; `known` gives the registers known to be constant at
/// an address.
pub(crate) fn resolve(
    module: &Module,
    pc: u64,
    jump: &Block,
    granule: u64,
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
                if b.insts.iter().any(|i| matches!(i.op, Op::Store { .. })) {
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
        while check.len() <= CMP_BACK {
            let Some(prev) = back.checked_sub(granule) else {
                break;
            };
            let Some(b) = block_at(prev) else {
                break;
            };
            if !falls_into(&b, back) || writes(&b, r) || has_memory(&b) {
                break;
            }
            if b.insts.iter().any(|i| matches!(i.op, Op::Load { .. })) {
                break;
            }
            check.insert(0, b);
            back = prev;
        }
        let check_seeds = known(check[0].pc);
        let mut targets = Vec::new();
        for idx in 0..=MAX_ENTRIES {
            let mut st = fresh(&check_seeds, r, idx);
            let Some(after) = run(&check, &mut st, &mut mem) else {
                continue 'cand;
            };
            if after != start {
                break;
            }
            let mut st = fresh(&seeds, r, idx);
            let Some(t) = run(&body, &mut st, &mut mem) else {
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

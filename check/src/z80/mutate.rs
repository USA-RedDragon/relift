//! The negative control: a deliberately broken lifted block, which the
//! SingleStepTests run must catch.

use relift_ir::{Block, Op};

/// How a block is broken.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mutation {
    /// A constant changes by one bit: the one a register is last set to,
    /// else the first.
    FlipConst,
    /// The last register write is dropped.
    DropSet,
}

impl Mutation {
    pub fn parse(s: &str) -> Option<Mutation> {
        match s {
            "flip-const" => Some(Mutation::FlipConst),
            "drop-set" => Some(Mutation::DropSet),
            _ => None,
        }
    }

    /// Breaks `block`; false when it has nothing to break.
    pub fn apply(self, block: &mut Block) -> bool {
        let last_set = block
            .insts
            .iter()
            .rposition(|i| matches!(i.op, Op::Set(..)));
        match self {
            Mutation::DropSet => {
                let Some(i) = last_set else { return false };
                block.insts[i].op = Op::Sync;
                true
            }
            Mutation::FlipConst => {
                let set_to_const = block.insts.iter().rev().find_map(|i| match i.op {
                    Op::Set(_, v) if matches!(block.insts[v.0 as usize].op, Op::Const(_)) => {
                        Some(v.0 as usize)
                    }
                    _ => None,
                });
                let Some(i) = set_to_const.or_else(|| {
                    block
                        .insts
                        .iter()
                        .position(|i| matches!(i.op, Op::Const(_)))
                }) else {
                    return false;
                };
                if let Op::Const(k) = block.insts[i].op {
                    block.insts[i].op = Op::konst(k.get() ^ 1);
                }
                true
            }
        }
    }
}

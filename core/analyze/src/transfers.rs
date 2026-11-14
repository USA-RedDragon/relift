//! Computed transfers that analysis did not resolve: indirect calls, and
//! computed jumps that are not a recognized jump table. The code runs them
//! through the dispatcher, so a target that was never discovered is a miss
//! at run time.
//!
//! Each transfer also says where its target came from, as far as a walk back
//! along the function's straight-line code can tell: from the instruction
//! that computes the target, through register copies, to the last write of
//! the register on the one path that reaches it.

use std::collections::BTreeMap;

use relift_ir::{Block, Flow, Next, Op, Reg, Value};

use crate::{Function, Insn};

/// Whether a computed transfer is a call or a jump.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum TransferKind {
    /// An indirect call ([`Flow::Call`] to a computed address).
    Call,
    /// A computed jump ([`Flow::Jump`] to a computed address) with no
    /// resolved jump table.
    Jump,
}

/// Where the target of a computed transfer came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Source {
    /// A load from memory (a vtable, a struct field, a loader-filled slot).
    Loaded,
    /// A register as the function was entered (an argument).
    Argument,
    /// A register a call wrote, which the calling convention does not keep
    /// (a call's result).
    Returned,
    /// Arithmetic or another operation on values (a jump-table shape, an
    /// address built from parts).
    Computed,
    /// Paths that merge, or a walk too long to follow.
    Unknown,
}

/// One computed transfer that analysis did not resolve.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Transfer {
    /// The address of the transfer instruction.
    pub pc: u64,
    /// Call or jump.
    pub kind: TransferKind,
    /// Where the target came from.
    pub source: Source,
}

/// The most blocks the walk back from one transfer visits.
const WALK: usize = 64;

/// The computed transfers of `f` that analysis did not resolve, by address:
/// every lifted instruction whose exit goes to a [`Next::Dynamic`] address
/// as a [`Flow::Call`], or as a [`Flow::Jump`] with no entry in
/// [`Function::tables`]. Returns ([`Flow::Return`]) and traps are not
/// transfers. `preserved` are the registers a call keeps
/// ([`relift_guest::Frontend::preserved`]).
pub fn unresolved_transfers(f: &Function, preserved: &[Reg]) -> Vec<Transfer> {
    let mut out = Vec::new();
    let mut preds: Option<Preds> = None;
    for (&pc, i) in &f.insts {
        let Insn::Lifted(b) = i else {
            continue;
        };
        let Next::Dynamic(v) = b.exit.next else {
            continue;
        };
        let kind = match b.exit.flow {
            Flow::Call { .. } => TransferKind::Call,
            Flow::Jump if !f.tables.contains_key(&pc) => TransferKind::Jump,
            _ => continue,
        };
        let preds = preds.get_or_insert_with(|| predecessors(f));
        let source = source(f, preds, preserved, pc, b, v);
        out.push(Transfer { pc, kind, source });
    }
    out
}

/// Each address's predecessors in `f`, with whether the edge is a call's
/// return.
type Preds = BTreeMap<u64, Vec<(u64, bool)>>;

fn predecessors(f: &Function) -> Preds {
    let mut out: Preds = BTreeMap::new();
    for (&pc, i) in &f.insts {
        let Insn::Lifted(b) = i else {
            continue;
        };
        let mut add = |t: u64, call: bool| out.entry(t).or_default().push((pc, call));
        match (b.exit.flow, b.exit.next) {
            (Flow::Return, _) => {}
            (Flow::Call { ret }, _) => add(ret, true),
            (_, Next::Const(t)) => add(t, false),
            (
                _,
                Next::Select {
                    taken, not_taken, ..
                },
            ) => {
                add(taken, false);
                add(not_taken, false);
            }
            (_, Next::Dynamic(_)) => {
                for &t in f.tables.get(&pc).into_iter().flatten() {
                    add(t, false);
                }
            }
        }
    }
    out
}

/// What computes value `v` of block `b`.
enum Def {
    Load,
    Reg(Reg),
    Other,
}

fn def(b: &Block, v: Value) -> Def {
    match b.insts.get(v.index()).map(|i| &i.op) {
        Some(Op::Load { .. } | Op::LoadExclusive { .. }) => Def::Load,
        Some(Op::Get(r)) => Def::Reg(*r),
        Some(Op::Ext(_, x)) => def(b, *x),
        _ => Def::Other,
    }
}

/// The last value block `b` writes to `r`.
fn written(b: &Block, r: Reg) -> Option<Value> {
    b.insts.iter().rev().find_map(|i| match i.op {
        Op::Set(s, v) if s == r => Some(v),
        _ => None,
    })
}

fn source(f: &Function, preds: &Preds, preserved: &[Reg], pc: u64, b: &Block, v: Value) -> Source {
    let mut r = match def(b, v) {
        Def::Load => return Source::Loaded,
        Def::Other => return Source::Computed,
        Def::Reg(r) => r,
    };
    let mut at = pc;
    for _ in 0..WALK {
        let from = preds.get(&at).map_or(&[][..], Vec::as_slice);
        let &[(p, call)] = from else {
            return if from.is_empty() && at == f.entry {
                Source::Argument
            } else {
                Source::Unknown
            };
        };
        let Some(Insn::Lifted(pb)) = f.insts.get(&p) else {
            return Source::Unknown;
        };
        if let Some(w) = written(pb, r) {
            r = match def(pb, w) {
                Def::Load => return Source::Loaded,
                Def::Other => return Source::Computed,
                Def::Reg(s) => s,
            };
        } else if call && !preserved.contains(&r) {
            return Source::Returned;
        }
        at = p;
    }
    Source::Unknown
}

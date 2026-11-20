//! Whether a program only moves its stack pointer relative to itself.
//!
//! The host stack of a program in direct-return mode mirrors the guest
//! stack: a guest call is a host call and a guest return is a host return
//! (`docs/abi.md` section 5.4). That holds as long as the guest never
//! leaves its stack for another one. A guest that does (a coroutine switch,
//! a goroutine switch, a stack copy, `longjmp` across stacks) needs
//! trampoline mode (section 5.5), where no host frame stands for a guest
//! frame.
//!
//! This analysis proves the safe case. Every write of the stack pointer in
//! every function must be the function's entry stack pointer plus a
//! constant, a value derived from it with only constant steps (a loop that
//! probes the stack page by page), the entry value aligned down by a mask,
//! or a register that holds such a value: the restore of `mov sp, x29` after
//! a frame pointer was taken. A write of anything else (a value loaded from
//! memory, an argument, a sum with an unknown amount) is not proven, and
//! the program takes trampoline mode.
//!
//! Wide intermediate values of the CPU's add with carry pass through as long
//! as they come from and return to 64 bits.
//!
//! A forward dataflow per function tracks, for each register, whether it
//! holds the stack pointer at the function's entry plus a constant (known
//! or not), a constant, or something else. A call leaves the registers the
//! frontend names [`Frontend::preserved`] as they were and clobbers the
//! others. Memory is never read: a value restored from the stack is
//! unknown, so an epilogue that reloads the stack pointer from a slot is
//! not proven.

use std::collections::{BTreeMap, BTreeSet};

use relift_guest::Frontend;
use relift_ir::{BinOp, Block, Flow, Next, Op, Value};

use crate::{Analysis, Function, Insn};

/// A stack pointer write the analysis could not prove relative.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Unproven {
    /// The entry of the function.
    pub function: u64,
    /// The address of the instruction that writes the stack pointer.
    pub at: u64,
}

/// What the analysis found in a module.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StackWrites {
    /// How many writes of the stack pointer it checked.
    pub writes: usize,
    /// The writes it could not prove, in address order.
    pub unproven: Vec<Unproven>,
}

impl StackWrites {
    /// Whether every write of the stack pointer is relative to itself.
    pub fn proven(&self) -> bool {
        self.unproven.is_empty()
    }
}

/// An abstract register value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum V {
    /// The function's entry stack pointer plus this offset, or plus an
    /// unknown multiple of constant steps.
    Sp(Option<i64>),
    /// A constant.
    Const(i64),
    Top,
}

impl V {
    fn join(self, o: V) -> V {
        match (self, o) {
            (a, b) if a == b => a,
            (V::Sp(_), V::Sp(_)) => V::Sp(None),
            _ => V::Top,
        }
    }
}

/// Whether `m` clears a run of low bits and keeps the rest.
fn aligns(m: i64) -> bool {
    let m = m as u64;
    m != 0 && m.leading_ones() + m.trailing_zeros() == 64
}

fn step(op: BinOp, x: V, y: V) -> V {
    match (op, x, y) {
        (BinOp::Add, V::Sp(k), V::Const(c)) | (BinOp::Add, V::Const(c), V::Sp(k)) => {
            V::Sp(k.map(|k| k.wrapping_add(c)))
        }
        (BinOp::Sub, V::Sp(k), V::Const(c)) => V::Sp(k.map(|k| k.wrapping_sub(c))),
        (BinOp::Add, V::Const(a), V::Const(b)) => V::Const(a.wrapping_add(b)),
        (BinOp::Sub, V::Const(a), V::Const(b)) => V::Const(a.wrapping_sub(b)),
        (BinOp::And, V::Sp(_), V::Const(m)) | (BinOp::And, V::Const(m), V::Sp(_)) if aligns(m) => {
            V::Sp(None)
        }
        (BinOp::And, V::Const(a), V::Const(b)) => V::Const(a & b),
        _ => V::Top,
    }
}

/// The state after `b` from `entry`, and whether it writes the stack
/// pointer with anything but a value derived from the entry one.
fn state_after(b: &Block, entry: &[V], sp: Option<usize>) -> (Vec<V>, bool) {
    let b = &relift_ir::simplify(b);
    let mut vals: Vec<V> = Vec::with_capacity(b.insts.len());
    let mut sets: Vec<(usize, V)> = Vec::new();
    let mut bad = false;
    let bits = |v: Value| {
        b.insts
            .get(v.index())
            .and_then(|i| i.width)
            .map(relift_ir::Width::bits)
    };
    for inst in &b.insts {
        let get = |v: Value| vals.get(v.index()).copied().unwrap_or(V::Top);
        let v = match &inst.op {
            Op::Const(c) => V::Const(c.get() as u64 as i64),
            Op::Get(r) => entry.get(usize::from(r.0)).copied().unwrap_or(V::Top),
            Op::Set(r, v) => {
                let v = get(*v);
                if sp == Some(usize::from(r.0)) && !matches!(v, V::Sp(_)) {
                    bad = true;
                }
                sets.push((usize::from(r.0), v));
                V::Top
            }
            Op::Bin(op, x, y) => step(*op, get(*x), get(*y)),
            Op::Ext(relift_ir::ExtKind::Zero, x) if bits(*x) == Some(64) => get(*x),
            Op::Extract(x, 0) if inst.width.map(relift_ir::Width::bits) == Some(64) => get(*x),
            _ => V::Top,
        };
        vals.push(v);
    }
    let mut out = entry.to_vec();
    for (r, v) in sets {
        if r < out.len() {
            out[r] = v;
        }
    }
    (out, bad)
}

fn function(fe: &dyn Frontend, f: &Function) -> (usize, Vec<u64>) {
    let regs = fe.regs().len();
    let sp = fe.stack_pointer().map(|r| usize::from(r.0));
    let preserved: BTreeSet<usize> = fe.preserved().iter().map(|r| usize::from(r.0)).collect();
    let mut start = vec![V::Top; regs];
    if let Some(s) = sp.filter(|&s| s < regs) {
        start[s] = V::Sp(Some(0));
    }
    let mut states: BTreeMap<u64, Vec<V>> = BTreeMap::new();
    states.insert(f.entry, start);
    let mut work = vec![f.entry];
    let (mut writes, mut bad) = (0, BTreeSet::new());
    let mut seen = BTreeSet::new();
    while let Some(pc) = work.pop() {
        let Some(Insn::Lifted(b)) = f.insts.get(&pc) else {
            continue;
        };
        let entry = states[&pc].clone();
        let (mut out, unproven) = state_after(b, &entry, sp);
        let sets_sp = sp.is_some_and(|s| {
            b.insts
                .iter()
                .any(|i| matches!(i.op, Op::Set(r, _) if usize::from(r.0) == s))
        });
        if sets_sp && seen.insert(pc) {
            writes += 1;
        }
        if unproven {
            bad.insert(pc);
        }
        let mut next: Vec<u64> = Vec::new();
        match (b.exit.flow, b.exit.next) {
            (Flow::Call { ret }, _) | (Flow::Trap(_), Next::Const(ret)) => {
                for (r, v) in out.iter_mut().enumerate() {
                    if !preserved.contains(&r) {
                        *v = V::Top;
                    }
                }
                next.push(ret);
            }
            (Flow::Return | Flow::Trap(_), _) => {}
            (Flow::Jump, Next::Const(t)) => next.push(t),
            (
                Flow::Jump,
                Next::Select {
                    taken, not_taken, ..
                },
            ) => next.extend([taken, not_taken]),
            (Flow::Jump, Next::Dynamic(_)) => {
                next.extend(f.tables.get(&pc).into_iter().flatten().copied());
            }
        }
        for t in next {
            if !f.insts.contains_key(&t) {
                continue;
            }
            let merged = match states.get(&t) {
                None => out.clone(),
                Some(old) => old.iter().zip(&out).map(|(a, b)| a.join(*b)).collect(),
            };
            if states.get(&t) != Some(&merged) {
                states.insert(t, merged);
                work.push(t);
            }
        }
    }
    (writes, bad.into_iter().collect())
}

/// The writes of the stack pointer in the functions of `a`, and the ones
/// that are not relative to the entry value.
pub fn stack_writes(fe: &dyn Frontend, a: &Analysis) -> StackWrites {
    let mut out = StackWrites::default();
    if fe.stack_pointer().is_none() {
        return out;
    }
    for f in a.functions.values() {
        let (writes, bad) = function(fe, f);
        out.writes += writes;
        out.unproven.extend(bad.into_iter().map(|at| Unproven {
            function: f.entry,
            at,
        }));
    }
    out.unproven.sort_by_key(|u| u.at);
    out.unproven.dedup();
    out
}

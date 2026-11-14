//! Code addresses the code computes: a forward constant propagation over
//! each straight-line run of a function, in address order.
//!
//! A run restarts (forgetting every register) at a branch target and after a
//! call or a trap. A value counts as a code address when a register write, a
//! store or a computed jump carries a known constant that lands in code and
//! is granule aligned, except the return address a call writes.
//! Page-aligned values are skipped: a page base on its
//! own (the first half of a PC-relative pair) is not a target, and real
//! functions rarely start on a page boundary.

use std::collections::{BTreeMap, BTreeSet, HashMap};

use relift_guest::Module;
use relift_ir::{BinOp, Block, ExtKind, Flow, Next, Op, Reg, Value};

use crate::Insn;

/// The value of `v` given the known values of earlier ones.
fn eval(b: &Block, vals: &[Option<u128>], known: &HashMap<Reg, u128>, i: usize) -> Option<u128> {
    let inst = &b.insts[i];
    let w = inst.width?;
    let get = |v: Value| vals.get(v.index()).copied().flatten();
    let wv = |v: Value| b.width_of(v).map(|x| x.bits()).unwrap_or(0);
    let r = match &inst.op {
        Op::Const(c) => c.get(),
        Op::Get(r) => *known.get(r)?,
        Op::Bin(op, x, y) => {
            let (x, y) = (get(*x)?, get(*y)?);
            match op {
                BinOp::Add => x.wrapping_add(y),
                BinOp::Sub => x.wrapping_sub(y),
                BinOp::And => x & y,
                BinOp::Or => x | y,
                BinOp::Xor => x ^ y,
                BinOp::Shl => x.checked_shl(u32::try_from(y).ok()?).unwrap_or(0),
                BinOp::LShr => x.checked_shr(u32::try_from(y).ok()?).unwrap_or(0),
                _ => return None,
            }
        }
        Op::Ext(ExtKind::Zero, x) => get(*x)?,
        Op::Ext(ExtKind::Sign, x) => {
            let (v, from) = (get(*x)?, wv(*x));
            if from == 0 || from >= 128 {
                v
            } else {
                (((v << (128 - from)) as i128) >> (128 - from)) as u128
            }
        }
        Op::Extract(x, lo) => get(*x)? >> lo,
        Op::Concat(h, l) => (get(*h)? << wv(*l)) | get(*l)?,
        Op::Select(c, x, y) => {
            if get(*c)? != 0 {
                get(*x)?
            } else {
                get(*y)?
            }
        }
        _ => return None,
    };
    Some(r & w.mask())
}

/// Code addresses computed in `insts`.
pub(crate) fn code_addresses(
    module: &Module,
    granule: u64,
    insts: &BTreeMap<u64, Insn>,
) -> BTreeSet<u64> {
    let mut targets = BTreeSet::new();
    for (&pc, i) in insts {
        if let Insn::Lifted(b) = i {
            let end = pc + u64::from(b.len);
            match b.exit.next {
                Next::Const(t) if t != end => {
                    targets.insert(t);
                }
                Next::Select { taken, .. } => {
                    targets.insert(taken);
                }
                _ => {}
            }
        }
    }
    let is_addr = |v: u128| {
        u64::try_from(v)
            .ok()
            .filter(|a| module.is_code(*a) && a % granule == 0 && a % 0x1000 != 0)
    };
    let mut out = BTreeSet::new();
    let mut known: HashMap<Reg, u128> = HashMap::new();
    let mut prev_end: Option<u64> = None;
    for (&pc, i) in insts {
        if prev_end != Some(pc) || targets.contains(&pc) {
            known.clear();
        }
        let Insn::Lifted(b) = i else {
            prev_end = None;
            continue;
        };
        let ret = match b.exit.flow {
            Flow::Call { ret } => Some(ret),
            _ => None,
        };
        let mut vals = Vec::with_capacity(b.insts.len());
        let mut sets = Vec::new();
        for (n, inst) in b.insts.iter().enumerate() {
            let v = eval(b, &vals, &known, n);
            vals.push(v);
            let get = |x: &Value| vals.get(x.index()).copied().flatten();
            match &inst.op {
                Op::Set(r, x) => {
                    let k = get(x);
                    if let Some(a) = k.and_then(is_addr)
                        && Some(a) != ret
                    {
                        out.insert(a);
                    }
                    sets.push((*r, k));
                }
                Op::Store { value, .. } => {
                    if let Some(a) = get(value).and_then(is_addr) {
                        out.insert(a);
                    }
                }
                _ => {}
            }
        }
        if let Next::Dynamic(v) = b.exit.next
            && let Some(a) = vals[v.index()].and_then(is_addr)
        {
            out.insert(a);
        }
        for (r, k) in sets {
            match k {
                Some(k) => {
                    known.insert(r, k);
                }
                None => {
                    known.remove(&r);
                }
            }
        }
        if matches!(b.exit.flow, Flow::Call { .. } | Flow::Trap(_)) {
            known.clear();
        }
        let end = pc + u64::from(b.len);
        let falls = match (b.exit.flow, b.exit.next) {
            (Flow::Return, _) => false,
            (Flow::Call { ret }, _) => ret == end,
            (_, Next::Const(t)) => t == end,
            (_, Next::Select { not_taken, .. }) => not_taken == end,
            (_, Next::Dynamic(_)) => false,
        };
        prev_end = falls.then_some(end);
    }
    out
}

/// What a forward constant propagation knows about a register.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Fact {
    Const(u128),
    Unknown,
}

/// Meets `from` into `into` (a missing register is unknown); whether
/// `into` changed.
fn meet(into: &mut HashMap<Reg, Fact>, from: &HashMap<Reg, Fact>) -> bool {
    let mut changed = false;
    for (r, g) in into.iter_mut() {
        if *g != Fact::Unknown && from.get(r) != Some(g) {
            *g = Fact::Unknown;
            changed = true;
        }
    }
    changed
}

/// The registers known to hold one constant when each instruction of the
/// function at `entry` starts, by forward propagation over its control flow.
/// Nothing is known at the entry; a call or trap keeps only the `preserved`
/// registers.
pub(crate) fn in_states(
    insts: &BTreeMap<u64, Insn>,
    entry: u64,
    preserved: &[Reg],
) -> HashMap<u64, HashMap<Reg, u128>> {
    let mut ins: HashMap<u64, HashMap<Reg, Fact>> = HashMap::new();
    ins.insert(entry, HashMap::new());
    let mut work = vec![entry];
    let mut steps = 0usize;
    while let Some(pc) = work.pop() {
        steps += 1;
        if steps > 200_000 {
            break;
        }
        let Some(Insn::Lifted(b)) = insts.get(&pc) else {
            continue;
        };
        let state = ins.get(&pc).cloned().unwrap_or_default();
        let known: HashMap<Reg, u128> = state
            .iter()
            .filter_map(|(r, f)| match f {
                Fact::Const(c) => Some((*r, *c)),
                Fact::Unknown => None,
            })
            .collect();
        let mut vals = Vec::with_capacity(b.insts.len());
        let mut out = state.clone();
        for n in 0..b.insts.len() {
            let v = eval(b, &vals, &known, n);
            vals.push(v);
        }
        for inst in &b.insts {
            if let Op::Set(r, x) = &inst.op {
                let f = vals[x.index()].map_or(Fact::Unknown, Fact::Const);
                out.insert(*r, f);
            }
        }
        let mut succ = Vec::new();
        let kill = matches!(b.exit.flow, Flow::Call { .. } | Flow::Trap(_));
        if kill {
            for (r, f) in out.iter_mut() {
                if !preserved.contains(r) {
                    *f = Fact::Unknown;
                }
            }
        }
        match (b.exit.flow, b.exit.next) {
            (Flow::Return, _) => {}
            (Flow::Call { ret }, _) => succ.push(ret),
            (_, Next::Const(t)) => succ.push(t),
            (
                _,
                Next::Select {
                    taken, not_taken, ..
                },
            ) => {
                succ.push(taken);
                succ.push(not_taken);
            }
            (_, Next::Dynamic(_)) => {}
        }
        for s in succ {
            if !insts.contains_key(&s) {
                continue;
            }
            match ins.get_mut(&s) {
                None => {
                    ins.insert(s, out.clone());
                    work.push(s);
                }
                Some(e) => {
                    if meet(e, &out) {
                        work.push(s);
                    }
                }
            }
        }
    }
    ins.into_iter()
        .map(|(pc, m)| {
            (
                pc,
                m.into_iter()
                    .filter_map(|(r, f)| match f {
                        Fact::Const(c) => Some((r, c)),
                        Fact::Unknown => None,
                    })
                    .collect(),
            )
        })
        .collect()
}

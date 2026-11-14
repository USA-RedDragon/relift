//! Computed transfers through loader-filled slots: a call or jump whose
//! target the code loads from a word the guest's loader writes (an ELF GOT
//! entry), so the target is whatever the loader puts there.
//!
//! A forward propagation over each function tracks registers that hold a
//! known constant or the value loaded from a slot. A computed call or jump
//! to a slot's value is a transfer through that slot. Any other use of the
//! value or of the slot's address lets it reach code the analysis cannot
//! follow, so the slot escapes: a store, arithmetic, a load through it, a
//! register that holds it at a call, trap, return or other transfer, or a
//! merge of paths that disagree about it. A compare of the value is not a
//! use, and on the edge where a compare with zero holds the register is
//! known to be zero (a null check before an optional call).

use std::collections::{BTreeMap, BTreeSet, HashMap};

use relift_ir::{Block, CmpOp, Flow, Next, Op, Reg, Value};

use crate::{Function, Insn};

/// What [`slot_uses`] found in one function.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SlotUses {
    /// The slot each computed transfer goes through, by the transfer's
    /// address.
    pub transfers: BTreeMap<u64, u64>,
    /// The slots whose value or address escapes.
    pub escaped: BTreeSet<u64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Fact {
    Const(u128),
    Slot(u64),
}

/// The most propagation steps per function before giving up (every slot
/// the function may reference then escapes).
const STEPS: usize = 200_000;

/// The uses of the slots `slots` (each `size` bytes) in `f`.
pub fn slot_uses(f: &Function, slots: &BTreeSet<u64>, size: u64) -> SlotUses {
    let mut out = SlotUses::default();
    let pages: BTreeSet<u64> = slots.iter().map(|s| s & !0xfff).collect();
    let slot_at = |a: u128| -> Option<u64> {
        let a = u64::try_from(a).ok()?;
        let &s = slots.range(..=a).next_back()?;
        (a < s + size).then_some(s)
    };
    // Only a function that names a slot's page or a slot can use one.
    let mut named: BTreeSet<u64> = BTreeSet::new();
    for i in f.insts.values() {
        if let Insn::Lifted(b) = i {
            for inst in &b.insts {
                if let Op::Const(c) = inst.op
                    && let Ok(c) = u64::try_from(c.get())
                    && pages.contains(&(c & !0xfff))
                {
                    named.extend(slots.range(c & !0xfff..(c & !0xfff) + 0x1000));
                }
            }
        }
    }
    if named.is_empty() {
        return out;
    }
    let mut ins: HashMap<u64, HashMap<Reg, Fact>> = HashMap::new();
    ins.insert(f.entry, HashMap::new());
    let mut work = vec![f.entry];
    let mut steps = 0usize;
    while let Some(pc) = work.pop() {
        steps += 1;
        if steps > STEPS {
            out.escaped.extend(named);
            out.transfers.clear();
            return out;
        }
        let Some(Insn::Lifted(b)) = f.insts.get(&pc) else {
            continue;
        };
        let state = ins.get(&pc).cloned().unwrap_or_default();
        let inside = |a: u64| f.insts.contains_key(&a);
        for (s, edge) in step(b, &inside, &state, &slot_at, &mut out) {
            if !f.insts.contains_key(&s) {
                continue;
            }
            match ins.get_mut(&s) {
                None => {
                    ins.insert(s, edge);
                    work.push(s);
                }
                Some(e) => {
                    if meet(e, &edge, &mut out.escaped) {
                        work.push(s);
                    }
                }
            }
        }
    }
    out
}

/// Meets `from` into `into`; whether `into` changed.
fn meet(
    into: &mut HashMap<Reg, Fact>,
    from: &HashMap<Reg, Fact>,
    escaped: &mut BTreeSet<u64>,
) -> bool {
    let mut gone = Vec::new();
    for (r, g) in into.iter() {
        if from.get(r) != Some(g) {
            gone.push(*r);
        }
    }
    for (r, g) in from {
        if into.get(r) != Some(g)
            && let Fact::Slot(s) = g
        {
            escaped.insert(*s);
        }
    }
    for r in &gone {
        if let Some(Fact::Slot(s)) = into.remove(r) {
            escaped.insert(s);
        }
    }
    !gone.is_empty()
}

/// Runs block `b` from `state`; gives each successor with its state.
fn step(
    b: &Block,
    inside: &dyn Fn(u64) -> bool,
    state: &HashMap<Reg, Fact>,
    slot_at: &dyn Fn(u128) -> Option<u64>,
    out: &mut SlotUses,
) -> Vec<(u64, HashMap<Reg, Fact>)> {
    let n = b.insts.len();
    let mut vals: Vec<Option<Fact>> = Vec::with_capacity(n);
    let mut from: Vec<Option<Reg>> = vec![None; n];
    let mut post = state.clone();
    let get = |vals: &[Option<Fact>], v: Value| vals.get(v.index()).copied().flatten();
    let konst = |vals: &[Option<Fact>], v: Value| match get(vals, v) {
        Some(Fact::Const(c)) => Some(c),
        _ => None,
    };
    let exit_value = match b.exit.next {
        Next::Dynamic(v) => Some(v),
        _ => None,
    };
    for (i, inst) in b.insts.iter().enumerate() {
        let w = inst.width.map(|w| w.mask());
        let fact = match &inst.op {
            Op::Const(c) => Some(Fact::Const(c.get())),
            Op::Get(r) => {
                from[i] = Some(*r);
                state.get(r).copied()
            }
            Op::Bin(op, x, y) => match (konst(&vals, *x), konst(&vals, *y)) {
                (Some(x), Some(y)) => {
                    use relift_ir::BinOp;
                    let r = match op {
                        BinOp::Add => Some(x.wrapping_add(y)),
                        BinOp::Sub => Some(x.wrapping_sub(y)),
                        BinOp::And => Some(x & y),
                        BinOp::Or => Some(x | y),
                        BinOp::Xor => Some(x ^ y),
                        _ => None,
                    };
                    r.map(|r| Fact::Const(r & w.unwrap_or(u128::MAX)))
                }
                _ => None,
            },
            Op::Ext(_, x) => konst(&vals, *x).map(Fact::Const),
            Op::Load { addr, .. } => konst(&vals, *addr).and_then(slot_at).map(Fact::Slot),
            _ => None,
        };
        // Uses: slot values and slot addresses anywhere but the allowed
        // places escape.
        for (k, v) in inst.op.operands().into_iter().enumerate() {
            let allowed = match &inst.op {
                Op::Set(..) | Op::Cmp(..) => true,
                Op::Load { .. } => k == 0 && !matches!(get(&vals, v), Some(Fact::Slot(_))),
                Op::Bin(..) | Op::Ext(..) => !matches!(get(&vals, v), Some(Fact::Slot(_))),
                _ => false,
            };
            if allowed {
                continue;
            }
            match get(&vals, v) {
                Some(Fact::Slot(s)) => {
                    out.escaped.insert(s);
                }
                Some(Fact::Const(c)) => {
                    if let Some(s) = slot_at(c) {
                        out.escaped.insert(s);
                    }
                }
                None => {}
            }
        }
        if let Op::Set(r, v) = &inst.op {
            match get(&vals, *v) {
                Some(f) => {
                    if let Fact::Const(c) = f
                        && let Some(s) = slot_at(c)
                    {
                        // A slot's address in a register.
                        out.escaped.insert(s);
                    }
                    post.insert(*r, f);
                }
                None => {
                    post.remove(r);
                }
            }
        }
        vals.push(fact);
    }
    // At any transfer that leaves the function or calls, a register that
    // still holds a slot value escapes, except a transfer's own target.
    let target = exit_value.and_then(|v| match get(&vals, v) {
        Some(Fact::Slot(s)) => Some(s),
        _ => None,
    });
    let leaves = match (b.exit.flow, b.exit.next) {
        (Flow::Call { .. } | Flow::Return | Flow::Trap(_), _) | (_, Next::Dynamic(_)) => true,
        (_, Next::Const(t)) => !inside(t),
        (
            _,
            Next::Select {
                taken, not_taken, ..
            },
        ) => !inside(taken) || !inside(not_taken),
    };
    if leaves {
        for f in post.values() {
            if let Fact::Slot(s) = f
                && Some(*s) != target
            {
                out.escaped.insert(*s);
            }
        }
    }
    if let (Some(s), Some(_)) = (target, exit_value) {
        out.transfers.insert(b.pc, s);
    } else if let Some(v) = exit_value
        && let Some(Fact::Const(c)) = get(&vals, v)
        && let Some(s) = slot_at(c)
    {
        out.escaped.insert(s);
    }
    match (b.exit.flow, b.exit.next) {
        (Flow::Return, _) | (_, Next::Dynamic(_)) => Vec::new(),
        // The callee or the runtime may change any register: forget them
        // (a slot value held across was counted as escaping above).
        (Flow::Call { ret }, _) => vec![(ret, HashMap::new())],
        (Flow::Trap(_), Next::Const(t)) => vec![(t, HashMap::new())],
        (_, Next::Const(t)) => vec![(t, post)],
        (
            _,
            Next::Select {
                cond,
                taken,
                not_taken,
            },
        ) => {
            let mut t = post.clone();
            let mut nt = post.clone();
            // A compare of a register's slot value with zero: on the edge
            // where they are equal the register is zero.
            if let Some((r, eq)) = zero_test(b, &from, &vals, cond)
                && matches!(state.get(&r), Some(Fact::Slot(_)))
                && post.get(&r) == state.get(&r)
            {
                let side = if eq { &mut t } else { &mut nt };
                side.insert(r, Fact::Const(0));
            }
            vec![(taken, t), (not_taken, nt)]
        }
    }
}

/// Whether `cond` of `b` compares a register with zero, directly or through
/// compares of that one-bit result with 0 or 1 (a frontend may test a
/// condition that way); gives the register and whether `cond` holds when it
/// is zero.
fn zero_test(
    b: &Block,
    from: &[Option<Reg>],
    vals: &[Option<Fact>],
    cond: Value,
) -> Option<(Reg, bool)> {
    let inst = b.insts.get(cond.index())?;
    let Op::Cmp(op @ (CmpOp::Eq | CmpOp::Ne), x, y) = inst.op else {
        return None;
    };
    let konst = |v: Value| match vals.get(v.index()).copied().flatten() {
        Some(Fact::Const(c)) => Some(c),
        _ => None,
    };
    let (v, k) = match (konst(x), konst(y)) {
        (_, Some(k)) => (x, k),
        (Some(k), None) => (y, k),
        (None, None) => return None,
    };
    let eq = op == CmpOp::Eq;
    if k == 0
        && let Some(r) = from.get(v.index()).copied().flatten()
    {
        return Some((r, eq));
    }
    let bit = b.insts.get(v.index())?;
    if bit.width != Some(relift_ir::Width::W1) || !matches!(bit.op, Op::Cmp(..)) || k > 1 {
        return None;
    }
    let (r, z) = zero_test(b, from, vals, v)?;
    Some((r, z == (eq == (k == 1))))
}

#[cfg(test)]
mod tests {
    use relift_guest::Frontend;
    use relift_guest::toy::Toy;
    use relift_ir::{Builder, Endian, Exit, Order, Width};

    use super::*;
    use crate::EntryKind;

    const SLOT: u64 = 0x31b0;
    const R0: Reg = Reg(0);
    const R1: Reg = Reg(1);

    fn b(pc: u64) -> Builder {
        Builder::new(Toy.regs(), Width::W64, pc)
    }

    /// `r0 = [page + off]` into r0, as `adrp; ldr` do.
    fn load_slot(pc: u64) -> Block {
        let mut x = b(pc);
        let page = x.konst(Width::W64, u128::from(SLOT & !0xfff));
        let off = x.konst(Width::W64, u128::from(SLOT & 0xfff));
        let a = x.add(page, off);
        let v = x.load(a, Width::W64, Endian::Little, Order::Plain);
        x.set(R0, v);
        x.jump(4, pc + 4)
    }

    /// `cbz r0, taken`.
    fn cbz(pc: u64, taken: u64) -> Block {
        let mut x = b(pc);
        let v = x.get(R0);
        let z = x.konst(Width::W64, 0);
        let c = x.cmp(CmpOp::Eq, v, z);
        x.finish(
            4,
            Exit {
                next: Next::Select {
                    cond: c,
                    taken,
                    not_taken: pc + 4,
                },
                flow: Flow::Jump,
            },
        )
    }

    fn br(pc: u64, r: Reg, flow: Flow) -> Block {
        let mut x = b(pc);
        let v = x.get(r);
        x.finish(
            4,
            Exit {
                next: Next::Dynamic(v),
                flow,
            },
        )
    }

    fn call(pc: u64, t: u64) -> Block {
        b(pc).finish(
            4,
            Exit {
                next: Next::Const(t),
                flow: Flow::Call { ret: pc + 4 },
            },
        )
    }

    fn ret(pc: u64) -> Block {
        let mut x = b(pc);
        let v = x.get(Reg(3));
        x.finish(
            4,
            Exit {
                next: Next::Dynamic(v),
                flow: Flow::Return,
            },
        )
    }

    fn func(blocks: Vec<Block>) -> Function {
        Function {
            entry: blocks[0].pc,
            name: None,
            kind: EntryKind::Called,
            insts: blocks
                .into_iter()
                .map(|b| (b.pc, Insn::Lifted(b.into())))
                .collect(),
            tables: BTreeMap::new(),
            pads: BTreeSet::new(),
        }
    }

    fn uses(f: &Function) -> SlotUses {
        slot_uses(f, &BTreeSet::from([SLOT]), 8)
    }

    #[test]
    fn a_null_checked_jump_through_a_slot_is_a_transfer() {
        // ldr r0, slot; cbz r0, 0x10c; br r0; 0x10c: bl 0x200 (r0 is 0); ret
        let f = func(vec![
            load_slot(0x100),
            cbz(0x104, 0x10c),
            br(0x108, R0, Flow::Jump),
            call(0x10c, 0x200),
            ret(0x110),
        ]);
        let u = uses(&f);
        assert_eq!(u.transfers, BTreeMap::from([(0x108, SLOT)]));
        assert!(u.escaped.is_empty());
    }

    #[test]
    fn a_null_check_that_tests_a_compare_result_is_a_null_check() {
        // cbz as (r0 == 0) == 1 and the other ways to write it; the last
        // two hold when r0 is not zero, so their edges are swapped.
        for (op, k, inner, swap) in [
            (CmpOp::Eq, 1, CmpOp::Eq, false),
            (CmpOp::Ne, 1, CmpOp::Ne, false),
            (CmpOp::Ne, 0, CmpOp::Eq, false),
            (CmpOp::Eq, 0, CmpOp::Ne, false),
            (CmpOp::Ne, 1, CmpOp::Eq, true),
            (CmpOp::Eq, 1, CmpOp::Ne, true),
        ] {
            let mut x = b(0x104);
            let v = x.get(R0);
            let z = x.konst(Width::W64, 0);
            let c = x.cmp(inner, v, z);
            let one = x.konst(Width::W1, k);
            let c = x.cmp(op, c, one);
            let (taken, not_taken) = if swap { (0x108, 0x10c) } else { (0x10c, 0x108) };
            let test = x.finish(
                4,
                Exit {
                    next: Next::Select {
                        cond: c,
                        taken,
                        not_taken,
                    },
                    flow: Flow::Jump,
                },
            );
            let f = func(vec![
                load_slot(0x100),
                test,
                br(0x108, R0, Flow::Jump),
                call(0x10c, 0x200),
                ret(0x110),
            ]);
            let u = uses(&f);
            assert_eq!(u.transfers, BTreeMap::from([(0x108, SLOT)]), "{op:?} {k}");
            assert!(u.escaped.is_empty(), "{op:?} {k}");
        }
    }

    #[test]
    fn a_compare_of_a_slot_value_with_one_is_no_null_check() {
        let mut x = b(0x104);
        let v = x.get(R0);
        let one = x.konst(Width::W64, 1);
        let c = x.cmp(CmpOp::Eq, v, one);
        let test = x.finish(
            4,
            Exit {
                next: Next::Select {
                    cond: c,
                    taken: 0x10c,
                    not_taken: 0x108,
                },
                flow: Flow::Jump,
            },
        );
        let f = func(vec![
            load_slot(0x100),
            test,
            br(0x108, R0, Flow::Jump),
            call(0x10c, 0x200),
            ret(0x110),
        ]);
        assert_eq!(uses(&f).escaped, BTreeSet::from([SLOT]));
    }

    #[test]
    fn a_slot_value_that_reaches_other_code_escapes() {
        // Passed to a callee in a register.
        let f = func(vec![load_slot(0x100), call(0x104, 0x200), ret(0x108)]);
        assert_eq!(uses(&f).escaped, BTreeSet::from([SLOT]));
        // Stored.
        let mut x = b(0x104);
        let a = x.get(R1);
        let v = x.get(R0);
        x.store(a, v, Endian::Little, Order::Plain);
        let st = x.jump(4, 0x108);
        let f = func(vec![load_slot(0x100), st, ret(0x108)]);
        assert_eq!(uses(&f).escaped, BTreeSet::from([SLOT]));
        // The slot's address in a register.
        let mut x = b(0x100);
        let k = x.konst(Width::W64, u128::from(SLOT));
        x.set(R1, k);
        let f = func(vec![x.jump(4, 0x104), ret(0x104)]);
        assert_eq!(uses(&f).escaped, BTreeSet::from([SLOT]));
        // A call through it, with nothing else, is a transfer.
        let f = func(vec![
            load_slot(0x100),
            br(0x104, R0, Flow::Call { ret: 0x108 }),
            ret(0x108),
        ]);
        let u = uses(&f);
        assert_eq!(u.transfers, BTreeMap::from([(0x104, SLOT)]));
        assert!(u.escaped.is_empty());
    }

    #[test]
    fn a_merge_that_loses_a_slot_value_escapes() {
        // cbz r1 skips the load; the join holds a slot value on one path.
        let mut x = b(0x100);
        let v = x.get(R1);
        let z = x.konst(Width::W64, 0);
        let c = x.cmp(CmpOp::Eq, v, z);
        let first = x.finish(
            4,
            Exit {
                next: Next::Select {
                    cond: c,
                    taken: 0x108,
                    not_taken: 0x104,
                },
                flow: Flow::Jump,
            },
        );
        let f = func(vec![first, load_slot(0x104), br(0x108, R0, Flow::Jump)]);
        assert_eq!(uses(&f).escaped, BTreeSet::from([SLOT]));
    }
}

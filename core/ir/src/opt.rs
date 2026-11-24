//! Block-local optimization passes.

use std::collections::HashMap;

use crate::interp::{eval_bin, eval_cmp, eval_un};
use crate::{
    AddrClass, BinOp, Block, CmpOp, Exit, ExtKind, Inst, IoKind, IoMap, Next, Op, Order, Reg, UnOp,
    Value, Width,
};

/// Removes instructions whose values nothing observable uses.
///
/// An instruction is kept when it has an effect ([`Op::has_effect`]: `Set`,
/// every load and store including exclusive ones, `ClearExclusive`, `Fence`,
/// every intrinsic, the clock ticks `Tick` and `TickIf`, the I/O
/// accesses `IoLoad` and `IoStore`, and `Sync`) or when a kept instruction or the exit uses its
/// value, transitively. Plain loads are kept even when the loaded value is
/// unused, because a load may fault and the fault is observable. Intrinsics
/// are all treated as effectful, since the IR cannot see into them.
///
/// The surviving instructions keep their order and are renumbered densely, so
/// the result passes [`Block::verify`] whenever the input does and behaves
/// the same in the interpreter.
pub fn dce(block: &Block) -> Block {
    dce_with(block, true)
}

/// [`dce`] for a block whose loads were emitted for arms the lifter did not
/// take (a template that loads at every operand size and uses the one the
/// instruction has): a load whose value nothing uses goes too, since no
/// fault of it belongs to the instruction.
pub fn dce_unused_loads(block: &Block) -> Block {
    dce_with(block, false)
}

fn dce_with(block: &Block, loads_are_effects: bool) -> Block {
    let n = block.insts.len();
    let mut live = vec![false; n];
    let mark = |live: &mut Vec<bool>, v: Value| {
        if let Some(l) = live.get_mut(v.index()) {
            *l = true;
        }
    };
    match block.exit.next {
        Next::Const(_) => {}
        Next::Select { cond, .. } => mark(&mut live, cond),
        Next::Dynamic(v) => mark(&mut live, v),
    }
    for i in (0..n).rev() {
        let op = &block.insts[i].op;
        if op.has_effect() && (loads_are_effects || !matches!(op, Op::Load { .. })) {
            live[i] = true;
        }
        if live[i] {
            for v in op.operands() {
                mark(&mut live, v);
            }
        }
    }
    let mut map = vec![0u32; n];
    let mut insts = Vec::new();
    for (i, inst) in block.insts.iter().enumerate() {
        if live[i] {
            map[i] = insts.len() as u32;
            insts.push(Inst {
                op: remap(&inst.op, &map),
                width: inst.width,
            });
        }
    }
    let r = |v: Value| Value(map[v.index()]);
    let next = match block.exit.next {
        Next::Const(a) => Next::Const(a),
        Next::Select {
            cond,
            taken,
            not_taken,
        } => Next::Select {
            cond: r(cond),
            taken,
            not_taken,
        },
        Next::Dynamic(v) => Next::Dynamic(r(v)),
    };
    Block {
        regs: block.regs,
        addr_width: block.addr_width,
        pc: block.pc,
        len: block.len,
        insts,
        exit: Exit {
            next,
            flow: block.exit.flow,
        },
        bank: block.bank,
    }
}

/// Folds constants and drops operations that give one of their operands.
///
/// A pure integer operation whose operands are all constant
/// ([`constants`]) becomes [`Op::Const`]. An operation that gives one of
/// its operands unchanged is replaced by that operand at every use: `x + 0`,
/// `x - 0`, `x | 0`, `x ^ 0`, `x & ones`, `x * 1`, shifts and rotates by 0,
/// an extension or extract that keeps the width, a select on a constant
/// condition or with equal arms, a one-bit `x == 1` or `x != 0`, the low or
/// high part of a concat, a zero extension's low bits, and a double
/// complement. An extract reads through the concats, extracts and zero
/// extensions it falls inside of, and a one-bit select between 1 and 0 is
/// its condition or the complement. An operation that gives a constant whatever its other
/// operand is (`x & 0`, `x * 0`, `x | ones`, a shift left or logical right
/// by at least the width) becomes that constant, and a select exit whose
/// condition is constant becomes a constant exit.
///
/// Instructions with effects stay in place with their operands replaced,
/// so the result behaves the same in the interpreter as the input. The
/// replaced instructions stay too (unused); [`dce`] removes them.
pub fn simplify(block: &Block) -> Block {
    let n = block.insts.len();
    let mut alias: Vec<Value> = (0..n as u32).map(Value).collect();
    let mut known: Vec<Option<u128>> = vec![None; n];
    let mut insts: Vec<Inst> = Vec::with_capacity(n);
    for (i, inst) in block.insts.iter().enumerate() {
        let mut op = inst.op.map_values(&|v: &Value| alias[v.index()]);
        let w = inst.width.unwrap_or(Width::W1);
        let wid = |v: &Value| insts[v.index()].width.unwrap_or(Width::W1);
        let k = |v: &Value| known[v.index()];
        let is = |v: &Value, c: u128| k(v) == Some(c);
        let ones = w.mask();
        // An extract reads through the concats, extracts and zero
        // extensions it falls inside of.
        while let Op::Extract(x, lo) = op {
            let (lo32, n) = (u32::from(lo), w.bits());
            op = match insts[x.index()].op {
                Op::Concat(_, l) if lo32 + n <= wid(&l).bits() => Op::Extract(l, lo),
                Op::Concat(h, l) if lo32 >= wid(&l).bits() => {
                    Op::Extract(h, (lo32 - wid(&l).bits()) as u8)
                }
                Op::Extract(y, lo2) => Op::Extract(y, lo + lo2),
                Op::Ext(ExtKind::Zero, a) if lo32 + n <= wid(&a).bits() => Op::Extract(a, lo),
                _ => break,
            };
        }
        // A one-bit select of the two constants is its condition or the
        // condition's complement.
        if let Op::Select(c, x, y) = op
            && w == Width::W1
            && wid(&c) == Width::W1
        {
            if is(&x, 1) && is(&y, 0) {
                op = Op::Extract(c, 0);
            } else if is(&x, 0) && is(&y, 1) {
                op = Op::Un(UnOp::Not, c);
            }
        }
        let mut same: Option<Value> = None;
        let mut konst = match &op {
            Op::Const(c) => Some(c.get()),
            Op::Un(u, a) => k(a).map(|a| eval_un(*u, a, w)),
            Op::Bin(b, x, y) => k(x).zip(k(y)).map(|(a, b2)| eval_bin(*b, a, b2, w)),
            Op::Cmp(c, x, y) => k(x)
                .zip(k(y))
                .map(|(a, b2)| u128::from(eval_cmp(*c, a, b2, wid(x)))),
            Op::Ext(ExtKind::Zero, a) => k(a),
            Op::Ext(ExtKind::Sign, a) => {
                let s = 128 - wid(a).bits();
                k(a).map(|x| ((((x << s) as i128) >> s) as u128) & w.mask())
            }
            Op::Extract(a, lo) => k(a).map(|a| (a >> lo) & w.mask()),
            Op::Concat(h, l) => k(h)
                .zip(k(l))
                .map(|(h, l2)| ((h << wid(l).bits()) | l2) & w.mask()),
            _ => None,
        };
        if konst.is_none() {
            match &op {
                Op::Bin(b, x, y) => match b {
                    BinOp::Add | BinOp::Or | BinOp::Xor if is(x, 0) => same = Some(*y),
                    BinOp::Add
                    | BinOp::Sub
                    | BinOp::Or
                    | BinOp::Xor
                    | BinOp::Shl
                    | BinOp::LShr
                    | BinOp::AShr
                    | BinOp::RotR
                        if is(y, 0) =>
                    {
                        same = Some(*x);
                    }
                    BinOp::And if is(x, ones) => same = Some(*y),
                    BinOp::And if is(y, ones) => same = Some(*x),
                    BinOp::Mul if is(x, 1) => same = Some(*y),
                    BinOp::Mul if is(y, 1) => same = Some(*x),
                    BinOp::And | BinOp::Mul if is(x, 0) || is(y, 0) => konst = Some(0),
                    BinOp::Or if is(x, ones) || is(y, ones) => konst = Some(ones),
                    BinOp::Shl | BinOp::LShr if k(y).is_some_and(|a| a >= u128::from(w.bits())) => {
                        konst = Some(0);
                    }
                    _ => {}
                },
                Op::Un(UnOp::Not, x) => {
                    if let Op::Un(UnOp::Not, y) = insts[x.index()].op {
                        same = Some(y);
                    }
                }
                Op::Cmp(CmpOp::Eq, x, y) if w == Width::W1 && wid(x) == Width::W1 => {
                    if is(y, 1) {
                        same = Some(*x);
                    } else if is(x, 1) {
                        same = Some(*y);
                    }
                }
                Op::Cmp(CmpOp::Ne, x, y) if wid(x) == Width::W1 => {
                    if is(y, 0) {
                        same = Some(*x);
                    } else if is(x, 0) {
                        same = Some(*y);
                    }
                }
                Op::Select(c, x, y) => {
                    if x == y {
                        same = Some(*x);
                    } else if let Some(c) = k(c) {
                        same = Some(if c != 0 { *x } else { *y });
                    }
                }
                Op::Ext(_, x) if wid(x) == w => same = Some(*x),
                Op::Extract(x, lo) => {
                    if *lo == 0 && wid(x) == w {
                        same = Some(*x);
                    } else {
                        match insts[x.index()].op {
                            Op::Concat(_, l) if *lo == 0 && wid(&l) == w => same = Some(l),
                            Op::Concat(h, l)
                                if u32::from(*lo) == wid(&l).bits() && wid(&h) == w =>
                            {
                                same = Some(h);
                            }
                            Op::Ext(ExtKind::Zero, a) if *lo == 0 && wid(&a) == w => {
                                same = Some(a);
                            }
                            _ => {}
                        }
                    }
                }
                _ => {}
            }
        }
        if let Some(v) = same {
            alias[i] = v;
            known[i] = k(&v);
        }
        let pure = !op.has_effect() && !matches!(op, Op::Fp(..));
        let op = match konst {
            Some(c) if pure && inst.width.is_some() => {
                let c = c & ones;
                known[i] = Some(c);
                Op::konst(c)
            }
            _ => op,
        };
        insts.push(Inst {
            op,
            width: inst.width,
        });
    }
    let r = |v: Value| alias[v.index()];
    let next = match block.exit.next {
        Next::Const(a) => Next::Const(a),
        Next::Select {
            cond,
            taken,
            not_taken,
        } => match known[r(cond).index()] {
            Some(c) => Next::Const(if c != 0 { taken } else { not_taken }),
            None => Next::Select {
                cond: r(cond),
                taken,
                not_taken,
            },
        },
        Next::Dynamic(v) => Next::Dynamic(r(v)),
    };
    Block {
        regs: block.regs,
        addr_width: block.addr_width,
        pc: block.pc,
        len: block.len,
        insts,
        exit: Exit {
            next,
            flow: block.exit.flow,
        },
        bank: block.bank,
    }
}

/// Classifies the I/O accesses of `block` whose address is constant
/// ([`constants`]) with the guest's `map`.
///
/// An [`Op::IoLoad`] or [`Op::IoStore`] of kind [`IoKind::Any`] at a
/// constant address becomes a plain [`Op::Load`] or [`Op::Store`] (ordering
/// [`Order::Plain`]) when every byte it touches is memory in `map`, and
/// kind [`IoKind::Device`] otherwise, so the emitter needs no range check
/// for it. Other instructions, and accesses whose address is not constant,
/// are unchanged. The instructions keep their positions, and the result
/// behaves the same in the interpreter as the input, given devices whose
/// [`crate::interp::Devices::map`] is `map`.
pub fn classify_io(block: &Block, map: &IoMap) -> Block {
    let consts = constants(block);
    let class = |addr: &Value, size: Option<Width>, write: bool| {
        let a = consts[addr.index()]?;
        let n = size?.bytes()?;
        Some(map.classify(a as u64, n, write, block.addr_width))
    };
    let mut out = block.clone();
    for inst in &mut out.insts {
        let op = match inst.op {
            Op::IoLoad {
                addr,
                endian,
                kind: IoKind::Any,
            } => match class(&addr, inst.width, false) {
                Some(AddrClass::Memory) => Op::Load {
                    addr,
                    endian,
                    order: Order::Plain,
                },
                Some(AddrClass::Device) => Op::IoLoad {
                    addr,
                    endian,
                    kind: IoKind::Device,
                },
                None => continue,
            },
            Op::IoStore {
                addr,
                value,
                endian,
                kind: IoKind::Any,
            } => match class(&addr, block.width_of(value), true) {
                Some(AddrClass::Memory) => Op::Store {
                    addr,
                    value,
                    endian,
                    order: Order::Plain,
                },
                Some(AddrClass::Device) => Op::IoStore {
                    addr,
                    value,
                    endian,
                    kind: IoKind::Device,
                },
                None => continue,
            },
            _ => continue,
        };
        inst.op = op;
    }
    out
}

/// The block that runs `head` and then `tail`: `head`'s address and length
/// with `tail`'s exit, for a `head` that always goes on to `tail`.
///
/// A read of a register after a write of it becomes the written value, and
/// only the last write of each register stays, so the result behaves in
/// the interpreter as `head` followed by `tail`.
pub fn fuse(head: &Block, tail: &Block) -> Block {
    let mut insts: Vec<Inst> = Vec::with_capacity(head.insts.len() + tail.insts.len());
    let mut written: HashMap<Reg, (Value, usize)> = HashMap::new();
    let mut map = Vec::new();
    let mut dead = vec![false; head.insts.len() + tail.insts.len()];
    for part in [head, tail] {
        map = vec![0u32; part.insts.len()];
        for (i, inst) in part.insts.iter().enumerate() {
            let op = remap(&inst.op, &map);
            match op {
                Op::Get(r) => {
                    if let Some((v, _)) = written.get(&r) {
                        map[i] = v.0;
                        continue;
                    }
                }
                Op::Set(r, v) => {
                    if let Some((_, at)) = written.insert(r, (v, insts.len())) {
                        dead[at] = true;
                    }
                }
                _ => {}
            }
            map[i] = insts.len() as u32;
            insts.push(Inst {
                op,
                width: inst.width,
            });
        }
    }
    let mut place = vec![0u32; insts.len()];
    let mut kept: Vec<Inst> = Vec::with_capacity(insts.len());
    for (i, inst) in insts.iter().enumerate() {
        if !dead[i] {
            place[i] = kept.len() as u32;
            kept.push(Inst {
                op: remap(&inst.op, &place),
                width: inst.width,
            });
        }
    }
    let r = |v: Value| Value(place[map[v.index()] as usize]);
    let next = match tail.exit.next {
        Next::Const(a) => Next::Const(a),
        Next::Select {
            cond,
            taken,
            not_taken,
        } => Next::Select {
            cond: r(cond),
            taken,
            not_taken,
        },
        Next::Dynamic(v) => Next::Dynamic(r(v)),
    };
    Block {
        regs: head.regs,
        addr_width: head.addr_width,
        pc: head.pc,
        len: head.len,
        insts: kept,
        exit: Exit {
            next,
            flow: tail.exit.flow,
        },
        bank: head.bank,
    }
}

/// The values of `block` that are constant: built from [`Op::Const`] by
/// pure integer operations only. Entry `n` is the value of instruction `n`
/// when it is constant, with the semantics of [`crate::interp`].
pub fn constants(block: &Block) -> Vec<Option<u128>> {
    let mut out: Vec<Option<u128>> = Vec::with_capacity(block.insts.len());
    for inst in &block.insts {
        let k = |v: &Value| out.get(v.index()).copied().flatten();
        let wid = |v: &Value| block.width_of(*v).unwrap_or(Width::W1);
        let w = inst.width.unwrap_or(Width::W1);
        let c = match &inst.op {
            Op::Const(c) => Some(c.get()),
            Op::Un(u, a) => k(a).map(|a| eval_un(*u, a, w)),
            Op::Bin(b, x, y) => k(x).zip(k(y)).map(|(a, b2)| eval_bin(*b, a, b2, w)),
            Op::Cmp(c, x, y) => k(x)
                .zip(k(y))
                .map(|(a, b2)| u128::from(eval_cmp(*c, a, b2, wid(x)))),
            Op::Select(c, x, y) => k(c).and_then(|c| if c != 0 { k(x) } else { k(y) }),
            Op::Ext(ExtKind::Zero, a) => k(a),
            Op::Ext(ExtKind::Sign, a) => {
                let s = 128 - wid(a).bits();
                k(a).map(|x| ((((x << s) as i128) >> s) as u128) & w.mask())
            }
            Op::Extract(a, lo) => k(a).map(|a| (a >> lo) & w.mask()),
            Op::Concat(h, l) => k(h)
                .zip(k(l))
                .map(|(h, l2)| ((h << wid(l).bits()) | l2) & w.mask()),
            _ => None,
        };
        out.push(c);
    }
    out
}

fn remap(op: &Op, map: &[u32]) -> Op {
    op.map_values(&|v: &Value| Value(map[v.index()]))
}

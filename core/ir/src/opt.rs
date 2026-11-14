//! Block-local optimization passes.

use crate::interp::{eval_bin, eval_cmp, eval_un};
use crate::{AddrClass, Block, Exit, ExtKind, Inst, IoKind, IoMap, Next, Op, Order, Value, Width};

/// Removes instructions whose values nothing observable uses.
///
/// An instruction is kept when it has an effect ([`Op::has_effect`]: `Set`,
/// every load and store including exclusive ones, `ClearExclusive`, `Fence`,
/// every intrinsic, the clock ticks `Tick` and `TickIf`, and the I/O
/// accesses `IoLoad` and `IoStore`) or when a kept instruction or the exit uses its
/// value, transitively. Plain loads are kept even when the loaded value is
/// unused, because a load may fault and the fault is observable. Intrinsics
/// are all treated as effectful, since the IR cannot see into them.
///
/// The surviving instructions keep their order and are renumbered densely, so
/// the result passes [`Block::verify`] whenever the input does and behaves
/// the same in the interpreter.
pub fn dce(block: &Block) -> Block {
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
        if op.has_effect() {
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
            Op::Const(c) => Some(*c),
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
    let r = |v: &Value| Value(map[v.index()]);
    match op {
        Op::Const(c) => Op::Const(*c),
        Op::Get(g) => Op::Get(*g),
        Op::Set(g, v) => Op::Set(*g, r(v)),
        Op::Un(u, v) => Op::Un(*u, r(v)),
        Op::Bin(b, x, y) => Op::Bin(*b, r(x), r(y)),
        Op::Cmp(c, x, y) => Op::Cmp(*c, r(x), r(y)),
        Op::Select(c, x, y) => Op::Select(r(c), r(x), r(y)),
        Op::Ext(k, v) => Op::Ext(*k, r(v)),
        Op::Extract(v, lo) => Op::Extract(r(v), *lo),
        Op::Concat(h, l) => Op::Concat(r(h), r(l)),
        Op::Load {
            addr,
            endian,
            order,
        } => Op::Load {
            addr: r(addr),
            endian: *endian,
            order: *order,
        },
        Op::Store {
            addr,
            value,
            endian,
            order,
        } => Op::Store {
            addr: r(addr),
            value: r(value),
            endian: *endian,
            order: *order,
        },
        Op::LoadExclusive {
            addr,
            endian,
            order,
        } => Op::LoadExclusive {
            addr: r(addr),
            endian: *endian,
            order: *order,
        },
        Op::StoreExclusive {
            addr,
            value,
            endian,
            order,
        } => Op::StoreExclusive {
            addr: r(addr),
            value: r(value),
            endian: *endian,
            order: *order,
        },
        Op::ClearExclusive => Op::ClearExclusive,
        Op::Guard(v, code) => Op::Guard(r(v), *code),
        Op::Fence(o) => Op::Fence(*o),
        Op::Fp(f, args) => Op::Fp(*f, args.iter().map(r).collect()),
        Op::Intrinsic(name, args) => Op::Intrinsic(name, args.iter().map(r).collect()),
        Op::Tick(n) => Op::Tick(*n),
        Op::TickIf(v, n) => Op::TickIf(r(v), *n),
        Op::IoLoad { addr, endian, kind } => Op::IoLoad {
            addr: r(addr),
            endian: *endian,
            kind: *kind,
        },
        Op::IoStore {
            addr,
            value,
            endian,
            kind,
        } => Op::IoStore {
            addr: r(addr),
            value: r(value),
            endian: *endian,
            kind: *kind,
        },
    }
}

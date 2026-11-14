//! Block-local optimization passes.

use crate::{Block, Exit, Inst, Next, Op, Value};

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

//! The negative control: a deliberately broken lifted block, which the
//! differential run must catch.

use relift_ir::{BinOp, Block, CmpOp, Inst, Next, Op, Value};

fn swap_bin(op: BinOp) -> BinOp {
    match op {
        BinOp::Add => BinOp::Sub,
        BinOp::Sub => BinOp::Add,
        BinOp::Mul => BinOp::Add,
        BinOp::UDiv => BinOp::URem,
        BinOp::SDiv => BinOp::SRem,
        BinOp::URem => BinOp::SRem,
        BinOp::SRem => BinOp::URem,
        BinOp::And => BinOp::Or,
        BinOp::Or => BinOp::Xor,
        BinOp::Xor => BinOp::And,
        BinOp::Shl => BinOp::LShr,
        BinOp::LShr => BinOp::Shl,
        BinOp::AShr => BinOp::Shl,
        BinOp::RotR => BinOp::LShr,
    }
}

/// Which instructions reach an effect or the exit; a mutation of any other
/// one cannot change the result.
fn live(block: &Block) -> Vec<bool> {
    let mut live = vec![false; block.insts.len()];
    let mark = |v: Value, live: &mut Vec<bool>| live[v.0 as usize] = true;
    match block.exit.next {
        Next::Const(_) => {}
        Next::Select { cond, .. } => mark(cond, &mut live),
        Next::Dynamic(t) => mark(t, &mut live),
    }
    for i in (0..block.insts.len()).rev() {
        let op = &block.insts[i].op;
        let effect = matches!(
            op,
            Op::Set(..)
                | Op::Store { .. }
                | Op::StoreExclusive { .. }
                | Op::LoadExclusive { .. }
                | Op::ClearExclusive
                | Op::Fence(_)
                | Op::Intrinsic(..)
        );
        if effect || live[i] {
            live[i] = true;
            for v in op.operands() {
                mark(v, &mut live);
            }
        }
    }
    live
}

/// Breaks `block`: its first live two-operand op without a zero constant
/// operand becomes another, else its first live comparison changes (`Eq` and
/// `Ne` swap, an ordered comparison swaps its operands), else its exit moves
/// by 4 bytes (or swaps its two targets). Returns what it changed.
pub fn mutate(block: &mut Block) -> String {
    let live = live(block);
    let zeros: Vec<bool> = block.insts.iter().map(|i| i.op == Op::Const(0)).collect();
    for (i, inst) in block.insts.iter_mut().enumerate() {
        if !live[i] {
            continue;
        }
        if let Op::Bin(op, a, b) = inst.op
            && !zeros[a.0 as usize]
            && !zeros[b.0 as usize]
        {
            let to = swap_bin(op);
            inst.op = Op::Bin(to, a, b);
            return format!("{op:?} -> {to:?}");
        }
    }
    for (i, inst) in block.insts.iter_mut().enumerate() {
        if !live[i] {
            continue;
        }
        if let Op::Cmp(op, a, b) = inst.op {
            let to = match op {
                CmpOp::Eq => CmpOp::Ne,
                CmpOp::Ne => CmpOp::Eq,
                _ => {
                    inst.op = Op::Cmp(op, b, a);
                    return format!("{op:?} operands swapped");
                }
            };
            inst.op = Op::Cmp(to, a, b);
            return format!("{op:?} -> {to:?}");
        }
    }
    match block.exit.next {
        Next::Const(t) => {
            block.exit.next = Next::Const(t.wrapping_add(4));
            "exit + 4".to_string()
        }
        Next::Select {
            cond,
            taken,
            not_taken,
        } => {
            block.exit.next = Next::Select {
                cond,
                taken: not_taken,
                not_taken: taken,
            };
            "exit targets swapped".to_string()
        }
        Next::Dynamic(t) => {
            let w = block.addr_width;
            let k = Value(block.insts.len() as u32);
            block.insts.push(Inst {
                op: Op::Const(4),
                width: Some(w),
            });
            let moved = Value(block.insts.len() as u32);
            block.insts.push(Inst {
                op: Op::Bin(BinOp::Add, t, k),
                width: Some(w),
            });
            block.exit.next = Next::Dynamic(moved);
            "dynamic exit + 4".to_string()
        }
    }
}

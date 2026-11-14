//! The negative control: a deliberately broken lifted block, which the
//! differential run must catch.

use relift_ir::{BinOp, Block, CmpOp, Inst, Next, Op, Value};

fn swap_bin(op: BinOp) -> BinOp {
    match op {
        BinOp::Add => BinOp::Sub,
        BinOp::Sub => BinOp::Add,
        BinOp::Mul => BinOp::Add,
        BinOp::UDiv => BinOp::SDiv,
        BinOp::SDiv => BinOp::UDiv,
        BinOp::URem => BinOp::SRem,
        BinOp::SRem => BinOp::URem,
        BinOp::And => BinOp::Or,
        BinOp::Or => BinOp::Xor,
        BinOp::Xor => BinOp::And,
        BinOp::Shl => BinOp::LShr,
        BinOp::LShr => BinOp::AShr,
        BinOp::AShr => BinOp::LShr,
        BinOp::RotR => BinOp::LShr,
    }
}

fn swap_cmp(op: CmpOp) -> CmpOp {
    match op {
        CmpOp::Eq => CmpOp::Ne,
        CmpOp::Ne => CmpOp::Eq,
        CmpOp::Ult => CmpOp::Slt,
        CmpOp::Ule => CmpOp::Sle,
        CmpOp::Slt => CmpOp::Ult,
        CmpOp::Sle => CmpOp::Ule,
    }
}

/// Breaks `block`: its first two-operand op becomes another, else its first
/// comparison flips, else its exit moves by 4 bytes (or swaps its two
/// targets). Returns what it changed.
pub fn mutate(block: &mut Block) -> String {
    for inst in block.insts.iter_mut() {
        if let Op::Bin(op, a, b) = inst.op {
            let to = swap_bin(op);
            inst.op = Op::Bin(to, a, b);
            return format!("{op:?} -> {to:?}");
        }
    }
    for inst in block.insts.iter_mut() {
        if let Op::Cmp(op, a, b) = inst.op {
            let to = swap_cmp(op);
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

//! The mutation control, copied from `check/src/mutate.rs`: a deliberately
//! broken lifted block, which the differential run must catch.

use relift_ir::{
    AtomicOp, BinOp, Block, CmpOp, CryptoOp, FpArgs, FpOp, FpRound, Inst, Next, Op, Value,
};

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
                | Op::AtomicRmw { .. }
                | Op::CompareSwap { .. }
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
    let zeros: Vec<bool> = block.insts.iter().map(|i| i.op == Op::konst(0)).collect();
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
                op: Op::konst(4),
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

fn swap_atomic(op: AtomicOp) -> AtomicOp {
    match op {
        AtomicOp::Add => AtomicOp::Xor,
        AtomicOp::Clear => AtomicOp::Or,
        AtomicOp::Xor => AtomicOp::Or,
        AtomicOp::Or => AtomicOp::Xor,
        AtomicOp::SMax => AtomicOp::SMin,
        AtomicOp::SMin => AtomicOp::SMax,
        AtomicOp::UMax => AtomicOp::UMin,
        AtomicOp::UMin => AtomicOp::UMax,
        AtomicOp::Swap => AtomicOp::Add,
    }
}

fn swap_round(r: FpRound) -> FpRound {
    match r {
        FpRound::TiesEven => FpRound::Floor,
        FpRound::TiesAway => FpRound::Zero,
        FpRound::Floor => FpRound::Ceil,
        FpRound::Ceil => FpRound::TiesEven,
        FpRound::Zero => FpRound::TiesAway,
    }
}

/// The broken form of a binary16 operation (an operand or the result 16
/// bits wide), with the same operand and result widths.
fn swap_fp(f: FpOp, args: &[Value]) -> Op {
    let other = match f {
        FpOp::Add => FpOp::Sub,
        FpOp::Sub => FpOp::Add,
        FpOp::Mul => FpOp::Div,
        FpOp::Div => FpOp::Mul,
        FpOp::Sqrt => FpOp::Neg,
        FpOp::Neg => FpOp::Abs,
        FpOp::Abs => FpOp::Neg,
        FpOp::MulAdd => {
            return Op::Fp(FpOp::MulAdd, FpArgs::new(&[args[2], args[1], args[0]]));
        }
        FpOp::RoundInt(r) => FpOp::RoundInt(swap_round(r)),
        FpOp::ToSInt(r) => FpOp::ToUInt(r),
        FpOp::ToUInt(r) => FpOp::ToSInt(r),
        FpOp::FromSInt => FpOp::FromUInt,
        FpOp::FromUInt | FpOp::Convert => FpOp::FromSInt,
    };
    Op::Fp(other, FpArgs::new(args))
}

/// Breaks the first live atomic, cipher or binary16 operation of `block`:
/// an atomic update becomes another, a compare-and-swap swaps its expected
/// and new values, an AES step becomes its inverse, a binary16 operation another of the same widths. A block
/// without one is broken by [`mutate`]. Returns what it changed.
pub fn mutate_newer(block: &mut Block) -> String {
    let live = live(block);
    let widths: Vec<u32> = block
        .insts
        .iter()
        .map(|i| i.width.map_or(1, |w| w.bits()))
        .collect();
    for (i, inst) in block.insts.iter_mut().enumerate() {
        if !live[i] {
            continue;
        }
        let to = match &inst.op {
            Op::AtomicRmw {
                op,
                addr,
                value,
                endian,
                order,
            } => Op::AtomicRmw {
                op: swap_atomic(*op),
                addr: *addr,
                value: *value,
                endian: *endian,
                order: *order,
            },
            Op::CompareSwap {
                addr,
                expected,
                value,
                endian,
                order,
            } => Op::CompareSwap {
                addr: *addr,
                expected: *value,
                value: *expected,
                endian: *endian,
                order: *order,
            },
            Op::Crypto(c, x) => match c {
                CryptoOp::AesSubBytes => Op::Crypto(CryptoOp::AesInvSubBytes, *x),
                CryptoOp::AesInvSubBytes => Op::Crypto(CryptoOp::AesSubBytes, *x),
                CryptoOp::AesMixColumns => Op::Crypto(CryptoOp::AesInvMixColumns, *x),
                CryptoOp::AesInvMixColumns => Op::Crypto(CryptoOp::AesMixColumns, *x),
            },
            Op::Fp(f, args) if widths[i] == 16 || widths[args[0].index()] == 16 => {
                swap_fp(*f, args)
            }
            _ => continue,
        };
        let what = format!("{:?} -> {to:?}", inst.op);
        inst.op = to;
        return what;
    }
    mutate(block)
}

use relift_ir::{BinOp, Value, Width};

use super::{Cx, LiftError, Lifted};
use crate::regs::{self, FC};
use crate::{Mnemonic, Opcode, Operand};

pub(super) fn lift(cx: &mut Cx, op: &Opcode) -> Lifted<()> {
    match op.mnemonic {
        Mnemonic::Rlca | Mnemonic::Rrca | Mnemonic::Rla | Mnemonic::Rra => {
            let x = cx.get(regs::A);
            let kind = match op.mnemonic {
                Mnemonic::Rlca => Shift::Rlc,
                Mnemonic::Rrca => Shift::Rrc,
                Mnemonic::Rla => Shift::Rl,
                _ => Shift::Rr,
            };
            let (res, c) = shift(cx, kind, x);
            cx.b.set(regs::A, res);
            let zero = cx.bit_const(false);
            cx.flags(Some(zero), Some(zero), Some(zero), Some(c));
            Ok(())
        }
        Mnemonic::Bit | Mnemonic::Res | Mnemonic::Set => bit_op(cx, op),
        m => {
            let kind = match m {
                Mnemonic::Rlc => Shift::Rlc,
                Mnemonic::Rrc => Shift::Rrc,
                Mnemonic::Rl => Shift::Rl,
                Mnemonic::Rr => Shift::Rr,
                Mnemonic::Sla => Shift::Sla,
                Mnemonic::Sra => Shift::Sra,
                Mnemonic::Swap => Shift::Swap,
                Mnemonic::Srl => Shift::Srl,
                _ => return Err(LiftError::Table("not a rotate or shift")),
            };
            let x = cx.read8(op.dst)?;
            let (res, c) = shift(cx, kind, x);
            let z = cx.is_zero(res);
            let zero = cx.bit_const(false);
            cx.flags(Some(z), Some(zero), Some(zero), Some(c));
            cx.write8(op.dst, res)
        }
    }
}

#[derive(Clone, Copy)]
enum Shift {
    Rlc,
    Rrc,
    Rl,
    Rr,
    Sla,
    Sra,
    Swap,
    Srl,
}

fn shift(cx: &mut Cx, kind: Shift, x: Value) -> (Value, Value) {
    let one = cx.k8(1);
    let seven = cx.k8(7);
    let left = |cx: &mut Cx| cx.b.bin(BinOp::Shl, x, one);
    let right = |cx: &mut Cx| cx.b.bin(BinOp::LShr, x, one);
    let top = cx.b.bit(x, 7);
    let bottom = cx.b.bit(x, 0);
    match kind {
        Shift::Rlc => {
            let r = cx.b.bin(BinOp::LShr, x, seven);
            let l = left(cx);
            (cx.b.or(l, r), top)
        }
        Shift::Rrc => {
            let r = right(cx);
            let l = cx.b.bin(BinOp::Shl, x, seven);
            (cx.b.or(l, r), bottom)
        }
        Shift::Rl => {
            let cin = cx.get(FC);
            let cin = cx.b.zext(cin, Width::W8);
            let l = left(cx);
            (cx.b.or(l, cin), top)
        }
        Shift::Rr => {
            let cin = cx.get(FC);
            let cin = cx.b.zext(cin, Width::W8);
            let moved = cx.b.bin(BinOp::Shl, cin, seven);
            let r = right(cx);
            (cx.b.or(r, moved), bottom)
        }
        Shift::Sla => (left(cx), top),
        Shift::Sra => (cx.b.bin(BinOp::AShr, x, one), bottom),
        Shift::Srl => (right(cx), bottom),
        Shift::Swap => {
            let four = cx.k8(4);
            let hi = cx.b.bin(BinOp::Shl, x, four);
            let lo = cx.b.bin(BinOp::LShr, x, four);
            let zero = cx.bit_const(false);
            (cx.b.or(hi, lo), zero)
        }
    }
}

fn bit_op(cx: &mut Cx, op: &Opcode) -> Lifted<()> {
    let Operand::Bit(n) = op.dst else {
        return Err(LiftError::Table("a bit number was expected"));
    };
    let x = cx.read8(op.src)?;
    let mask = cx.k8(1 << n);
    match op.mnemonic {
        Mnemonic::Bit => {
            let m = cx.b.and(x, mask);
            let z = cx.is_zero(m);
            let zero = cx.bit_const(false);
            let one = cx.bit_const(true);
            cx.flags(Some(z), Some(zero), Some(one), None);
            Ok(())
        }
        Mnemonic::Res => {
            let inv = cx.b.not(mask);
            let r = cx.b.and(x, inv);
            cx.write8(op.src, r)
        }
        _ => {
            let r = cx.b.or(x, mask);
            cx.write8(op.src, r)
        }
    }
}

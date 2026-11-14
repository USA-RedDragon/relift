use relift_ir::{Value, Width};

use super::load::sp_plus_offset;
use super::{Cx, LiftError, Lifted};
use crate::regs::{self, FC};
use crate::{Mnemonic, Opcode, Operand, Reg16};

pub(super) fn lift(cx: &mut Cx, op: &Opcode) -> Lifted<()> {
    match (op.mnemonic, op.dst) {
        (Mnemonic::Inc | Mnemonic::Dec, Operand::R16(r)) => {
            let v = cx.pair(r);
            let k = cx.k16(1);
            let n = if op.mnemonic == Mnemonic::Inc {
                cx.b.add(v, k)
            } else {
                cx.b.sub(v, k)
            };
            cx.set_pair(r, n);
            Ok(())
        }
        (Mnemonic::Inc | Mnemonic::Dec, dst) => inc_dec(cx, dst, op.mnemonic == Mnemonic::Dec),
        (Mnemonic::Add, Operand::R16(Reg16::Hl)) => add_hl(cx, op),
        (Mnemonic::Add, Operand::R16(Reg16::Sp)) => add_sp(cx),
        (Mnemonic::Cpl, _) => {
            let a = cx.get(regs::A);
            let n = cx.b.not(a);
            cx.b.set(regs::A, n);
            let one = cx.bit_const(true);
            cx.flags(None, Some(one), Some(one), None);
            Ok(())
        }
        (Mnemonic::Scf, _) => {
            let zero = cx.bit_const(false);
            let one = cx.bit_const(true);
            cx.flags(None, Some(zero), Some(zero), Some(one));
            Ok(())
        }
        (Mnemonic::Ccf, _) => {
            let c = cx.get(FC);
            let n = cx.b.not(c);
            let zero = cx.bit_const(false);
            cx.flags(None, Some(zero), Some(zero), Some(n));
            Ok(())
        }
        _ => alu8(cx, op),
    }
}

fn inc_dec(cx: &mut Cx, dst: Operand, dec: bool) -> Lifted<()> {
    let x = cx.read8(dst)?;
    let one = cx.k8(1);
    let res = if dec {
        cx.b.sub(x, one)
    } else {
        cx.b.add(x, one)
    };
    let mask = cx.k8(0x0F);
    let low = cx.b.and(x, mask);
    let edge = cx.k8(if dec { 0 } else { 0x0F });
    let h = cx.b.cmp(relift_ir::CmpOp::Eq, low, edge);
    let z = cx.is_zero(res);
    let n = cx.bit_const(dec);
    cx.flags(Some(z), Some(n), Some(h), None);
    cx.write8(dst, res)
}

fn alu8(cx: &mut Cx, op: &Opcode) -> Lifted<()> {
    let a = cx.get(regs::A);
    let x = cx.read8(op.src)?;
    let (res, n, h, c) = match op.mnemonic {
        Mnemonic::Add | Mnemonic::Adc => {
            let cin = if op.mnemonic == Mnemonic::Adc {
                cx.get(FC)
            } else {
                cx.bit_const(false)
            };
            let (sum, h, c) = add_parts(cx, a, x, cin, false);
            (sum, false, h, c)
        }
        Mnemonic::Sub | Mnemonic::Sbc | Mnemonic::Cp => {
            let cin = if op.mnemonic == Mnemonic::Sbc {
                cx.get(FC)
            } else {
                cx.bit_const(false)
            };
            let (diff, h, c) = add_parts(cx, a, x, cin, true);
            (diff, true, h, c)
        }
        Mnemonic::And | Mnemonic::Xor | Mnemonic::Or => {
            let r = match op.mnemonic {
                Mnemonic::And => cx.b.and(a, x),
                Mnemonic::Xor => cx.b.xor(a, x),
                _ => cx.b.or(a, x),
            };
            let h = cx.bit_const(op.mnemonic == Mnemonic::And);
            let c = cx.bit_const(false);
            (r, false, h, c)
        }
        _ => return Err(LiftError::Table("not an 8-bit ALU instruction")),
    };
    let z = cx.is_zero(res);
    let n = cx.bit_const(n);
    cx.flags(Some(z), Some(n), Some(h), Some(c));
    if op.mnemonic != Mnemonic::Cp {
        cx.b.set(regs::A, res);
    }
    Ok(())
}

/// `a + x + cin` or `a - x - cin` with the half carry out of bit 3 and the
/// carry (borrow) out of bit 7.
fn add_parts(cx: &mut Cx, a: Value, x: Value, cin: Value, sub: bool) -> (Value, Value, Value) {
    let a9 = cx.b.zext(a, Width::new(9));
    let x9 = cx.b.zext(x, Width::new(9));
    let c9 = cx.b.zext(cin, Width::new(9));
    let mask = cx.k8(0x0F);
    let al = cx.b.and(a, mask);
    let xl = cx.b.and(x, mask);
    let c8 = cx.b.zext(cin, Width::W8);
    let (wide, nib) = if sub {
        let w = cx.b.sub(a9, x9);
        let nb = cx.b.sub(al, xl);
        (cx.b.sub(w, c9), cx.b.sub(nb, c8))
    } else {
        let w = cx.b.add(a9, x9);
        let nb = cx.b.add(al, xl);
        (cx.b.add(w, c9), cx.b.add(nb, c8))
    };
    let res = cx.b.trunc(wide, Width::W8);
    let h = cx.b.bit(nib, 4);
    let c = cx.b.bit(wide, 8);
    (res, h, c)
}

fn add_hl(cx: &mut Cx, op: &Opcode) -> Lifted<()> {
    let Operand::R16(r) = op.src else {
        return Err(LiftError::Table("ADD HL needs a register pair"));
    };
    let hl = cx.pair(Reg16::Hl);
    let rr = cx.pair(r);
    let hl17 = cx.b.zext(hl, Width::new(17));
    let rr17 = cx.b.zext(rr, Width::new(17));
    let wide = cx.b.add(hl17, rr17);
    let sum = cx.b.trunc(wide, Width::W16);
    let c = cx.b.bit(wide, 16);
    let mask = cx.k16(0x0FFF);
    let hl12 = cx.b.and(hl, mask);
    let rr12 = cx.b.and(rr, mask);
    let hs = cx.b.add(hl12, rr12);
    let h = cx.b.bit(hs, 12);
    cx.set_pair(Reg16::Hl, sum);
    let zero = cx.bit_const(false);
    cx.flags(None, Some(zero), Some(h), Some(c));
    Ok(())
}

fn add_sp(cx: &mut Cx) -> Lifted<()> {
    let (sum, h, c) = sp_plus_offset(cx)?;
    cx.set_pair(Reg16::Sp, sum);
    let zero = cx.bit_const(false);
    cx.flags(Some(zero), Some(zero), Some(h), Some(c));
    Ok(())
}

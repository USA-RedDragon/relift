use crate::{Mnemonic, Opcode, Operand, Reg16};

use super::{Cx, LiftError, Lifted};
use relift_ir::Width;

pub(super) fn lift(cx: &mut Cx, op: &Opcode) -> Lifted<()> {
    match op.mnemonic {
        Mnemonic::Push => push(cx, op),
        Mnemonic::Pop => pop(cx, op),
        _ => ld(cx, op),
    }
}

fn reg16(o: Operand) -> Lifted<Reg16> {
    match o {
        Operand::R16(r) => Ok(r),
        _ => Err(LiftError::Table("a register pair was expected")),
    }
}

fn ld(cx: &mut Cx, op: &Opcode) -> Lifted<()> {
    match (op.dst, op.src) {
        (Operand::R16(Reg16::Hl), Operand::SpPlusSigned8) => ld_hl_sp_offset(cx),
        (Operand::R16(d), Operand::Imm16) => {
            let k = cx.imm16v()?;
            cx.set_pair(d, k);
            Ok(())
        }
        (Operand::R16(Reg16::Sp), Operand::R16(Reg16::Hl)) => {
            let hl = cx.pair(Reg16::Hl);
            cx.set_pair(Reg16::Sp, hl);
            Ok(())
        }
        (Operand::IndirectImm16, Operand::R16(Reg16::Sp)) => {
            let a0 = cx.imm16v()?;
            let sp = cx.pair(Reg16::Sp);
            let lo = cx.b.trunc(sp, Width::W8);
            let hi = cx.b.extract(sp, 8, Width::W8);
            let one = cx.k16(1);
            let a1 = cx.b.add(a0, one);
            cx.store(a0, lo)?;
            cx.store(a1, hi)
        }
        (dst, src) => {
            let v = cx.read8(src)?;
            cx.write8(dst, v)
        }
    }
}

fn ld_hl_sp_offset(cx: &mut Cx) -> Lifted<()> {
    let (sum, h, c) = sp_plus_offset(cx)?;
    cx.set_pair(Reg16::Hl, sum);
    let zero = cx.bit_const(false);
    cx.flags(Some(zero), Some(zero), Some(h), Some(c));
    Ok(())
}

/// `sp + e` with the half carry and carry the SM83 takes from the low byte.
pub(super) fn sp_plus_offset(
    cx: &mut Cx,
) -> Lifted<(relift_ir::Value, relift_ir::Value, relift_ir::Value)> {
    let e = cx.imm8()?;
    let wide = cx.b.sext(e, Width::W16);
    let sp = cx.pair(Reg16::Sp);
    let sum = cx.b.add(sp, wide);
    let sp8 = cx.b.trunc(sp, Width::W8);
    let mask = cx.k8(0x0F);
    let sl = cx.b.and(sp8, mask);
    let el = cx.b.and(e, mask);
    let hs = cx.b.add(sl, el);
    let h = cx.b.bit(hs, 4);
    let sp9 = cx.b.zext(sp8, Width::new(9));
    let e9 = cx.b.zext(e, Width::new(9));
    let cs = cx.b.add(sp9, e9);
    let c = cx.b.bit(cs, 8);
    Ok((sum, h, c))
}

fn push(cx: &mut Cx, op: &Opcode) -> Lifted<()> {
    let r = reg16(op.src)?;
    let v = cx.pair(r);
    let sp = cx.pair(Reg16::Sp);
    let one = cx.k16(1);
    let two = cx.k16(2);
    let hi_at = cx.b.sub(sp, one);
    let lo_at = cx.b.sub(sp, two);
    let hi = cx.b.extract(v, 8, Width::W8);
    let lo = cx.b.trunc(v, Width::W8);
    cx.store(hi_at, hi)?;
    cx.store(lo_at, lo)?;
    cx.set_pair(Reg16::Sp, lo_at);
    Ok(())
}

fn pop(cx: &mut Cx, op: &Opcode) -> Lifted<()> {
    let r = reg16(op.dst)?;
    let sp = cx.pair(Reg16::Sp);
    let one = cx.k16(1);
    let two = cx.k16(2);
    let hi_at = cx.b.add(sp, one);
    let lo = cx.load(sp)?;
    let hi = cx.load(hi_at)?;
    let v = cx.b.concat(hi, lo);
    cx.set_pair(r, v);
    let after = cx.b.add(sp, two);
    cx.set_pair(Reg16::Sp, after);
    Ok(())
}

use relift_ir::{BinOp, Builder, ExtKind, Op, UnOp, Value, Width};

use super::{
    End, LiftError, LiftFn, condition_holds, read_x, read_x_sp, set_flags, sext_imm, sf_width,
    write_x, write_x_sp,
};
use crate::decode::{Encoding, Insn};

pub(super) fn lifter(enc: Encoding) -> Option<LiftFn> {
    use Encoding::*;
    Some(match enc {
        ADD_32_addsub_imm | ADDS_32S_addsub_imm | SUB_32_addsub_imm | SUBS_32S_addsub_imm
        | ADD_64_addsub_imm | ADDS_64S_addsub_imm | SUB_64_addsub_imm | SUBS_64S_addsub_imm => {
            addsub_imm
        }
        ADD_32_addsub_shift | ADDS_32_addsub_shift | SUB_32_addsub_shift | SUBS_32_addsub_shift
        | ADD_64_addsub_shift | ADDS_64_addsub_shift | SUB_64_addsub_shift
        | SUBS_64_addsub_shift => addsub_shift,
        AND_32_log_imm | ORR_32_log_imm | EOR_32_log_imm | ANDS_32S_log_imm | AND_64_log_imm
        | ORR_64_log_imm | EOR_64_log_imm | ANDS_64S_log_imm => log_imm,
        AND_32_log_shift | BIC_32_log_shift | ORR_32_log_shift | ORN_32_log_shift
        | EOR_32_log_shift | EON_32_log_shift | ANDS_32_log_shift | BICS_32_log_shift
        | AND_64_log_shift | BIC_64_log_shift | ORR_64_log_shift | ORN_64_log_shift
        | EOR_64_log_shift | EON_64_log_shift | ANDS_64_log_shift | BICS_64_log_shift => log_shift,
        MOVN_32_movewide | MOVZ_32_movewide | MOVK_32_movewide | MOVN_64_movewide
        | MOVZ_64_movewide | MOVK_64_movewide => movewide,
        ADR_only_pcreladdr | ADRP_only_pcreladdr => pcreladdr,
        CSEL_32_condsel | CSINC_32_condsel | CSINV_32_condsel | CSNEG_32_condsel
        | CSEL_64_condsel | CSINC_64_condsel | CSINV_64_condsel | CSNEG_64_condsel => condsel,
        SBFM_32M_bitfield | BFM_32M_bitfield | UBFM_32M_bitfield | SBFM_64M_bitfield
        | BFM_64M_bitfield | UBFM_64M_bitfield => bitfield,
        MADD_32A_dp_3src | MSUB_32A_dp_3src | MADD_64A_dp_3src | MSUB_64A_dp_3src => madd,
        SMULH_64_dp_3src | UMULH_64_dp_3src => mulh,
        UDIV_32_dp_2src | SDIV_32_dp_2src | UDIV_64_dp_2src | SDIV_64_dp_2src => div,
        LSLV_32_dp_2src | LSRV_32_dp_2src | ASRV_32_dp_2src | RORV_32_dp_2src | LSLV_64_dp_2src
        | LSRV_64_dp_2src | ASRV_64_dp_2src | RORV_64_dp_2src => shiftv,
        _ => return None,
    })
}

/// `a + b` or `a - b` as AddWithCarry, setting flags when `s`.
fn add_sub(b: &mut Builder, x: Value, y: Value, sub: bool, s: bool) -> Value {
    let (y, cin) = if sub {
        (b.not(y), b.konst(Width::W1, 1))
    } else {
        (y, b.konst(Width::W1, 0))
    };
    let (sum, carry, overflow) = b.add_with_carry(x, y, cin);
    if s {
        set_flags(b, sum, carry, overflow);
    }
    sum
}

fn addsub_imm(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = sf_width(&insn);
    let imm = u128::from(insn.get("imm12")) << (12 * insn.get("sh"));
    let x = read_x_sp(b, insn.get("Rn"), w);
    let y = b.konst(w, imm);
    let s = insn.get("S") == 1;
    let r = add_sub(b, x, y, insn.get("op") == 1, s);
    if s {
        write_x(b, insn.get("Rd"), r);
    } else {
        write_x_sp(b, insn.get("Rd"), r);
    }
    Ok(End::Next)
}

/// Shifts `v` by the constant `amount` with A64 shift type `kind`.
fn shift_reg(b: &mut Builder, v: Value, kind: u32, amount: u32) -> Value {
    if amount == 0 {
        return v;
    }
    let w = b.width(v);
    let k = b.konst(w, u128::from(amount));
    let op = match kind {
        0 => BinOp::Shl,
        1 => BinOp::LShr,
        2 => BinOp::AShr,
        _ => BinOp::RotR,
    };
    b.bin(op, v, k)
}

fn addsub_shift(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = sf_width(&insn);
    let kind = insn.get("shift");
    let amount = insn.get("imm6");
    if kind == 3 || amount >= w.bits() {
        return Err(LiftError::Undefined(insn.encoding));
    }
    let x = read_x(b, insn.get("Rn"), w);
    let m = read_x(b, insn.get("Rm"), w);
    let y = shift_reg(b, m, kind, amount);
    let r = add_sub(b, x, y, insn.get("op") == 1, insn.get("S") == 1);
    write_x(b, insn.get("Rd"), r);
    Ok(End::Next)
}

fn ones(n: u32) -> u64 {
    if n >= 64 { u64::MAX } else { (1u64 << n) - 1 }
}

fn ror(x: u64, r: u32, esize: u32) -> u64 {
    if r == 0 {
        return x;
    }
    ((x >> r) | (x << (esize - r))) & ones(esize)
}

fn replicate(x: u64, esize: u32, m: u32) -> u64 {
    let mut out = 0u64;
    let mut at = 0;
    while at < m {
        out |= x << at;
        at += esize;
    }
    out & ones(m)
}

/// The A64 bit-mask immediate decoder: `(wmask, tmask)` for an `m`-bit
/// operation, or `None` when the fields are reserved.
pub(super) fn decode_bit_masks(
    n: u32,
    imms: u32,
    immr: u32,
    immediate: bool,
    m: u32,
) -> Option<(u64, u64)> {
    let combined = (n << 6) | (!imms & 0x3f);
    if combined == 0 {
        return None;
    }
    let len = 31 - combined.leading_zeros();
    if len < 1 {
        return None;
    }
    let esize = 1u32 << len;
    if esize > m {
        return None;
    }
    let levels = esize - 1;
    if immediate && imms & levels == levels {
        return None;
    }
    let s = imms & levels;
    let r = immr & levels;
    let d = s.wrapping_sub(r) & levels;
    let welem = ones(s + 1);
    let telem = ones(d + 1);
    Some((
        replicate(ror(welem, r, esize), esize, m),
        replicate(telem, esize, m),
    ))
}

fn log_imm(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = sf_width(&insn);
    let (imm, _) = decode_bit_masks(
        insn.get("N"),
        insn.get("imms"),
        insn.get("immr"),
        true,
        w.bits(),
    )
    .ok_or(LiftError::Undefined(insn.encoding))?;
    let x = read_x(b, insn.get("Rn"), w);
    let y = b.konst(w, u128::from(imm));
    let opc = insn.get("opc");
    let r = match opc {
        0 | 3 => b.and(x, y),
        1 => b.or(x, y),
        _ => b.xor(x, y),
    };
    if opc == 3 {
        let zero = b.konst(Width::W1, 0);
        set_flags(b, r, zero, zero);
        write_x(b, insn.get("Rd"), r);
    } else {
        write_x_sp(b, insn.get("Rd"), r);
    }
    Ok(End::Next)
}

fn log_shift(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = sf_width(&insn);
    let amount = insn.get("imm6");
    if amount >= w.bits() {
        return Err(LiftError::Undefined(insn.encoding));
    }
    let x = read_x(b, insn.get("Rn"), w);
    let m = read_x(b, insn.get("Rm"), w);
    let mut y = shift_reg(b, m, insn.get("shift"), amount);
    if insn.get("N") == 1 {
        y = b.not(y);
    }
    let opc = insn.get("opc");
    let r = match opc {
        0 | 3 => b.and(x, y),
        1 => b.or(x, y),
        _ => b.xor(x, y),
    };
    if opc == 3 {
        let zero = b.konst(Width::W1, 0);
        set_flags(b, r, zero, zero);
    }
    write_x(b, insn.get("Rd"), r);
    Ok(End::Next)
}

fn movewide(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = sf_width(&insn);
    let pos = insn.get("hw") * 16;
    if pos >= w.bits() {
        return Err(LiftError::Undefined(insn.encoding));
    }
    let imm = u128::from(insn.get("imm16")) << pos;
    let rd = insn.get("Rd");
    let r = match insn.get("opc") {
        0 => b.konst(w, !imm),
        2 => b.konst(w, imm),
        3 => {
            let old = read_x(b, rd, w);
            let keep = b.konst(w, !(0xffffu128 << pos));
            let kept = b.and(old, keep);
            let new = b.konst(w, imm);
            b.or(kept, new)
        }
        _ => return Err(LiftError::Undefined(insn.encoding)),
    };
    write_x(b, rd, r);
    Ok(End::Next)
}

fn pcreladdr(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let imm = sext_imm((insn.get("immhi") << 2) | insn.get("immlo"), 21);
    let pc = b.pc();
    let addr = if insn.get("op") == 1 {
        (pc & !0xfff).wrapping_add((imm << 12) as u64)
    } else {
        pc.wrapping_add(imm as u64)
    };
    let v = b.konst(Width::W64, u128::from(addr));
    write_x(b, insn.get("Rd"), v);
    Ok(End::Next)
}

fn condsel(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = sf_width(&insn);
    let cond = condition_holds(b, insn.get("cond"));
    let x = read_x(b, insn.get("Rn"), w);
    let m = read_x(b, insn.get("Rm"), w);
    let y = match (insn.get("op"), insn.get("o2")) {
        (0, 0) => m,
        (0, _) => {
            let one = b.konst(w, 1);
            b.add(m, one)
        }
        (_, 0) => b.not(m),
        _ => b.un(UnOp::Neg, m),
    };
    let r = b.select(cond, x, y);
    write_x(b, insn.get("Rd"), r);
    Ok(End::Next)
}

fn bitfield(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = sf_width(&insn);
    let (n, immr, imms) = (insn.get("N"), insn.get("immr"), insn.get("imms"));
    if w == Width::W32 && (immr >= 32 || imms >= 32) {
        return Err(LiftError::Undefined(insn.encoding));
    }
    let (wmask, tmask) = decode_bit_masks(n, imms, immr, false, w.bits())
        .ok_or(LiftError::Undefined(insn.encoding))?;
    let rd = insn.get("Rd");
    let src = read_x(b, insn.get("Rn"), w);
    let r = b.konst(w, u128::from(immr));
    let rot = b.bin(BinOp::RotR, src, r);
    let wm = b.konst(w, u128::from(wmask));
    let tm = b.konst(w, u128::from(tmask));
    let ntm = b.konst(w, u128::from(!tmask));
    let result = match insn.get("opc") {
        0 => {
            let bot = b.and(rot, wm);
            let sign = b.bit(src, imms);
            let top = b.push(Op::Ext(ExtKind::Sign, sign), Some(w));
            let hi = b.and(top, ntm);
            let lo = b.and(bot, tm);
            b.or(hi, lo)
        }
        1 => {
            let dst = read_x(b, rd, w);
            let nwm = b.konst(w, u128::from(!wmask));
            let keep = b.and(dst, nwm);
            let ins = b.and(rot, wm);
            let bot = b.or(keep, ins);
            let hi = b.and(dst, ntm);
            let lo = b.and(bot, tm);
            b.or(hi, lo)
        }
        _ => {
            let bot = b.and(rot, wm);
            b.and(bot, tm)
        }
    };
    write_x(b, rd, result);
    Ok(End::Next)
}

fn madd(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = sf_width(&insn);
    let x = read_x(b, insn.get("Rn"), w);
    let y = read_x(b, insn.get("Rm"), w);
    let a = read_x(b, insn.get("Ra"), w);
    let p = b.bin(BinOp::Mul, x, y);
    let r = if insn.get("o0") == 1 {
        b.sub(a, p)
    } else {
        b.add(a, p)
    };
    write_x(b, insn.get("Rd"), r);
    Ok(End::Next)
}

fn mulh(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let x = read_x(b, insn.get("Rn"), Width::W64);
    let y = read_x(b, insn.get("Rm"), Width::W64);
    let (xw, yw) = if insn.get("U") == 1 {
        (b.zext(x, Width::W128), b.zext(y, Width::W128))
    } else {
        (b.sext(x, Width::W128), b.sext(y, Width::W128))
    };
    let p = b.bin(BinOp::Mul, xw, yw);
    let hi = b.extract(p, 64, Width::W64);
    write_x(b, insn.get("Rd"), hi);
    Ok(End::Next)
}

fn div(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = sf_width(&insn);
    let x = read_x(b, insn.get("Rn"), w);
    let y = read_x(b, insn.get("Rm"), w);
    let op = if insn.get("o1") == 1 {
        BinOp::SDiv
    } else {
        BinOp::UDiv
    };
    let r = b.bin(op, x, y);
    write_x(b, insn.get("Rd"), r);
    Ok(End::Next)
}

fn shiftv(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = sf_width(&insn);
    let x = read_x(b, insn.get("Rn"), w);
    let y = read_x(b, insn.get("Rm"), w);
    let m = b.konst(w, u128::from(w.bits() - 1));
    let amount = b.and(y, m);
    let op = match insn.get("op2") {
        0 => BinOp::Shl,
        1 => BinOp::LShr,
        2 => BinOp::AShr,
        _ => BinOp::RotR,
    };
    let r = b.bin(op, x, amount);
    write_x(b, insn.get("Rd"), r);
    Ok(End::Next)
}

#[cfg(test)]
mod tests {
    use super::decode_bit_masks;

    #[test]
    fn bit_masks_match_known_immediates() {
        assert_eq!(
            decode_bit_masks(0, 0b000111, 0, true, 32).map(|m| m.0),
            Some(0xff)
        );
        assert_eq!(
            decode_bit_masks(0, 0b111100, 0, true, 64).map(|m| m.0),
            Some(0x5555_5555_5555_5555)
        );
        assert_eq!(
            decode_bit_masks(1, 0b000000, 1, true, 64).map(|m| m.0),
            Some(0x8000_0000_0000_0000)
        );
        assert_eq!(decode_bit_masks(0, 0b111111, 0, true, 64), None);
        assert_eq!(decode_bit_masks(1, 0b111111, 0, true, 64), None);
    }
}

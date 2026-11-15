use relift_ir::{BinOp, Builder, Value, Width};

use super::{End, LiftError, LiftFn, read_x, read_x_sp, set_flags, sf_width, write_x, write_x_sp};
use crate::decode::{Encoding, Insn};

pub(super) fn lifter(enc: Encoding) -> Option<LiftFn> {
    use Encoding::*;
    Some(match enc {
        ADD_32_addsub_imm | ADD_64_addsub_imm => addsub_imm,
        SUBS_32_addsub_shift | SUBS_64_addsub_shift => addsub_shift,
        ORR_32_log_imm | ORR_64_log_imm => log_imm,
        MOVZ_32_movewide | MOVZ_64_movewide => movewide,
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

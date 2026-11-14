//! The half-precision conversions the floating-point cut cannot build from
//! one IR primitive without rounding twice. Every other half-precision
//! operation goes through `rt::Fp::new(16)`.

use relift_ir::{BinOp, Builder, CmpOp, FpOp, FpRound, Value, Width};

use super::rt::Fp;

/// `FPToFixed` from half precision: the value widened to double precision
/// (exact) and converted there, so `x * 2^fbits` cannot overflow before it
/// saturates.
pub fn to_fixed(
    b: &mut Builder,
    x: Value,
    fbits: u32,
    signed: bool,
    r: FpRound,
    out: u32,
) -> Value {
    let d = b.fp(FpOp::Convert, &[x], Width::W64);
    Fp::new(64).fp_to_fixed(b, d, fbits, signed, r, out)
}

/// `FixedToFP` to half precision with `fbits` fraction bits. A 32-bit
/// integer and its scaling are exact in double precision, so it is rounded
/// once to half precision. A wider integer is
/// reduced to at most 53 significant bits, keeping a sticky bit for the
/// dropped ones (round to odd, which a second rounding to 11 bits cannot
/// spoil), made exact in double precision and scaled there, then rounded
/// once to half precision.
pub fn fixed_to_half(b: &mut Builder, x: Value, fbits: u32, signed: bool) -> Value {
    if b.width(x).bits() <= 32 {
        let f = if signed {
            b.fp(FpOp::FromSInt, &[x], Width::W64)
        } else {
            b.fp(FpOp::FromUInt, &[x], Width::W64)
        };
        let scale = b.konst(Width::W64, u128::from((1023 - u64::from(fbits)) << 52));
        let f = b.fp(FpOp::Mul, &[f, scale], Width::W64);
        return b.fp(FpOp::Convert, &[f], Width::W16);
    }
    let x = if signed {
        b.sext(x, Width::W64)
    } else {
        b.zext(x, Width::W64)
    };
    let k = |b: &mut Builder, v: u64| b.konst(Width::W64, u128::from(v));
    let zero = k(b, 0);
    let neg = if signed {
        b.cmp(CmpOp::Slt, x, zero)
    } else {
        b.konst(Width::W1, 0)
    };
    let nx = b.bin(BinOp::Sub, zero, x);
    let m = b.select(neg, nx, x);
    let lim = k(b, 1 << 53);
    let big = b.cmp(CmpOp::Ule, lim, m);
    let eleven = k(b, 11);
    let hi = b.bin(BinOp::LShr, m, eleven);
    let low = k(b, 0x7ff);
    let lost = b.and(m, low);
    let sticky = b.cmp(CmpOp::Ne, lost, zero);
    let sticky = b.zext(sticky, Width::W64);
    let reduced = b.or(hi, sticky);
    let m2 = b.select(big, reduced, m);
    let f = b.fp(FpOp::FromUInt, &[m2], Width::W64);
    let scale = (1023 + 11 - i64::from(fbits)) as u64;
    let up = k(b, scale << 52);
    let down = k(b, (1023 - u64::from(fbits)) << 52);
    let s = b.select(big, up, down);
    let f = b.fp(FpOp::Mul, &[f, s], Width::W64);
    let nf = b.fp(FpOp::Neg, &[f], Width::W64);
    let f = b.select(neg, nf, f);
    b.fp(FpOp::Convert, &[f], Width::W16)
}

//! Pseudo-instructions: instructions of another GPU that no SM86 form
//! matches, which a front end puts in [`Program::pseudo`] in place of a
//! word (see `relift-gpu-sm53`'s lowering table). Each reads only its
//! decoded operands and is told apart from every SM86 instruction by a
//! modifier no SM86 form has.
//!
//! [`Program::pseudo`]: crate::program::Program::pseudo

use super::float::Round;
use super::{Ctx, Res, bad};
use crate::{Inst, Operand};

/// The modifier of Maxwell's `IPA.MUL`.
pub const IPA_MUL: &str = "MUL";

/// The modifier of a Maxwell `IPA` that saturates its result.
pub const IPA_SAT: &str = "SAT";

/// The modifier of a Maxwell `MUFU` that saturates its result.
pub const MUFU_SAT: &str = "SAT";

/// Maxwell's `RRO.SINCOS`, the range reduction before `MUFU.SIN` and
/// `MUFU.COS`.
pub const RRO_SINCOS: &str = "SINCOS";

/// Maxwell's `RRO.EX2`, the range reduction before `MUFU.EX2`.
pub const RRO_EX2: &str = "EX2";

/// The modifier of a Maxwell `I2F` that converts the absolute value of its
/// integer source.
pub const I2F_IABS: &str = "IABS";

/// The modifier of a Maxwell `I2F` that converts the negation of its
/// integer source (after `IABS` when both are set).
pub const I2F_INEG: &str = "INEG";

/// One turn per radian, `1 / (2 pi)` as a single-precision number.
const TURNS_PER_RADIAN: u32 = 0x3e22_f983;

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    if i.mnemonic == "IPA" && (i.has(IPA_MUL) || i.has(IPA_SAT)) {
        return Some(ipa(c, i));
    }
    if i.mnemonic == "MUFU" && i.has(MUFU_SAT) {
        return Some(mufu_sat(c, i));
    }
    if i.mnemonic == "RRO" {
        return Some(rro(c, i));
    }
    if i.mnemonic == "I2F" && (i.has(I2F_IABS) || i.has(I2F_INEG)) {
        return Some(i2f(c, i));
    }
    None
}

/// Maxwell `I2F` with an integer `.ABS` or `.NEG` of its source: the SM86
/// `I2F` of the same types and rounding, converting the source's wrapping
/// absolute value and then its wrapping negation, as Ryujinx's `I2F`
/// emitter does.
fn i2f(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut plain = i.clone();
    plain.mods.retain(|m| *m != I2F_IABS && *m != I2F_INEG);
    super::int::conv::i2f_with(c, &plain, i.has(I2F_IABS), i.has(I2F_INEG))
}

/// Maxwell `MUFU` with `.SAT`: the SM86 `MUFU` of the same function, then
/// the clamp to [0, 1] with NaN to 0 of every saturating float result.
/// The operand is read before the result is written, so it may be the
/// destination.
fn mufu_sat(c: &mut Ctx, i: &Inst) -> Res<()> {
    let d = Ctx::dst_reg(&i.ops[0])?;
    let mut plain = i.clone();
    plain.mods.retain(|m| *m != MUFU_SAT);
    super::mufu::mufu(c, &plain)?;
    if d == 255 {
        return Ok(());
    }
    let v = c.load_reg(d);
    let v = c.sat_bits(v, true);
    c.store_reg(d, v);
    Ok(())
}

/// Maxwell `RRO`: the operand (negated or absolute as asked) in the form
/// the SM86 `MUFU` takes. For `.SINCOS` that is turns, the operand times
/// `1 / (2 pi)` rounded toward zero, as NVIDIA's compiler multiplies
/// before `MUFU.SIN` and `MUFU.COS` on SM70 and later where Maxwell code
/// has `RRO.SINCOS`; for `.EX2` the operand itself, which the SM86
/// `MUFU.EX2` converts to fixed point as Maxwell's `RRO.EX2` does
/// (inferred: the Maxwell format is console-only; Ryujinx copies the
/// operand for both and takes radians in its sine and cosine).
fn rro(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.ops.len() != 2 || i.mods.len() != 1 {
        return bad(format!("pseudo RRO form {i}"));
    }
    let v = c.fsrc_bits(&i.ops[1])?;
    let r = if i.has(RRO_SINCOS) {
        let k = c.m.c_u32(TURNS_PER_RADIAN);
        c.fmul_exact(v, k, Round::Zero, false, 0)
    } else if i.has(RRO_EX2) {
        v
    } else {
        return bad(format!("pseudo RRO form {i}"));
    };
    c.store_dst(&i.ops[0], r)
}

/// Maxwell `IPA` with a multiplier or saturation: the SM86 `IPA` of the
/// same attribute, then `* b` when `.MUL` (round to nearest even,
/// denormals kept) and a clamp to [0, 1] with NaN to 0 when `.SAT`, as
/// Ryujinx's `Ipa` emitter does. The multiplier is read before the
/// interpolated value is written, so `b` may be the destination.
fn ipa(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mul = i.has(IPA_MUL);
    let sat = i.has(IPA_SAT);
    let want = if mul { 3 } else { 2 };
    if i.ops.len() != want {
        return bad(format!("pseudo IPA form {i}"));
    }
    let b = match i.ops.get(2) {
        Some(Operand::Reg(n, m)) if *m == Default::default() => Some(c.load_reg(*n)),
        Some(o) => return bad(format!("pseudo IPA multiplier {o}")),
        None => None,
    };
    let d = Ctx::dst_reg(&i.ops[0])?;
    if d == 255 {
        return Ok(());
    }
    let mut plain = i.clone();
    plain.mods.retain(|m| *m != IPA_MUL && *m != IPA_SAT);
    plain.ops.truncate(2);
    super::io::inst(c, &plain).unwrap_or_else(|| bad("pseudo IPA"))?;
    let mut v = c.load_reg(d);
    if let Some(b) = b {
        v = c.fmul_exact(v, b, Round::Nearest, false, 0);
    }
    let v = c.sat_bits(v, sat);
    c.store_reg(d, v);
    Ok(())
}

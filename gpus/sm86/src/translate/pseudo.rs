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

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    if i.mnemonic == "IPA" && (i.has(IPA_MUL) || i.has(IPA_SAT)) {
        return Some(ipa(c, i));
    }
    None
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

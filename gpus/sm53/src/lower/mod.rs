//! Lowering Maxwell instructions onto SM86 for the translator of
//! `relift-gpu-sm86`.
//!
//! Every Maxwell form has a row in [`table`]: lowered to exactly one SM86
//! form, a pseudo-instruction the translator handles from its decoded
//! operands, both by case, or a stop. [`lower`] does it for one
//! instruction. A form is lowered only with evidence that the two
//! behave the same in every case (rounding and denormal modes, saturation,
//! flags, edge values); otherwise it is a pseudo-instruction or a stop.
//! `LOWERING.md` is the table as text.

mod encode;
pub mod table;

pub use encode::Lowered;

use crate::bundle::slots;
use crate::form::{Op, Src};
use crate::inst::Inst;
use crate::program::Program;
use table::Row;

/// What `i` becomes: the stop of its table row, or its lowering.
pub fn lower(i: &Inst) -> Lowered {
    if let Row::Stop { reason } = table::row(i.op(), i.enc.src) {
        return Lowered::Stop(reason);
    }
    encode::lower(i).unwrap_or(Lowered::Stop("no lowering for a checked row"))
}

/// A form a program uses that does not lower: where, which and why.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Stop {
    /// Code offset of the first instruction of the form.
    pub at: u64,
    pub op: Op,
    pub src: Src,
    pub reason: &'static str,
}

/// The forms of `p` that stop, once per form and reason, at their first
/// use; empty when every instruction of the program's bundles lowers or is
/// a pseudo-instruction. The words after the last reachable instruction of
/// the last bundle count too (they are padding, usually `NOP`).
pub fn stops(p: &Program) -> Vec<Stop> {
    let mut out: Vec<Stop> = Vec::new();
    for s in slots(&p.code) {
        let Some(i) = Inst::decode(s.word, s.at) else {
            continue;
        };
        let (op, src) = (i.op(), i.enc.src);
        if let Lowered::Stop(reason) = lower(&i)
            && !out
                .iter()
                .any(|x| (x.op, x.src, x.reason) == (op, src, reason))
        {
            out.push(Stop {
                at: s.at,
                op,
                src,
                reason,
            });
        }
    }
    out
}

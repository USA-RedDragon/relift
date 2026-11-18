//! Lowering Maxwell instructions onto SM86 for the translator of
//! `relift-gpu-sm86`.
//!
//! Every Maxwell form has a row in [`table`]: lowered to exactly one SM86
//! form, a pseudo-instruction the translator handles from its decoded
//! operands, or a stop. A form is lowered only with evidence that the two
//! behave the same in every case (rounding and denormal modes, saturation,
//! flags, edge values); otherwise it is a pseudo-instruction or a stop.
//! `LOWERING.md` is the table as text.

pub mod table;

use crate::bundle::slots;
use crate::form::{Op, Src};
use crate::program::Program;
use table::Row;

/// A form a program uses that does not lower: where, which and why.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Stop {
    /// Code offset of the first instruction of the form.
    pub at: u64,
    pub op: Op,
    pub src: Src,
    pub reason: &'static str,
}

/// The forms of `p` that stop, once each, at their first use; empty when
/// every instruction of the program's bundles lowers or is a
/// pseudo-instruction. The words after the last reachable instruction of
/// the last bundle count too (they are padding, usually `NOP`).
pub fn stops(p: &Program) -> Vec<Stop> {
    let mut out: Vec<Stop> = Vec::new();
    for s in slots(&p.code) {
        let Some(e) = crate::form::classify(s.word) else {
            continue;
        };
        if let Row::Stop { reason } = table::row(e.op, e.src)
            && !out.iter().any(|x| (x.op, x.src) == (e.op, e.src))
        {
            out.push(Stop {
                at: s.at,
                op: e.op,
                src: e.src,
                reason,
            });
        }
    }
    out
}

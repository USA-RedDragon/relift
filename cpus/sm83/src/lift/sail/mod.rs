//! Lifting templates generated from the SM83 Sail model
//! (`cpus/sm83/sail/sm83.sail`), one per defined opcode, and the
//! hand-written runtime they call (`rt.rs`). `generated.rs` is the output of
//! `cpus/sm83/gen`; the build needs neither the generator nor Sail.
//!
//! The block structure (the `EI` pair, the two halves of a conditional
//! `CALL` or `RET`, the HALT bug view, sync points) is the frontend's, shared
//! with the hand-written lifter; only the instruction itself comes from a
//! template.

mod generated;
mod rt;

use relift_ir::Builder;

use super::flow::is_conditional_transfer;
use super::{Cx, Done, Kind, LiftError, Lifted};
use crate::{Immediate, Instruction, Prefix};
use rt::{Mode, St};

/// A generated template: emits the instruction's operations.
type GenFn = fn(u32, &mut Builder, &mut St) -> Result<(), rt::LiftError>;

/// The bytes of `ins`, padded with zeros.
fn bytes_of(ins: &Instruction) -> [u8; 3] {
    let op = ins.opcode;
    let mut out = [0u8; 3];
    let mut n = 0;
    if op.prefix == Prefix::Cb {
        out[0] = 0xCB;
        n = 1;
    }
    out[n] = op.code;
    n += 1;
    match ins.immediate {
        Immediate::None => {}
        Immediate::U8(v) => out[n] = v,
        Immediate::I8(v) => out[n] = v as u8,
        Immediate::U16(v) => {
            out[n] = v as u8;
            out[n + 1] = (v >> 8) as u8;
        }
    }
    out
}

/// Lifts one instruction into `cx`, which has begun it, from its generated
/// template.
pub(super) fn lift_one(cx: &mut Cx, ins: &Instruction) -> Lifted<Done> {
    let op = ins.opcode;
    let template = match op.prefix {
        Prefix::Cb => generated::cb(op.code),
        Prefix::None => generated::base(op.code),
    };
    let Some(template) = template else {
        return Err(match op.mnemonic {
            crate::Mnemonic::Illegal => LiftError::Illegal(op.code),
            _ => LiftError::Table("no template for the opcode"),
        });
    };
    let mode = match (is_conditional_transfer(op), cx.taken) {
        (false, _) => Mode::Plain,
        (true, false) => Mode::Decide,
        (true, true) => Mode::Taken,
    };
    let mut st = St::new(bytes_of(ins), cx.pc, cx.len(), mode, cx.elapsed, cx.view);
    let Ok(()) = template(u32::from(op.code), &mut cx.b, &mut st);
    let fin = st.finish(&mut cx.b).map_err(LiftError::Unsupported)?;
    let kind = match (mode, fin.control) {
        (Mode::Decide, _) => Kind::Half,
        (_, true) => Kind::Control,
        (_, false) => Kind::Plain,
    };
    Ok(Done {
        next: fin.next,
        flow: fin.flow,
        kind,
    })
}

#[cfg(test)]
mod tests;

//! Lifting templates generated from the sail-arm model, one per encoding id,
//! and the hand-written runtime they call. The files under `generated/` are
//! the generator's output (`cpus/a64/gen`), one per instruction class; the
//! build needs neither the generator nor Sail.

mod generated;
#[allow(dead_code)]
mod rt;

use relift_ir::{Block, Builder, Width};

use super::LiftError;
use crate::decode::{Encoding, Insn};
use crate::regs;
use rt::St;

/// A generated template: emits the instruction's operations.
pub(crate) type GenFn = fn(u32, &mut Builder, &mut St) -> Result<(), LiftError>;

/// Whether `enc` has a generated template.
pub fn has_template(enc: Encoding) -> bool {
    generated::template(enc).is_some()
}

/// Lifts `insn` at `pc` with its generated template.
pub fn lift_insn(insn: Insn, pc: u64) -> Result<Block, LiftError> {
    let f = generated::template(insn.encoding).ok_or(LiftError::Unsupported(insn.encoding))?;
    let mut b = Builder::new(regs::regs(), Width::W64, pc);
    let mut st = St::new();
    f(insn.word, &mut b, &mut st)?;
    let exit = st.finish(&mut b);
    Ok(b.finish(4, exit))
}

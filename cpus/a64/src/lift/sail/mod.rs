//! Lifting templates generated from the sail-arm model, one per encoding id,
//! and the hand-written runtime they call. The files under `generated/` are
//! the generator's output (`cpus/a64/gen`), one per instruction class; the
//! build needs neither the generator nor Sail.

mod generated;
#[allow(dead_code)]
mod rt;

use relift_ir::{Block, Builder, CmpOp, Width};

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

/// The slow-path code of a block that needs a non-default FPCR.
pub const SLOW_FPCR: u32 = 0x4650_4352;

/// Lifts `insn` at `pc` with its generated template.
///
/// The templates' floating-point semantics hold for FPCR zero only (see
/// `relift_ir::FpOp`). A block whose template uses the floating-point cut
/// (it calls `St::assume_default_fpcr`) starts with a guard
/// that leaves it for slow path [`SLOW_FPCR`] when FPCR is not zero.
pub fn lift_insn(insn: Insn, pc: u64) -> Result<Block, LiftError> {
    let f = generated::template(insn.encoding).ok_or(LiftError::Unsupported(insn.encoding))?;
    let run = |guard: bool| -> Result<(Block, bool), LiftError> {
        let mut b = Builder::new(regs::regs(), Width::W64, pc);
        if guard {
            let fpcr = b.get(regs::FPCR);
            let zero = b.konst(b.width(fpcr), 0);
            let ne = b.cmp(CmpOp::Ne, fpcr, zero);
            b.guard(ne, SLOW_FPCR);
        }
        let mut st = St::new();
        f(insn.word, &mut b, &mut st)?;
        let fp = st.assumes_default_fpcr();
        let exit = st.finish(&mut b);
        Ok((b.finish(4, exit), fp))
    };
    let (block, fp) = run(false)?;
    if fp {
        return Ok(run(true)?.0);
    }
    Ok(block)
}

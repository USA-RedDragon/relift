//! Lifting templates generated from the sail-arm model, one per encoding id,
//! and the hand-written runtime they call. The files under `generated/` are
//! the generator's output (`cpus/arm64/gen`), one per instruction class; the
//! build needs neither the generator nor Sail.

#[allow(dead_code)]
mod align;
mod generated;
#[allow(dead_code)]
mod half;
#[allow(dead_code)]
mod rt;

use relift_ir::{Block, Builder, CmpOp, Width};

use super::LiftError;
use crate::decode::{Encoding, Features, Insn};
use crate::regs;
use rt::St;
pub(crate) use rt::exact_host;

/// A generated template: emits the instruction's operations.
pub(crate) type GenFn = fn(u32, &mut Builder, &mut St) -> Result<(), LiftError>;

/// Whether `enc` has a generated template.
pub fn has_template(enc: Encoding) -> bool {
    generated::template(enc).is_some()
}

/// The slow-path code of an ordered, exclusive or atomic access that faults
/// for alignment (see `align.rs`): there is no slow path, the guest takes
/// an alignment fault, which the runtime reports like an undefined
/// instruction.
pub const SLOW_ALIGN: u32 = 0x414c_4e47;

/// The slow-path code of a block that needs a non-default FPCR.
pub const SLOW_FPCR: u32 = 0x4650_4352;

/// The slow-path code of a block whose FPSR flags the host cannot give, in a
/// program that produces them (`regs::FP_FLAGS` is 1).
pub const SLOW_FLAGS: u32 = 0x4650_5352;

/// Lifts `insn` at `pc` with its generated template, for a CPU with the
/// features `features` (a template whose Arm semantics test a feature asks
/// them).
///
/// The templates' floating-point semantics hold for FPCR zero only (see
/// `relift_ir::FpOp`). A block whose template uses the floating-point cut
/// (it calls `St::assume_default_fpcr`) starts with a guard
/// that leaves it for slow path [`SLOW_FPCR`] when FPCR is not zero.
pub fn lift_insn(insn: Insn, pc: u64, features: &Features) -> Result<Block, LiftError> {
    let f = generated::template(insn.encoding).ok_or(LiftError::Unsupported(insn.encoding))?;
    let run = |guard: bool, flags: bool| -> Result<(Block, bool, bool), LiftError> {
        let mut b = Builder::new(regs::regs(), Width::W64, pc);
        if guard {
            let fpcr = b.get(regs::FPCR);
            let zero = b.konst(b.width(fpcr), 0);
            let ne = b.cmp(CmpOp::Ne, fpcr, zero);
            b.guard(ne, SLOW_FPCR);
        }
        if flags {
            let on = b.get(regs::FP_FLAGS);
            b.guard(on, SLOW_FLAGS);
        }
        let mut st = St::new(*features);
        f(insn.word, &mut b, &mut st)?;
        let fp = st.assumes_default_fpcr();
        let slow_flags = st.flags_need_slow();
        let exit = st.finish(&mut b);
        Ok((b.finish(4, exit), fp, slow_flags))
    };
    let (block, fp, flags) = run(false, false)?;
    if fp || flags {
        return Ok(run(fp, flags)?.0);
    }
    Ok(block)
}

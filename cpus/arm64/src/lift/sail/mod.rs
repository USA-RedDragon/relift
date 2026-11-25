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
use crate::cpu::Cpu;
use crate::decode::{Encoding, Insn};
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

/// The slow-path code of a block whose floating-point semantics hold for FPCR zero or FPCR with only FZ set: scalar single and
/// double FADD, FSUB, FMUL, FDIV, FNMUL, FSQRT and the fused multiply-adds. The emitted operations take FZ into account
/// (abi.h, `rl_fp_fast`), as the differential tests of `runtime/cpus/arm64/tests/test_fp_fz_host.c` show.
pub const SLOW_FPCR_FZ: u32 = 0x4650_465a;

/// Whether the instruction `word` is one whose emitted code is exact under FPCR.FZ: scalar single or double FADD, FSUB, FMUL,
/// FDIV, FNMUL, FSQRT, FMADD, FMSUB, FNMADD and FNMSUB.
pub fn fz_tolerant(word: u32) -> bool {
    let ftype_sd = (word >> 22) & 3 < 2;
    let arith = word & 0xFF20_0C00 == 0x1E20_0800 && matches!((word >> 12) & 15, 0 | 1 | 2 | 3 | 8);
    let sqrt = word & 0xFF3F_FC00 == 0x1E21_C000;
    let fused = word & 0xFF00_0000 == 0x1F00_0000;
    // SCVTF, UCVTF (an integer is never denormal or tiny) and FCVTZS, FCVTZU (a denormal operand takes the exact path in
    // the emitted code), scalar from or to a general register; FCVT between single and double.
    let int_cvt = word & 0x7F3E_FC00 == 0x1E22_0000 || word & 0x7F3E_FC00 == 0x1E38_0000;
    let fcvt = word & 0xFFFF_FC00 == 0x1E62_4000 || word & 0xFFFF_FC00 == 0x1E22_C000;
    let scalar = ftype_sd && (arith || sqrt || fused || int_cvt) || fcvt;
    // Vector forms, every lane through the same operations: FADD, FSUB, FMUL, FDIV, FMLA, FMLS, FSQRT; FMUL, FMLA, FMLS by
    // element; SCVTF, UCVTF, FCVTZS, FCVTZU. Not FMULX, FABD, pairwise, estimates or steps.
    let m = word & 0xBFA0_FC00;
    let vec = matches!(
        m,
        0x0E20_D400 | 0x0EA0_D400 | 0x2E20_DC00 | 0x2E20_FC00 | 0x0E20_CC00 | 0x0EA0_CC00
    ) || matches!(word & 0xBF80_F400, 0x0F80_9000 | 0x0F80_1000 | 0x0F80_5000)
        || word & 0xBFBF_FC00 == 0x2EA1_F800
        || matches!(
            word & 0xBFBF_FC00,
            0x0E21_D800 | 0x2E21_D800 | 0x0EA1_B800 | 0x2EA1_B800
        );
    scalar || vec
}

/// The slow-path code of a block whose FPSR flags the host cannot give, in a
/// program that produces them (`regs::FP_FLAGS` is 1).
pub const SLOW_FLAGS: u32 = 0x4650_5352;

/// Lifts `insn` at `pc` with its generated template, for the CPU `cpu` (a
/// template whose Arm semantics test a feature or the address layout asks
/// it).
///
/// The templates' floating-point semantics hold for FPCR zero only (see
/// `relift_ir::FpOp`). A block whose template uses the floating-point cut
/// (it calls `St::assume_default_fpcr`) starts with a guard
/// that leaves it for slow path [`SLOW_FPCR`] when FPCR is not zero.
pub fn lift_insn(insn: Insn, pc: u64, cpu: &Cpu) -> Result<Block, LiftError> {
    let f = generated::template(insn.encoding).ok_or(LiftError::Unsupported(insn.encoding))?;
    let run = |guard: bool, flags: bool| -> Result<(Block, bool, bool), LiftError> {
        let mut b = Builder::new(regs::regs(), Width::W64, pc);
        if guard {
            let fpcr = b.get(regs::FPCR);
            let zero = b.konst(b.width(fpcr), 0);
            if fz_tolerant(insn.word) {
                let rest = b.konst(b.width(fpcr), u128::from(!(1u32 << 24)));
                let masked = b.and(fpcr, rest);
                let ne = b.cmp(CmpOp::Ne, masked, zero);
                b.guard(ne, SLOW_FPCR_FZ);
            } else {
                let ne = b.cmp(CmpOp::Ne, fpcr, zero);
                b.guard(ne, SLOW_FPCR);
            }
        }
        if flags {
            let on = b.get(regs::FP_FLAGS);
            b.guard(on, SLOW_FLAGS);
        }
        let mut st = St::new(*cpu);
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

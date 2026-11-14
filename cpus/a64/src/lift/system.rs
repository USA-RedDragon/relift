//! System instructions lifted by hand: SVC, MRS and MSR of the registers EL0
//! code reads, cache maintenance, WFE and SEV, and the feature hints; none
//! of them has a generated template. The registers here
//! are runtime state the Sail model cannot see. The intrinsics are
//! named `a64.*`; the emitter maps them to the runtime ABI (docs/abi.md
//! section 10).
//!
//! The hints BTI, CHKFEAT and XPACLRI lift to nothing. That is exact for the
//! Switch's Cortex-A57, an Armv8.0 core without FEAT_BTI, FEAT_CHK or
//! FEAT_PAuth, where every one of them executes as a NOP: XPACLRI has no PAC
//! bits to strip because no instruction ever adds them, and CHKFEAT leaves
//! x16 unchanged when no feature is implemented.

use relift_ir::{Builder, Exit, Flow, Next, Width};

use super::{End, LiftError, LiftFn, read_x, write_x};
use crate::decode::{Encoding, Insn};
use crate::regs::{C, FPCR, FPSR, N, TPIDR_EL0, TPIDRRO_EL0, V, Z};

/// Intrinsic: `CNTPCT_EL0` or `CNTVCT_EL0`, 64 bits, no operands.
pub const CNTPCT: &str = "a64.cntpct";
/// Intrinsic: `CNTFRQ_EL0`, 64 bits.
pub const CNTFRQ: &str = "a64.cntfrq";
/// Intrinsic: `CTR_EL0`, 64 bits.
pub const CTR: &str = "a64.ctr";
/// Intrinsic: `DCZID_EL0`, 64 bits.
pub const DCZID: &str = "a64.dczid";
/// Intrinsic: `DC ZVA` of the block holding the address operand; no result.
pub const DC_ZVA: &str = "a64.dc_zva";
/// Intrinsic: data or instruction cache maintenance by address; no result.
pub const CACHE_MAINT: &str = "a64.cache_maint";
/// Intrinsic: `WFE`; no operands, no result.
pub const WFE: &str = "a64.wfe";
/// Intrinsic: `SEV`; no operands, no result.
pub const SEV: &str = "a64.sev";

/// The hand-written system lifter for `enc`, if there is one.
pub fn lifter(enc: Encoding) -> Option<LiftFn> {
    use Encoding::*;
    let f: LiftFn = match enc {
        SVC_EX_exception => svc,
        MRS_RS_systemmove => mrs,
        MSR_SR_systemmove => msr,
        SYS_CR_systeminstrs => sys,
        WFE_HI_hints => wfe,
        SEV_HI_hints => sev,
        BTI_HB_hints | CHKFEAT_HF_hints | XPACLRI_HI_hints => nop,
        _ => return None,
    };
    Some(f)
}

fn svc(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    Ok(End::Exit(Exit {
        next: Next::Const(b.pc().wrapping_add(4)),
        flow: Flow::Trap(insn.get("imm16")),
    }))
}

/// A system register as `(op0, op1, CRn, CRm, op2)`.
type SysReg = (u32, u32, u32, u32, u32);

const NZCV: SysReg = (3, 3, 4, 2, 0);
const FPCR_R: SysReg = (3, 3, 4, 4, 0);
const FPSR_R: SysReg = (3, 3, 4, 4, 1);
const TPIDR: SysReg = (3, 3, 13, 0, 2);
const TPIDRRO: SysReg = (3, 3, 13, 0, 3);
const CNTFRQ_R: SysReg = (3, 3, 14, 0, 0);
const CNTPCT_R: SysReg = (3, 3, 14, 0, 1);
const CNTVCT_R: SysReg = (3, 3, 14, 0, 2);
const CTR_R: SysReg = (3, 3, 0, 0, 1);
const DCZID_R: SysReg = (3, 3, 0, 0, 7);

fn sysreg(insn: &Insn) -> SysReg {
    (
        2 | insn.get("o0"),
        insn.get("op1"),
        insn.get("CRn"),
        insn.get("CRm"),
        insn.get("op2"),
    )
}

fn mrs(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let t = insn.get("Rt");
    let v = match sysreg(&insn) {
        NZCV => {
            let bits = [N, Z, C, V].map(|f| b.get(f));
            let zero = b.konst(Width::new(28), 0);
            let nz = b.concat(bits[0], bits[1]);
            let cv = b.concat(bits[2], bits[3]);
            let f = b.concat(nz, cv);
            let w = b.concat(f, zero);
            b.zext(w, Width::W64)
        }
        FPCR_R => {
            let v = b.get(FPCR);
            b.zext(v, Width::W64)
        }
        FPSR_R => {
            let v = b.get(FPSR);
            b.zext(v, Width::W64)
        }
        TPIDR => b.get(TPIDR_EL0),
        TPIDRRO => b.get(TPIDRRO_EL0),
        CNTPCT_R | CNTVCT_R => b.intrinsic(CNTPCT, &[], Some(Width::W64)),
        CNTFRQ_R => b.intrinsic(CNTFRQ, &[], Some(Width::W64)),
        CTR_R => b.intrinsic(CTR, &[], Some(Width::W64)),
        DCZID_R => b.intrinsic(DCZID, &[], Some(Width::W64)),
        _ => return Err(LiftError::Unsupported(insn.encoding)),
    };
    write_x(b, t, v);
    Ok(End::Next)
}

fn msr(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let v = read_x(b, insn.get("Rt"), Width::W64);
    match sysreg(&insn) {
        NZCV => {
            for (bit, flag) in [(31, N), (30, Z), (29, C), (28, V)] {
                let f = b.bit(v, bit);
                b.set(flag, f);
            }
        }
        FPCR_R => {
            let w = b.trunc(v, Width::W32);
            b.set(FPCR, w);
        }
        FPSR_R => {
            let w = b.trunc(v, Width::W32);
            let m = b.konst(Width::W32, 0x0800_009f);
            let w = b.and(w, m);
            b.set(FPSR, w);
        }
        TPIDR => b.set(TPIDR_EL0, v),
        TPIDRRO => return Err(LiftError::Undefined(insn.encoding)),
        _ => return Err(LiftError::Unsupported(insn.encoding)),
    }
    Ok(End::Next)
}

fn sys(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let op = (
        insn.get("op1"),
        insn.get("CRn"),
        insn.get("CRm"),
        insn.get("op2"),
    );
    let name = match op {
        (3, 7, 4, 1) => DC_ZVA,
        (3, 7, 10..=14, 1) | (3, 7, 5, 1) => CACHE_MAINT,
        _ => return Err(LiftError::Unsupported(insn.encoding)),
    };
    let a = read_x(b, insn.get("Rt"), Width::W64);
    b.intrinsic(name, &[a], None);
    Ok(End::Next)
}

fn wfe(_insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    b.intrinsic(WFE, &[], None);
    Ok(End::Next)
}

fn sev(_insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    b.intrinsic(SEV, &[], None);
    Ok(End::Next)
}

fn nop(_insn: Insn, _b: &mut Builder) -> Result<End, LiftError> {
    Ok(End::Next)
}

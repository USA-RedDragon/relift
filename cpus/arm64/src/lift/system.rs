//! System instructions lifted by hand: SVC, BRK, MRS and MSR of the registers EL0
//! code reads, cache maintenance, WFE and SEV, and the feature hints; none
//! of them has a generated template. The registers here
//! are runtime state the Sail model cannot see. The intrinsics are
//! named `arm64.*`; the emitter maps them to the runtime ABI (docs/abi.md
//! section 10).
//!
//! The hints BTI, CHKFEAT and XPACLRI lift to nothing. That is exact for the
//! Switch's Cortex-A57, an Armv8.0 core without FEAT_BTI, FEAT_CHK or
//! FEAT_PAuth, where every one of them executes as a NOP: XPACLRI has no PAC
//! bits to strip because no instruction ever adds them, and CHKFEAT leaves
//! x16 unchanged when no feature is implemented.
//!
//! With `FEAT_PAuth` in the CPU's features the pointer-authentication hints
//! decode to their own encodings, which have no lifter (the lift fails
//! instead of dropping an authentication), and XPACLRI is refused too.
//! Without them those words decode to the plain `HINT`, which executes as
//! a NOP, as does every hint whose feature the CPU lacks.

use relift_ir::{Builder, Exit, Flow, Next, Width};

use super::{End, LiftError, LiftFn, read_x, write_x};
use crate::decode::{Encoding, Insn};
use crate::regs::{C, FPCR, FPSR, N, TPIDR_EL0, TPIDRRO_EL0, V, Z};

/// Intrinsic: `CNTPCT_EL0` or `CNTVCT_EL0`, 64 bits, no operands.
pub const CNTPCT: &str = "arm64.cntpct";
/// Intrinsic: `CNTFRQ_EL0`, 64 bits.
pub const CNTFRQ: &str = "arm64.cntfrq";
/// Intrinsic: `CTR_EL0`, 64 bits.
pub const CTR: &str = "arm64.ctr";
/// Intrinsic: `DCZID_EL0`, 64 bits.
pub const DCZID: &str = "arm64.dczid";
/// Intrinsic: `DC ZVA` of the block holding the address operand; no result.
pub const DC_ZVA: &str = "arm64.dc_zva";
/// Intrinsic: data or instruction cache maintenance by address; no result.
pub const CACHE_MAINT: &str = "arm64.cache_maint";
/// Intrinsic: `WFE`; no operands, no result.
pub const WFE: &str = "arm64.wfe";
/// Intrinsic: `SEV`; no operands, no result.
pub const SEV: &str = "arm64.sev";

/// The `relift-ir` interpreter's view of the intrinsics that have no effect
/// a single-threaded interpreter can see: `WFE` and `SEV` (an event wait
/// that may return at once, and its signal) and cache maintenance. Every
/// other intrinsic needs runtime state and stays unknown.
#[derive(Clone, Copy, Debug, Default)]
pub struct InterpIntrinsics;

impl relift_ir::interp::Intrinsics for InterpIntrinsics {
    fn call(
        &mut self,
        name: &'static str,
        _args: &[u128],
    ) -> Result<u128, relift_ir::interp::Error> {
        match name {
            WFE | SEV | CACHE_MAINT => Ok(0),
            _ => Err(relift_ir::interp::Error::UnknownIntrinsic(name)),
        }
    }
}

/// The trap code of `BRK #imm` is `TRAP_BRK | imm`; an SVC's code is its
/// 16-bit immediate, so the two never collide.
pub const TRAP_BRK: u32 = 0x1_0000;

/// The hand-written system lifter for `enc`, if there is one.
pub fn lifter(enc: Encoding) -> Option<LiftFn> {
    use Encoding::*;
    let f: LiftFn = match enc {
        SVC_EX_exception => svc,
        BRK_EX_exception => brk,
        MRS_RS_systemmove => mrs,
        MSR_SR_systemmove => msr,
        SYS_CR_systeminstrs => sys,
        WFE_HI_hints => wfe,
        SEV_HI_hints => sev,
        BTI_HB_hints | CHKFEAT_HF_hints | XPACLRI_HI_hints | HINT_HM_hints => nop,
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

/// A breakpoint: a trap the runtime reports as a guest crash (the Switch
/// kernel raises it to the process as an exception).
fn brk(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    Ok(End::Exit(Exit {
        next: Next::Const(b.pc().wrapping_add(4)),
        flow: Flow::Trap(TRAP_BRK | insn.get("imm16")),
    }))
}

/// A system register as `(op0, op1, CRn, CRm, op2)`.
type SysReg = (u32, u32, u32, u32, u32);

const NZCV: SysReg = (3, 3, 4, 2, 0);
const FPCR_R: SysReg = (3, 3, 4, 4, 0);
const FPSR_R: SysReg = (3, 3, 4, 4, 1);

/// MRS FPSR: the slot value (32 bits) with the host's flags ORed in.
pub const MRS_FPSR: &str = "arm64.mrs_fpsr";
/// After MSR FPSR: clears the host's flags.
pub const MSR_FPSR: &str = "arm64.msr_fpsr";
/// Raises FPSR flags (a 32-bit mask) an IR-built FP operation computed.
pub const FP_RAISE: &str = "arm64.fp_raise";

/// The runtime's exact-flags bit of the FPCR slot (`RL_FPCR_EXACT`): RES0
/// in the architecture, so guest reads and writes never see it.
pub const FPCR_EXACT: u128 = 1 << 31;
/// The FPCR bits the guest sees (`RL_FPCR_GUEST`): AHP, DN, FZ, RMode and
/// FZ16. The Cortex-A57 implements no FP trap enables, so they read as 0.
const FPCR_GUEST: u128 = 0x07c8_0000;
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
            // The runtime's exact-flags bit (31, RES0) never reaches the guest.
            let v = b.get(FPCR);
            let m = b.konst(Width::W32, FPCR_GUEST);
            let v = b.and(v, m);
            b.zext(v, Width::W64)
        }
        FPSR_R => {
            // The slot holds what IR wrote (QC, MSR, slow paths); the
            // runtime ORs in the flags host FP operations raised.
            let v = b.get(FPSR);
            let v = b.intrinsic(MRS_FPSR, &[v], Some(Width::W32));
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
            // Guest bits from the write, the exact-flags bit kept.
            let w = b.trunc(v, Width::W32);
            let m = b.konst(Width::W32, FPCR_GUEST);
            let w = b.and(w, m);
            let old = b.get(FPCR);
            let e = b.konst(Width::W32, FPCR_EXACT);
            let keep = b.and(old, e);
            let w = b.or(w, keep);
            b.set(FPCR, w);
        }
        FPSR_R => {
            let w = b.trunc(v, Width::W32);
            let m = b.konst(Width::W32, 0x0800_009f);
            let w = b.and(w, m);
            b.set(FPSR, w);
            b.intrinsic(MSR_FPSR, &[], None);
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

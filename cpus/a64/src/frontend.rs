//! The A64 [`Frontend`]: lifting through [`crate::lift`] and the A64 half of
//! the lifted-code ABI (`runtime/cpus/a64/include/relift/a64/abi.h`).

use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::{Block, Reg, RegFile};

use crate::lift::{self, system};
use crate::regs;

/// The A64 frontend.
#[derive(Clone, Copy, Debug, Default)]
pub struct A64;

impl Frontend for A64 {
    fn granule(&self) -> u64 {
        4
    }

    fn regs(&self) -> RegFile {
        regs::regs()
    }

    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        let Some(w) = bytes.get(..4) else {
            return Err(LiftFailure {
                insn: 0,
                message: "past the end of the image".into(),
            });
        };
        let word = u32::from_le_bytes([w[0], w[1], w[2], w[3]]);
        lift::lift(word, pc).map_err(|e| LiftFailure {
            insn: u64::from(word),
            message: e.to_string(),
        })
    }

    fn abi_header(&self) -> &'static str {
        "relift/a64/abi.h"
    }

    fn home(&self, reg: Reg) -> Home {
        let i = reg.index();
        match i {
            0..=30 => Home::Field(format!("c->x[{i}]")),
            31 => Home::Field("c->sp".into()),
            32..=35 => Home::Flag {
                word: "c->nzcv".into(),
                bit: 31 - (i as u32 - 32),
            },
            36..=67 => Home::Field(format!("c->v[{}].q", i - 36)),
            _ if reg == regs::FPCR => Home::Field("c->fpcr".into()),
            _ if reg == regs::FPSR => Home::Field("c->fpsr".into()),
            _ if reg == regs::TPIDR_EL0 => Home::Field("c->tpidr_el0".into()),
            _ => Home::Field("c->tpidrro_el0".into()),
        }
    }

    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        let arg = |i: usize| args.get(i).cloned().unwrap_or_default();
        Some(match name {
            system::CNTPCT => "rl_mrs_cntpct(c)".into(),
            system::CNTFRQ => "rl_mrs_cntfrq_el0()".into(),
            system::CTR => "rl_mrs_ctr_el0()".into(),
            system::DCZID => "rl_mrs_dczid_el0()".into(),
            system::DC_ZVA => format!("rl_dc_zva(mem, {})", arg(0)),
            system::CACHE_MAINT => format!("rl_cache_maint({})", arg(0)),
            system::WFE => "rl_wfe(c)".into(),
            system::SEV => "rl_sev()".into(),
            _ => return None,
        })
    }

    fn trap(&self, code: u32) -> String {
        format!("rl_svc(c, mem, {code:#x}u)")
    }

    fn undefined(&self, pc: u64, insn: u64) -> String {
        format!("rl_undefined(c, mem, UINT64_C({pc:#x}), {insn:#x}u)")
    }

    fn monitor(&self) -> &'static str {
        "&c->excl"
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn homes_follow_the_context_layout() {
        let f = A64;
        assert_eq!(f.home(regs::x(30)), Home::Field("c->x[30]".into()));
        assert_eq!(f.home(regs::SP), Home::Field("c->sp".into()));
        assert_eq!(
            f.home(regs::V),
            Home::Flag {
                word: "c->nzcv".into(),
                bit: 28
            }
        );
        assert_eq!(f.home(regs::vreg(31)), Home::Field("c->v[31].q".into()));
        assert_eq!(
            f.home(regs::TPIDRRO_EL0),
            Home::Field("c->tpidrro_el0".into())
        );
        assert_eq!(f.regs().len(), 72);
    }

    #[test]
    fn lifts_and_reports_failures() {
        let f = A64;
        assert!(f.lift(&0xd503_201fu32.to_le_bytes(), 0x100).is_ok());
        let e = f.lift(&[0, 0, 0, 0], 0x100).expect_err("udf");
        assert_eq!(e.insn, 0);
        assert!(f.lift(&[0, 0], 0x100).is_err());
    }
}

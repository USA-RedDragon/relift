//! The A64 [`Frontend`]: lifting through [`crate::lift`] and the A64 half of
//! the lifted-code ABI (`runtime/cpus/a64/include/relift/a64/abi.h`).

use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::{Block, FpOp, FpRound, Reg, RegFile, Width};

use crate::lift::{self, sail, system};
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

    fn abi_headers(&self) -> &'static [&'static str] {
        &["relift/a64/abi.h", "relift/a64/fp_slow.h"]
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

    fn fp(
        &self,
        op: FpOp,
        from: Width,
        to: Width,
        args: &[String],
        reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        let fpcr = reg(regs::FPCR);
        let (f, t) = (from.bits(), to.bits());
        let arg = |i: usize| args.get(i).cloned().unwrap_or_default();
        let host = |sym: &str| {
            let (val, bits) = if t == 32 {
                ("rl_e_f32", "rl_e_b32")
            } else {
                ("rl_e_f64", "rl_e_b64")
            };
            format!("{bits}({val}({}) {sym} {val}({}))", arg(0), arg(1))
        };
        let fast =
            |host: String, slow: String| format!("(rl_fp_fast({fpcr}, 0) ? {host} : {slow})");
        Some(match op {
            FpOp::Add | FpOp::Sub | FpOp::Mul | FpOp::Div => {
                let (name, sym) = match op {
                    FpOp::Add => ("add", "+"),
                    FpOp::Sub => ("sub", "-"),
                    FpOp::Mul => ("mul", "*"),
                    _ => ("div", "/"),
                };
                fast(
                    host(sym),
                    format!("rl_fp{t}_{name}({}, {}, {fpcr}, 0)", arg(0), arg(1)),
                )
            }
            FpOp::Sqrt => format!("rl_fp{t}_sqrt({}, {fpcr}, 0)", arg(0)),
            FpOp::MulAdd => format!(
                "rl_fp{t}_muladd({}, {}, {}, {fpcr}, 0)",
                arg(2),
                arg(0),
                arg(1)
            ),
            FpOp::RoundInt(r) => {
                format!("rl_fp{t}_round_int({}, {}, 0, {fpcr}, 0)", arg(0), rmode(r))
            }
            FpOp::ToSInt(r) | FpOp::ToUInt(r) => format!(
                "rl_fp_to_fixed({}, {f}, {t}, 0, {}, {}, {fpcr}, 0)",
                arg(0),
                u8::from(matches!(op, FpOp::ToUInt(_))),
                rmode(r)
            ),
            FpOp::FromSInt | FpOp::FromUInt => format!(
                "rl_fixed_to_fp({}, {f}, {t}, 0, {}, RL_RM_FPCR, {fpcr}, 0)",
                arg(0),
                u8::from(op == FpOp::FromUInt)
            ),
            FpOp::Convert => format!("rl_fp_convert({}, {f}, {t}, RL_RM_FPCR, {fpcr}, 0)", arg(0)),
            FpOp::Neg | FpOp::Abs => return None,
        })
    }

    fn fp_state(&self) -> Vec<Reg> {
        vec![regs::FPCR]
    }

    fn guard_state(&self, code: u32) -> Option<Reg> {
        (code == sail::SLOW_FPCR).then_some(regs::FPCR)
    }

    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        (code == sail::SLOW_FPCR)
            .then(|| format!("rl_fp_slow(c, mem, UINT64_C({pc:#x}), {insn:#x}u)"))
    }
}

/// The runtime's `enum rl_rmode` value for an IR rounding direction.
fn rmode(r: FpRound) -> &'static str {
    match r {
        FpRound::TiesEven => "RL_RM_NEAREST",
        FpRound::Ceil => "RL_RM_POSINF",
        FpRound::Floor => "RL_RM_NEGINF",
        FpRound::Zero => "RL_RM_ZERO",
        FpRound::TiesAway => "RL_RM_AWAY",
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

    #[test]
    fn the_fpcr_guard_is_hoisted_and_has_a_slow_path() {
        let f = A64;
        assert_eq!(f.guard_state(sail::SLOW_FPCR), Some(regs::FPCR));
        assert_eq!(
            f.guard_slow(sail::SLOW_FPCR, 0x40, 0x1e602820).as_deref(),
            Some("rl_fp_slow(c, mem, UINT64_C(0x40), 0x1e602820u)")
        );
        assert_eq!(f.guard_slow(1, 0x40, 0), None);
    }

    #[test]
    fn fp_ops_take_the_fpcr_local() {
        let f = A64;
        let name = |r: Reg| format!("r{}", r.index());
        let args = ["a".to_string(), "b".to_string(), "c".to_string()];
        let add = f
            .fp(FpOp::Add, Width::W64, Width::W64, &args, &name)
            .expect("add");
        assert!(add.contains("rl_fp_fast(r68, 0)") && add.contains("rl_fp64_add(a, b, r68, 0)"));
        let fma = f
            .fp(FpOp::MulAdd, Width::W32, Width::W32, &args, &name)
            .expect("fma");
        assert_eq!(fma, "rl_fp32_muladd(c, a, b, r68, 0)");
        let cvt = f
            .fp(
                FpOp::ToUInt(FpRound::Zero),
                Width::W64,
                Width::W32,
                &args,
                &name,
            )
            .expect("cvt");
        assert_eq!(cvt, "rl_fp_to_fixed(a, 64, 32, 0, 1, RL_RM_ZERO, r68, 0)");
        assert!(
            f.fp(FpOp::Neg, Width::W32, Width::W32, &args, &name)
                .is_none()
        );
    }
}

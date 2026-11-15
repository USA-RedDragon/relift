//! The A64 [`Frontend`]: lifting through [`crate::lift`] and the A64 half of
//! the lifted-code ABI (`runtime/cpus/arm64/include/relift/arm64/abi.h`).

use relift_guest::{Frontend, Home, LiftFailure, WasmIntrinsic};
use relift_ir::{Block, CryptoOp, FpOp, FpRound, Reg, RegFile, Width};

use crate::decode::Features;
use crate::lift::{self, sail, system};
use crate::regs;

/// `CNTFRQ_EL0`, `RL_CNTFRQ` in `abi.h`.
const CNTFRQ: u128 = 19_200_000;
/// `CTR_EL0`, `RL_CTR_EL0` in `abi.h`.
const CTR_EL0: u128 = 0x8444_c004;
/// `DCZID_EL0`, `RL_DCZID_EL0` in `abi.h`.
const DCZID_EL0: u128 = 0x4;
/// The `DC ZVA` block size, `RL_DC_ZVA_BYTES` in `abi.h`.
const DC_ZVA_BYTES: u32 = 64;

/// The A64 frontend for a CPU with the architecture features `features`
/// (the guest's CPU, from its descriptor).
#[derive(Clone, Copy, Debug)]
pub struct Arm64 {
    /// The CPU's features: the decoder refuses words of encodings the CPU
    /// lacks and templates take the CPU's branch of feature tests.
    pub features: Features,
}

impl Frontend for Arm64 {
    fn granule(&self) -> u64 {
        4
    }

    fn regs(&self) -> RegFile {
        regs::regs()
    }

    fn stack_pointer(&self) -> Option<Reg> {
        Some(regs::SP)
    }

    fn pad_resume(&self) -> Option<&'static str> {
        Some("rl_pad_resume")
    }

    fn preserved(&self) -> Vec<Reg> {
        (19..=29).map(regs::x).chain([regs::SP]).collect()
    }

    fn callee_saved(&self) -> Vec<Reg> {
        self.preserved()
    }

    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        let Some(w) = bytes.get(..4) else {
            return Err(LiftFailure {
                insn: 0,
                message: "past the end of the image".into(),
            });
        };
        let word = u32::from_le_bytes([w[0], w[1], w[2], w[3]]);
        lift::lift_for_emit(word, pc, &self.features).map_err(|e| LiftFailure {
            insn: u64::from(word),
            message: e.to_string(),
        })
    }

    fn abi_headers(&self) -> &'static [&'static str] {
        &[
            "relift/arm64/abi.h",
            "relift/arm64/fp_slow.h",
            "relift/arm64/fp16.h",
            "relift/arm64/crypto.h",
        ]
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
            _ if reg == regs::FP_FLAGS => Home::Field("RL_FP_FLAGS_ON".into()),
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
            system::MRS_FPSR => format!("rl_mrs_fpsr_flags({})", arg(0)),
            system::MSR_FPSR => "rl_msr_fpsr_flags()".into(),
            system::FP_RAISE => format!("rl_fp_raise({})", arg(0)),
            _ => return lift::native::c_call(name, args),
        })
    }

    fn wasm_intrinsic(&self, name: &str) -> Option<WasmIntrinsic> {
        Some(match name {
            system::CNTPCT | system::WFE => WasmIntrinsic::Import,
            system::CNTFRQ => WasmIntrinsic::Const(CNTFRQ),
            system::CTR => WasmIntrinsic::Const(CTR_EL0),
            system::DCZID => WasmIntrinsic::Const(DCZID_EL0),
            system::DC_ZVA => WasmIntrinsic::ZeroBlock(DC_ZVA_BYTES),
            system::CACHE_MAINT | system::SEV => WasmIntrinsic::Nop,
            _ => return None,
        })
    }

    fn trap(&self, code: u32) -> String {
        if code & crate::lift::system::TRAP_BRK != 0 {
            return format!(
                "rl_brk(c, mem, {:#x}u)",
                code & !crate::lift::system::TRAP_BRK
            );
        }
        format!("rl_svc(c, mem, {code:#x}u)")
    }

    fn undefined(&self, pc: u64, insn: u64) -> String {
        format!("rl_undefined(c, mem, UINT64_C({pc:#x}), {insn:#x}u)")
    }

    fn monitor(&self) -> &'static str {
        "&c->excl"
    }

    fn preempt_clock(&self) -> Option<&'static str> {
        Some("c->time")
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
        // The binary16 helpers take over when the operation's floating-point
        // format is half precision: the source of a conversion to an
        // integer, the result of one from an integer, either side of a
        // format conversion. An integer side of 16 bits is not.
        let half_fmt = match op {
            FpOp::ToSInt(_) | FpOp::ToUInt(_) => f == 16,
            FpOp::FromSInt | FpOp::FromUInt => t == 16,
            FpOp::Convert => f == 16 || t == 16,
            _ => t == 16,
        };
        if half_fmt {
            return half(op, f, t, &arg);
        }
        Some(match op {
            FpOp::Add | FpOp::Sub | FpOp::Mul | FpOp::Div => {
                // The inline host operation with its NaN and (in programs
                // that produce FPSR flags) tininess fix-ups, abi.h.
                let name = match op {
                    FpOp::Add => "add",
                    FpOp::Sub => "sub",
                    FpOp::Mul => "mul",
                    _ => "div",
                };
                format!("rl_f{name}{t}({}, {}, {fpcr}, 0)", arg(0), arg(1))
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

    fn crypto(&self, op: CryptoOp, arg: &str) -> Option<String> {
        Some(match op {
            CryptoOp::AesSubBytes => format!("rl_aes_subbytes({arg})"),
            CryptoOp::AesInvSubBytes => format!("rl_aes_invsubbytes({arg})"),
            CryptoOp::AesMixColumns => format!("rl_aesmc({arg})"),
            CryptoOp::AesInvMixColumns => format!("rl_aesimc({arg})"),
            CryptoOp::Sm4Sbox => format!("rl_sm4_sbox((uint8_t)({arg}))"),
        })
    }

    fn fp_state(&self) -> Vec<Reg> {
        vec![regs::FPCR]
    }

    fn guard_state(&self, code: u32) -> Option<Reg> {
        match code {
            sail::SLOW_FPCR => Some(regs::FPCR),
            sail::SLOW_FLAGS => Some(regs::FP_FLAGS),
            _ => None,
        }
    }

    fn guard_flag(&self, r: Reg, value: &str) -> String {
        if r == regs::FPCR {
            format!("RL_FPCR_GUARD({value})")
        } else {
            format!("{value} != 0")
        }
    }

    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        matches!(code, sail::SLOW_FPCR | sail::SLOW_FLAGS)
            .then(|| format!("rl_fp_slow(c, mem, UINT64_C({pc:#x}), {insn:#x}u)"))
    }
}

/// A half-precision operation (an operand or the result binary16) as a call
/// to `relift/arm64/fp16.h`. Only the fast path: a block with FP starts with
/// the FPCR guard, so these run with FPCR zero.
fn half(op: FpOp, f: u32, t: u32, arg: &dyn Fn(usize) -> String) -> Option<String> {
    Some(match op {
        FpOp::Add => format!("rl_fp16_add({}, {})", arg(0), arg(1)),
        FpOp::Sub => format!("rl_fp16_sub({}, {})", arg(0), arg(1)),
        FpOp::Mul => format!("rl_fp16_mul({}, {})", arg(0), arg(1)),
        FpOp::Div => format!("rl_fp16_div({}, {})", arg(0), arg(1)),
        FpOp::Sqrt => format!("rl_fp16_sqrt({})", arg(0)),
        FpOp::MulAdd => format!("rl_fp16_muladd({}, {}, {})", arg(0), arg(1), arg(2)),
        FpOp::RoundInt(r) => format!("rl_fp16_round_int({}, {})", arg(0), rmode(r)),
        FpOp::ToSInt(r) | FpOp::ToUInt(r) => format!(
            "rl_fp16_to_int({}, {t}, {}, {})",
            arg(0),
            u8::from(matches!(op, FpOp::ToUInt(_))),
            rmode(r)
        ),
        FpOp::FromSInt | FpOp::FromUInt => format!(
            "rl_fp16_from_int((uint64_t)({}), {f}, {})",
            arg(0),
            u8::from(op == FpOp::FromUInt)
        ),
        FpOp::Convert => match (f, t) {
            (16, 32) => format!("rl_fp16_to_f32({})", arg(0)),
            (16, 64) => format!("rl_fp16_to_f64({})", arg(0)),
            (32, 16) => format!("rl_fp16_from_f32({})", arg(0)),
            (64, 16) => format!("rl_fp16_from_f64({})", arg(0)),
            _ => arg(0),
        },
        FpOp::Neg | FpOp::Abs => return None,
    })
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

    const ARM64: Arm64 = Arm64 {
        features: Features::ALL,
    };

    #[test]
    fn homes_follow_the_context_layout() {
        let f = ARM64;
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
        assert_eq!(f.regs().len(), 73);
    }

    #[test]
    fn wasm_intrinsics_match_the_c_header() {
        let h = include_str!("../../../runtime/cpus/arm64/include/relift/arm64/abi.h");
        let define = |name: &str| -> u128 {
            let line = h
                .lines()
                .find(|l| l.starts_with(&format!("#define {name} ")))
                .unwrap_or_else(|| panic!("abi.h defines {name}"));
            let v = line[format!("#define {name} ").len()..]
                .trim()
                .trim_end_matches('u');
            match v.strip_prefix("0x") {
                Some(hex) => u128::from_str_radix(hex, 16).expect("hex"),
                None => v.parse().expect("decimal"),
            }
        };
        assert_eq!(CNTFRQ, define("RL_CNTFRQ"));
        assert_eq!(CTR_EL0, define("RL_CTR_EL0"));
        assert_eq!(DCZID_EL0, define("RL_DCZID_EL0"));
        assert_eq!(u128::from(DC_ZVA_BYTES), define("RL_DC_ZVA_BYTES"));
        assert_eq!(4u128 << (DCZID_EL0 & 0xf), u128::from(DC_ZVA_BYTES));
        let f = ARM64;
        for name in [
            system::CNTPCT,
            system::CNTFRQ,
            system::CTR,
            system::DCZID,
            system::DC_ZVA,
            system::CACHE_MAINT,
            system::WFE,
            system::SEV,
        ] {
            assert!(f.intrinsic(name, &["a".into()]).is_some(), "{name} in C");
            assert!(f.wasm_intrinsic(name).is_some(), "{name} in wasm");
        }
        assert_eq!(f.wasm_intrinsic("arm64.none"), None);
    }

    #[test]
    fn lifts_and_reports_failures() {
        let f = ARM64;
        assert!(f.lift(&0xd503_201fu32.to_le_bytes(), 0x100).is_ok());
        let e = f.lift(&[0, 0, 0, 0], 0x100).expect_err("udf");
        assert_eq!(e.insn, 0);
        assert!(f.lift(&[0, 0], 0x100).is_err());
    }

    #[test]
    fn the_fpcr_guard_is_hoisted_and_has_a_slow_path() {
        let f = ARM64;
        assert_eq!(f.guard_state(sail::SLOW_FPCR), Some(regs::FPCR));
        assert_eq!(
            f.guard_slow(sail::SLOW_FPCR, 0x40, 0x1e602820).as_deref(),
            Some("rl_fp_slow(c, mem, UINT64_C(0x40), 0x1e602820u)")
        );
        assert_eq!(f.guard_slow(1, 0x40, 0), None);
        assert_eq!(f.guard_flag(regs::FPCR, "r68"), "RL_FPCR_GUARD(r68)");
        assert_eq!(f.guard_flag(regs::FP_FLAGS, "r72"), "r72 != 0");
    }

    #[test]
    fn fp_ops_take_the_fpcr_local() {
        let f = ARM64;
        let name = |r: Reg| format!("r{}", r.index());
        let args = ["a".to_string(), "b".to_string(), "c".to_string()];
        let add = f
            .fp(FpOp::Add, Width::W64, Width::W64, &args, &name)
            .expect("add");
        assert_eq!(add, "rl_fadd64(a, b, r68, 0)");
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

    #[test]
    fn half_precision_and_cipher_steps_map_to_runtime_helpers() {
        let f = ARM64;
        let name = |r: Reg| format!("r{}", r.index());
        let args = ["a".to_string(), "b".to_string(), "c".to_string()];
        let fp = |op, from, to| f.fp(op, from, to, &args, &name).expect("maps");
        assert_eq!(fp(FpOp::Add, Width::W16, Width::W16), "rl_fp16_add(a, b)");
        assert_eq!(
            fp(FpOp::MulAdd, Width::W16, Width::W16),
            "rl_fp16_muladd(a, b, c)"
        );
        assert_eq!(
            fp(FpOp::Convert, Width::W16, Width::W64),
            "rl_fp16_to_f64(a)"
        );
        assert_eq!(
            fp(FpOp::Convert, Width::W32, Width::W16),
            "rl_fp16_from_f32(a)"
        );
        assert_eq!(
            fp(FpOp::ToSInt(FpRound::Zero), Width::W16, Width::W32),
            "rl_fp16_to_int(a, 32, 0, RL_RM_ZERO)"
        );
        // A double to a 16-bit integer and a 16-bit integer to a double are
        // not half precision.
        assert_eq!(
            fp(FpOp::ToSInt(FpRound::TiesAway), Width::W64, Width::W16),
            "rl_fp_to_fixed(a, 64, 16, 0, 0, RL_RM_AWAY, r1, 0)".replace("r1", &name(regs::FPCR))
        );
        assert_eq!(
            fp(FpOp::FromUInt, Width::W16, Width::W64),
            "rl_fixed_to_fp(a, 16, 64, 0, 1, RL_RM_FPCR, r1, 0)".replace("r1", &name(regs::FPCR))
        );
        assert_eq!(
            f.crypto(CryptoOp::AesMixColumns, "x").as_deref(),
            Some("rl_aesmc(x)")
        );
    }

    #[test]
    fn crypto_instructions_lift_to_one_intrinsic() {
        let f = Arm64 {
            features: Features::CORTEX_A57,
        };
        let aese = 0x4e28_4820u32.to_le_bytes();
        let b = f.lift(&aese, 0x1000).expect("lifts");
        let calls: Vec<&str> = b
            .insts
            .iter()
            .filter_map(|i| match &i.op {
                relift_ir::Op::Intrinsic(call) => Some(call.name),
                _ => None,
            })
            .collect();
        assert_eq!(calls, ["arm64.aese"]);
        let c = f.intrinsic("arm64.aese", &["d".into(), "n".into()]);
        assert_eq!(c.as_deref(), Some("rl_aese(d, n)"));
        let v80 = Arm64 {
            features: Features::ARMV8_0,
        };
        assert!(v80.lift(&aese, 0x1000).is_err());
    }
}

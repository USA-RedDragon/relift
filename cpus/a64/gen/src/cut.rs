//! The cut line: Sail functions the generator does not translate but
//! replaces with hand-written code. Everything not listed here comes from
//! the Sail sources.
//!
//! - STATE: guest register access, through the template's `St`.
//! - CTRL: the PC and branches.
//! - ENV: checks that are no-ops in every configuration (EL0, AArch64, FP
//!   and SIMD enabled, no SPE, BRBE or GCS).
//! - FEAT: `IsFeatureImplemented`: true for the Armv8.0-A base, a lift-time
//!   test of the CPU's `Features` (`St::has`) for the extensions in
//!   `eval::LIFT_TIME_FEATURES`, false for every other feature.
//!   A template that reads an absent feature refuses to lift (Unsupported)
//!   on a CPU that has it (except `NO_EL0_EFFECT`), so no semantics of a
//!   missing extension is silently assumed.
//!   `HaveFP16Ext`, which `impdefs.sail` defines as true, is the same test
//!   of `FEAT_FP16`.
//! - MEM: memory access, ordering, the exclusive monitor and barriers
//!   (fences).
//! - FP: arithmetic, compares, min/max, conversions, rounding to integral
//!   values and the reciprocal and square-root estimates and steps, built by
//!   `rt::Fp` for FPCR zero; each use marks the template as assuming it.
//! - CRYPTO: the table-driven AES and SM4 steps, as `relift_ir::CryptoOp`;
//!   `verify_crypto` checks them against the Sail tables while generating.
//! - Also `DecodeBitMasks`, `ConstrainUnpredictable` (refused at lift time),
//!   the integer quotient of SDIV and UDIV, and the unsigned estimates of
//!   URECPE and URSQRTE (`rt::urecpe`, `rt::ursqrte`, integer formulas).
//!
//! The generator refuses to inline a function of `stubs.sail` (placeholders
//! for implementation choices) other than hints with no effect.

use std::rc::Rc;

use relift_sail::ast::Expr;
use relift_sail::eval::{Cpu, Gen, R, err, fail};
use relift_sail::stage::Stage;
use relift_sail::value::{K, V};

/// The A64 layer of the evaluator, and its per-run state: the exclusive
/// monitor bookkeeping.
#[derive(Default)]
pub struct A64 {
    /// The next load arms the exclusive monitor for this many bytes.
    pub excl_load: Option<u32>,
    /// The bytes of an exclusive load not yet read: the IR value, its
    /// width in bits and the next bit to read.
    pub excl_rest: Option<(Rc<str>, u32, u32)>,
    /// The pending exclusive-store result, named before the store defines it.
    pub excl_pass: Option<Rc<str>>,
    /// Run the cipher functions from Sail instead of cutting them (to
    /// check the cut).
    pub no_cut: bool,
}

/// Features every configuration has: Armv8.0-A with floating point and
/// Advanced SIMD, AArch64 at every exception level.
const BASE_FEATURES: &[&str] = &[
    "FEAT_AdvSIMD",
    "FEAT_FP",
    "FEAT_AA64EL0",
    "FEAT_AA64EL1",
    "FEAT_AA64EL2",
    "FEAT_AA64EL3",
    "FEAT_EL0",
    "FEAT_EL1",
    "FEAT_EL2",
    "FEAT_EL3",
    "FEAT_AA32EL0",
];

/// Optional features a template may test at lift time (`St::has`): the
/// user-visible instruction-set extensions relift lifts. Every other
/// feature is absent in every configuration.
const LIFT_TIME_FEATURES: &[&str] = &[
    "FEAT_AES",
    "FEAT_PMULL",
    "FEAT_SHA1",
    "FEAT_SHA256",
    "FEAT_SHA512",
    "FEAT_SHA3",
    "FEAT_SM3",
    "FEAT_SM4",
    "FEAT_CRC32",
    "FEAT_LSE",
    "FEAT_LSE128",
    "FEAT_RDM",
    "FEAT_FP16",
    "FEAT_FHM",
    "FEAT_DotProd",
    "FEAT_I8MM",
    "FEAT_BF16",
    "FEAT_FCMA",
    "FEAT_JSCVT",
    "FEAT_FRINTTS",
    "FEAT_FlagM",
    "FEAT_FlagM2",
    "FEAT_LRCPC",
    "FEAT_LRCPC2",
    "FEAT_LRCPC3",
    "FEAT_CSSC",
    "FEAT_HBC",
    "FEAT_SB",
];

/// How a template sees a feature: `Some(true)` for a base feature,
/// `Some(false)` for one no configuration has, `None` for one tested at
/// lift time.
pub fn feature(name: &str) -> Option<bool> {
    if BASE_FEATURES.contains(&name) {
        return Some(true);
    }
    if LIFT_TIME_FEATURES.contains(&name) {
        return None;
    }
    Some(false)
}

/// Stubs of `stubs.sail` whose empty body is right for a user-mode guest:
/// hints with no architectural effect.
const STUBS_OK: &[&str] = &[
    "Hint_Yield",
    "Hint_PreloadData",
    "Hint_PreloadDataForWrite",
    "Hint_PreloadInstr",
];

/// The model files the A64 generator reads, in load order.
pub const A64_SRC_FILES: &[&str] = &[
    "prelude.sail",
    "decode_start.sail",
    "builtins.sail",
    "v8_base.sail",
    "stubs.sail",
    "interrupts.sail",
    "interface.sail",
    "no_devices.sail",
    "impdefs.sail",
    "mem.sail",
    "sysregs_autogen.sail",
    "sysregs.sail",
    "reset.sail",
    "instrs64.sail",
    "decode_end.sail",
    "fetch.sail",
];

impl Cpu for A64 {
    fn cut(g: &mut Gen<'_, A64>, name: &str, a: &[V], hint: Option<u32>) -> R<Option<V>> {
        cut(g, name, a, hint)
    }

    fn init_reg(g: &mut Gen<'_, A64>, name: &str) -> Option<R<V>> {
        match name {
            "__ThisInstrEnc" => Some(match g.m.enum_of.get("__A64") {
                Some((t, i)) => Ok(V::Enum(t.as_str().into(), *i)),
                None => fail("no __A64"),
            }),
            "PSTATE" => Some(default_pstate(g)),
            "SEE" => Some(Ok(V::Int(-1))),
            "__BranchTaken" | "SPESampleInFlight" | "InGuardedPage" => Some(Ok(V::Bool(false))),
            "BTypeNext" => Some(Ok(V::Bits(2, 0))),
            _ => None,
        }
    }

    fn is_register(name: &str) -> bool {
        matches!(name, "SEE" | "__BranchTaken" | "BTypeNext")
    }

    fn read_field(g: &mut Gen<'_, A64>, b: &Expr, f: &str) -> Option<R<V>> {
        if !is_flag(b, f) {
            return None;
        }
        g.used_cut.insert("PSTATE.NZCV");
        Some(Ok(g.dy(K::Bits(1), format!("st.get(b, regs::{f})"))))
    }

    fn assign_field(g: &mut Gen<'_, A64>, b: &Expr, f: &str, v: &V) -> Option<R<()>> {
        if !is_flag(b, f) {
            return None;
        }
        g.used_cut.insert("PSTATE.NZCV");
        let r = (|| {
            let v = g.bitify(v)?;
            let x = g.dx(&v)?;
            let gd = g.gd();
            g.effect(format!("st.set(b, regs::{f}, {x}, {gd});"));
            Ok(())
        })();
        Some(r)
    }

    fn field_width(b: &Expr, f: &str) -> Option<u32> {
        is_flag(b, f).then_some(1)
    }

    fn decode_args(word: V) -> Vec<V> {
        vec![V::S(K::Bits(64), "pc".into(), 0), word]
    }

    fn reg_config(name: &str) -> u128 {
        match name {
            "SCR_EL3" => 1 | (1 << 10),
            "HCR_EL2" => 1 << 31,
            _ => 0,
        }
    }

    fn stub_ok(name: &str) -> bool {
        STUBS_OK.contains(&name)
    }

    fn assign_index(g: &mut Gen<'_, A64>, b: &Expr, i: &Expr, v: &V) -> Option<R<()>> {
        if !(matches!(b, Expr::Id(r) if r == "FPSR") && matches!(i, Expr::Id(f) if f == "QC")) {
            return None;
        }
        // The saturation flag is the one FPSR write outside the FP
        // cut: it is sticky, so a write ORs it in.
        if !matches!(g.defd(v), V::Bits(1, 1)) {
            return Some(fail("FPSR.QC written with a value other than 1"));
        }
        g.used_cut.insert("FPSR.QC");
        let gd = g.gd();
        g.effect(format!("st.set_qc(b, {gd});"));
        Some(Ok(()))
    }
}

fn is_flag(b: &Expr, f: &str) -> bool {
    matches!(b, Expr::Id(n) if n == "PSTATE") && matches!(f, "N" | "Z" | "C" | "V")
}

fn default_pstate(g: &Gen<'_, A64>) -> R<V> {
    let fields =
        g.m.structs
            .get("ProcState")
            .ok_or_else(|| err("no ProcState"))?;
    let mut f = Vec::new();
    for (n, _) in fields {
        let w = match n.as_str() {
            "EL" | "BTYPE" => 2,
            "GE" => 4,
            "IT" => 8,
            "M" => 5,
            _ => 1,
        };
        f.push((n.clone(), V::Bits(w, 0)));
    }
    Ok(V::Struct("ProcState".into(), Rc::new(f)))
}

impl Stage for A64 {
    const PURE_READS: &'static [&'static str] = &["st.x_read(", "st.v_read(", "st.sp_read("];

    fn finish_run(g: &Gen<'_, A64>) -> Result<(), String> {
        if g.cpu.excl_pass.is_some() {
            return Err("exclusive monitor check without a store".into());
        }
        Ok(())
    }
}

fn cut(g: &mut Gen<'_, A64>, name: &str, a: &[V], hint: Option<u32>) -> R<Option<V>> {
    g.a64_cut(name, a, hint)
}

/// The ENV functions: no effect and no value in the fixed configuration.
pub const ENV_NOOPS: &[&str] = &[
    "BranchNotTaken",
    "Hint_Branch",
    "CheckFPEnabled64",
    "CheckFPAdvSIMDEnabled64",
    "AArch64_CheckFPAdvSIMDEnabled",
    "AArch64_CheckFPEnabled",
    "CheckSPAlignment",
    "AArch64_SetLSInstructionSyndrome",
    "AArch64_CheckSystemAccess",
    "CheckSVEEnabled",
    "SPESampleGeneralPurposeLoadStore",
    "SPESampleAddOpOther",
    "Hint_Prefetch",
    "Prefetch",
    "AArch64_CheckFPAdvSIMDTrap",
];

/// Every cut function, for the generated file's header.
pub const CUT: &[&str] = &[
    "X_read",
    "X_set",
    "SP_read",
    "SP_set",
    "V_read",
    "V_set",
    "PC_read",
    "BranchTo",
    "IsFeatureImplemented",
    "Mem_read",
    "Mem_read__1",
    "Mem_read__2",
    "Mem_set",
    "Mem_set__1",
    "Mem_set__2",
    "MemAtomic",
    "AArch64_SetExclusiveMonitors",
    "AArch64_ExclusiveMonitorsPass",
    "AlignmentFault",
    "AArch64_Abort",
    "ClearExclusiveLocal",
    "AArch64_ClearExclusiveLocal",
    "ConstrainUnpredictable",
    "DecodeBitMasks",
    "FPAdd",
    "FPAdd__1",
    "FPSub",
    "FPSub__1",
    "FPMul",
    "FPDiv",
    "FPSqrt",
    "FPMulAdd",
    "FPMulAdd__1",
    "FPCompare",
    "FPCompareEQ",
    "FPCompareGE",
    "FPCompareGT",
    "FPMax",
    "FPMax__1",
    "FPMin",
    "FPMin__1",
    "FPMaxNum",
    "FPMinNum",
    "DataMemoryBarrier",
    "DataSynchronizationBarrier",
    "InstructionSynchronizationBarrier",
    "SpeculationBarrier",
    "SpeculativeStoreBypassBarrierToVA",
    "SpeculativeStoreBypassBarrierToPA",
    "FPRecipEstimate",
    "FPRSqrtEstimate",
    "FPRecipStepFused",
    "FPRSqrtStepFused",
    "FPToFixed",
    "FixedToFP",
    "FPConvert",
    "FPConvert__1",
    "FPRoundInt",
    "FPRoundIntN",
    "UnsignedRecipEstimate",
    "UnsignedRSqrtEstimate",
    "AESSubBytes",
    "AESInvSubBytes",
    "AESMixColumns",
    "AESInvMixColumns",
    "Sbox",
];

/// Absent features whose tests do not change what an EL0 instruction does
/// here: SME (streaming mode is never entered), LSE2 (it makes pair
/// accesses single-copy atomic, the same bytes), GCS (off unless the OS
/// enables it), SPE (profiling) and XS (barrier variants). Every other
/// absent feature a template reads makes it refuse CPUs that have it.
const NO_EL0_EFFECT: &[&str] = &["FEAT_SME", "FEAT_LSE2", "FEAT_GCS", "FEAT_SPE", "FEAT_XS"];

/// The table-driven cipher functions, cut whole to `relift_ir::CryptoOp`.
pub const CRYPTO_CUT: &[(&str, relift_ir::CryptoOp)] = &[
    ("AESSubBytes", relift_ir::CryptoOp::AesSubBytes),
    ("AESInvSubBytes", relift_ir::CryptoOp::AesInvSubBytes),
    ("AESMixColumns", relift_ir::CryptoOp::AesMixColumns),
    ("AESInvMixColumns", relift_ir::CryptoOp::AesInvMixColumns),
    ("Sbox", relift_ir::CryptoOp::Sm4Sbox),
];

/// Checks each cipher cut against the Sail function it replaces, run on
/// concrete values: every byte value in every position (the functions
/// work bytewise or, for MixColumns, are linear over GF(2), so the basis
/// vectors decide them). Fails with the first difference.
pub fn verify_crypto(m: &relift_sail::model::Model) -> Result<(), String> {
    for (name, op) in CRYPTO_CUT {
        let w = op.width().bits();
        let mut inputs: Vec<u128> = Vec::new();
        if name.contains("MixColumns") {
            inputs.extend((0..128).map(|i| 1u128 << i));
        } else {
            for x in 0..256u128 {
                let mut v = 0;
                for k in 0..w / 8 {
                    v |= ((x + u128::from(k) * 37) & 0xff) << (8 * k);
                }
                inputs.push(v);
            }
        }
        for x in inputs {
            let mut g: Gen<'_, A64> = Gen::new(m, 0, 0);
            g.cpu.no_cut = true;
            let got = g
                .call(name, vec![V::Bits(w, x)], None)
                .map_err(|e| format!("{name}({x:#x}) does not evaluate: {e:?}"))?;
            let want = relift_ir::interp::eval_crypto(*op, x);
            if got != V::Bits(w, want) {
                return Err(format!("{name}({x:#x}): Sail {got:?}, cut {want:#x}"));
            }
        }
    }
    Ok(())
}

/// The FP functions cut whole; `rt::Fp` in `relift-cpu-a64` builds them.
const FP_CUT: &[&str] = &[
    "FPAdd",
    "FPAdd__1",
    "FPSub",
    "FPSub__1",
    "FPMul",
    "FPDiv",
    "FPSqrt",
    "FPMulAdd",
    "FPMulAdd__1",
    "FPCompare",
    "FPCompareEQ",
    "FPCompareGE",
    "FPCompareGT",
    "FPMax",
    "FPMax__1",
    "FPMin",
    "FPMin__1",
    "FPMaxNum",
    "FPMinNum",
];

/// The `fp_cut` operations whose binary16 form maps to a software helper
/// of the runtime (`a64/fp16.h`), which raises no host FPSR flags: in a
/// program that produces flags their blocks take the slow path. Compares
/// and min/max build their flags in IR at every width.
const HALF_SOFT: &[&str] = &[
    "FPAdd",
    "FPAdd__1",
    "FPSub",
    "FPSub__1",
    "FPMul",
    "FPDiv",
    "FPSqrt",
    "FPMulAdd",
    "FPMulAdd__1",
];

/// FPCR bits the floating-point cut assumes zero: FIZ, AH, FZ16, RMode, FZ,
/// DN and AHP.
const FPCR_FIXED: u128 = 0b11 | (1 << 19) | (0b11 << 22) | (1 << 24) | (1 << 25) | (1 << 26);

fn real_parts(v: &V) -> Option<(V, V)> {
    match v {
        V::Union(c, x) if c.as_ref() == "$real" => match x.as_ref() {
            V::Tuple(p) if p.len() == 2 => Some((p[0].clone(), p[1].clone())),
            _ => None,
        },
        _ => None,
    }
}

fn real_of(n: V, d: V) -> V {
    V::Union("$real".into(), Box::new(V::tuple(vec![n, d])))
}

trait A64Cut {
    fn real(&mut self, name: &str, a: &[V]) -> R<Option<V>>;
    fn fp_cut(&mut self, name: &str, a: &[V]) -> R<V>;
    fn fp_estimate(&mut self, name: &str, a: &[V]) -> R<V>;
    fn fp_fpcr_ok(&self, name: &str, fpcr: &V) -> R<()>;
    fn fp_round(&self, v: &V) -> R<&'static str>;
    fn fp_round_odd(&self, v: &V) -> bool;
    fn unsigned_estimate(&mut self, name: &str, a: &[V]) -> R<V>;
    fn fp_convert(&mut self, name: &str, a: &[V]) -> R<V>;
    fn reg_no(&self, v: &V) -> R<String>;
    fn order(&self, acc: &V) -> &'static str;
    fn a64_cut(&mut self, name: &str, a: &[V], _hint: Option<u32>) -> R<Option<V>>;
}

impl A64Cut for Gen<'_, A64> {
    /// REAL: the one use of reals in the integer instructions, an integer
    /// quotient rounded toward zero (`SDIV`, `UDIV`), kept as a fraction.
    fn real(&mut self, name: &str, a: &[V]) -> R<Option<V>> {
        Ok(Some(match name {
            "to_real" | "Real" if a.len() == 1 && a[0].kind() == relift_sail::value::Kind::Int => {
                real_of(a[0].clone(), V::Int(1))
            }
            "op/" | "div_real" if a.len() == 2 => {
                let (Some((n1, d1)), Some((n2, d2))) = (real_parts(&a[0]), real_parts(&a[1]))
                else {
                    return Ok(None);
                };
                if d1 != V::Int(1) || d2 != V::Int(1) {
                    return fail("real division of fractions");
                }
                real_of(n1, n2)
            }
            "RoundTowardsZero" if a.len() == 1 => {
                let Some((n, d)) = real_parts(&a[0]) else {
                    return fail("RoundTowardsZero of a non-fraction");
                };
                if d == V::Int(1) {
                    n
                } else {
                    self.int_op(relift_sail::ops::IOp::TDiv, &n, &d)?
                }
            }
            _ => return Ok(None),
        }))
    }

    fn fp_cut(&mut self, name: &str, a: &[V]) -> R<V> {
        let nops = match name {
            "FPSqrt" => 1,
            "FPMulAdd" | "FPMulAdd__1" => 3,
            _ => 2,
        };
        let fpcr = if name == "FPCompare" { &a[3] } else { &a[nops] };
        let bits = match fpcr {
            V::Bf(_, b) => (**b).clone(),
            v => v.clone(),
        };
        match bits {
            V::Bits(_, x) if x & FPCR_FIXED == 0 => {}
            _ => return fail(format!("{name} with FPCR {bits:?}")),
        }
        if matches!(name, "FPMax__1" | "FPMin__1") && a.get(3) != Some(&V::Bool(false)) {
            return fail("alternative FP min/max");
        }
        let w = a[0]
            .width()
            .ok_or_else(|| relift_sail::eval::err("FP operand width"))?;
        if w != 16 && w != 32 && w != 64 {
            return fail(format!("{name} on {w} bits"));
        }
        let mut xs = Vec::new();
        for v in &a[..nops] {
            let v = self.bitify(v)?;
            xs.push(self.dx(&v)?.to_string());
        }
        let f = match self.gd().as_str() {
            "None" => format!("rt::Fp::new({w})"),
            g => format!("rt::Fp::new({w}).guarded({g})"),
        };
        let two = |m: &str| format!("{f}.{m}(b, {}, {})", xs[0], xs[1]);
        let (k, call) = match name {
            "FPAdd" | "FPAdd__1" => (K::Bits(w), two("bin").replace("(b, ", "(b, FpOp::Add, ")),
            "FPSub" | "FPSub__1" => (K::Bits(w), two("bin").replace("(b, ", "(b, FpOp::Sub, ")),
            "FPMul" => (K::Bits(w), two("bin").replace("(b, ", "(b, FpOp::Mul, ")),
            "FPDiv" => (K::Bits(w), two("bin").replace("(b, ", "(b, FpOp::Div, ")),
            "FPSqrt" => (K::Bits(w), format!("{f}.sqrt(b, {})", xs[0])),
            "FPMulAdd" | "FPMulAdd__1" => (
                K::Bits(w),
                format!("{f}.mul_add(b, {}, {}, {})", xs[0], xs[1], xs[2]),
            ),
            "FPCompare" => {
                let signal = match &a[2] {
                    V::Bool(s) => *s,
                    V::S(_, _, d) if *d != 0 => return Err(relift_sail::eval::E::Split(*d)),
                    v => return fail(format!("FPCompare signal {v:?}")),
                };
                (
                    K::Bits(4),
                    format!("{f}.compare(b, {}, {}, {signal})", xs[0], xs[1]),
                )
            }
            "FPCompareEQ" => (K::Bool, two("compare_eq")),
            "FPCompareGE" => (K::Bool, two("compare_ge")),
            "FPCompareGT" => (K::Bool, two("compare_gt")),
            "FPMax" | "FPMax__1" => (K::Bits(w), two("max")),
            "FPMin" | "FPMin__1" => (K::Bits(w), two("min")),
            "FPMaxNum" => (K::Bits(w), two("max_num")),
            _ => (K::Bits(w), two("min_num")),
        };
        self.line("st.assume_default_fpcr();".into());
        if w == 16 && HALF_SOFT.contains(&name) {
            // The runtime's binary16 helpers compute in software and
            // raise no host flags.
            self.line("st.assume_flags_slow();".into());
        }
        Ok(self.dy(k, call))
    }

    fn fp_estimate(&mut self, name: &str, a: &[V]) -> R<V> {
        if name.ends_with("Estimate") {
            self.fp_fpcr_ok(name, &a[1])?;
        }
        let x = self.bitify(&a[0])?;
        let w = x
            .width()
            .ok_or_else(|| relift_sail::eval::err("FP operand width"))?;
        if w != 16 && w != 32 && w != 64 {
            return fail(format!("{name} on {w} bits"));
        }
        let xs = self.dx(&x)?.to_string();
        let call = match name {
            "FPRecipEstimate" => format!("rt::Fp::new({w}).recip_estimate(b, {xs})"),
            "FPRSqrtEstimate" => format!("rt::Fp::new({w}).rsqrt_estimate(b, {xs})"),
            _ => {
                let y = self.bitify(&a[1])?;
                let ys = self.dx(&y)?.to_string();
                let m = if name == "FPRecipStepFused" {
                    "recip_step"
                } else {
                    "rsqrt_step"
                };
                format!("rt::Fp::new({w}).{m}(b, {xs}, {ys})")
            }
        };
        self.line("st.assume_default_fpcr();".into());
        if w == 16 && !name.ends_with("Estimate") {
            // The steps' binary16 multiply-adds use the runtime's software
            // helpers, which raise no host flags.
            self.line("st.assume_flags_slow();".into());
        }
        Ok(self.dy(K::Bits(w), call))
    }

    fn fp_fpcr_ok(&self, name: &str, fpcr: &V) -> R<()> {
        let bits = match fpcr {
            V::Bf(_, b) => (**b).clone(),
            v => v.clone(),
        };
        match bits {
            V::Bits(_, x) if x & FPCR_FIXED == 0 => Ok(()),
            _ => fail(format!("{name} with FPCR {bits:?}")),
        }
    }

    /// The `FpRound` of a concrete `FPRounding`, or a split on its bits.
    fn fp_round(&self, v: &V) -> R<&'static str> {
        let i = match v {
            V::Enum(_, i) => *i,
            V::S(_, _, d) if *d != 0 => return Err(relift_sail::eval::E::Split(*d)),
            _ => return fail(format!("rounding {v:?}")),
        };
        let names = &self.m.enums["FPRounding"];
        Ok(match names[i].as_str() {
            "FPRounding_TIEEVEN" => "TiesEven",
            "FPRounding_TIEAWAY" => "TiesAway",
            "FPRounding_NEGINF" => "Floor",
            "FPRounding_POSINF" => "Ceil",
            "FPRounding_ZERO" => "Zero",
            r => return fail(format!("rounding {r}")),
        })
    }

    /// Whether `v` is the concrete `FPRounding_ODD`.
    fn fp_round_odd(&self, v: &V) -> bool {
        matches!(v, V::Enum(_, i) if self.m.enums["FPRounding"][*i] == "FPRounding_ODD")
    }

    /// `UnsignedRecipEstimate` or `UnsignedRSqrtEstimate` on 32 bits.
    fn unsigned_estimate(&mut self, name: &str, a: &[V]) -> R<V> {
        let x = self.bitify(&a[0])?;
        if x.width() != Some(32) {
            return fail(format!("{name} on {:?} bits", x.width()));
        }
        let xs = self.dx(&x)?.to_string();
        let f = if name == "UnsignedRecipEstimate" {
            "urecpe"
        } else {
            "ursqrte"
        };
        Ok(self.dy(K::Bits(32), format!("rt::{f}(b, {xs})")))
    }

    fn fp_convert(&mut self, name: &str, a: &[V]) -> R<V> {
        let fpw = |w: u32| -> R<u32> {
            if w == 16 || w == 32 || w == 64 {
                Ok(w)
            } else {
                fail(format!("{name} on {w} bits"))
            }
        };
        let x = self.bitify(&a[0])?;
        let iw = fpw(x
            .width()
            .ok_or_else(|| relift_sail::eval::err("FP operand width"))?)?;
        let xs = self.dx(&x)?.to_string();
        let (k, call) = match name {
            "FPToFixed" | "FixedToFP" => {
                self.fp_fpcr_ok(name, &a[3])?;
                let fbits = self.cint(&a[1])?;
                let unsigned = match &a[2] {
                    V::Bool(u) => *u,
                    V::S(_, _, d) if *d != 0 => return Err(relift_sail::eval::E::Split(*d)),
                    v => return fail(format!("signedness {v:?}")),
                };
                let r = self.fp_round(&a[4])?;
                let ow = fpw(self.cint(&a[5])? as u32)?;
                let signed = !unsigned;
                if name == "FPToFixed" && iw == 16 {
                    (
                        K::Bits(ow),
                        format!("half::to_fixed(b, {xs}, {fbits}, {signed}, FpRound::{r}, {ow})"),
                    )
                } else if name == "FPToFixed" {
                    (
                        K::Bits(ow),
                        format!(
                            "rt::Fp::new({iw}).fp_to_fixed(b, {xs}, {fbits}, {signed}, FpRound::{r}, {ow})"
                        ),
                    )
                } else if ow == 16 && fbits != 0 {
                    if r != "TiesEven" {
                        return fail(format!("{name} rounding {r}"));
                    }
                    (
                        K::Bits(16),
                        format!("half::fixed_to_half(b, {xs}, {fbits}, {signed})"),
                    )
                } else {
                    if r != "TiesEven" {
                        return fail(format!("{name} rounding {r}"));
                    }
                    (
                        K::Bits(ow),
                        format!("rt::Fp::new({ow}).fixed_to_fp(b, {xs}, {fbits}, {signed})"),
                    )
                }
            }
            "FPConvert" | "FPConvert__1" => {
                self.fp_fpcr_ok(name, &a[1])?;
                let odd = name == "FPConvert" && self.fp_round_odd(&a[2]);
                let ow_v = if name == "FPConvert" {
                    if !odd && self.fp_round(&a[2])? != "TiesEven" {
                        return fail("FPConvert rounding");
                    }
                    &a[3]
                } else {
                    &a[2]
                };
                let ow = fpw(self.cint(ow_v)? as u32)?;
                if odd {
                    if ow >= iw {
                        return fail("FPConvert rounding to odd to a wider format");
                    }
                    // Rounding to odd overflows and underflows where the
                    // host's nearest-even conversion does not.
                    self.line("st.assume_flags_slow();".into());
                }
                let m = if odd { "convert_odd" } else { "convert" };
                (
                    K::Bits(ow),
                    format!("rt::Fp::new({iw}).{m}(b, {xs}, &rt::Fp::new({ow}))"),
                )
            }
            "FPRoundIntN" => {
                self.fp_fpcr_ok(name, &a[1])?;
                let r = self.fp_round(&a[2])?;
                let n = self.cint(&a[3])?;
                if n != 32 && n != 64 {
                    return fail(format!("{name} to {n} bits"));
                }
                // The invalid flag of an out-of-range result is not a host
                // flag of the rounding.
                self.line("st.assume_flags_slow();".into());
                (
                    K::Bits(iw),
                    format!("rt::Fp::new({iw}).round_int_n(b, {xs}, FpRound::{r}, {n})"),
                )
            }
            _ => {
                self.fp_fpcr_ok(name, &a[1])?;
                let r = self.fp_round(&a[2])?;
                let exact = match &a[3] {
                    V::Bool(e) => *e,
                    V::S(_, _, d) if *d != 0 => return Err(relift_sail::eval::E::Split(*d)),
                    v => return fail(format!("FPRoundInt exact {v:?}")),
                };
                (
                    K::Bits(iw),
                    format!("rt::Fp::new({iw}).round_int(b, {xs}, FpRound::{r}, {exact})"),
                )
            }
        };
        self.line("st.assume_default_fpcr();".into());
        let ow = match k {
            K::Bits(w) => w,
            _ => 0,
        };
        if iw == 16 || ow == 16 {
            // Conversions from or to binary16 use the runtime's software
            // helpers, which raise no host flags.
            self.line("st.assume_flags_slow();".into());
        }
        Ok(self.dy(k, call))
    }

    fn reg_no(&self, v: &V) -> R<String> {
        match v {
            V::Int(i) => Ok(format!("{i}")),
            V::S(K::Int, e, _) => Ok(format!("{e} as u32")),
            _ => fail(format!("register number {v:?}")),
        }
    }

    fn order(&self, acc: &V) -> &'static str {
        let V::Struct(_, f) = acc else {
            return "Plain";
        };
        let get = |n: &str| f.iter().any(|(k, v)| k == n && *v == V::Bool(true));
        let acq = get("acqsc") || get("acqpc");
        let rel = get("relsc");
        match (acq, rel) {
            (true, true) => "AcqRel",
            (true, false) => "Acquire",
            (false, true) => "Release",
            (false, false) => "Plain",
        }
    }

    /// Runs the cut function `name`, or gives `None` when `name` is not cut.
    fn a64_cut(&mut self, name: &str, a: &[V], _hint: Option<u32>) -> R<Option<V>> {
        if self.cpu.no_cut && CRYPTO_CUT.iter().any(|(n, _)| *n == name) {
            return Ok(None);
        }
        if name == "HaveFP16Ext" {
            self.used_cut.insert("FEAT");
            return Ok(Some(self.st(K::Bool, "st.has(Feature::FP16)".into(), 0)));
        }
        if let Some(v) = self.real(name, a)? {
            self.used_cut.insert("REAL");
            return Ok(Some(v));
        }
        if ENV_NOOPS.contains(&name) {
            self.used_cut.insert("ENV");
            return Ok(Some(V::Unit));
        }
        let Some(&key) = CUT.iter().find(|c| **c == name) else {
            return Ok(None);
        };
        self.used_cut.insert(key);
        let v = match name {
            "X_read" => {
                let n = self.reg_no(&a[0])?;
                let w = self.cint(&a[1])?;
                self.dy(K::Bits(w as u32), format!("st.x_read(b, {n}, {w})"))
            }
            "X_set" => {
                let n = self.reg_no(&a[0])?;
                let v = self.bitify(&a[2])?;
                let x = self.dx(&v)?;
                let g = self.gd();
                self.effect(format!("st.x_set(b, {n}, {x}, {g});"));
                V::Unit
            }
            "SP_read" => self.dy(K::Bits(64), "st.sp_read(b)".into()),
            "V_read" => {
                let n = self.reg_no(&a[0])?;
                let w = self.cint(&a[1])?;
                self.dy(K::Bits(w as u32), format!("st.v_read(b, {n}, {w})"))
            }
            "V_set" => {
                let n = self.reg_no(&a[0])?;
                let v = self.bitify(&a[2])?;
                let x = self.dx(&v)?;
                let g = self.gd();
                self.effect(format!("st.v_set(b, {n}, {x}, {g});"));
                V::Unit
            }
            "SP_set" => {
                let v = self.extend(&a[0], 64, false)?;
                let x = self.dx(&v)?;
                let g = self.gd();
                self.effect(format!("st.sp_set(b, {x}, {g});"));
                V::Unit
            }
            "PC_read" => V::S(K::Bits(64), "pc".into(), 0),
            "BranchTo" => {
                let t = self.extend(&a[0], 64, false)?;
                let flow = match &a[1] {
                    V::Enum(_, i) => {
                        let names = &self.m.enums["BranchType"];
                        match names[*i].as_str() {
                            "BranchType_DIRCALL" | "BranchType_INDCALL" => "Call",
                            "BranchType_RET" => "Return",
                            _ => "Jump",
                        }
                    }
                    _ => return fail("symbolic branch type"),
                };
                let g = self.gd();
                let stmt = if t.is_dyn() {
                    let x = self.dx(&t)?;
                    format!("st.branch(b, Target::Dynamic({x}), Kind::{flow}, {g});")
                } else {
                    let x = self.sx(&t)?;
                    format!("st.branch(b, Target::Const({x} as u64), Kind::{flow}, {g});")
                };
                self.effect(stmt);
                V::Unit
            }
            "IsFeatureImplemented" => match &a[0] {
                V::Enum(t, i) => {
                    let name = self.m.enums[t.as_ref()][*i].clone();
                    match feature(&name) {
                        Some(false) => {
                            if let Some(f) = relift_cpu_a64::decode::Feature::from_name(&name)
                                && !NO_EL0_EFFECT.contains(&name.as_str())
                            {
                                self.line(format!(
                                    "if st.has(Feature::{f:?}) {{ return Err(LiftError::Unsupported(ENC)); }}"
                                ));
                            }
                            V::Bool(false)
                        }
                        Some(b) => V::Bool(b),
                        None => {
                            let f = relift_cpu_a64::decode::Feature::from_name(&name).ok_or_else(
                                || relift_sail::eval::err(format!("no Feature {name}")),
                            )?;
                            self.st(K::Bool, format!("st.has(Feature::{f:?})"), 0)
                        }
                    }
                }
                _ => return fail("symbolic feature"),
            },
            "Mem_read" | "Mem_read__1" | "Mem_read__2" => {
                let size = self.cint(&a[1])? as u32;
                let addr = self.extend(&a[0], 64, false)?;
                let x = self.dx(&addr)?;
                if self.guard.d.is_some() {
                    return fail("load under a dynamic condition");
                }
                if let Some((v, w, lo)) = self.cpu.excl_rest.take() {
                    // The next part of a pair the exclusive load read whole.
                    if lo + size * 8 > w {
                        return fail("read past the exclusive load");
                    }
                    if lo + size * 8 < w {
                        self.cpu.excl_rest = Some((v.clone(), w, lo + size * 8));
                    }
                    return Ok(Some(self.dy(
                        K::Bits(size * 8),
                        format!("b.extract({v}, {lo}, Width::new({}))", size * 8),
                    )));
                }
                let order = self.order(&a[2]);
                let mon = self.cpu.excl_load.take();
                let excl = mon.is_some();
                // An exclusive load reads the monitor's whole span at once
                // (a pair of 64-bit registers reads as one 16-byte access).
                let size = mon.map_or(size, |m| m.max(size));
                let part = self.cint(&a[1])? as u32;
                let f = if excl { "load_exclusive" } else { "load" };
                if excl || order != "Plain" {
                    self.effect(format!("align::check(b, st, {x}, {size}, {excl});"));
                }
                let call = format!(
                    "b.{f}({x}, Width::new({}), Endian::Little, Order::{order})",
                    size * 8
                );
                if part < size {
                    let expr = match self.guard.s.clone() {
                        None => call,
                        Some((s, _)) => format!(
                            "if {s} {{ {call} }} else {{ b.konst(Width::new({}), 0) }}",
                            size * 8
                        ),
                    };
                    let whole = self.dy(K::Bits(size * 8), expr);
                    let v = self.dx(&whole)?;
                    self.cpu.excl_rest = Some((v.clone(), size * 8, part * 8));
                    return Ok(Some(self.dy(
                        K::Bits(part * 8),
                        format!("b.extract({v}, 0, Width::new({}))", part * 8),
                    )));
                }
                match self.guard.s.clone() {
                    None => self.dy(K::Bits(size * 8), call),
                    Some((s, _)) => self.dy(
                        K::Bits(size * 8),
                        format!(
                            "if {s} {{ {call} }} else {{ b.konst(Width::new({}), 0) }}",
                            size * 8
                        ),
                    ),
                }
            }
            "Mem_set" | "Mem_set__1" | "Mem_set__2" => {
                let addr = self.extend(&a[0], 64, false)?;
                let data = self.bitify(a.last().expect("data"))?;
                let x = self.dx(&addr)?;
                let y = self.dx(&data)?;
                let order = self.order(&a[2]);
                let size = data.width().unwrap_or(8) / 8;
                if let Some(p) = self.cpu.excl_pass.clone()
                    && self.guard.d.as_deref() == Some(p.as_ref())
                {
                    if self.guard.s.is_none() {
                        self.line(format!("align::check(b, st, {x}, {size}, true);"));
                    } else if let Some((s, _)) = self.guard.s.clone() {
                        self.line(format!(
                            "if {s} {{ align::check(b, st, {x}, {size}, true); }}"
                        ));
                    }
                    let call =
                        format!("b.store_exclusive({x}, {y}, Endian::Little, Order::{order})");
                    let l = match self.guard.s.clone() {
                        None => format!("let {p} = {call};"),
                        Some((s, _)) => {
                            format!(
                                "let {p} = if {s} {{ {call} }} else {{ b.konst(Width::W1, 0) }};"
                            )
                        }
                    };
                    self.line(l);
                    self.cpu.excl_pass = None;
                    return Ok(Some(V::Unit));
                }
                if self.guard.d.is_some() {
                    return fail("store under a dynamic condition");
                }
                if order != "Plain" {
                    self.effect(format!("align::check(b, st, {x}, {size}, false);"));
                }
                self.effect(format!(
                    "b.store({x}, {y}, Endian::Little, Order::{order});"
                ));
                V::Unit
            }
            "MemAtomic" => {
                let V::Struct(_, f) = &a[3] else {
                    return fail("MemAtomic access descriptor");
                };
                let modop = f
                    .iter()
                    .find(|(k, _)| k == "modop")
                    .map(|(_, v)| v.clone())
                    .ok_or_else(|| relift_sail::eval::err("no modop"))?;
                let op = match &modop {
                    V::Enum(t, i) => self.m.enums[t.as_ref()][*i].clone(),
                    V::S(_, _, d) if *d != 0 => return Err(relift_sail::eval::E::Split(*d)),
                    _ => return fail(format!("MemAtomic modop {modop:?}")),
                };
                let atomic = match op.as_str() {
                    "MemAtomicOp_ADD" => "Add",
                    "MemAtomicOp_BIC" => "Clear",
                    "MemAtomicOp_EOR" => "Xor",
                    "MemAtomicOp_ORR" => "Or",
                    "MemAtomicOp_SMAX" => "SMax",
                    "MemAtomicOp_SMIN" => "SMin",
                    "MemAtomicOp_UMAX" => "UMax",
                    "MemAtomicOp_UMIN" => "UMin",
                    "MemAtomicOp_SWP" => "Swap",
                    "MemAtomicOp_CAS" => "",
                    _ => return fail(format!("MemAtomic {op}")),
                };
                let addr = self.extend(&a[0], 64, false)?;
                let v = self.bitify(&a[2])?;
                let w = v
                    .width()
                    .ok_or_else(|| relift_sail::eval::err("atomic width"))?;
                let x = self.dx(&addr)?;
                let y = self.dx(&v)?;
                if self.guard.d.is_some() {
                    return fail("atomic under a dynamic condition");
                }
                let order = self.order(&a[3]);
                self.effect(format!("align::check(b, st, {x}, {}, false);", w / 8));
                let call = if atomic.is_empty() {
                    let c = self.bitify(&a[1])?;
                    let cx = self.dx(&c)?;
                    format!("b.compare_swap({x}, {cx}, {y}, Endian::Little, Order::{order})")
                } else {
                    format!(
                        "b.atomic_rmw(AtomicOp::{atomic}, {x}, {y}, Endian::Little, Order::{order})"
                    )
                };
                match self.guard.s.clone() {
                    None => self.dy(K::Bits(w), call),
                    Some((s, _)) => self.dy(
                        K::Bits(w),
                        format!("if {s} {{ {call} }} else {{ b.konst(Width::new({w}), 0) }}"),
                    ),
                }
            }
            "AArch64_SetExclusiveMonitors" => {
                self.cpu.excl_load = Some(self.cint(&a[1])? as u32);
                V::Unit
            }
            "AlignmentFault" => V::Enum("relift_fault".into(), 0),
            "AArch64_Abort" => {
                // Only the alignment fault of a check written in the
                // instruction (LDXP with 64-bit registers) reaches EL0.
                if !matches!(&a[1], V::Enum(t, 0) if t.as_ref() == "relift_fault") {
                    return fail("AArch64_Abort of a fault other than alignment");
                }
                let Some(d) = self.guard.d.clone() else {
                    return fail("unconditional alignment fault");
                };
                self.effect(format!("align::fault(b, {d});"));
                V::Unit
            }
            "AArch64_ExclusiveMonitorsPass" => {
                let p = format!("xp{}", self.fresh());
                self.cpu.excl_pass = Some(p.as_str().into());
                V::D(K::Bool, p.into())
            }
            "ConstrainUnpredictable" => {
                self.throw("Unpredictable")?;
                V::Undef(None)
            }
            "DecodeBitMasks" => {
                if a.iter().any(V::is_dyn) || self.guard.d.is_some() {
                    return fail("dynamic DecodeBitMasks");
                }
                let m = self.cint(&a[4])? as u32;
                let (n, s, r, i) = (
                    self.sx(&a[0])?,
                    self.sx(&a[1])?,
                    self.sx(&a[2])?,
                    self.sx(&a[3])?,
                );
                let d = a.iter().fold(0, |x, v| x | v.deps());
                let k = self.fresh();
                self.line(format!(
                    "let (s{k}w, s{k}t, s{k}ok) = rt::decode_bit_masks({n}, {s}, {r}, {i}, {m});"
                ));
                self.effect(format!(
                    "if !s{k}ok {{ return Err(LiftError::Undefined(ENC)); }}"
                ));
                V::tuple(vec![
                    V::S(K::Bits(m), format!("s{k}w").into(), d),
                    V::S(K::Bits(m), format!("s{k}t").into(), d),
                ])
            }
            n if FP_CUT.contains(&n) => return self.fp_cut(name, a).map(Some),
            "FPRecipEstimate" | "FPRSqrtEstimate" | "FPRecipStepFused" | "FPRSqrtStepFused" => {
                return self.fp_estimate(name, a).map(Some);
            }
            "FPToFixed" | "FixedToFP" | "FPConvert" | "FPConvert__1" | "FPRoundInt"
            | "FPRoundIntN" => {
                return self.fp_convert(name, a).map(Some);
            }
            "UnsignedRecipEstimate" | "UnsignedRSqrtEstimate" => {
                return self.unsigned_estimate(name, a).map(Some);
            }
            n if CRYPTO_CUT.iter().any(|(c, _)| *c == n) => {
                let op = CRYPTO_CUT.iter().find(|(c, _)| *c == n).expect("listed").1;
                let x = self.bitify(&a[0])?;
                if x.width() != Some(op.width().bits()) {
                    return fail(format!("{name} on {:?} bits", x.width()));
                }
                let xs = self.dx(&x)?;
                self.dy(
                    K::Bits(op.width().bits()),
                    format!("b.crypto(CryptoOp::{op:?}, {xs})"),
                )
            }
            "DataMemoryBarrier" | "DataSynchronizationBarrier" => {
                let order = match a.get(1) {
                    Some(V::Enum(t, i)) if name == "DataMemoryBarrier" => {
                        match self.m.enums[t.as_ref()][*i].as_str() {
                            "MBReqTypes_Reads" => "Acquire",
                            "MBReqTypes_Writes" => "Release",
                            _ => "SeqCst",
                        }
                    }
                    Some(V::Enum(..)) => "SeqCst",
                    Some(V::S(_, _, d)) if *d != 0 => return Err(relift_sail::eval::E::Split(*d)),
                    v => return fail(format!("{name} types {v:?}")),
                };
                self.effect(format!("b.fence(Order::{order});"));
                V::Unit
            }
            "InstructionSynchronizationBarrier"
            | "SpeculationBarrier"
            | "SpeculativeStoreBypassBarrierToVA"
            | "SpeculativeStoreBypassBarrierToPA" => {
                self.effect("b.fence(Order::SeqCst);".into());
                V::Unit
            }
            "ClearExclusiveLocal" | "AArch64_ClearExclusiveLocal" => {
                self.effect("b.clear_exclusive();".into());
                V::Unit
            }
            _ => return Ok(None),
        };
        Ok(Some(v))
    }
}

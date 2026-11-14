//! The cut line: Sail functions the generator does not translate but
//! replaces with hand-written code. Everything not listed here comes from
//! the Sail sources.
//!
//! - STATE: guest register access, through the template's `St`.
//! - CTRL: the PC and branches.
//! - ENV: checks that are no-ops in the fixed configuration (EL0, AArch64,
//!   FP and SIMD enabled, Armv8.0-A, no SPE, BRBE or GCS).
//! - MEM: memory access, ordering and the exclusive monitor.

use crate::eval::{Gen, R, fail, feature};
use crate::value::{K, V};

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
    "AArch64_SetExclusiveMonitors",
    "AArch64_ExclusiveMonitorsPass",
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
];

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

impl Gen<'_> {
    /// REAL: the one use of reals in the integer instructions, an integer
    /// quotient rounded toward zero (`SDIV`, `UDIV`), kept as a fraction.
    fn real(&mut self, name: &str, a: &[V]) -> R<Option<V>> {
        Ok(Some(match name {
            "to_real" | "Real" if a.len() == 1 && a[0].kind() == crate::value::Kind::Int => {
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
                    self.int_op(crate::ops::IOp::TDiv, &n, &d)?
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
            .ok_or_else(|| crate::eval::err("FP operand width"))?;
        if w != 32 && w != 64 {
            return fail(format!("{name} on {w} bits"));
        }
        let mut xs = Vec::new();
        for v in &a[..nops] {
            let v = self.bitify(v)?;
            xs.push(self.dx(&v)?.to_string());
        }
        let f = format!("rt::Fp::new({w})");
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
            "FPCompare" => (K::Bits(4), two("compare")),
            "FPCompareEQ" => (K::Bool, two("compare_eq")),
            "FPCompareGE" => (K::Bool, two("compare_ge")),
            "FPCompareGT" => (K::Bool, two("compare_gt")),
            "FPMax" | "FPMax__1" => (K::Bits(w), two("max")),
            "FPMin" | "FPMin__1" => (K::Bits(w), two("min")),
            "FPMaxNum" => (K::Bits(w), two("max_num")),
            _ => (K::Bits(w), two("min_num")),
        };
        self.line("st.assume_default_fpcr();".into());
        Ok(self.dy(k, call))
    }

    fn fp_estimate(&mut self, name: &str, a: &[V]) -> R<V> {
        if name.ends_with("Estimate") {
            self.fp_fpcr_ok(name, &a[1])?;
        }
        let x = self.bitify(&a[0])?;
        let w = x
            .width()
            .ok_or_else(|| crate::eval::err("FP operand width"))?;
        if w != 32 && w != 64 {
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
            V::S(_, _, d) if *d != 0 => return Err(crate::eval::E::Split(*d)),
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

    fn fp_convert(&mut self, name: &str, a: &[V]) -> R<V> {
        let fpw = |w: u32| -> R<u32> {
            if w == 32 || w == 64 {
                Ok(w)
            } else {
                fail(format!("{name} on {w} bits"))
            }
        };
        let x = self.bitify(&a[0])?;
        let iw = fpw(x
            .width()
            .ok_or_else(|| crate::eval::err("FP operand width"))?)?;
        let xs = self.dx(&x)?.to_string();
        let (k, call) = match name {
            "FPToFixed" | "FixedToFP" => {
                self.fp_fpcr_ok(name, &a[3])?;
                let fbits = self.cint(&a[1])?;
                let unsigned = match &a[2] {
                    V::Bool(u) => *u,
                    V::S(_, _, d) if *d != 0 => return Err(crate::eval::E::Split(*d)),
                    v => return fail(format!("signedness {v:?}")),
                };
                let r = self.fp_round(&a[4])?;
                let ow = fpw(self.cint(&a[5])? as u32)?;
                let signed = !unsigned;
                if name == "FPToFixed" {
                    (
                        K::Bits(ow),
                        format!(
                            "rt::Fp::new({iw}).fp_to_fixed(b, {xs}, {fbits}, {signed}, FpRound::{r}, {ow})"
                        ),
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
                let ow_v = if name == "FPConvert" {
                    if self.fp_round(&a[2])? != "TiesEven" {
                        return fail("FPConvert rounding");
                    }
                    &a[3]
                } else {
                    &a[2]
                };
                let ow = fpw(self.cint(ow_v)? as u32)?;
                (
                    K::Bits(ow),
                    format!("rt::Fp::new({iw}).convert(b, {xs}, &rt::Fp::new({ow}))"),
                )
            }
            _ => {
                self.fp_fpcr_ok(name, &a[1])?;
                let r = self.fp_round(&a[2])?;
                (
                    K::Bits(iw),
                    format!("rt::Fp::new({iw}).round_int(b, {xs}, FpRound::{r})"),
                )
            }
        };
        self.line("st.assume_default_fpcr();".into());
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
    pub fn cut(&mut self, name: &str, a: &[V], _hint: Option<u32>) -> R<Option<V>> {
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
                V::Enum(t, i) => V::Bool(feature(&self.m.enums[t.as_ref()][*i])),
                _ => return fail("symbolic feature"),
            },
            "Mem_read" | "Mem_read__1" | "Mem_read__2" => {
                let size = self.cint(&a[1])? as u32;
                let addr = self.extend(&a[0], 64, false)?;
                let x = self.dx(&addr)?;
                if self.guard.d.is_some() {
                    return fail("load under a dynamic condition");
                }
                let order = self.order(&a[2]);
                let f = if std::mem::take(&mut self.excl_load) {
                    "load_exclusive"
                } else {
                    "load"
                };
                let call = format!(
                    "b.{f}({x}, Width::new({}), Endian::Little, Order::{order})",
                    size * 8
                );
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
                if let Some(p) = self.excl_pass.clone()
                    && self.guard.d.as_deref() == Some(p.as_ref())
                {
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
                    self.excl_pass = None;
                    return Ok(Some(V::Unit));
                }
                if self.guard.d.is_some() {
                    return fail("store under a dynamic condition");
                }
                self.effect(format!(
                    "b.store({x}, {y}, Endian::Little, Order::{order});"
                ));
                V::Unit
            }
            "AArch64_SetExclusiveMonitors" => {
                self.excl_load = true;
                V::Unit
            }
            "AArch64_ExclusiveMonitorsPass" => {
                let p = format!("xp{}", self.fresh());
                self.excl_pass = Some(p.as_str().into());
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
            "FPToFixed" | "FixedToFP" | "FPConvert" | "FPConvert__1" | "FPRoundInt" => {
                return self.fp_convert(name, a).map(Some);
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
                    Some(V::S(_, _, d)) if *d != 0 => return Err(crate::eval::E::Split(*d)),
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

//! The VR4300 cut line: the Sail functions and registers the generator
//! replaces with calls on the lifter's `St` (`cpus/mips/src/lift/rt.rs`).
//! Everything VR4300-specific that can be said in Sail is in the overlay
//! (`vr4300.sail`); what is here is guest state access, branches and delay
//! slots, address translation, the bus, exceptions and the FPU.
//!
//! - GPR: `rGPR` and `wGPR` (`r0` reads 0 and drops writes, kept by `St`);
//!   `rFPR` and `wFPR`, the 64-bit FPU registers as the FR=1 view.
//! - SYS: every other model register (HI, LO, the CP0 registers, FCR31)
//!   through `st.reg` and `st.set_reg` by name, when the lifter's register
//!   file has a slot of that name. `PC` is the instruction's
//!   address and `InBranchDelay` is known at lift time: the frontend lifts a
//!   delay slot inside its branch's block.
//! - DELAY: `NextInBranchDelay = 1` marks a delay slot, `NextPC = PC + 8`
//!   is a likely branch's annul, and any other `NextPC` write (ERET) is a
//!   jump without a slot.
//! - BRANCH: `MIPS_Branch`, the overlay's `execute_branch`.
//! - MEM: `TLBTranslate`, the memory wrappers and `SignalException*` (a
//!   guarded exit to the exception path, or a static one).
//! - COP: `checkCP0Access` and `MIPS_Cop1Usable`, guards on the coprocessor
//!   usable bits.
//! - FPU: `MIPS_Fp` and `MIPS_FpCmp` with the operation and format known in
//!   each template, and `MIPS_FpUnimpl`, the unimplemented-operation FPE.
//! - CP0: `MIPS_Random`, `MIPS_Tlb`, `MIPS_Cache` and `MIPS_Cp0Sync`, state
//!   and effects the runtime owns.

use relift_cpu_mips::regs;
use relift_sail::eval::{Cpu, E, Gen, R, fail};
use relift_sail::stage::Stage;
use relift_sail::value::{K, V};

/// The VR4300 layer.
#[derive(Default)]
pub struct Mips;

/// The model and overlay functions the cut replaces.
const CUT: &[&str] = &[
    "rGPR",
    "wGPR",
    "rFPR",
    "wFPR",
    "MIPS_Branch",
    "TLBTranslate",
    "MEMr_wrapper",
    "MEMr_reserve_wrapper",
    "MEMw_wrapper",
    "MEMw_conditional_wrapper",
    "SignalException",
    "SignalExceptionBadAddr",
    "SignalExceptionTLB",
    "checkCP0Access",
    "MIPS_Cop1Usable",
    "MIPS_Fp",
    "MIPS_FpCmp",
    "MIPS_FpUnimpl",
    "MIPS_Random",
    "MIPS_Tlb",
    "MIPS_Cache",
    "MIPS_Cp0Sync",
];

/// The width of model register `name` when the template reads it as a
/// dynamic value: a bit vector, a bitfield or a boolean.
fn sys_width(g: &Gen<'_, Mips>, name: &str) -> Option<u32> {
    let (ty, _) = g.m.registers.get(name)?;
    let mut ty = ty.trim();
    if let Some(t) = g.m.types.get(ty) {
        ty = t.trim().trim_start_matches('(').trim_end_matches(')');
    }
    if let Some(n) = ty.strip_prefix("bits(").and_then(|r| r.strip_suffix(')')) {
        return n.trim().parse().ok();
    }
    if let Some((w, _)) = g.m.bitfields.get(ty) {
        return Some(*w);
    }
    (ty == "bool").then_some(1)
}

/// Refuses a model register the lifter's register file has no slot for.
fn has_slot(name: &str) -> R<()> {
    match regs::slot(name) {
        Some(_) => Ok(()),
        None => fail(format!("the register file has no slot for {name}")),
    }
}

/// The bitfield type of model register `name`, if it has one.
fn bitfield_of(g: &Gen<'_, Mips>, name: &str) -> Option<String> {
    let (ty, _) = g.m.registers.get(name)?;
    let ty = ty.trim().to_string();
    g.m.bitfields.contains_key(&ty).then_some(ty)
}

impl Cpu for Mips {
    fn cut(g: &mut Gen<'_, Mips>, name: &str, a: &[V], _hint: Option<u32>) -> R<Option<V>> {
        cut(g, name, a)
    }

    fn init_reg(g: &mut Gen<'_, Mips>, name: &str) -> Option<R<V>> {
        match name {
            "PC" => Some(Ok(V::S(K::Bits(64), "st.pc()".into(), 0))),
            "InBranchDelay" => Some(Ok(g.st(K::Bits(1), "u128::from(st.in_delay())".into(), 0))),
            _ => None,
        }
    }

    fn read_reg(g: &mut Gen<'_, Mips>, name: &str) -> Option<R<V>> {
        if name == "PC" || name == "InBranchDelay" {
            return None;
        }
        let w = sys_width(g, name)?;
        if let Err(e) = has_slot(name) {
            return Some(Err(e));
        }
        g.used_cut.insert("SYS");
        let v = g.dy(K::Bits(w), format!("st.reg(b, \"{name}\")"));
        Some(Ok(match bitfield_of(g, name) {
            Some(t) => V::Bf(t.into(), Box::new(v)),
            None => v,
        }))
    }

    fn assign_reg(g: &mut Gen<'_, Mips>, name: &str, v: &V) -> Option<R<()>> {
        match name {
            "NextInBranchDelay" => {
                g.used_cut.insert("DELAY");
                return Some(delay_slot(g, v));
            }
            "NextPC" => {
                g.used_cut.insert("DELAY");
                return Some(next_pc(g, v));
            }
            _ => {}
        }
        sys_width(g, name)?;
        if let Err(e) = has_slot(name) {
            return Some(Err(e));
        }
        g.used_cut.insert("SYS");
        Some(set_reg(g, name, v))
    }
}

impl Stage for Mips {
    const MAX_SPLIT: u32 = 10;
    const PURE_READS: &'static [&'static str] = &["st.reg(", "st.gpr(", "st.fpr("];
}

fn delay_slot(g: &mut Gen<'_, Mips>, v: &V) -> R<()> {
    let gd = g.gd();
    match v {
        V::Bits(1, 1) => g.effect(format!("st.delay_slot(b, {gd});")),
        V::Bits(1, 0) => {}
        _ => return fail("dynamic NextInBranchDelay"),
    }
    Ok(())
}

/// A static `NextPC` is the `PC + 8` of a likely branch that annuls its
/// slot; a dynamic one is ERET's jump.
fn next_pc(g: &mut Gen<'_, Mips>, v: &V) -> R<()> {
    let gd = g.gd();
    if v.is_dyn() {
        let x = g.dx(v)?;
        g.effect(format!("st.jump_now(b, {x}, {gd});"));
    } else {
        let x = g.sx(v)?;
        g.effect(format!("st.annul(b, ({x}) as u64, {gd});"));
    }
    Ok(())
}

fn set_reg(g: &mut Gen<'_, Mips>, name: &str, v: &V) -> R<()> {
    let v = match v {
        V::Bf(_, x) => (**x).clone(),
        v => v.clone(),
    };
    let v = g.bitify(&v)?;
    let x = g.dx(&v)?;
    let gd = g.gd();
    g.effect(format!("st.set_reg(b, \"{name}\", {x}, {gd});"));
    Ok(())
}

fn enum_name(g: &Gen<'_, Mips>, v: &V, ty: &str) -> R<String> {
    match v {
        V::Enum(_, i) => Ok(g.m.enums[ty][*i].clone()),
        V::Union(c, _) => Ok(c.to_string()),
        v => fail(format!("symbolic {ty}: {v:?}")),
    }
}

/// After an exception the rest of this path does not run: a static path
/// ends, a dynamic one leaves the block at run time (a guarded exit).
fn exception(g: &mut Gen<'_, Mips>, name: &str, addr: Option<&V>) -> R<V> {
    let gd = g.gd();
    let a = match addr {
        Some(a) => format!("Some({})", g.dx(a)?),
        None => "None".into(),
    };
    g.effect(format!("st.exception(b, Exc::{name}, {a}, {gd});"));
    if g.guard.d.is_none() {
        g.guard.dead = true;
    }
    Ok(V::Undef(None))
}

/// A concrete bit-vector argument, or a split request for a symbolic one.
fn concrete(v: &V, what: &str) -> R<u128> {
    match v {
        V::Bits(_, x) => Ok(*x),
        V::S(_, _, d) if *d != 0 => Err(E::Split(*d)),
        v => fail(format!("symbolic {what}: {v:?}")),
    }
}

/// A register number argument as the Rust expression of a `u8`.
fn reg_no(g: &mut Gen<'_, Mips>, v: &V) -> R<String> {
    Ok(format!("({}) as u8", g.sx(v)?))
}

/// A value argument as an IR value.
fn value(g: &mut Gen<'_, Mips>, v: &V) -> R<String> {
    let v = g.bitify(v)?;
    Ok(g.dx(&v)?.to_string())
}

fn cut(g: &mut Gen<'_, Mips>, name: &str, a: &[V]) -> R<Option<V>> {
    let Some(&key) = CUT.iter().find(|c| **c == name) else {
        return Ok(None);
    };
    g.used_cut.insert(key);
    let v = match name {
        "rGPR" | "rFPR" => {
            let r = reg_no(g, &a[0])?;
            let f = if name == "rGPR" { "gpr" } else { "fpr" };
            g.dy(K::Bits(64), format!("st.{f}(b, {r})"))
        }
        "wGPR" | "wFPR" => {
            let r = reg_no(g, &a[0])?;
            let x = value(g, &a[1])?;
            let gd = g.gd();
            let f = if name == "wGPR" { "set_gpr" } else { "set_fpr" };
            g.effect(format!("st.{f}(b, {r}, {x}, {gd});"));
            V::Unit
        }
        "MIPS_Branch" => {
            let t = if a[0].is_dyn() {
                format!("Target::Dynamic({})", g.dx(&a[0])?)
            } else {
                format!("Target::Const(({}) as u64)", g.sx(&a[0])?)
            };
            let gd = g.gd();
            g.effect(format!("st.branch(b, {t}, {gd});"));
            V::Unit
        }
        "TLBTranslate" => {
            let kind = enum_name(g, &a[1], "MemAccessType")?;
            let x = g.dx(&a[0])?;
            let gd = g.gd();
            g.dy(
                K::Bits(64),
                format!("st.translate(b, {x}, Access::{kind}, {gd})"),
            )
        }
        "MEMr_wrapper" | "MEMr_reserve_wrapper" => {
            let n = g.cint(&a[1])?;
            let x = g.dx(&a[0])?;
            let gd = g.gd();
            let ll = name == "MEMr_reserve_wrapper";
            g.dy(
                K::Bits(8 * n as u32),
                format!("st.load(b, {x}, {n}, {ll}, {gd})"),
            )
        }
        "MEMw_wrapper" | "MEMw_conditional_wrapper" => {
            let n = g.cint(&a[1])?;
            let x = g.dx(&a[0])?;
            let d = value(g, &a[2])?;
            let gd = g.gd();
            if name == "MEMw_wrapper" {
                g.effect(format!("st.store(b, {x}, {n}, {d}, {gd});"));
                V::Unit
            } else {
                g.dy(K::Bool, format!("st.store_cond(b, {x}, {n}, {d}, {gd})"))
            }
        }
        "SignalException" => {
            let e = enum_name(g, &a[0], "Exception")?;
            exception(g, &e, None)?
        }
        "SignalExceptionBadAddr" | "SignalExceptionTLB" => {
            let e = enum_name(g, &a[0], "Exception")?;
            exception(g, &e, Some(&a[1]))?
        }
        "MIPS_FpUnimpl" => exception(g, "FPE", None)?,
        "checkCP0Access" | "MIPS_Cop1Usable" => {
            let n = u32::from(name == "MIPS_Cop1Usable");
            let gd = g.gd();
            g.effect(format!("st.check_cop(b, {n}, {gd});"));
            V::Unit
        }
        "MIPS_Fp" => {
            let op = concrete(&a[0], "fp operation")?;
            let fmt = concrete(&a[1], "fp format")?;
            let x = value(g, &a[2])?;
            let y = value(g, &a[3])?;
            let gd = g.gd();
            g.dy(
                K::Bits(64),
                format!("st.fp(b, {op:#x}, {fmt:#x}, {x}, {y}, {gd})"),
            )
        }
        "MIPS_FpCmp" => {
            let cond = concrete(&a[0], "fp condition")?;
            let fmt = concrete(&a[1], "fp format")?;
            let x = value(g, &a[2])?;
            let y = value(g, &a[3])?;
            let gd = g.gd();
            g.effect(format!(
                "st.fp_cmp(b, {cond:#x}, {fmt:#x}, {x}, {y}, {gd});"
            ));
            V::Unit
        }
        "MIPS_Random" => g.dy(K::Bits(64), "st.random(b)".into()),
        "MIPS_Tlb" => {
            let op = concrete(&a[0], "TLB operation")?;
            let gd = g.gd();
            g.effect(format!("st.tlb(b, {op}, {gd});"));
            V::Unit
        }
        "MIPS_Cache" => {
            let op = concrete(&a[0], "cache operation")?;
            let x = value(g, &a[1])?;
            let gd = g.gd();
            g.effect(format!("st.cache(b, {op:#x}, {x}, {gd});"));
            V::Unit
        }
        "MIPS_Cp0Sync" => {
            let gd = g.gd();
            g.effect(format!("st.cp0_sync(b, {gd});"));
            V::Unit
        }
        _ => return fail(format!("{name} is in the cut list but has no cut")),
    };
    Ok(Some(v))
}

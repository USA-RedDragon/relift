//! The RSP cut line: the Sail functions and registers the generator replaces
//! with calls on the lifter's `St` (`cpus/rsp/src/lift/rt.rs`) or with IR
//! operations directly. Everything RSP-specific that can be said in Sail is
//! in the overlay (`rsp.sail`); what is here is guest state access, branches
//! and delay slots, DMEM, the SP interface, the divider ROMs and the lane
//! builtins.
//!
//! - GPR: `rGPR` and `wGPR` (`r0` reads 0 and drops writes, kept by `St`);
//!   `rVPR` and `wVPR`, the vector registers.
//! - SYS: every other overlay register (the accumulator, the flag masks and
//!   the divider state) through `st.reg` and `st.set_reg` by name. `PC` is
//!   the instruction's address and `InBranchDelay` is known at lift time.
//! - DELAY and BRANCH: `NextInBranchDelay = 1` marks a delay slot;
//!   `MIPS_Branch` is the overlay's `execute_branch`.
//! - MEM: `TLBTranslate` (the low 12 bits) and the scalar memory wrappers;
//!   `rDMEM_line`, `rDMEM16`, `wDMEM_line` and `wDMEM16`, the vector unit's
//!   16-byte accesses.
//! - SP: `RSP_Cop0Read`, `RSP_Cop0Write` and `RSP_Break`.
//! - EXC: `SignalException`, which the model raises only for a branch in a
//!   delay slot; the RSP has no exceptions, and the template refuses it as
//!   unpredictable (`lift` refuses such a pair before it gets there).
//! - ROM: `RSP_RcpRom` and `RSP_RsqRom`, an entry of the divider ROMs.
//! - LANE: the lane builtins, each one IR operation. `lane1`, `lane2` and
//!   `lane_imm` take a `lane_kind` (an `L_` member named after
//!   `relift_ir::LaneKind`) and the lane width in bits; `lane_imm` also takes
//!   the immediate, known when lifting. `lane_select`, `lane_permute` and
//!   `lane_permute2` take a bitwise mask or a byte pattern known when
//!   lifting; `lane_extract16`, `lane_insert16` and `lane_splat16` move one
//!   16-bit lane. `vrotl` rotates 128 bits left by whole bytes, and
//!   `RSP_ElemPattern` is the byte pattern of an element selector, computed
//!   when lifting.

use relift_cpu_rsp::lift::rt;
use relift_cpu_rsp::regs;
use relift_sail::eval::{Cpu, E, Gen, R, fail};
use relift_sail::stage::Stage;
use relift_sail::value::{K, V};

/// The RSP layer.
#[derive(Default)]
pub struct Rsp;

/// The model and overlay functions the cut replaces.
const CUT: &[&str] = &[
    "rGPR",
    "wGPR",
    "rVPR",
    "wVPR",
    "MIPS_Branch",
    "TLBTranslate",
    "MEMr_wrapper",
    "MEMw_wrapper",
    "rDMEM_line",
    "rDMEM16",
    "wDMEM_line",
    "wDMEM16",
    "RSP_Cop0Read",
    "RSP_Cop0Write",
    "RSP_Break",
    "SignalException",
    "RSP_RcpRom",
    "RSP_RsqRom",
    "RSP_ElemPattern",
    "lane1",
    "lane2",
    "lane_imm",
    "lane_select",
    "lane_permute",
    "lane_permute2",
    "lane_extract16",
    "lane_insert16",
    "lane_splat16",
    "vrotl",
];

/// The `LaneKind` variants the overlay's `lane_kind` members name.
const LANE_KINDS: &[&str] = &[
    "Add",
    "Sub",
    "AddSatS",
    "AddSatU",
    "SubSatS",
    "SubSatU",
    "MulLo",
    "MulHiS",
    "MulHiU",
    "MulHiSU",
    "And",
    "Or",
    "Xor",
    "AndNot",
    "Not",
    "MinS",
    "MinU",
    "MaxS",
    "MaxU",
    "Abs",
    "Neg",
    "Eq",
    "Ne",
    "Slt",
    "Sle",
    "Ult",
    "Ule",
    "ShlImm",
    "LShrImm",
    "AShrImm",
    "Shl",
    "LShr",
    "AShr",
    "WidenLoS",
    "WidenLoU",
    "WidenHiS",
    "WidenHiU",
    "NarrowTrunc",
    "NarrowSS",
    "NarrowUU",
    "NarrowSU",
    "MulWideLoSS",
    "MulWideLoUU",
    "MulWideLoSU",
    "MulWideHiSS",
    "MulWideHiUU",
    "MulWideHiSU",
    "Broadcast",
];

/// The width of model register `name` when the template reads it as a
/// dynamic value.
fn sys_width(g: &Gen<'_, Rsp>, name: &str) -> Option<u32> {
    let (ty, _) = g.m.registers.get(name)?;
    let mut ty = ty.trim();
    if let Some(t) = g.m.types.get(ty) {
        ty = t.trim().trim_start_matches('(').trim_end_matches(')');
    }
    if let Some(n) = ty.strip_prefix("bits(").and_then(|r| r.strip_suffix(')')) {
        return n.trim().parse().ok();
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

impl Cpu for Rsp {
    fn cut(g: &mut Gen<'_, Rsp>, name: &str, a: &[V], _hint: Option<u32>) -> R<Option<V>> {
        cut(g, name, a)
    }

    fn init_reg(g: &mut Gen<'_, Rsp>, name: &str) -> Option<R<V>> {
        match name {
            "PC" => Some(Ok(V::S(K::Bits(64), "st.pc()".into(), 0))),
            "InBranchDelay" => Some(Ok(g.st(K::Bits(1), "u128::from(st.in_delay())".into(), 0))),
            _ => None,
        }
    }

    fn read_reg(g: &mut Gen<'_, Rsp>, name: &str) -> Option<R<V>> {
        if name == "PC" || name == "InBranchDelay" {
            return None;
        }
        let w = sys_width(g, name)?;
        if let Err(e) = has_slot(name) {
            return Some(Err(e));
        }
        g.used_cut.insert("SYS");
        Some(Ok(g.dy(K::Bits(w), format!("st.reg(b, \"{name}\")"))))
    }

    fn assign_reg(g: &mut Gen<'_, Rsp>, name: &str, v: &V) -> Option<R<()>> {
        if name == "NextInBranchDelay" {
            g.used_cut.insert("DELAY");
            return Some(delay_slot(g, v));
        }
        sys_width(g, name)?;
        if let Err(e) = has_slot(name) {
            return Some(Err(e));
        }
        g.used_cut.insert("SYS");
        Some(set_reg(g, name, v))
    }
}

impl Stage for Rsp {
    const MAX_SPLIT: u32 = 4;
    const PURE_READS: &'static [&'static str] = &[
        "st.reg(",
        "st.gpr(",
        "st.vpr(",
        "st.dmem_line(",
        "st.dmem16(",
        "st.rcp_rom(",
        "st.rsq_rom(",
        "st.permute(",
        "b.lane",
        "b.permute",
        "rt::rotl_bytes",
    ];
}

fn delay_slot(g: &mut Gen<'_, Rsp>, v: &V) -> R<()> {
    let gd = g.gd();
    match v {
        V::Bits(1, 1) => g.effect(format!("st.delay_slot(b, {gd});")),
        V::Bits(1, 0) => {}
        _ => return fail("dynamic NextInBranchDelay"),
    }
    Ok(())
}

fn set_reg(g: &mut Gen<'_, Rsp>, name: &str, v: &V) -> R<()> {
    let v = g.bitify(v)?;
    let x = g.dx(&v)?;
    let gd = g.gd();
    g.effect(format!("st.set_reg(b, \"{name}\", {x}, {gd});"));
    Ok(())
}

/// A concrete integer argument.
fn concrete_int(g: &Gen<'_, Rsp>, v: &V, what: &str) -> R<u32> {
    match g.cint(v) {
        Ok(n) => u32::try_from(n).or_else(|_| fail(format!("{what} {n}"))),
        Err(E::Fail(_)) => fail(format!("symbolic {what}: {v:?}")),
        Err(e) => Err(e),
    }
}

/// An integer argument known when lifting, as the Rust expression of a
/// `u8`.
fn static_u8(g: &mut Gen<'_, Rsp>, v: &V, what: &str) -> R<String> {
    if v.is_dyn() {
        return fail(format!("dynamic {what}"));
    }
    Ok(format!("({}) as u8", g.sx(v)?))
}

/// A register number argument as the Rust expression of a `u8`.
fn reg_no(g: &mut Gen<'_, Rsp>, v: &V) -> R<String> {
    static_u8(g, v, "register number")
}

/// A value argument as an IR value.
fn value(g: &mut Gen<'_, Rsp>, v: &V) -> R<String> {
    let v = g.bitify(v)?;
    Ok(g.dx(&v)?.to_string())
}

/// A 128-bit value known when lifting (a byte pattern or a mask), as the
/// Rust expression of a `u128`.
fn static_u128(g: &mut Gen<'_, Rsp>, v: &V, what: &str) -> R<String> {
    if v.is_dyn() {
        return fail(format!("dynamic {what}"));
    }
    g.sx(v)
}

/// The `LaneKind` a `lane_kind` member names.
fn lane_kind(g: &Gen<'_, Rsp>, v: &V) -> R<&'static str> {
    let V::Enum(ty, i) = v else {
        return fail(format!("symbolic lane kind: {v:?}"));
    };
    let member = &g.m.enums[ty.as_ref()][*i];
    let kind = member
        .strip_prefix("L_")
        .ok_or_else(|| E::Fail(format!("lane kind {member}")))?;
    LANE_KINDS
        .iter()
        .find(|k| **k == kind)
        .copied()
        .ok_or_else(|| E::Fail(format!("no LaneKind::{kind}")))
}

/// The `LaneWidth` of a width in bits.
fn lane_width(g: &Gen<'_, Rsp>, v: &V) -> R<&'static str> {
    Ok(match concrete_int(g, v, "lane width")? {
        8 => "L8",
        16 => "L16",
        32 => "L32",
        64 => "L64",
        n => return fail(format!("{n}-bit lanes")),
    })
}

fn cut(g: &mut Gen<'_, Rsp>, name: &str, a: &[V]) -> R<Option<V>> {
    let Some(&key) = CUT.iter().find(|c| **c == name) else {
        return Ok(None);
    };
    g.used_cut.insert(match key {
        "rGPR" | "wGPR" | "rVPR" | "wVPR" => "GPR",
        "MIPS_Branch" => "BRANCH",
        "TLBTranslate" | "MEMr_wrapper" | "MEMw_wrapper" | "rDMEM_line" | "rDMEM16"
        | "wDMEM_line" | "wDMEM16" => "MEM",
        "RSP_Cop0Read" | "RSP_Cop0Write" | "RSP_Break" => "SP",
        "SignalException" => "EXC",
        "RSP_RcpRom" | "RSP_RsqRom" => "ROM",
        _ => "LANE",
    });
    let v = match name {
        "rGPR" => {
            let r = reg_no(g, &a[0])?;
            g.dy(K::Bits(64), format!("st.gpr(b, {r})"))
        }
        "rVPR" => {
            let r = reg_no(g, &a[0])?;
            g.dy(K::Bits(128), format!("st.vpr(b, {r})"))
        }
        "wGPR" | "wVPR" => {
            let r = reg_no(g, &a[0])?;
            let x = value(g, &a[1])?;
            let gd = g.gd();
            let f = if name == "wGPR" { "set_gpr" } else { "set_vpr" };
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
            let V::Enum(_, i) = &a[1] else {
                return fail("symbolic MemAccessType");
            };
            let kind = g.m.enums["MemAccessType"][*i].clone();
            let x = g.dx(&a[0])?;
            let gd = g.gd();
            g.dy(
                K::Bits(64),
                format!("st.translate(b, {x}, Access::{kind}, {gd})"),
            )
        }
        "MEMr_wrapper" => {
            let n = g.cint(&a[1])?;
            let x = g.dx(&a[0])?;
            let gd = g.gd();
            g.dy(
                K::Bits(8 * n as u32),
                format!("st.load(b, {x}, {n}, false, {gd})"),
            )
        }
        "MEMw_wrapper" => {
            let n = g.cint(&a[1])?;
            let x = g.dx(&a[0])?;
            let d = value(g, &a[2])?;
            let gd = g.gd();
            g.effect(format!("st.store(b, {x}, {n}, {d}, {gd});"));
            V::Unit
        }
        "rDMEM_line" | "rDMEM16" => {
            let x = value(g, &a[0])?;
            let f = if name == "rDMEM_line" {
                "dmem_line"
            } else {
                "dmem16"
            };
            g.dy(K::Bits(128), format!("st.{f}(b, {x})"))
        }
        "wDMEM_line" | "wDMEM16" => {
            let x = value(g, &a[0])?;
            let d = value(g, &a[1])?;
            let m = value(g, &a[2])?;
            let gd = g.gd();
            let f = if name == "wDMEM_line" {
                "write_line"
            } else {
                "write16"
            };
            g.effect(format!("st.{f}(b, {x}, {d}, {m}, {gd});"));
            V::Unit
        }
        "RSP_Cop0Read" => {
            let r = reg_no(g, &a[0])?;
            g.dy(K::Bits(32), format!("st.cop0_read(b, {r})"))
        }
        "RSP_Cop0Write" => {
            let r = reg_no(g, &a[0])?;
            let x = value(g, &a[1])?;
            let gd = g.gd();
            g.effect(format!("st.cop0_write(b, {r}, {x}, {gd});"));
            V::Unit
        }
        "RSP_Break" => {
            let gd = g.gd();
            g.effect(format!("st.brk(b, {gd});"));
            V::Unit
        }
        "SignalException" => {
            g.throw("Unpredictable")?;
            V::Unit
        }
        "RSP_RcpRom" | "RSP_RsqRom" => {
            let x = value(g, &a[0])?;
            let f = if name == "RSP_RcpRom" {
                "rcp_rom"
            } else {
                "rsq_rom"
            };
            g.dy(K::Bits(16), format!("st.{f}(b, {x})"))
        }
        "RSP_ElemPattern" => match &a[0] {
            V::Bits(_, e) => V::Bits(128, rt::elem_pattern(*e)),
            v => {
                let e = static_u128(g, v, "element selector")?;
                g.st(K::Bits(128), format!("rt::elem_pattern({e})"), v.deps())
            }
        },
        "lane1" | "lane2" => {
            let kind = lane_kind(g, &a[0])?;
            let lw = lane_width(g, &a[1])?;
            let x = value(g, &a[2])?;
            let call = if name == "lane1" {
                format!("b.lane1(LaneKind::{kind}, LaneWidth::{lw}, {x})")
            } else {
                let y = value(g, &a[3])?;
                format!("b.lane2(LaneKind::{kind}, LaneWidth::{lw}, {x}, {y})")
            };
            g.dy(K::Bits(128), call)
        }
        "lane_imm" => {
            let kind = lane_kind(g, &a[0])?;
            let lw = lane_width(g, &a[1])?;
            let imm = static_u8(g, &a[2], "lane immediate")?;
            let x = value(g, &a[3])?;
            g.dy(
                K::Bits(128),
                format!("b.lane_imm(LaneKind::{kind}, LaneWidth::{lw}, {imm}, &[{x}])"),
            )
        }
        "lane_select" => {
            let m = value(g, &a[0])?;
            let x = value(g, &a[1])?;
            let y = value(g, &a[2])?;
            g.dy(K::Bits(128), format!("b.lane_select({m}, {x}, {y})"))
        }
        "lane_permute" => {
            let p = static_u128(g, &a[0], "byte pattern")?;
            let x = value(g, &a[1])?;
            g.dy(K::Bits(128), format!("st.permute(b, {x}, {p})"))
        }
        "lane_permute2" => {
            let p = static_u128(g, &a[0], "byte pattern")?;
            let x = value(g, &a[1])?;
            let y = value(g, &a[2])?;
            g.dy(
                K::Bits(128),
                format!("b.permute2({x}, {y}, ({p}).to_le_bytes())"),
            )
        }
        "lane_extract16" => {
            let i = static_u8(g, &a[0], "lane index")?;
            let x = value(g, &a[1])?;
            g.dy(
                K::Bits(16),
                format!("b.lane_imm(LaneKind::Extract, LaneWidth::L16, {i}, &[{x}])"),
            )
        }
        "lane_insert16" => {
            let x = value(g, &a[0])?;
            let i = static_u8(g, &a[1], "lane index")?;
            let y = value(g, &a[2])?;
            g.dy(
                K::Bits(128),
                format!("b.lane_imm(LaneKind::Insert, LaneWidth::L16, {i}, &[{x}, {y}])"),
            )
        }
        "lane_splat16" => {
            let x = value(g, &a[0])?;
            g.dy(
                K::Bits(128),
                format!("b.lane1(LaneKind::Splat, LaneWidth::L16, {x})"),
            )
        }
        "vrotl" => {
            let x = value(g, &a[0])?;
            if a[1].is_dyn() {
                let n = g.dx(&a[1])?;
                g.dy(K::Bits(128), format!("rt::rotl_bytes_ir(b, {x}, {n})"))
            } else {
                let n = g.sx(&a[1])?;
                g.dy(
                    K::Bits(128),
                    format!("rt::rotl_bytes(b, {x}, ({n}) as i128)"),
                )
            }
        }
        _ => return fail(format!("{name} is in the cut list but has no cut")),
    };
    Ok(Some(v))
}

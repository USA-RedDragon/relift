//! The ARMv4T cut line: the Sail functions the generator replaces with
//! hand-written code. Everything ARMv4T-specific that can be said in Sail
//! is in the overlay (`armv4t.sail`); what is here is guest state access,
//! the PC, branches, the condition guard and the bus.
//!
//! - STATE: `__v4t_R` and `__v4t_RSet` (the current mode's `r0` to `r14`),
//!   `Rmode_read` and `Rmode_set` with `M32_User` (the user bank, for
//!   LDM and STM with the S bit), `__v4t_SetT` (the Thumb bit), the PSR
//!   functions `__v4t_CPSR`, `__v4t_SPSR`, `__v4t_WriteCPSR`,
//!   `__v4t_WriteSPSR`, `__v4t_RestoreCPSR`, and `PSTATE.N`, `.Z`, `.C`,
//!   `.V`. A read of `PSTATE.M` is refused: the mode is only known at run
//!   time, so every model function that tests it is replaced.
//! - CTRL: `_PC` (the instruction address), `BranchTo`,
//!   `__v4t_BranchAligned` (a branch aligned by the Thumb bit as it is
//!   after a CPSR restore), `__v4t_SVC` (the software interrupt) and
//!   `ConditionPassed` (a guard on the condition field that leaves before
//!   any effect, see `rt::COND_FAIL`).
//! - BUS: `__v4t_read`, `__v4t_write` (one aligned access of 1, 2 or 4
//!   bytes, recorded as an N or S data cycle), `__v4t_internal`,
//!   `__v4t_internal_if`, `__v4t_force_n`.
//! - FORM: `__v4t_RegShiftForm`, true in the templates of the A32
//!   register-shifted-register forms, where ARM7TDMI reads R15 as PC+12.

use std::rc::Rc;

use relift_cpu_arm32::regs::Mode;
use relift_sail::ast::Expr;
use relift_sail::eval::{Cpu, Gen, R, err, fail};
use relift_sail::stage::Stage;
use relift_sail::value::{K, V};

/// The ARMv4T layer and its per-template configuration.
#[derive(Default)]
pub struct Arm32 {
    /// The template is for a Thumb encoding.
    pub thumb: bool,
    /// The Rust expression of the condition field at lift time.
    pub cond: Option<&'static str>,
    /// The encoding is a register-shifted-register form.
    pub reg_shift: bool,
}

const ENV_NOOPS: &[&str] = &["Hint_PreloadData", "Hint_PreloadDataForWrite"];

/// Stubs whose empty body is right on ARMv4T: the syndrome only matters to
/// an abort handler, and the GBA has no aborts.
const STUBS_OK: &[&str] = &["AArch32_SetLSInstructionSyndrome"];

/// The user mode's mode bits, as `Rmode_read` and `Rmode_set` take them.
const M32_USER: u128 = Mode::Usr.bits() as u128;

fn is_pstate(b: &Expr) -> bool {
    matches!(b, Expr::Id(n) if n == "PSTATE")
}

fn is_flag(b: &Expr, f: &str) -> bool {
    is_pstate(b) && matches!(f, "N" | "Z" | "C" | "V")
}

/// The ARMv5TE sticky overflow flag, kept as bit 27 of `psr_rsvd`.
fn is_q(b: &Expr, f: &str) -> bool {
    is_pstate(b) && f == "Q"
}

impl Cpu for Arm32 {
    fn stub_ok(name: &str) -> bool {
        STUBS_OK.contains(&name)
    }

    fn cut(g: &mut Gen<'_, Arm32>, name: &str, a: &[V], _hint: Option<u32>) -> R<Option<V>> {
        cut(g, name, a)
    }

    fn init_reg(g: &mut Gen<'_, Arm32>, name: &str) -> Option<R<V>> {
        match name {
            "_PC" => {
                g.used_cut.insert("_PC");
                Some(Ok(V::S(K::Bits(64), "pc".into(), 0)))
            }
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

    fn read_field(g: &mut Gen<'_, Arm32>, b: &Expr, f: &str) -> Option<R<V>> {
        if is_pstate(b) && f == "M" {
            return Some(fail("PSTATE.M read: the mode is only known at run time"));
        }
        if is_q(b, f) {
            g.used_cut.insert("PSTATE.Q");
            return Some(Ok(g.dy(K::Bits(1), "st.q(b)".to_string())));
        }
        if !is_flag(b, f) {
            return None;
        }
        g.used_cut.insert("PSTATE.NZCV");
        Some(Ok(g.dy(K::Bits(1), format!("st.get(b, regs::{f})"))))
    }

    fn assign_field(g: &mut Gen<'_, Arm32>, b: &Expr, f: &str, v: &V) -> Option<R<()>> {
        if is_pstate(b) && f == "M" {
            return Some(fail("PSTATE.M write outside the PSR cut"));
        }
        if is_q(b, f) {
            g.used_cut.insert("PSTATE.Q");
            let r = (|| {
                let v = g.bitify(v)?;
                let x = g.dx(&v)?;
                let gd = g.gd();
                g.effect(format!("st.set_q(b, {x}, {gd});"));
                Ok(())
            })();
            return Some(r);
        }
        // The Thumb bit's writes go through `__v4t_SetT` wherever they can
        // change it; the model's own `PSTATE.T` write is dropped. A write
        // of any other field with no cut would vanish from the template.
        if is_pstate(b) && !is_flag(b, f) && f != "T" {
            return Some(fail(format!("PSTATE.{f} write with no cut")));
        }
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
        (is_flag(b, f) || is_q(b, f)).then_some(1)
    }

    fn decode_args(word: V) -> Vec<V> {
        vec![V::S(K::Bits(64), "pc".into(), 0), word]
    }
}

/// The PSTATE every template starts from: every field zero but the Thumb
/// bit of the template's instruction set and `nRW` (AArch32).
fn default_pstate(g: &Gen<'_, Arm32>) -> R<V> {
    let fields =
        g.m.structs
            .get("ProcState")
            .ok_or_else(|| err("no ProcState"))?;
    let mut f = Vec::new();
    for (n, _) in fields {
        let v = match n.as_str() {
            "EL" | "BTYPE" => V::Bits(2, 0),
            "GE" => V::Bits(4, 0),
            "IT" => V::Bits(8, 0),
            "M" => V::Bits(5, M32_USER),
            "T" => V::Bits(1, u128::from(g.cpu.thumb)),
            "nRW" => V::Bits(1, 1),
            _ => V::Bits(1, 0),
        };
        f.push((n.clone(), v));
    }
    Ok(V::Struct("ProcState".into(), Rc::new(f)))
}

thread_local! {
    /// The configuration of the template being generated.
    pub static CONFIG: std::cell::RefCell<Arm32> = std::cell::RefCell::new(Arm32::default());
}

impl Stage for Arm32 {
    const PURE_READS: &'static [&'static str] =
        &["st.r_read(", "st.user_read(", "st.cpsr(", "st.spsr("];

    fn setup(g: &mut Gen<'_, Arm32>) {
        CONFIG.with(|c| {
            let c = c.borrow();
            g.cpu = Arm32 {
                thumb: c.thumb,
                cond: c.cond,
                reg_shift: c.reg_shift,
            };
        });
    }
}

fn reg_no(v: &V) -> R<String> {
    match v {
        V::Int(i) => Ok(format!("{i}")),
        V::S(K::Int, e, _) => Ok(format!("{e} as u32")),
        _ => fail(format!("register number {v:?}")),
    }
}

/// The lift-time Rust expression of a static field such as an MSR mask.
fn static_u32(g: &mut Gen<'_, Arm32>, v: &V) -> R<String> {
    match v {
        V::Bits(_, x) => Ok(format!("{x}u32")),
        V::S(_, e, _) => Ok(format!("(({e}) as u32)")),
        _ => {
            let v = g.bitify(v)?;
            match v {
                V::Bits(_, x) => Ok(format!("{x}u32")),
                V::S(_, e, _) => Ok(format!("(({e}) as u32)")),
                _ => fail(format!("lift-time value expected, got {v:?}")),
            }
        }
    }
}

fn user_mode(v: &V) -> R<()> {
    match v {
        V::Bits(5, M32_USER) => Ok(()),
        _ => fail(format!("banked access to mode {v:?}")),
    }
}

fn cut(g: &mut Gen<'_, Arm32>, name: &str, a: &[V]) -> R<Option<V>> {
    if name == "IsFeatureImplemented" {
        g.used_cut.insert("IsFeatureImplemented");
        return Ok(Some(V::Bool(false)));
    }
    if ENV_NOOPS.contains(&name) {
        g.used_cut.insert("ENV");
        return Ok(Some(V::Unit));
    }
    let key: &'static str = match name {
        "__v4t_R" | "__v4t_RSet" | "Rmode_read" | "Rmode_set" => "REG",
        "BranchTo" | "__v4t_BranchAligned" | "__v4t_SVC" => "BRANCH",
        "ConditionPassed" => "ConditionPassed",
        "__v4t_SetT" => "__v4t_SetT",
        "__v4t_CPSR" | "__v4t_SPSR" | "__v4t_WriteCPSR" | "__v4t_WriteSPSR"
        | "__v4t_RestoreCPSR" => "PSR",
        "__v4t_read" | "__v4t_write" => "BUS",
        "__v4t_internal" | "__v4t_internal_if" | "__v4t_force_n" => "BUS",
        "__v4t_RegShiftForm" => "FORM",
        "__v5_CPWrite" | "__v5_CPRead" => "CP",
        "__v5_Exception" => "BRANCH",
        _ => return Ok(None),
    };
    g.used_cut.insert(key);
    let v = match name {
        "__v4t_R" => {
            let n = reg_no(&a[0])?;
            g.dy(K::Bits(32), format!("st.r_read(b, {n})"))
        }
        "__v4t_RSet" => {
            let n = reg_no(&a[0])?;
            let v = g.bitify(&a[1])?;
            let x = g.dx(&v)?;
            let gd = g.gd();
            g.effect(format!("st.r_set(b, {n}, {x}, {gd});"));
            V::Unit
        }
        "Rmode_read" => {
            user_mode(&a[1])?;
            let n = reg_no(&a[0])?;
            g.dy(K::Bits(32), format!("st.user_read(b, {n})"))
        }
        "Rmode_set" => {
            user_mode(&a[1])?;
            let n = reg_no(&a[0])?;
            let v = g.bitify(&a[2])?;
            let x = g.dx(&v)?;
            let gd = g.gd();
            g.effect(format!("st.user_set(b, {n}, {x}, {gd});"));
            V::Unit
        }
        "__v4t_SetT" => {
            let v = g.bitify(&a[0])?;
            let x = g.dx(&v)?;
            let gd = g.gd();
            g.effect(format!("st.set_thumb(b, {x}, {gd});"));
            V::Unit
        }
        "__v4t_CPSR" => g.dy(K::Bits(32), "st.cpsr(b)".to_string()),
        "__v4t_SPSR" => g.dy(K::Bits(32), "st.spsr(b)".to_string()),
        "__v4t_WriteCPSR" | "__v4t_WriteSPSR" => {
            let v = g.bitify(&a[0])?;
            let x = g.dx(&v)?;
            let mask = static_u32(g, &a[1])?;
            let gd = g.gd();
            let f = if name == "__v4t_WriteCPSR" {
                "write_cpsr"
            } else {
                "write_spsr"
            };
            g.effect(format!("st.{f}(b, {x}, {mask}, {gd});"));
            V::Unit
        }
        "__v4t_RestoreCPSR" => {
            let keep_t = match &a[0] {
                V::Bool(k) => *k,
                v => return fail(format!("RestoreCPSR with {v:?}")),
            };
            let gd = g.gd();
            g.effect(format!("st.restore_cpsr(b, {keep_t}, {gd});"));
            V::Unit
        }
        "ConditionPassed" => {
            if let Some(c) = g.cpu.cond {
                g.effect(format!("st.cond_guard(b, {c});"));
            }
            V::Bool(true)
        }
        "BranchTo" => {
            let t = g.bitify(&a[0])?;
            let t = if t.width() == Some(32) {
                t
            } else {
                g.extract(&t, &V::Int(0), 32)?
            };
            let flow = match &a[1] {
                V::Enum(_, i) => match g.m.enums["BranchType"][*i].as_str() {
                    "BranchType_DIRCALL" | "BranchType_INDCALL" => "Call",
                    "BranchType_RET" => "Return",
                    _ => "Jump",
                },
                _ => return fail("symbolic branch type"),
            };
            let gd = g.gd();
            let stmt = if t.is_dyn() {
                let x = g.dx(&t)?;
                format!("st.branch(b, Target::Dynamic({x}), Kind::{flow}, {gd});")
            } else {
                let x = g.sx(&t)?;
                format!("st.branch(b, Target::Const({x} as u64), Kind::{flow}, {gd});")
            };
            g.effect(stmt);
            V::Unit
        }
        "__v4t_BranchAligned" => {
            let t = g.bitify(&a[0])?;
            let x = g.dx(&t)?;
            let gd = g.gd();
            g.effect(format!("st.branch_aligned(b, {x}, {gd});"));
            V::Unit
        }
        "__v4t_SVC" => {
            let gd = g.gd();
            g.effect(format!("st.svc(b, {gd});"));
            V::Unit
        }
        "__v4t_read" => {
            let size = g.cint(&a[1])? as u32;
            let addr = g.bitify(&a[0])?;
            let x = g.dx(&addr)?;
            if g.guard.d.is_some() {
                return fail("load under a dynamic condition");
            }
            g.effect(format!("st.data(false, {size});"));
            let call = format!(
                "b.load({x}, Width::new({}), Endian::Little, Order::Plain)",
                size * 8
            );
            match g.guard.s.clone() {
                None => g.dy(K::Bits(size * 8), call),
                Some((s, _)) => g.dy(
                    K::Bits(size * 8),
                    format!(
                        "if {s} {{ {call} }} else {{ b.konst(Width::new({}), 0) }}",
                        size * 8
                    ),
                ),
            }
        }
        "__v4t_write" => {
            let size = g.cint(&a[1])? as u32;
            let addr = g.bitify(&a[0])?;
            let data = g.bitify(&a[2])?;
            let x = g.dx(&addr)?;
            let y = g.dx(&data)?;
            if g.guard.d.is_some() {
                return fail("store under a dynamic condition");
            }
            g.effect(format!("st.data(true, {size});"));
            g.effect(format!("b.store({x}, {y}, Endian::Little, Order::Plain);"));
            V::Unit
        }
        "__v4t_internal" => {
            let n = g.cint(&a[0])?;
            g.effect(format!("st.internal({n});"));
            V::Unit
        }
        "__v4t_internal_if" => {
            let c = g.truth(&a[0])?;
            match c {
                V::Bool(true) => g.effect("st.internal(1);".into()),
                V::Bool(false) => {}
                V::S(_, e, _) => g.effect(format!("if {e} {{ st.internal(1); }}")),
                _ => {
                    let c = g.bitify(&c)?;
                    let x = g.dx(&c)?;
                    g.effect(format!("st.internal_if(b, {x});"));
                }
            }
            V::Unit
        }
        "__v4t_force_n" => {
            g.effect("st.force_n();".into());
            V::Unit
        }
        "__v4t_RegShiftForm" => V::Bool(g.cpu.reg_shift),
        // Coprocessor register transfers: the register key (coproc bit,
        // opc1, CRn, CRm, opc2) is known at lift time; the runtime's
        // coprocessor hook does the access.
        "__v5_CPWrite" => {
            let key = static_u32(g, &a[0])?;
            let v = g.bitify(&a[1])?;
            let x = g.dx(&v)?;
            let gd = g.gd();
            g.effect(format!("st.cp_write(b, {key}, {x}, {gd});"));
            V::Unit
        }
        "__v5_CPRead" => {
            let key = static_u32(g, &a[0])?;
            if g.guard.d.is_some() {
                return fail("coprocessor read under a dynamic condition");
            }
            g.dy(K::Bits(32), format!("st.cp_read(b, {key})"))
        }
        // An exception entry: mode bits, vector, and the return address
        // as an offset from the instruction.
        "__v5_Exception" => {
            let mode = static_u32(g, &a[0])?;
            let vector = static_u32(g, &a[1])?;
            let off = g.cint(&a[2])?;
            let gd = g.gd();
            g.effect(format!("st.exception(b, {mode}, {vector}, {off}, {gd});"));
            V::Unit
        }
        _ => unreachable!(),
    };
    Ok(Some(v))
}

#[cfg(test)]
mod tests {
    use relift_cpu_arm32::regs;

    #[test]
    fn the_flags_the_cut_names_are_slots() {
        let file = regs::regs();
        for (f, r) in [
            ("n", regs::N),
            ("z", regs::Z),
            ("c", regs::C),
            ("v", regs::V),
        ] {
            assert_eq!(file.decl(r).name, f);
        }
    }
}

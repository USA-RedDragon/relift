//! The cut line of the SM83 model: the Sail functions declared with `val`
//! and no body in `sm83.sail`, which the generator replaces with calls into
//! the template's `St` (`cpus/sm83/src/lift/sail/rt.rs`).
//!
//! - STATE: the registers A, B, C, D, E, H, L, SP and the flags FZ, FN, FH
//!   and FC, through `st.get` and `st.set`; PC is the instruction's address,
//!   known at lift time.
//! - FETCH: `MemFetch`, the instruction's own bytes, known at lift time.
//! - BUS: `MemRead` and `MemWrite`, at the M-cycle the model gives.
//! - CLOCK: `Cycles`, the instruction's total.
//! - CTRL: `BranchTo` and `Condition` (a conditional `CALL` or `RET` lifts as
//!   two blocks, which `St` handles from the condition).
//! - IRQ: `SM83_EI`, `SM83_DI`, `SM83_RETI`, `SM83_Halt` and `SM83_Stop`,
//!   the per-CPU layer.

use relift_sail::eval::{Cpu, Gen, R, fail};
use relift_sail::stage::Stage;
use relift_sail::value::{K, V};

/// The SM83 layer of the evaluator. It keeps no state across a run.
#[derive(Default)]
pub struct Sm83;

/// The guest registers and their widths.
const REGS: &[(&str, u32)] = &[
    ("A", 8),
    ("B", 8),
    ("C", 8),
    ("D", 8),
    ("E", 8),
    ("H", 8),
    ("L", 8),
    ("SP", 16),
    ("FZ", 1),
    ("FN", 1),
    ("FH", 1),
    ("FC", 1),
];

/// Every cut function.
pub const CUT: &[&str] = &[
    "MemFetch",
    "MemRead",
    "MemWrite",
    "Cycles",
    "BranchTo",
    "Condition",
    "SM83_EI",
    "SM83_DI",
    "SM83_RETI",
    "SM83_Halt",
    "SM83_Stop",
];

impl Cpu for Sm83 {
    fn cut(g: &mut Gen<'_, Sm83>, name: &str, a: &[V], _hint: Option<u32>) -> R<Option<V>> {
        cut(g, name, a)
    }

    fn init_reg(_g: &mut Gen<'_, Sm83>, name: &str) -> Option<R<V>> {
        (name == "PC").then(|| Ok(V::S(K::Bits(16), "st.pc()".into(), 0)))
    }

    fn read_reg(g: &mut Gen<'_, Sm83>, name: &str) -> Option<R<V>> {
        let (reg, width) = REGS.iter().find(|(r, _)| *r == name)?;
        g.used_cut.insert("STATE");
        Some(Ok(g.dy(K::Bits(*width), format!("st.get(b, regs::{reg})"))))
    }

    fn assign_reg(g: &mut Gen<'_, Sm83>, name: &str, v: &V) -> Option<R<()>> {
        let (reg, _) = REGS.iter().find(|(r, _)| *r == name)?;
        g.used_cut.insert("STATE");
        Some((|| {
            let v = g.bitify(v)?;
            let x = g.dx(&v)?;
            let gd = g.gd();
            g.effect(format!("st.set(b, regs::{reg}, {x}, {gd});"));
            Ok(())
        })())
    }
}

impl Stage for Sm83 {
    const PURE_READS: &'static [&'static str] = &["st.fetch("];
}

fn cut(g: &mut Gen<'_, Sm83>, name: &str, a: &[V]) -> R<Option<V>> {
    let Some(&key) = CUT.iter().find(|c| **c == name) else {
        return Ok(None);
    };
    g.used_cut.insert(key);
    let v = match name {
        "MemFetch" => {
            let at = g.sx(&a[0])?;
            g.st(K::Bits(8), format!("st.fetch({at})"), 0)
        }
        "MemRead" => {
            let at = g.dx(&a[0])?;
            let cycle = g.cint(&a[1])?;
            let gd = g.gd();
            g.dy(K::Bits(8), format!("st.read(b, {at}, {cycle}, {gd})"))
        }
        "MemWrite" => {
            let at = g.dx(&a[0])?;
            let x = g.dx(&a[1])?;
            let cycle = g.cint(&a[2])?;
            let gd = g.gd();
            g.effect(format!("st.write(b, {at}, {x}, {cycle}, {gd});"));
            V::Unit
        }
        "Cycles" => {
            let n = g.cint(&a[0])?;
            let gd = g.gd();
            g.effect(format!("st.cycles(b, {n}, {gd});"));
            V::Unit
        }
        "Condition" => {
            let cc = match &a[0] {
                V::Bits(_, x) => *x,
                v => return fail(format!("condition {v:?}")),
            };
            g.dy(K::Bool, format!("st.cond(b, {cc})"))
        }
        "BranchTo" => {
            let kind = match &a[1] {
                V::Enum(_, i) => match g.m.enums["BranchType"][*i].as_str() {
                    "BranchType_CALL" => "Call",
                    "BranchType_RET" => "Return",
                    _ => "Jump",
                },
                _ => return fail("symbolic branch type"),
            };
            let gd = g.gd();
            let target = if a[0].is_dyn() {
                format!("Target::Dynamic({})", g.dx(&a[0])?)
            } else {
                format!("Target::Const(({}) as u16)", g.sx(&a[0])?)
            };
            g.effect(format!("st.branch(b, {target}, Kind::{kind}, {gd});"));
            V::Unit
        }
        _ => {
            let irq = match name {
                "SM83_EI" => "Ei",
                "SM83_DI" => "Di",
                "SM83_RETI" => "Reti",
                "SM83_Halt" => "Halt",
                _ => "Stop",
            };
            g.effect(format!("st.irq(b, Irq::{irq});"));
            V::Unit
        }
    };
    Ok(Some(v))
}

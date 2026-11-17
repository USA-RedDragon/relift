//! The cut line of the Z80 model: the Sail functions declared with `val`
//! and no body in `z80.sail`, which the generator replaces with calls into
//! the template's `St` (`cpus/z80/src/lift/rt.rs`).
//!
//! - STATE: the guest registers below, through `st.get` and `st.set`. PC is
//!   the instruction's address, known at lift time; in an interrupt entry
//!   it is the address the interrupt returns to, a run-time value
//!   (`st.ret`).
//! - PAGE: the model's `Page`, fixed per template (base, CB, ED, DD, FD,
//!   DDCB, FDCB; a lone prefix uses the base page's start).
//! - FETCH: `MemFetch`, the instruction's own bytes, known at lift time,
//!   with the T-state of their machine cycle recorded; in a live template
//!   (`LIVE`) a byte the template reads is an operand, read from memory in
//!   its machine cycle when the instruction runs (`st.operand`).
//! - BUS: `MemRead`, `MemWrite`, `PortRead` and `PortWrite`, at the T-state
//!   the model gives.
//! - CLOCK: `Cycles`, the instruction's total.
//! - CTRL: `BranchTo` and `Decide` (a conditional `CALL` or `RET` lifts as
//!   two blocks, which `St` handles from the condition).
//! - HALT: `Z80_Halt`, the per-CPU layer.

use std::cell::Cell;

use relift_sail::eval::{Cpu, Gen, R, fail};
use relift_sail::stage::Stage;
use relift_sail::value::{K, V};

use relift_cpu_z80::Group;

thread_local! {
    /// The group of the template being generated.
    pub static GROUP: Cell<Group> = const { Cell::new(Group::Base) };
    /// Whether the template reads its operand bytes when it runs.
    pub static LIVE: Cell<bool> = const { Cell::new(false) };
}

/// The Z80 layer of the evaluator. `err` keeps a failure of `Begin`.
#[derive(Default)]
pub struct Z80 {
    err: Option<String>,
}

/// The guest registers and their widths, in the order of
/// `cpus/z80/src/regs.rs`, whose constants are these names in upper case.
const REGS: &[(&str, u32)] = &[
    ("A", 8),
    ("FS", 1),
    ("FZ", 1),
    ("FY", 1),
    ("FH", 1),
    ("FX", 1),
    ("FP", 1),
    ("FN", 1),
    ("FC", 1),
    ("B", 8),
    ("C", 8),
    ("D", 8),
    ("E", 8),
    ("H", 8),
    ("L", 8),
    ("IX", 16),
    ("IY", 16),
    ("SP", 16),
    ("WZ", 16),
    ("I", 8),
    ("R", 8),
    ("AF_", 16),
    ("BC_", 16),
    ("DE_", 16),
    ("HL_", 16),
    ("IFF1", 1),
    ("IFF2", 1),
    ("IM", 2),
    ("Q", 1),
    ("EIDelay", 1),
    ("LDAIR", 1),
];

/// Every cut function.
pub const CUT: &[&str] = &[
    "MemFetch",
    "MemRead",
    "MemWrite",
    "PortRead",
    "PortWrite",
    "Cycles",
    "BranchTo",
    "Decide",
    "Z80_Halt",
];

impl Cpu for Z80 {
    fn cut(g: &mut Gen<'_, Z80>, name: &str, a: &[V], _hint: Option<u32>) -> R<Option<V>> {
        cut(g, name, a)
    }

    fn init_reg(g: &mut Gen<'_, Z80>, name: &str) -> Option<R<V>> {
        let group = GROUP.with(Cell::get);
        match name {
            "PC" if group == Group::Accept => {
                g.used_cut.insert("STATE");
                Some(Ok(g.dy(K::Bits(16), "st.ret(b)".into())))
            }
            "PC" => Some(Ok(V::S(K::Bits(16), "st.pc()".into(), 0))),
            "Page" => Some(Ok(V::Bits(3, u128::from(crate::page(group))))),
            _ => None,
        }
    }

    fn read_reg(g: &mut Gen<'_, Z80>, name: &str) -> Option<R<V>> {
        let (reg, width) = REGS.iter().find(|(r, _)| *r == name)?;
        let reg = reg.to_ascii_uppercase();
        g.used_cut.insert("STATE");
        Some(Ok(g.dy(K::Bits(*width), format!("st.get(b, regs::{reg})"))))
    }

    fn assign_reg(g: &mut Gen<'_, Z80>, name: &str, v: &V) -> Option<R<()>> {
        let reg = REGS
            .iter()
            .find(|(r, _)| *r == name)?
            .0
            .to_ascii_uppercase();
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

impl Stage for Z80 {
    const PURE_READS: &'static [&'static str] = &["st.fetch("];

    fn setup(g: &mut Gen<'_, Self>) {
        if GROUP.with(Cell::get) != Group::Accept {
            g.used_cut.insert("PAGE");
            if let Err(e) = g.call("Begin", vec![], None) {
                g.cpu.err = Some(format!("Begin: {e:?}"));
            }
        }
    }

    fn finish_run(g: &Gen<'_, Self>) -> Result<(), String> {
        match &g.cpu.err {
            Some(e) => Err(e.clone()),
            None => Ok(()),
        }
    }
}

fn cut(g: &mut Gen<'_, Z80>, name: &str, a: &[V]) -> R<Option<V>> {
    let Some(&key) = CUT.iter().find(|c| **c == name) else {
        return Ok(None);
    };
    g.used_cut.insert(key);
    let v = match name {
        "MemFetch" => {
            let at = g.sx(&a[0])?;
            let t = g.cint(&a[1])?;
            let gd = g.gd();
            g.effect(format!("st.fetched({at}, {t}, {gd});"));
            if LIVE.with(Cell::get) {
                g.dy(K::Bits(8), format!("st.operand(b, {at}, {t}, {gd})"))
            } else {
                g.st(K::Bits(8), format!("st.fetch({at})"), 0)
            }
        }
        "MemRead" => {
            let at = g.dx(&a[0])?;
            let t = g.cint(&a[1])?;
            let gd = g.gd();
            g.dy(K::Bits(8), format!("st.read(b, {at}, {t}, {gd})"))
        }
        "MemWrite" => {
            let at = g.dx(&a[0])?;
            let x = g.dx(&a[1])?;
            let t = g.cint(&a[2])?;
            let gd = g.gd();
            g.effect(format!("st.write(b, {at}, {x}, {t}, {gd});"));
            V::Unit
        }
        "PortRead" => {
            let at = g.dx(&a[0])?;
            let t = g.cint(&a[1])?;
            let gd = g.gd();
            g.dy(K::Bits(8), format!("st.port_in(b, {at}, {t}, {gd})"))
        }
        "PortWrite" => {
            let at = g.dx(&a[0])?;
            let x = g.dx(&a[1])?;
            let t = g.cint(&a[2])?;
            let gd = g.gd();
            g.effect(format!("st.port_out(b, {at}, {x}, {t}, {gd});"));
            V::Unit
        }
        "Cycles" => {
            let n = g.cint(&a[0])?;
            let gd = g.gd();
            g.effect(format!("st.cycles(b, {n}, {gd});"));
            V::Unit
        }
        "Decide" => {
            let c = g.bitify(&a[0])?;
            let c = g.dx(&c)?;
            g.dy(K::Bool, format!("st.decide(b, {c})"))
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
            let gd = g.gd();
            g.effect(format!("st.halt(b, {gd});"));
            V::Unit
        }
    };
    Ok(Some(v))
}

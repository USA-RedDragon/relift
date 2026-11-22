//! `relift coverage`: what discovery could not resolve, per module. For
//! each module of the import it gives the functions found, the
//! instructions that do not lift, every computed call or jump that analysis
//! left to the dispatcher ([`relift_analyze::unresolved_transfers`]), and
//! the hints in `relift.toml` that earlier hot-load runs recorded. Discovery
//! starts from the hints too, as the next `relift build` does, so a hint
//! shows as lifted once the build will include it.
//!
//! Each unresolved transfer gets one [`Reason`], decided in this order:
//!
//! - **slot**: the target is loaded from a loader-filled slot
//!   ([`relift_analyze::slot_uses`] over the module's slots from
//!   [`program::linkage`]) whose symbol no module exports as a function, so
//!   nothing in the program tells where it goes. A slot transfer whose
//!   symbol some module does export is resolved by the guest loader at run
//!   time: it is not listed, only counted as *through loader slots*.
//! - **pointer**: a computed call, or a computed jump whose target is a
//!   value loaded from memory (a vtable or a struct field), a register the
//!   function was entered with (an argument) or a register a call wrote (a
//!   call's result): a pointer the analysis cannot follow. A jump of this
//!   shape is a tail call through a pointer.
//! - **table**: any other computed jump: the target is computed from other
//!   values, or reached over merging paths, and no jump table was
//!   recognized there.
//!
//! The [`relift_analyze::Source`] of the target is kept beside the reason
//! as detail.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::path::Path;

use relift_analyze::{
    Analysis, Insn, Progress, Source, Transfer, TransferKind, discover_with, slot_uses,
    unresolved_transfers,
};
use relift_cpu_arm64::frontend::Arm64;
use relift_guest::Frontend;
use relift_progress::{Line, Metrics, noun};
use relift_switch::program::{self, Placed};

use crate::hints::Hint;
use crate::import::BoxError;
use crate::settings::Settings;
use crate::ui::Ui;

/// The size of a loader-filled slot, in bytes.
const SLOT: u64 = 8;

/// Why a computed transfer is unresolved.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Reason {
    /// Through a loader-filled slot whose symbol no module exports.
    Slot,
    /// A computed jump with no recognized jump table.
    Table,
    /// Through a pointer the analysis cannot follow.
    Pointer,
}

impl Reason {
    /// The reason's name in the output.
    pub fn name(self) -> &'static str {
        match self {
            Reason::Slot => "slot",
            Reason::Table => "table",
            Reason::Pointer => "pointer",
        }
    }
}

/// The name of a transfer kind in the output.
pub fn kind_name(k: TransferKind) -> &'static str {
    match k {
        TransferKind::Call => "call",
        TransferKind::Jump => "jump",
    }
}

/// The name of a target source in the output.
pub fn source_name(s: Source) -> &'static str {
    match s {
        Source::Loaded => "loaded",
        Source::Argument => "argument",
        Source::Returned => "returned",
        Source::Computed => "computed",
        Source::Unknown => "unknown",
    }
}

/// The reason of transfer `t`, which goes through the slot of symbol
/// `slot` when it has one; `None` when the guest loader resolves it
/// (`exported` holds the symbol).
pub fn classify(
    t: &Transfer,
    slot: Option<&str>,
    exported: &dyn Fn(&str) -> bool,
) -> Option<Reason> {
    match (slot, t.kind, t.source) {
        (Some(s), _, _) if exported(s) => None,
        (Some(_), _, _) => Some(Reason::Slot),
        (None, TransferKind::Call, _) => Some(Reason::Pointer),
        (None, TransferKind::Jump, Source::Loaded | Source::Argument | Source::Returned) => {
            Some(Reason::Pointer)
        }
        (None, TransferKind::Jump, Source::Computed | Source::Unknown) => Some(Reason::Table),
    }
}

/// One unresolved computed transfer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Site {
    /// Its guest address.
    pub addr: u64,
    /// Its offset from the module's load address.
    pub offset: u64,
    /// The containing function: its symbol, else `fn_<offset>`.
    pub function: String,
    /// Call or jump.
    pub kind: TransferKind,
    /// Why it is unresolved.
    pub reason: Reason,
    /// Where its target came from.
    pub source: Source,
    /// The slot's symbol, for a [`Reason::Slot`] site.
    pub symbol: Option<String>,
}

/// One hint of `relift.toml`, placed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HintRow {
    /// The offset from the module's load address.
    pub offset: u64,
    /// The guest address.
    pub addr: u64,
    /// Whether a function starts there now, so the build lifts it.
    pub lifted: bool,
}

/// What coverage found in one module.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModuleReport {
    /// The module name.
    pub name: String,
    /// Its load address.
    pub base: u64,
    /// The functions found.
    pub functions: usize,
    /// The distinct instructions that do not lift.
    pub unlifted: usize,
    /// The computed transfers the guest loader resolves through a slot.
    pub through_slots: usize,
    /// The unresolved transfers, by address.
    pub unresolved: Vec<Site>,
    /// The hints for this module, by offset.
    pub hints: Vec<HintRow>,
}

impl ModuleReport {
    /// How many unresolved transfers have reason `r`.
    pub fn count(&self, r: Reason) -> usize {
        self.unresolved.iter().filter(|s| s.reason == r).count()
    }
}

/// The report for placed module `p` analyzed as `analysis`; `slots` are its
/// loader-filled slots by address with their symbols, `exported` the
/// function symbols some module exports, `hints` the offsets hinted for it
/// and `preserved` the registers a call keeps.
pub fn report(
    p: &Placed,
    analysis: &Analysis,
    slots: &BTreeMap<u64, String>,
    exported: &BTreeSet<String>,
    hints: &BTreeSet<u64>,
    preserved: &[relift_ir::Reg],
) -> ModuleReport {
    let addrs: BTreeSet<u64> = slots.keys().copied().collect();
    let mut unlifted = BTreeSet::new();
    let mut seen = BTreeSet::new();
    let mut through_slots = 0;
    let mut unresolved = Vec::new();
    for f in analysis.functions.values() {
        unlifted.extend(
            f.insts
                .iter()
                .filter(|(_, i)| matches!(i, Insn::Failed(_)))
                .map(|(&pc, _)| pc),
        );
        let ts = unresolved_transfers(f, preserved);
        if ts.is_empty() {
            continue;
        }
        let via = if addrs.is_empty() {
            BTreeMap::new()
        } else {
            slot_uses(f, &addrs, SLOT).transfers
        };
        let function = f
            .name
            .clone()
            .unwrap_or_else(|| format!("fn_{:x}", f.entry - p.base));
        for t in ts {
            if !seen.insert(t.pc) {
                continue;
            }
            let symbol = via.get(&t.pc).and_then(|s| slots.get(s));
            match classify(&t, symbol.map(String::as_str), &|s| exported.contains(s)) {
                None => through_slots += 1,
                Some(reason) => unresolved.push(Site {
                    addr: t.pc,
                    offset: t.pc - p.base,
                    function: function.clone(),
                    kind: t.kind,
                    reason,
                    source: t.source,
                    symbol: (reason == Reason::Slot).then(|| symbol.cloned()).flatten(),
                }),
            }
        }
    }
    unresolved.sort_by_key(|s| s.addr);
    ModuleReport {
        name: p.name.clone(),
        base: p.base,
        functions: analysis.functions.len(),
        unlifted: unlifted.len(),
        through_slots,
        unresolved,
        hints: hints
            .iter()
            .map(|&o| HintRow {
                offset: o,
                addr: p.base + o,
                lifted: analysis.is_entry(p.base + o),
            })
            .collect(),
    }
}

/// The plain-text report: a summary table, then each module's sites and
/// hints, one line each.
pub fn text(reports: &[ModuleReport]) -> String {
    let head = [
        "module",
        "base",
        "functions",
        "unlifted",
        "via slots",
        "slot",
        "table",
        "pointer",
        "hints",
    ];
    let rows: Vec<[String; 9]> = reports
        .iter()
        .map(|r| {
            [
                r.name.clone(),
                format!("{:#x}", r.base),
                r.functions.to_string(),
                r.unlifted.to_string(),
                r.through_slots.to_string(),
                r.count(Reason::Slot).to_string(),
                r.count(Reason::Table).to_string(),
                r.count(Reason::Pointer).to_string(),
                r.hints.len().to_string(),
            ]
        })
        .collect();
    let mut widths = head.map(str::len);
    for row in &rows {
        for (w, c) in widths.iter_mut().zip(row) {
            *w = (*w).max(c.len());
        }
    }
    let mut out = String::new();
    let line = |out: &mut String, cells: &[&str]| {
        let mut l = String::new();
        for (k, (c, w)) in cells.iter().zip(widths).enumerate() {
            if k < 2 {
                let _ = write!(l, "{c:<w$}  ");
            } else {
                let _ = write!(l, "{c:>w$}  ");
            }
        }
        out.push_str(l.trim_end());
        out.push('\n');
    };
    line(&mut out, &head);
    for row in &rows {
        line(&mut out, &row.each_ref().map(String::as_str));
    }
    for r in reports {
        if !r.unresolved.is_empty() {
            let _ = writeln!(
                out,
                "\n{}: {} unresolved",
                r.name,
                noun(r.unresolved.len() as u64, "transfer")
            );
        }
        for s in &r.unresolved {
            let detail = s.symbol.as_deref().unwrap_or(source_name(s.source));
            let _ = writeln!(
                out,
                "  {:#x}  {}+{:#x}  {}  {}  {}  {}",
                s.addr,
                r.name,
                s.offset,
                s.function,
                kind_name(s.kind),
                s.reason.name(),
                detail
            );
        }
        if !r.hints.is_empty() {
            let _ = writeln!(out, "\n{}: {}", r.name, noun(r.hints.len() as u64, "hint"));
        }
        for h in &r.hints {
            let _ = writeln!(
                out,
                "  {:#x}  {}+{:#x}  {}",
                h.addr,
                r.name,
                h.offset,
                if h.lifted { "lifted" } else { "not lifted" }
            );
        }
    }
    out
}

/// The JSON report:
/// `{modules:[{name,base,functions,unlifted,through_slots,unresolved:[{addr,offset,function,kind,reason,source,symbol}],hints:[{offset,addr,lifted}]}]}`,
/// with addresses and offsets as numbers.
pub fn json(reports: &[ModuleReport]) -> serde_json::Value {
    let modules: Vec<serde_json::Value> = reports
        .iter()
        .map(|r| {
            serde_json::json!({
                "name": r.name,
                "base": r.base,
                "functions": r.functions,
                "unlifted": r.unlifted,
                "through_slots": r.through_slots,
                "unresolved": r.unresolved.iter().map(|s| serde_json::json!({
                    "addr": s.addr,
                    "offset": s.offset,
                    "function": s.function,
                    "kind": kind_name(s.kind),
                    "reason": s.reason.name(),
                    "source": source_name(s.source),
                    "symbol": s.symbol,
                })).collect::<Vec<_>>(),
                "hints": r.hints.iter().map(|h| serde_json::json!({
                    "offset": h.offset,
                    "addr": h.addr,
                    "lifted": h.lifted,
                })).collect::<Vec<_>>(),
            })
        })
        .collect();
    serde_json::json!({ "modules": modules })
}

/// Runs `relift coverage` in the project in the working directory: the
/// report for every module, or for the one named `only`, on standard
/// output as text, or as JSON with `as_json`.
///
/// # Errors
///
/// No project, an import that does not load, a `relift.toml` hint that does
/// not parse, or an `only` that names no module.
pub fn run(
    settings: &Settings,
    as_json: bool,
    only: Option<&str>,
    ui: &mut Ui,
) -> Result<(), BoxError> {
    let import = crate::build::import_dir(settings)?;
    let (mods, _) = crate::build::load(&import)?;
    let placed = program::place(mods);
    if let Some(m) = only
        && !placed.iter().any(|p| p.name == m)
    {
        let names: Vec<&str> = placed.iter().map(|p| p.name.as_str()).collect();
        return Err(format!("no module {m:?}; the import has {}", names.join(", ")).into());
    }
    let hints = crate::hints::read(Path::new("relift.toml"))?;
    for h in &hints {
        if !placed.iter().any(|p| p.name == h.module) {
            ui.note(&Line::new().text(format!("hint {} names no module of the import", h.text())));
        }
    }
    let linkages: Vec<_> = placed.iter().map(program::linkage).collect();
    let exported: BTreeSet<String> = linkages
        .iter()
        .flat_map(|l| l.exports.iter().map(|(n, _)| n.clone()))
        .collect();
    let cpu = Arm64 {
        cpu: crate::build::project_cpu(settings)?,
    };
    let preserved = cpu.preserved();
    let mut reports = Vec::new();
    for (p, l) in placed.iter().zip(&linkages) {
        if only.is_some_and(|m| m != p.name) {
            continue;
        }
        let row = ui.row(&p.name);
        ui.begin(row, "discover");
        let module = crate::build::module_with_hints(p, &hints);
        let analysis = discover_with(&cpu, &module, &mut |e| {
            let (done, total) = match e {
                Progress::Found { entries, .. } => (entries, None),
                Progress::Lifted { done, total, .. } => (done, Some(total as u64)),
            };
            ui.update(
                row,
                Metrics {
                    done: done as u64,
                    total,
                    unit: "functions",
                    ..Metrics::default()
                },
            );
        });
        let slots: BTreeMap<u64, String> =
            l.slots.iter().map(|s| (s.addr, s.name.clone())).collect();
        let mine: BTreeSet<u64> = hints
            .iter()
            .filter(|h: &&Hint| h.module == p.name)
            .map(|h| h.offset)
            .collect();
        let r = report(p, &analysis, &slots, &exported, &mine, &preserved);
        ui.done(
            row,
            Line::new().text(format!(
                "{}, {} unresolved",
                noun(r.functions as u64, "function"),
                noun(r.unresolved.len() as u64, "transfer")
            )),
        );
        reports.push(r);
    }
    if as_json {
        println!("{}", serde_json::to_string_pretty(&json(&reports))?);
    } else {
        print!("{}", text(&reports));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn t(kind: TransferKind, source: Source) -> Transfer {
        Transfer {
            pc: 0x100,
            kind,
            source,
        }
    }

    #[test]
    fn transfers_get_the_reason_the_module_docs_give() {
        let exported = |s: &str| s == "known";
        use Source::*;
        use TransferKind::*;
        assert_eq!(classify(&t(Call, Loaded), Some("known"), &exported), None);
        assert_eq!(classify(&t(Jump, Loaded), Some("known"), &exported), None);
        assert_eq!(
            classify(&t(Call, Loaded), Some("weak"), &exported),
            Some(Reason::Slot)
        );
        for s in [Loaded, Argument, Returned, Computed, Unknown] {
            assert_eq!(
                classify(&t(Call, s), None, &exported),
                Some(Reason::Pointer)
            );
        }
        for s in [Loaded, Argument, Returned] {
            assert_eq!(
                classify(&t(Jump, s), None, &exported),
                Some(Reason::Pointer)
            );
        }
        for s in [Computed, Unknown] {
            assert_eq!(classify(&t(Jump, s), None, &exported), Some(Reason::Table));
        }
    }

    fn sample() -> ModuleReport {
        ModuleReport {
            name: "main".into(),
            base: 0x800_0000,
            functions: 3,
            unlifted: 1,
            through_slots: 2,
            unresolved: vec![
                Site {
                    addr: 0x800_0040,
                    offset: 0x40,
                    function: "fn_30".into(),
                    kind: TransferKind::Call,
                    reason: Reason::Pointer,
                    source: Source::Loaded,
                    symbol: None,
                },
                Site {
                    addr: 0x800_0080,
                    offset: 0x80,
                    function: "f".into(),
                    kind: TransferKind::Call,
                    reason: Reason::Slot,
                    source: Source::Loaded,
                    symbol: Some("weak".into()),
                },
            ],
            hints: vec![HintRow {
                offset: 0x30,
                addr: 0x800_0030,
                lifted: true,
            }],
        }
    }

    #[test]
    fn the_text_has_a_summary_then_one_line_per_site_and_hint() {
        let t = text(&[sample()]);
        let lines: Vec<&str> = t.lines().collect();
        assert_eq!(
            lines,
            [
                "module  base       functions  unlifted  via slots  slot  table  pointer  hints",
                "main    0x8000000          3         1          2     1      0        1      1",
                "",
                "main: 2 transfers unresolved",
                "  0x8000040  main+0x40  fn_30  call  pointer  loaded",
                "  0x8000080  main+0x80  f  call  slot  weak",
                "",
                "main: 1 hint",
                "  0x8000030  main+0x30  lifted",
            ]
        );
    }

    #[test]
    fn the_json_keeps_its_shape() {
        let j = json(&[sample()]);
        let m = &j["modules"][0];
        assert_eq!(m["name"], "main");
        assert_eq!(m["base"], 0x800_0000);
        assert_eq!(m["through_slots"], 2);
        assert_eq!(m["unresolved"][0]["reason"], "pointer");
        assert_eq!(m["unresolved"][0]["kind"], "call");
        assert_eq!(m["unresolved"][0]["symbol"], serde_json::Value::Null);
        assert_eq!(m["unresolved"][1]["symbol"], "weak");
        assert_eq!(m["hints"][0]["lifted"], true);
        assert_eq!(m["hints"][0]["addr"], 0x800_0030);
    }
}

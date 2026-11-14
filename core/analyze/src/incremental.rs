//! Incremental analysis: after a change to a module's bytes, lift again
//! only the functions whose bytes changed ([`rediscover`]), and a text form
//! of an [`Analysis`] so an earlier build's result can be read back
//! ([`to_text`], [`from_text`]).

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::ops::Range;
use std::sync::Arc;

use relift_guest::{Frontend, LiftFailure, Module, RootKind};
use relift_ir::RegFile;

use crate::{Analysis, EntryKind, Function, Insn, Progress, explore};

/// What [`rediscover`] did.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Reuse {
    /// The entries of the functions taken unchanged from the earlier analysis.
    pub reused: BTreeSet<u64>,
    /// The entries of the functions lifted again or found new.
    pub lifted: BTreeSet<u64>,
}

fn overlaps(ranges: &[Range<u64>], lo: u64, hi: u64) -> bool {
    ranges.iter().any(|r| r.start < hi && lo < r.end)
}

/// The entries of the functions of `base` whose instructions overlap a
/// changed range, and of those with jump tables when a change lies outside
/// code (a table may live in the changed data).
pub fn stale(
    module: &Module,
    granule: u64,
    base: &Analysis,
    changed: &[Range<u64>],
) -> BTreeSet<u64> {
    let data_changed = changed.iter().any(|r| {
        (r.start..r.end)
            .step_by(usize::try_from(granule.max(1)).unwrap_or(1))
            .any(|a| !module.is_code(a))
    });
    base.functions
        .values()
        .filter(|f| {
            (data_changed && !f.tables.is_empty())
                || f.insts.iter().any(|(&pc, i)| {
                    let len = match i {
                        Insn::Lifted(b) => u64::from(b.len).max(1),
                        Insn::Failed(_) => granule,
                    };
                    overlaps(changed, pc, pc + len)
                })
        })
        .map(|f| f.entry)
        .collect()
}

/// The functions of `module`, whose bytes differ from those `base` was
/// found in only inside `changed`: every function of `base` that holds no
/// changed byte is taken as it is, and discovery runs again from the
/// others and from the roots of `module` that `base` does not know.
///
/// The result equals [`crate::discover`] on `module` except where the
/// change adds an entry inside a function taken unchanged: that function
/// keeps its copy of the code, as with [`crate::discover_more`]. An entry
/// only the changed code reached stays a function.
pub fn rediscover(
    fe: &(dyn Frontend + Sync),
    module: &Module,
    base: &Analysis,
    changed: &[Range<u64>],
    on: &mut dyn FnMut(Progress),
) -> (Analysis, Reuse) {
    let granule = fe.granule();
    let stale = stale(module, granule, base, changed);
    let mut kinds: BTreeMap<u64, EntryKind> =
        base.functions.values().map(|f| (f.entry, f.kind)).collect();
    let mut names: BTreeMap<u64, String> = BTreeMap::new();
    for r in &module.roots {
        if !module.is_code(r.addr) || r.addr % granule != 0 {
            continue;
        }
        let k = kinds.entry(r.addr).or_insert(EntryKind::Root(r.kind));
        *k = (*k).min(EntryKind::Root(r.kind));
        if let Some(n) = &r.name {
            names.entry(r.addr).or_insert_with(|| n.clone());
        }
    }
    let skip: BTreeMap<u64, EntryKind> = base
        .functions
        .values()
        .filter(|f| !stale.contains(&f.entry))
        .map(|f| (f.entry, kinds[&f.entry]))
        .collect();
    let (fresh, _) = explore(fe, module, kinds, &skip, &names, on, &|_| ());
    let mut reuse = Reuse {
        reused: skip.keys().copied().collect(),
        lifted: fresh.functions.keys().copied().collect(),
    };
    let mut functions: BTreeMap<u64, Function> = fresh
        .functions
        .values()
        .map(|o| (o.entry, o.lift(fe, module)))
        .collect();
    for (&e, f) in &base.functions {
        if skip.contains_key(&e) {
            let mut f = f.clone();
            f.kind = skip[&e];
            if f.name.is_none() {
                f.name = names.get(&e).cloned();
            }
            functions.insert(e, f);
        }
    }
    reuse.reused.retain(|e| !reuse.lifted.contains(e));
    (Analysis { functions }, reuse)
}

fn kind_name(k: EntryKind) -> &'static str {
    match k {
        EntryKind::Root(RootKind::Entry) => "entry",
        EntryKind::Root(RootKind::Symbol) => "symbol",
        EntryKind::Root(RootKind::Unwind) => "unwind",
        EntryKind::Root(RootKind::Pointer) => "pointer",
        EntryKind::Root(RootKind::Hint) => "hint",
        EntryKind::Called => "called",
        EntryKind::AddressTaken => "taken",
    }
}

fn kind_of(s: &str) -> Option<EntryKind> {
    Some(match s {
        "entry" => EntryKind::Root(RootKind::Entry),
        "symbol" => EntryKind::Root(RootKind::Symbol),
        "unwind" => EntryKind::Root(RootKind::Unwind),
        "pointer" => EntryKind::Root(RootKind::Pointer),
        "hint" => EntryKind::Root(RootKind::Hint),
        "called" => EntryKind::Called,
        "taken" => EntryKind::AddressTaken,
        _ => return None,
    })
}

/// The first line of [`to_text`].
const MAGIC: &str = "relift-analysis 1";

fn hex_list(v: impl IntoIterator<Item = u64>) -> String {
    v.into_iter().map(|a| format!(" {a:x}")).collect::<String>()
}

/// The text form of `a`: every instruction once, then the functions by
/// the addresses of their instructions. `key` is written in the header;
/// [`from_text`] returns it, so a caller can refuse a text written for
/// other inputs.
pub fn to_text(a: &Analysis, key: &str) -> String {
    let mut insts: BTreeMap<u64, &Insn> = BTreeMap::new();
    for f in a.functions.values() {
        for (pc, i) in &f.insts {
            insts.entry(*pc).or_insert(i);
        }
    }
    let mut t = format!("{MAGIC}\nkey {}\n", key.replace('\n', " "));
    for (pc, i) in insts {
        match i {
            Insn::Lifted(b) => {
                let _ = write!(t, "lifted {pc:x}\n{b}");
            }
            Insn::Failed(e) => {
                let _ = writeln!(
                    t,
                    "failed {pc:x} {:x} {}",
                    e.insn,
                    e.message.replace('\n', " ")
                );
            }
        }
    }
    for f in a.functions.values() {
        let _ = writeln!(
            t,
            "fn {:x} {}{}",
            f.entry,
            kind_name(f.kind),
            f.name
                .as_deref()
                .map(|n| format!(" {}", n.replace('\n', " ")))
                .unwrap_or_default()
        );
        let _ = writeln!(t, "insts{}", hex_list(f.insts.keys().copied()));
        for (j, ts) in &f.tables {
            let _ = writeln!(t, "table {j:x}{}", hex_list(ts.iter().copied()));
        }
        if !f.pads.is_empty() {
            let _ = writeln!(t, "pads{}", hex_list(f.pads.iter().copied()));
        }
    }
    t
}

fn hex(s: &str) -> Result<u64, String> {
    u64::from_str_radix(s, 16).map_err(|e| format!("{s}: {e}"))
}

fn hexes(s: &str) -> Result<Vec<u64>, String> {
    s.split(' ').filter(|w| !w.is_empty()).map(hex).collect()
}

/// Reads back the text of [`to_text`], with the blocks' register files
/// looked up in `files`. Returns the key and the analysis.
///
/// # Errors
///
/// The text is not of [`to_text`], or a block does not parse.
pub fn from_text(text: &str, files: &[RegFile]) -> Result<(String, Analysis), String> {
    let mut lines = text.split_inclusive('\n').peekable();
    if lines.next().map(str::trim_end) != Some(MAGIC) {
        return Err("not an analysis text".into());
    }
    let key = lines
        .next()
        .and_then(|l| l.trim_end().strip_prefix("key "))
        .ok_or("no key line")?
        .to_string();
    let mut insts: BTreeMap<u64, Insn> = BTreeMap::new();
    let mut functions: BTreeMap<u64, Function> = BTreeMap::new();
    let mut cur: Option<Function> = None;
    while let Some(line) = lines.next() {
        let l = line.trim_end_matches('\n');
        let (word, rest) = l.split_once(' ').unwrap_or((l, ""));
        match word {
            "lifted" => {
                let pc = hex(rest)?;
                let mut block = String::new();
                for b in lines.by_ref() {
                    block.push_str(b);
                    if b.trim_start().starts_with("exit ") {
                        break;
                    }
                }
                let b = relift_ir::parse(&block, files).map_err(|e| format!("{pc:#x}: {e}"))?;
                insts.insert(pc, Insn::Lifted(Arc::new(b)));
            }
            "failed" => {
                let mut f = rest.splitn(3, ' ');
                let pc = hex(f.next().unwrap_or_default())?;
                let insn = hex(f.next().unwrap_or_default())?;
                let message = f.next().unwrap_or_default().to_string();
                insts.insert(pc, Insn::Failed(LiftFailure { insn, message }));
            }
            "fn" => {
                if let Some(f) = cur.take() {
                    functions.insert(f.entry, f);
                }
                let mut f = rest.splitn(3, ' ');
                let entry = hex(f.next().unwrap_or_default())?;
                let kind = kind_of(f.next().unwrap_or_default()).ok_or("a bad entry kind")?;
                cur = Some(Function {
                    entry,
                    name: f.next().map(str::to_string),
                    kind,
                    insts: BTreeMap::new(),
                    tables: BTreeMap::new(),
                    pads: BTreeSet::new(),
                });
            }
            "insts" | "table" | "pads" => {
                let f = cur.as_mut().ok_or("a function line before any fn")?;
                match word {
                    "insts" => {
                        for pc in hexes(rest)? {
                            let i = insts.get(&pc).ok_or(format!("no instruction {pc:#x}"))?;
                            f.insts.insert(pc, i.clone());
                        }
                    }
                    "table" => {
                        let v = hexes(rest)?;
                        let (&j, ts) = v.split_first().ok_or("an empty table line")?;
                        f.tables.insert(j, ts.to_vec());
                    }
                    _ => f.pads.extend(hexes(rest)?),
                }
            }
            "" => {}
            w => return Err(format!("unknown line {w}")),
        }
    }
    if let Some(f) = cur.take() {
        functions.insert(f.entry, f);
    }
    Ok((key, Analysis { functions }))
}

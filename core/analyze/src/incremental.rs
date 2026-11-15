//! Incremental analysis: after a change to a module's bytes, lift again
//! only the functions whose bytes changed ([`rediscover`]), from the
//! outlines of an earlier analysis.

use std::collections::{BTreeMap, BTreeSet};
use std::ops::Range;

use relift_guest::{Frontend, Module};

use crate::{Analysis, EntryKind, Function, Insn, Layout, Outline, Progress, each_lifted, explore};

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

/// The functions of `layout` that hold a byte of `ranges`, lifted with `fe`
/// from `module`, the module `layout` outlines. Only the functions whose
/// extent reaches a range are lifted.
pub fn touching(
    fe: &(dyn Frontend + Sync),
    module: &Module,
    layout: &Layout,
    ranges: &[Range<u64>],
) -> Analysis {
    let near: Vec<&Outline> = layout
        .functions
        .values()
        .filter(|o| {
            o.runs
                .first()
                .is_some_and(|&(lo, _)| overlaps(ranges, lo, o.reach))
        })
        .collect();
    let granule = fe.granule();
    let held = |f: &Function| {
        f.insts.iter().any(|(&pc, i)| {
            let len = match i {
                Insn::Lifted(b) => u64::from(b.len).max(1),
                Insn::Failed(_) => granule,
            };
            overlaps(ranges, pc, pc + len)
        })
    };
    Analysis {
        functions: each_lifted(fe, module, &near, |_, f| held(f).then(|| f.clone()))
            .into_iter()
            .flatten()
            .map(|f| (f.entry, f))
            .collect(),
    }
}

/// The entries of the functions of `base` (the outlines of `original`)
/// that hold a changed byte, and of those with jump tables when a change
/// lies outside code (a table may live in the changed data).
pub fn stale(
    fe: &(dyn Frontend + Sync),
    original: &Module,
    base: &Layout,
    changed: &[Range<u64>],
) -> BTreeSet<u64> {
    let granule = fe.granule();
    let data_changed = changed.iter().any(|r| {
        (r.start..r.end)
            .step_by(usize::try_from(granule.max(1)).unwrap_or(1))
            .any(|a| !original.is_code(a))
    });
    let mut out: BTreeSet<u64> = touching(fe, original, base, changed)
        .functions
        .into_keys()
        .collect();
    if data_changed {
        out.extend(
            base.functions
                .values()
                .filter(|o| !o.tables.is_empty())
                .map(|o| o.entry),
        );
    }
    out
}

/// The functions of `module`, whose bytes differ from those of `original`
/// only inside `changed`, from `base`, the outlines of `original`: every
/// function of `base` that holds no changed byte is taken as it is, and
/// discovery runs again from the others and from the roots of `module`
/// that `base` does not know. Gives `visit` of each function, in entry
/// order, as [`crate::outline_visit`] does.
///
/// The result equals [`crate::outline_visit`] on `module` except where the
/// change adds an entry inside a function taken unchanged: that function
/// keeps its copy of the code, as with [`crate::discover_more`]. An entry
/// only the changed code reached stays a function.
pub fn rediscover<T: Send>(
    fe: &(dyn Frontend + Sync),
    module: &Module,
    original: &Module,
    base: &Layout,
    changed: &[Range<u64>],
    on: &mut dyn FnMut(Progress),
    visit: &(dyn Fn(&Function) -> T + Sync),
) -> (Layout, Vec<T>, Reuse) {
    let granule = fe.granule();
    let stale = stale(fe, original, base, changed);
    let mut kinds: BTreeMap<u64, EntryKind> =
        base.functions.values().map(|o| (o.entry, o.kind)).collect();
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
        .filter(|o| !stale.contains(&o.entry))
        .map(|o| (o.entry, kinds[&o.entry]))
        .collect();
    let (fresh, fresh_visits) = explore(fe, module, kinds, &skip, &names, on, visit);
    let reused: Vec<Outline> = base
        .functions
        .values()
        .filter(|o| skip.contains_key(&o.entry) && !fresh.functions.contains_key(&o.entry))
        .map(|o| {
            let mut o = o.clone();
            o.kind = skip[&o.entry];
            if o.name.is_none() {
                o.name = names.get(&o.entry).cloned();
            }
            o
        })
        .collect();
    let refs: Vec<&Outline> = reused.iter().collect();
    let reused_visits = each_lifted(fe, module, &refs, |_, f| visit(f));
    let reuse = Reuse {
        reused: reused.iter().map(|o| o.entry).collect(),
        lifted: fresh.functions.keys().copied().collect(),
    };
    let mut a = fresh.functions.into_values().zip(fresh_visits).peekable();
    let mut b = reused.into_iter().zip(reused_visits).peekable();
    let mut functions = BTreeMap::new();
    let mut visits = Vec::new();
    loop {
        let take_a = match (a.peek(), b.peek()) {
            (Some(x), Some(y)) => x.0.entry < y.0.entry,
            (Some(_), None) => true,
            (None, Some(_)) => false,
            (None, None) => break,
        };
        let next = if take_a { a.next() } else { b.next() };
        if let Some((o, v)) = next {
            visits.push(v);
            functions.insert(o.entry, o);
        }
    }
    (Layout { functions }, visits, reuse)
}

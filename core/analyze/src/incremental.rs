//! Incremental analysis: after a change to a module's bytes, lift again
//! only the functions whose bytes changed ([`rediscover`]).

use std::collections::{BTreeMap, BTreeSet};
use std::ops::Range;

use relift_guest::{Frontend, Module};

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

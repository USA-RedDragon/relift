//! Function boundaries, control flow, jump tables and indirect targets, generic over
//! a guest's CPU frontend.
//!
//! [`discover`] finds the functions of one [`Module`]: it starts from the
//! entries the guest names (entry point, symbols, unwind entries,
//! relocation-stored pointers), lifts each function's extent by
//! reachability over the IR's next-pc ([`extent`]), and adds direct call
//! targets and code addresses the code computes ([`consts`]) as new entries
//! until nothing new appears. Then every extent is lifted again against the
//! final entry set, so a jump to an entry found late becomes a tail call
//! rather than a copy of its code. Jump tables are resolved by running the
//! code before the jump ([`tables`]); their targets join the function. A
//! computed jump that is not a recognized table stays a dynamic exit through
//! the dispatcher.

mod consts;
mod tables;

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

use relift_guest::{Frontend, LiftFailure, Module, RootKind};
use relift_ir::{Block, Flow, Next};

/// Why analysis treats an address as a function entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum EntryKind {
    /// The guest named it.
    Root(RootKind),
    /// A direct call targets it.
    Called,
    /// The code computes it as a value (address taken).
    AddressTaken,
}

/// One lifted instruction, or why it could not be lifted.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Insn {
    /// The lifted block.
    Lifted(Block),
    /// The frontend refused it; the emitter reports it at run time.
    Failed(LiftFailure),
}

/// One function: an entry and every instruction reachable from it without
/// entering another function.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Function {
    /// The entry address.
    pub entry: u64,
    /// The symbol name, when the guest gave one.
    pub name: Option<String>,
    /// Why it is an entry (the first reason found, in [`EntryKind`] order).
    pub kind: EntryKind,
    /// The instructions by address.
    pub insts: BTreeMap<u64, Insn>,
    /// The targets of the jump tables, by the address of their computed jump.
    pub tables: BTreeMap<u64, Vec<u64>>,
}

impl Function {
    /// The guest bytes from the entry to the end of the last instruction,
    /// for crash mapping.
    pub fn span(&self, granule: u64) -> u64 {
        let last = self
            .insts
            .iter()
            .next_back()
            .map(|(pc, i)| match i {
                Insn::Lifted(b) => pc + u64::from(b.len),
                Insn::Failed(_) => pc + granule,
            })
            .unwrap_or(self.entry);
        last.saturating_sub(self.entry)
    }
}

/// The functions of one module.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Analysis {
    /// The functions by entry address.
    pub functions: BTreeMap<u64, Function>,
}

impl Analysis {
    /// Whether `addr` is a function entry.
    pub fn is_entry(&self, addr: u64) -> bool {
        self.functions.contains_key(&addr)
    }

    /// How many instructions failed to lift, over all functions (an
    /// instruction shared by several extents counts once per function).
    pub fn failures(&self) -> usize {
        self.functions
            .values()
            .flat_map(|f| f.insts.values())
            .filter(|i| matches!(i, Insn::Failed(_)))
            .count()
    }
}

/// Lifts and caches instructions by address.
struct Lifter<'a> {
    fe: &'a dyn Frontend,
    module: &'a Module,
    cache: HashMap<u64, Insn>,
}

impl Lifter<'_> {
    fn get(&mut self, pc: u64) -> Insn {
        if let Some(i) = self.cache.get(&pc) {
            return i.clone();
        }
        let bytes = self.module.bytes_at(pc).unwrap_or(&[]);
        let i = match self.fe.lift(bytes, pc) {
            Ok(b) => Insn::Lifted(b),
            Err(e) => Insn::Failed(e),
        };
        self.cache.insert(pc, i.clone());
        i
    }
}

/// The successors of `insn` inside the function whose entry is
/// `entry`: fall-through, branch targets and return points, but not call
/// targets, other entries (tail calls) or addresses outside code.
pub fn successors(module: &Module, entries: &BTreeSet<u64>, entry: u64, insn: &Insn) -> Vec<u64> {
    let Insn::Lifted(b) = insn else {
        return Vec::new();
    };
    let inside = |t: u64| module.is_code(t) && (t == entry || !entries.contains(&t));
    let mut out = Vec::new();
    match b.exit.flow {
        Flow::Return => {}
        Flow::Call { ret } => {
            if module.is_code(ret) {
                out.push(ret);
            }
        }
        Flow::Jump | Flow::Trap(_) => match b.exit.next {
            Next::Const(t) => out.push(t),
            Next::Select {
                taken, not_taken, ..
            } => {
                out.push(taken);
                out.push(not_taken);
            }
            Next::Dynamic(_) => {}
        },
    }
    out.retain(|t| inside(*t));
    out
}

/// A function's instructions and its jump tables.
type Extent = (BTreeMap<u64, Insn>, BTreeMap<u64, Vec<u64>>);

/// Lifts the extent of the function at `entry`: everything reachable over
/// [`successors`] and over the jump tables found in it.
fn extent(lifter: &mut Lifter<'_>, entries: &BTreeSet<u64>, entry: u64) -> Extent {
    let mut insts: BTreeMap<u64, Insn> = BTreeMap::new();
    let mut tables: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
    let mut work = vec![entry];
    let granule = lifter.fe.granule();
    let preserved = lifter.fe.preserved();
    loop {
        while let Some(pc) = work.pop() {
            if insts.contains_key(&pc) || !lifter.module.is_code(pc) {
                continue;
            }
            let i = lifter.get(pc);
            work.extend(successors(lifter.module, entries, entry, &i));
            insts.insert(pc, i);
        }
        let jumps: Vec<(u64, Block)> = insts
            .iter()
            .filter(|(pc, _)| !tables.contains_key(pc))
            .filter_map(|(pc, i)| match i {
                Insn::Lifted(b)
                    if b.exit.flow == Flow::Jump && matches!(b.exit.next, Next::Dynamic(_)) =>
                {
                    Some((*pc, b.clone()))
                }
                _ => None,
            })
            .collect();
        if jumps.is_empty() {
            break;
        }
        let states = consts::in_states(&insts, entry, &preserved);
        let known = |a: u64| states.get(&a).cloned().unwrap_or_default();
        let module = lifter.module;
        let mut found = false;
        for (pc, b) in jumps {
            let mut at = |a: u64| match lifter.get(a) {
                Insn::Lifted(b) => Some(b),
                Insn::Failed(_) => None,
            };
            let entering = |a: u64| {
                insts.values().find_map(|i| match i {
                    Insn::Lifted(b) => match b.exit.next {
                        Next::Select { taken, .. } if taken == a => Some(b.clone()),
                        _ => None,
                    },
                    Insn::Failed(_) => None,
                })
            };
            let t = tables::resolve(module, pc, &b, granule, &mut at, &entering, &known)
                .unwrap_or_default();
            work.extend(
                t.iter()
                    .copied()
                    .filter(|a| *a == entry || !entries.contains(a)),
            );
            found |= !t.is_empty();
            tables.insert(pc, t);
        }
        if !found {
            break;
        }
    }
    tables.retain(|_, t| !t.is_empty());
    (insts, tables)
}

/// Direct call targets and computed code addresses in `insts`.
fn new_entries(
    module: &Module,
    granule: u64,
    insts: &BTreeMap<u64, Insn>,
) -> Vec<(u64, EntryKind)> {
    let mut out = Vec::new();
    for i in insts.values() {
        if let Insn::Lifted(b) = i
            && let (Flow::Call { .. }, Next::Const(t)) = (b.exit.flow, b.exit.next)
        {
            out.push((t, EntryKind::Called));
        }
    }
    for a in consts::code_addresses(module, granule, insts) {
        out.push((a, EntryKind::AddressTaken));
    }
    out.retain(|(a, _)| module.is_code(*a) && a % granule == 0);
    out
}

/// What [`discover_with`] reports while it works.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Progress {
    /// Entries found so far, and how many of them have been explored.
    Found {
        /// Function entries known.
        entries: usize,
        /// Entries whose code has been followed.
        explored: usize,
    },
    /// Functions built so far.
    Lifted {
        /// Functions finished.
        done: usize,
        /// Functions in all.
        total: usize,
        /// Distinct addresses that failed to lift, over the finished
        /// functions.
        unlifted: usize,
    },
}

/// Finds the functions of `module` with frontend `fe`.
pub fn discover(fe: &dyn Frontend, module: &Module) -> Analysis {
    discover_with(fe, module, &mut |_| {})
}

/// As [`discover`], calling `on` after every entry explored and every
/// function built.
pub fn discover_with(fe: &dyn Frontend, module: &Module, on: &mut dyn FnMut(Progress)) -> Analysis {
    let granule = fe.granule();
    let mut lifter = Lifter {
        fe,
        module,
        cache: HashMap::new(),
    };
    let mut kinds: BTreeMap<u64, EntryKind> = BTreeMap::new();
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
    let mut done: BTreeSet<u64> = BTreeSet::new();
    loop {
        let entries: BTreeSet<u64> = kinds.keys().copied().collect();
        let todo: Vec<u64> = entries.difference(&done).copied().collect();
        if todo.is_empty() {
            break;
        }
        for e in todo {
            let (insts, _) = extent(&mut lifter, &entries, e);
            for (a, k) in new_entries(module, granule, &insts) {
                kinds.entry(a).or_insert(k);
            }
            done.insert(e);
            on(Progress::Found {
                entries: kinds.len(),
                explored: done.len(),
            });
        }
    }
    let entries: BTreeSet<u64> = kinds.keys().copied().collect();
    let total = kinds.len();
    let mut failed: HashSet<u64> = HashSet::new();
    let functions = kinds
        .iter()
        .enumerate()
        .map(|(n, (&e, &kind))| {
            let (insts, tables) = extent(&mut lifter, &entries, e);
            failed.extend(
                insts
                    .iter()
                    .filter(|(_, i)| matches!(i, Insn::Failed(_)))
                    .map(|(&pc, _)| pc),
            );
            on(Progress::Lifted {
                done: n + 1,
                total,
                unlifted: failed.len(),
            });
            (
                e,
                Function {
                    entry: e,
                    name: names.get(&e).cloned(),
                    kind,
                    insts,
                    tables,
                },
            )
        })
        .collect();
    Analysis { functions }
}

#[cfg(test)]
mod tests;

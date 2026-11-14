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
//! the dispatcher. [`unresolved_transfers`] lists those dynamic exits and
//! the indirect calls, for coverage reports. [`rediscover`] lifts again only
//! the functions a change to the module's bytes touches.

mod binary;
mod consts;
mod incremental;
mod slots;
mod tables;
mod transfers;

pub use binary::{FORMAT, from_bytes, to_bytes};
pub use incremental::{Reuse, rediscover, stale};
pub use slots::{SlotUses, slot_uses};
pub use transfers::{Source, Transfer, TransferKind, unresolved_transfers};

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::sync::Arc;

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
    /// The lifted block, one allocation shared by every function that
    /// holds the instruction.
    Lifted(Arc<Block>),
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
    /// The landing pads of the module's unwind tables that serve a call in
    /// this function and lie in its extent.
    pub pads: BTreeSet<u64>,
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

/// A hasher for instruction addresses.
#[derive(Clone, Copy, Default)]
struct PcHasher(u64);

impl std::hash::Hasher for PcHasher {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.0 = (self.0.rotate_left(8) ^ u64::from(b)).wrapping_mul(0x9e37_79b9_7f4a_7c15);
        }
    }

    fn write_u64(&mut self, n: u64) {
        self.0 = (self.0 ^ n)
            .wrapping_mul(0x9e37_79b9_7f4a_7c15)
            .rotate_left(29);
    }
}

type PcHash = std::hash::BuildHasherDefault<PcHasher>;

/// Lifts and caches instructions by address, for a group of functions
/// that one thread explores or lifts one after another.
struct Lifter<'a> {
    fe: &'a dyn Frontend,
    module: &'a Module,
    cache: HashMap<u64, Insn, PcHash>,
    pads: &'a Pads,
}

impl<'a> Lifter<'a> {
    fn new(fe: &'a dyn Frontend, module: &'a Module, pads: &'a Pads) -> Lifter<'a> {
        Lifter {
            fe,
            module,
            cache: HashMap::default(),
            pads,
        }
    }

    /// Drops the cached instructions below `next`, the entry explored
    /// next (all of them when there is none): code below an entry is
    /// rarely part of its extent.
    fn keep_from(&mut self, next: Option<u64>) {
        match next {
            Some(n) => self.cache.retain(|pc, _| *pc >= n),
            None => self.cache.clear(),
        }
    }

    fn get(&mut self, pc: u64) -> Insn {
        if let Some(i) = self.cache.get(&pc) {
            return i.clone();
        }
        let bytes = self.module.bytes_at(pc).unwrap_or(&[]);
        let i = match self.fe.lift(bytes, pc) {
            Ok(mut b) => {
                b.insts.shrink_to_fit();
                Insn::Lifted(Arc::new(b))
            }
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

/// The landing pads of a module by the address of the call they serve.
type Pads = BTreeMap<u64, Vec<u64>>;

static NO_PADS: Pads = BTreeMap::new();

fn module_pads(module: &Module) -> Pads {
    let mut by_site: Pads = BTreeMap::new();
    for p in &module.pads {
        by_site.entry(p.site).or_default().push(p.addr);
    }
    by_site
}

/// A function's instructions, its jump tables and its landing pads.
type Extent = (BTreeMap<u64, Insn>, BTreeMap<u64, Vec<u64>>, BTreeSet<u64>);

/// Lifts the extent of the function at `entry`: everything reachable over
/// [`successors`], over the jump tables found in it and from the landing
/// pads that serve its calls.
fn extent(lifter: &mut Lifter<'_>, entries: &BTreeSet<u64>, entry: u64) -> Extent {
    let mut insts: BTreeMap<u64, Insn> = BTreeMap::new();
    let mut tables: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
    let mut pads: BTreeSet<u64> = BTreeSet::new();
    let mut work = vec![entry];
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
        let new_pads: Vec<u64> = lifter
            .pads
            .range(entry..)
            .filter(|(site, _)| insts.contains_key(site))
            .flat_map(|(_, p)| p.iter().copied())
            .filter(|p| !pads.contains(p) && !entries.contains(p))
            .collect();
        if !new_pads.is_empty() {
            pads.extend(new_pads.iter().copied());
            work.extend(new_pads);
            continue;
        }
        let jumps: Vec<(u64, Arc<Block>)> = insts
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
        let fe = lifter.fe;
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
            let t =
                tables::resolve(module, pc, &b, fe, &mut at, &entering, &known).unwrap_or_default();
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
    pads.retain(|p| insts.contains_key(p));
    (insts, tables, pads)
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

/// The code addresses `f` computes (its functions whose address is taken,
/// among other code addresses), as discovery found them.
pub fn address_taken(module: &Module, granule: u64, f: &Function) -> BTreeSet<u64> {
    consts::code_addresses(module, granule, &f.insts)
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

/// One function without its lifted code: where its instructions lie, its
/// jump tables and the instructions that failed to lift. [`Outline::lift`]
/// lifts it again into a [`Function`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Outline {
    /// The entry address.
    pub entry: u64,
    /// The symbol name, when the guest gave one.
    pub name: Option<String>,
    /// Why it is an entry (the first reason found, in [`EntryKind`] order).
    pub kind: EntryKind,
    /// The instruction addresses, as runs of `(first, count)` addresses one
    /// granule apart, in address order.
    pub runs: Vec<(u64, u32)>,
    /// The targets of the jump tables, by the address of their computed jump.
    pub tables: BTreeMap<u64, Vec<u64>>,
    /// The landing pads that serve a call in the function and lie in its
    /// extent.
    pub pads: BTreeSet<u64>,
    /// The instructions the frontend refused, by address.
    pub failed: Vec<(u64, LiftFailure)>,
    /// The direct calls, as `(target, return address)`, in address order.
    pub calls: Vec<(u64, u64)>,
    /// The address after the last instruction.
    pub end: u64,
    granule: u64,
}

impl Outline {
    fn new(entry: u64, kind: EntryKind, granule: u64, ext: &Extent) -> Outline {
        Outline::of_parts(entry, kind, granule, &ext.0, &ext.1, &ext.2)
    }

    /// The outline of `f`, whose instructions lie `granule` bytes apart.
    pub fn of(f: &Function, granule: u64) -> Outline {
        let mut o = Outline::of_parts(f.entry, f.kind, granule, &f.insts, &f.tables, &f.pads);
        o.name.clone_from(&f.name);
        o
    }

    fn of_parts(
        entry: u64,
        kind: EntryKind,
        granule: u64,
        insts: &BTreeMap<u64, Insn>,
        tables: &BTreeMap<u64, Vec<u64>>,
        pads: &BTreeSet<u64>,
    ) -> Outline {
        let mut runs: Vec<(u64, u32)> = Vec::new();
        for &pc in insts.keys() {
            match runs.last_mut() {
                Some((first, n)) if *first + u64::from(*n) * granule == pc && *n < u32::MAX => {
                    *n += 1;
                }
                _ => runs.push((pc, 1)),
            }
        }
        let failed = insts
            .iter()
            .filter_map(|(&pc, i)| match i {
                Insn::Failed(e) => Some((pc, e.clone())),
                Insn::Lifted(_) => None,
            })
            .collect();
        let calls = insts
            .values()
            .filter_map(|i| match i {
                Insn::Lifted(b) => match (b.exit.flow, b.exit.next) {
                    (Flow::Call { ret }, Next::Const(t)) => Some((t, ret)),
                    _ => None,
                },
                Insn::Failed(_) => None,
            })
            .collect();
        let end = insts
            .iter()
            .next_back()
            .map(|(pc, i)| match i {
                Insn::Lifted(b) => pc + u64::from(b.len),
                Insn::Failed(_) => pc + granule,
            })
            .unwrap_or(entry);
        Outline {
            entry,
            name: None,
            kind,
            runs,
            tables: tables.clone(),
            pads: pads.clone(),
            failed,
            calls,
            end,
            granule,
        }
    }

    /// The instruction addresses, in order.
    pub fn pcs(&self) -> impl Iterator<Item = u64> + '_ {
        let g = self.granule;
        self.runs
            .iter()
            .flat_map(move |&(first, n)| (0..u64::from(n)).map(move |k| first + k * g))
    }

    /// The number of instructions.
    pub fn len(&self) -> usize {
        self.runs.iter().map(|r| r.1 as usize).sum()
    }

    /// Whether the function has no instructions.
    pub fn is_empty(&self) -> bool {
        self.runs.is_empty()
    }

    /// Whether `pc` is one of the function's instructions.
    pub fn contains(&self, pc: u64) -> bool {
        let i = self.runs.partition_point(|r| r.0 <= pc);
        i > 0 && {
            let (first, n) = self.runs[i - 1];
            let off = pc - first;
            off.is_multiple_of(self.granule) && off / self.granule < u64::from(n)
        }
    }

    /// The guest bytes from the entry to the end of the last instruction,
    /// as [`Function::span`] gives them.
    pub fn span(&self) -> u64 {
        self.end.saturating_sub(self.entry)
    }

    /// Lifts the function's instructions again with `fe` from `module`, the
    /// frontend and module that outlined it.
    pub fn lift(&self, fe: &dyn Frontend, module: &Module) -> Function {
        self.lift_with(&mut Lifter::new(fe, module, &NO_PADS))
    }

    fn lift_with(&self, lifter: &mut Lifter<'_>) -> Function {
        Function {
            entry: self.entry,
            name: self.name.clone(),
            kind: self.kind,
            insts: self.pcs().map(|pc| (pc, lifter.get(pc))).collect(),
            tables: self.tables.clone(),
            pads: self.pads.clone(),
        }
    }
}

/// `f` of each of `outlines` lifted again with `fe` from `module`, in
/// order, on up to one thread per CPU. Functions lifted at the same time
/// share the blocks of the instructions they have in common.
pub fn each_lifted<R: Send>(
    fe: &(dyn Frontend + Sync),
    module: &Module,
    outlines: &[&Outline],
    f: impl Fn(&Outline, &Function) -> R + Sync,
) -> Vec<R> {
    grouped(outlines, |group| {
        let mut out = Vec::with_capacity(group.len());
        lift_each(fe, module, group, &mut |o, l| out.push(f(o, l)));
        out
    })
}

/// Calls `f` with each of `outlines` lifted again with `fe` from `module`,
/// in order, on this thread; a function shares the blocks of the
/// instructions it has in common with the one before.
pub fn lift_each(
    fe: &dyn Frontend,
    module: &Module,
    outlines: &[&Outline],
    f: &mut dyn FnMut(&Outline, &Function),
) {
    let mut lifter = Lifter::new(fe, module, &NO_PADS);
    for (k, o) in outlines.iter().enumerate() {
        f(o, &o.lift_with(&mut lifter));
        match outlines.get(k + 1) {
            Some(next) => lifter.cache.retain(|pc, _| next.contains(*pc)),
            None => lifter.cache.clear(),
        }
    }
}

/// The functions of one module as outlines, without their lifted code.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Layout {
    /// The functions by entry address.
    pub functions: BTreeMap<u64, Outline>,
}

impl Layout {
    /// The outlines of the functions of `a`, whose instructions lie
    /// `granule` bytes apart.
    pub fn of(a: &Analysis, granule: u64) -> Layout {
        Layout {
            functions: a
                .functions
                .values()
                .map(|f| (f.entry, Outline::of(f, granule)))
                .collect(),
        }
    }

    /// The functions of the layout lifted again with `fe` from `module`.
    pub fn lift(&self, fe: &(dyn Frontend + Sync), module: &Module) -> Analysis {
        let all: Vec<&Outline> = self.functions.values().collect();
        Analysis {
            functions: each_lifted(fe, module, &all, |_, f| f.clone())
                .into_iter()
                .map(|f| (f.entry, f))
                .collect(),
        }
    }

    /// Whether `addr` is a function entry.
    pub fn is_entry(&self, addr: u64) -> bool {
        self.functions.contains_key(&addr)
    }
}

/// Finds the functions of `module` with frontend `fe`.
pub fn discover(fe: &(dyn Frontend + Sync), module: &Module) -> Analysis {
    discover_with(fe, module, &mut |_| {})
}

/// As [`discover`], calling `on` after every entry explored and every
/// function built.
pub fn discover_with(
    fe: &(dyn Frontend + Sync),
    module: &Module,
    on: &mut dyn FnMut(Progress),
) -> Analysis {
    let layout = outline_with(fe, module, on);
    Analysis {
        functions: layout
            .functions
            .values()
            .map(|o| (o.entry, o.lift(fe, module)))
            .collect(),
    }
}

/// Finds the functions of `module` with frontend `fe` as [`discover`] does,
/// keeping only their outlines; calls `on` as [`discover_with`] does.
pub fn outline_with(
    fe: &(dyn Frontend + Sync),
    module: &Module,
    on: &mut dyn FnMut(Progress),
) -> Layout {
    outline_visit(fe, module, on, &|_| ()).0
}

/// As [`outline_with`], also giving `visit` of each function while it is
/// lifted, in entry order.
pub fn outline_visit<T: Send>(
    fe: &(dyn Frontend + Sync),
    module: &Module,
    on: &mut dyn FnMut(Progress),
    visit: &(dyn Fn(&Function) -> T + Sync),
) -> (Layout, Vec<T>) {
    let granule = fe.granule();
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
    explore(fe, module, kinds, &BTreeMap::new(), &names, on, visit)
}

/// The functions that become reachable in `module` from `roots` when
/// `known` (every entry of an earlier analysis, by kind) is already lifted:
/// discovery as [`discover`] does it, starting from the roots and stopping
/// at known entries, which it neither explores nor returns. Each root not
/// already known is a [`RootKind::Hint`] entry. A root inside the extent of
/// a known function gets a function of its own; the known one keeps its
/// copy of the code.
pub fn discover_more(
    fe: &(dyn Frontend + Sync),
    module: &Module,
    known: &BTreeMap<u64, EntryKind>,
    roots: &[u64],
) -> Analysis {
    let granule = fe.granule();
    let mut kinds = known.clone();
    for &r in roots {
        if module.is_code(r) && r % granule == 0 {
            kinds.entry(r).or_insert(EntryKind::Root(RootKind::Hint));
        }
    }
    let (layout, _) = explore(
        fe,
        module,
        kinds,
        known,
        &BTreeMap::new(),
        &mut |_| {},
        &|_| (),
    );
    Analysis {
        functions: layout
            .functions
            .values()
            .map(|o| (o.entry, o.lift(fe, module)))
            .collect(),
    }
}

/// How many entries one parallel step explores before reporting progress.
const STEP: usize = 512;

/// Explores every entry of `kinds` not in `skip` until no new entry
/// appears, then outlines every function not in `skip`, giving `visit` of
/// each while it is lifted.
fn explore<T: Send>(
    fe: &(dyn Frontend + Sync),
    module: &Module,
    mut kinds: BTreeMap<u64, EntryKind>,
    skip: &BTreeMap<u64, EntryKind>,
    names: &BTreeMap<u64, String>,
    on: &mut dyn FnMut(Progress),
    visit: &(dyn Fn(&Function) -> T + Sync),
) -> (Layout, Vec<T>) {
    let granule = fe.granule();
    let pads = module_pads(module);
    let mut done: BTreeSet<u64> = skip.keys().copied().collect();
    let mut explored: HashMap<u64, (Outline, T)> = HashMap::new();
    let outline = |e: u64, kind: EntryKind, ext: Extent| {
        let o = Outline::new(e, kind, granule, &ext);
        let f = Function {
            entry: e,
            name: names.get(&e).cloned(),
            kind,
            insts: ext.0,
            tables: ext.1,
            pads: ext.2,
        };
        (o, visit(&f))
    };
    // A function's extent stops at the entries of other functions, except
    // at addresses only known as computed values: those are often labels
    // inside a function (a jump-table base or a loop head taken with adr),
    // and cutting there would lose the state the code before them set up.
    // They still get a function of their own for indirect jumps to them.
    let stops = |kinds: &BTreeMap<u64, EntryKind>| -> BTreeSet<u64> {
        kinds
            .iter()
            .filter(|(_, k)| **k != EntryKind::AddressTaken)
            .map(|(&a, _)| a)
            .collect()
    };
    loop {
        let entries: BTreeSet<u64> = kinds.keys().copied().collect();
        let todo: Vec<u64> = entries.difference(&done).copied().collect();
        let entries = stops(&kinds);
        if todo.is_empty() {
            break;
        }
        for step in todo.chunks(STEP) {
            let found = grouped(step, |group| {
                let mut lifter = Lifter::new(fe, module, &pads);
                group
                    .iter()
                    .enumerate()
                    .map(|(k, &e)| {
                        let ext = extent(&mut lifter, &entries, e);
                        let new = new_entries(module, granule, &ext.0);
                        let o = outline(e, kinds[&e], ext);
                        lifter.keep_from(group.get(k + 1).copied());
                        (o, new)
                    })
                    .collect()
            });
            for (&e, (o, new)) in step.iter().zip(found) {
                for (a, k) in new {
                    kinds.entry(a).or_insert(k);
                }
                explored.insert(e, o);
                done.insert(e);
                on(Progress::Found {
                    entries: kinds.len(),
                    explored: done.len(),
                });
            }
        }
    }
    let entries = stops(&kinds);
    let all: Vec<(u64, EntryKind)> = kinds
        .into_iter()
        .filter(|(a, _)| !skip.contains_key(a))
        .collect();
    let total = all.len();
    let mut failed: HashSet<u64> = HashSet::new();
    let mut functions = BTreeMap::new();
    let mut visited = Vec::with_capacity(total);
    let mut n = 0;
    for step in all.chunks(STEP) {
        let kept: Vec<Option<(Outline, T)>> = step
            .iter()
            .map(|(e, kind)| {
                explored.remove(e).filter(|(o, _)| {
                    o.kind == *kind && o.pcs().all(|a| a == *e || !entries.contains(&a))
                })
            })
            .collect();
        let redo: Vec<(u64, EntryKind)> = step
            .iter()
            .zip(&kept)
            .filter(|(_, k)| k.is_none())
            .map(|(x, _)| *x)
            .collect();
        let mut redone = grouped(&redo, |group| {
            let mut lifter = Lifter::new(fe, module, &pads);
            group
                .iter()
                .enumerate()
                .map(|(k, &(e, kind))| {
                    let o = outline(e, kind, extent(&mut lifter, &entries, e));
                    lifter.keep_from(group.get(k + 1).map(|n| n.0));
                    o
                })
                .collect()
        })
        .into_iter();
        for k in kept {
            let Some((mut o, v)) = k.or_else(|| redone.next()) else {
                continue;
            };
            visited.push(v);
            o.name = names.get(&o.entry).cloned();
            failed.extend(o.failed.iter().map(|(pc, _)| *pc));
            n += 1;
            on(Progress::Lifted {
                done: n,
                total,
                unlifted: failed.len(),
            });
            functions.insert(o.entry, o);
        }
    }
    (Layout { functions }, visited)
}

/// How many neighboring items one thread takes at a time in [`grouped`]:
/// neighboring functions often share code, which a group lifts once.
const GROUP: usize = 16;

/// `f` of each group of up to [`GROUP`] neighboring items of `items`,
/// flattened in order, on up to one thread per CPU; `f` gives one result
/// per item of its group.
fn grouped<T: Sync, R: Send>(items: &[T], f: impl Fn(&[T]) -> Vec<R> + Sync) -> Vec<R> {
    let groups: Vec<&[T]> = items.chunks(GROUP).collect();
    parallel(&groups, |g| f(g)).into_iter().flatten().collect()
}

/// `f` of every item of `items`, in order, on up to one thread per CPU.
pub fn parallel<T: Sync, R: Send>(items: &[T], f: impl Fn(&T) -> R + Sync) -> Vec<R> {
    let threads = std::thread::available_parallelism()
        .map_or(1, |n| n.get())
        .min(items.len());
    if threads <= 1 {
        return items.iter().map(f).collect();
    }
    let next = std::sync::atomic::AtomicUsize::new(0);
    let mut out: Vec<(usize, R)> = std::thread::scope(|s| {
        let workers: Vec<_> = (0..threads)
            .map(|_| {
                s.spawn(|| {
                    let mut mine = Vec::new();
                    loop {
                        let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                        let Some(item) = items.get(i) else {
                            break;
                        };
                        mine.push((i, f(item)));
                    }
                    mine
                })
            })
            .collect();
        workers
            .into_iter()
            .flat_map(|w| w.join().unwrap_or_else(|e| std::panic::resume_unwind(e)))
            .collect()
    });
    out.sort_unstable_by_key(|r| r.0);
    out.into_iter().map(|r| r.1).collect()
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod incremental_tests;

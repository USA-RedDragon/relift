//! The driver: roots, the fixpoint over functions, RAM pointers and
//! summaries, and the report.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet};
use std::fmt;

use relift_gb_formats::banks::ROM_BANK_SIZE;
use relift_gb_formats::{Cartridge, Error as FormatError, MapperKind};

use crate::bankmodel::BankModel;
use crate::func::{self, CellInfo, CopyLoop, Dep, Env, Outcome, Store, Summary};
use crate::fx::{FxMap, FxSet};
use crate::image::Image;
use crate::loc::Loc;
use crate::report::{
    BankCoverage, Edge, Function, Origin, RamCode, Report, TableBound, Trampoline, Unresolved,
    UnresolvedKind,
};
use crate::state::{Bv, State, Val};

/// Why an image cannot be analyzed.
#[derive(Debug)]
pub enum AnalyzeError {
    /// The header does not parse.
    Format(FormatError),
    /// The mapper's ROM banking is not modelled.
    Mapper(&'static str),
}

impl fmt::Display for AnalyzeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AnalyzeError::Format(e) => write!(f, "{e}"),
            AnalyzeError::Mapper(m) => write!(f, "the {m} mapper's ROM banking is not modelled"),
        }
    }
}

impl std::error::Error for AnalyzeError {}

const VECTORS: [u16; 13] = [
    0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60,
];
const MAX_PROCESS: usize = 200_000;
const MAX_RESTARTS: usize = 8;
const STEP_BUDGET: usize = 2_000_000;

fn reg_name(i: usize) -> char {
    ['A', 'B', 'C', 'D', 'E', 'H', 'L'][i % 7]
}

/// Find the functions of the image `rom`.
///
/// # Errors
///
/// The header does not parse, or the mapper's ROM banking is not modelled.
pub fn analyze(rom: &[u8]) -> Result<Report, AnalyzeError> {
    let cart = Cartridge::parse(rom).map_err(AnalyzeError::Format)?;
    let kind = cart.mapper();
    let banks = rom.len().div_ceil(ROM_BANK_SIZE).max(1);
    let model = BankModel::new(kind, banks).ok_or(AnalyzeError::Mapper(kind.name()))?;
    let mut limits: FxMap<Loc, usize> = FxMap::default();
    for _ in 0..MAX_RESTARTS {
        let mut d = Driver::new(
            BankModel::new(kind, banks).ok_or(AnalyzeError::Mapper(kind.name()))?,
            Image::new(rom),
            &limits,
        );
        d.seed(rom);
        d.solve();
        let fences = d.fences();
        if fences.is_empty() {
            return Ok(d.report(kind, banks));
        }
        for (t, n) in fences {
            let m = limits.entry(t).or_insert(n);
            *m = (*m).min(n);
        }
    }
    let mut d = Driver::new(model, Image::new(rom), &limits);
    d.seed(rom);
    d.solve();
    Ok(d.report(kind, banks))
}

impl<'a> Driver<'a> {
    fn new(model: BankModel, image: Image<'a>, limits: &'a FxMap<Loc, usize>) -> Driver<'a> {
        Driver {
            model,
            image,
            limits,
            table_log: BTreeSet::new(),
            probes: RefCell::new(FxMap::default()),
            called: FxSet::default(),
            steps: 0,
            entries: BTreeMap::new(),
            roots: BTreeMap::new(),
            results: FxMap::default(),
            inline_bodies: FxMap::default(),
            summaries: FxMap::default(),
            cells: FxMap::default(),
            known: FxSet::default(),
            dependents: FxMap::default(),
            cell_dependents: FxMap::default(),
            dirty: Vec::new(),
            queued: FxSet::default(),
        }
    }

    /// Tables bounded by the run of code pointers whose rows overlap code
    /// found later: the row where each must stop.
    fn fences(&self) -> Vec<(Loc, usize)> {
        let mut bytes: FxSet<Loc> = FxSet::default();
        for (alone, root) in self.results.values() {
            for o in std::iter::once(alone).chain(root.iter()) {
                for (l, n) in &o.body {
                    for k in 0..u16::from(*n) {
                        bytes.insert(Loc {
                            addr: l.addr.wrapping_add(k),
                            ..*l
                        });
                    }
                }
            }
        }
        for (l, n) in self
            .inline_bodies
            .values()
            .flat_map(|m| m.values())
            .flatten()
        {
            for k in 0..u16::from(*n) {
                bytes.insert(Loc {
                    addr: l.addr.wrapping_add(k),
                    ..*l
                });
            }
        }
        let mut out = Vec::new();
        {
            {
                for t in self.table_log.iter().filter(|t| t.bound == TableBound::Run) {
                    let step = u16::from(t.step.max(1));
                    for i in 1..t.rows {
                        let a = t.table.addr.wrapping_add(step.wrapping_mul(i as u16));
                        let at = |x: u16| Loc { addr: x, ..t.table };
                        if bytes.contains(&at(a)) || bytes.contains(&at(a.wrapping_add(1))) {
                            out.push((t.table, i));
                            break;
                        }
                    }
                }
            }
        }
        out
    }
}

struct Driver<'a> {
    model: BankModel,
    image: Image<'a>,
    limits: &'a FxMap<Loc, usize>,
    table_log: BTreeSet<crate::report::JumpTable>,
    probes: RefCell<func::Probes>,
    called: FxSet<Loc>,
    steps: usize,
    entries: BTreeMap<Loc, Origin>,
    roots: BTreeMap<Loc, State>,
    results: FxMap<Loc, (Outcome, Option<Outcome>)>,
    inline_bodies: FxMap<Loc, FxMap<Loc, BTreeMap<Loc, u8>>>,
    summaries: FxMap<Loc, Summary>,
    cells: FxMap<u16, CellInfo>,
    known: FxSet<Loc>,
    dependents: FxMap<Loc, BTreeSet<Loc>>,
    cell_dependents: FxMap<u16, BTreeSet<Loc>>,
    dirty: Vec<Loc>,
    queued: FxSet<Loc>,
}

fn merge_val(a: Val, b: Val) -> Val {
    if a == b { a } else { Val::Top }
}

fn merge_bv(a: Bv, b: Bv) -> Bv {
    if a == b { a } else { Bv::Unknown }
}

/// The join of two summaries of one function, so that a summary only
/// moves up and the fixpoint ends.
fn join(a: &Summary, b: &Summary) -> Summary {
    let empty = |s: &Summary| s.exits.is_empty() || s.assumed;
    let (ra, rb) = match (empty(a), empty(b)) {
        (true, false) => (b, b),
        (false, true) => (a, a),
        _ => (a, b),
    };
    Summary {
        regs: std::array::from_fn(|i| merge_val(ra.regs[i], rb.regs[i])),
        lo: merge_bv(ra.lo, rb.lo),
        hi: merge_bv(ra.hi, rb.hi),
        exits: a.exits.union(&b.exits).copied().collect(),
        ctx: a.ctx || b.ctx,
        touches: a.touches || b.touches,
        bank_reg: a.bank_reg.or(b.bank_reg),
        target_regs: a.target_regs.or(b.target_regs),
        pure: a.pure && b.pure,
        store_only: a.store_only && b.store_only,
        assumed: a.assumed && b.assumed,
    }
}

fn summarize(o: &Outcome) -> Summary {
    let mut exits = BTreeSet::new();
    let mut regs: Option<[Val; 7]> = None;
    let mut lo: Option<Bv> = None;
    let mut hi: Option<Bv> = None;
    for (k, s) in &o.exits {
        exits.insert(*k);
        regs = Some(match regs {
            None => s.regs,
            Some(r) => std::array::from_fn(|i| merge_val(r[i], s.regs[i])),
        });
        lo = Some(lo.map_or(s.lo, |l| merge_bv(l, s.lo)));
        hi = Some(hi.map_or(s.hi, |h| merge_bv(h, s.hi)));
    }
    let assumed = exits.is_empty()
        && (o.waits || (!o.popped_ret && !(o.unresolved.is_empty() && o.ram.is_empty())));
    if assumed {
        exits.insert(0);
    }
    let assumed = assumed || (exits.is_empty() && o.popped_ret);
    let regs = regs
        .unwrap_or([Val::Top; 7])
        .map(|v| if v == Val::Dep { Val::Top } else { v });
    let kept = if o.touches { Bv::Unknown } else { Bv::Entry };
    let lo = lo.unwrap_or(kept);
    let hi = hi.unwrap_or(kept);
    let exit_dep = regs
        .iter()
        .any(|v| v.depends_on_entry() && !matches!(v, Val::Entry(_)))
        || lo == Bv::Param
        || hi == Bv::Param;
    Summary {
        exits,
        regs,
        lo,
        hi,
        ctx: o.entry_dep || o.computed_dep || o.store_dep || exit_dep || !o.escapes.is_empty(),
        touches: o.touches,
        bank_reg: o.bank_reg,
        target_regs: o.target_regs,
        pure: !o.impure && o.ram.is_empty() && o.unresolved.is_empty(),
        assumed,
        store_only: !(o.entry_dep || o.computed_dep || exit_dep || !o.escapes.is_empty()),
    }
}

impl Driver<'_> {
    fn add_entry(&mut self, loc: Loc, origin: Origin) {
        if let std::collections::btree_map::Entry::Vacant(v) = self.entries.entry(loc) {
            v.insert(origin);
            self.mark(loc);
        }
    }

    fn mark(&mut self, loc: Loc) {
        if self.queued.insert(loc) {
            self.dirty.push(loc);
        }
    }

    fn seed(&mut self, rom: &[u8]) {
        let mut start = State::symbolic();
        let reset = State::reset(&self.model);
        start.regs = [Val::Top; 7];
        start.lo = reset.lo;
        start.hi = reset.hi;
        self.roots.insert(Loc::fixed(0x100), start);
        self.add_entry(Loc::fixed(0x100), Origin::Root);
        for v in VECTORS {
            let used = rom.get(usize::from(v)).is_some_and(|b| {
                *b != 0xFF && !relift_cpu_sm83::BASE[usize::from(*b)].is_illegal()
            });
            if used {
                let mut s = State::symbolic();
                s.regs = [Val::Top; 7];
                s.lo = Bv::Unknown;
                s.hi = Bv::Unknown;
                self.roots.insert(Loc::fixed(v), s);
                self.add_entry(Loc::fixed(v), Origin::Root);
            }
        }
    }

    fn process(&mut self, e: Loc) {
        let env = Env {
            model: &self.model,
            image: &self.image,
            summaries: &self.summaries,
            cells: &self.cells,
            known: &self.known,
            limits: self.limits,
            cache: RefCell::new(FxMap::default()),
            probes: &self.probes,
            budget: RefCell::new(STEP_BUDGET),
            inline_body: RefCell::new(FxMap::default()),
        };
        let alone = if self.roots.contains_key(&e) && !self.called.contains(&e) {
            Outcome::default()
        } else {
            func::run_alone(&env, e, State::symbolic())
        };
        let root = self.roots.get(&e).map(|s| func::run_alone(&env, e, *s));
        self.steps += STEP_BUDGET - *env.budget.borrow();
        let inline_body = env.inline_body.take();
        drop(env);
        let mut sum = summarize(&alone);
        if let Some(old) = self.summaries.get(&e) {
            sum = join(old, &sum);
        }
        if self.summaries.get(&e) != Some(&sum) {
            self.summaries.insert(e, sum);
            let deps: Vec<Loc> = self
                .dependents
                .get(&e)
                .map(|d| d.iter().copied().collect())
                .unwrap_or_default();
            for d in deps {
                self.mark(d);
            }
        }
        for o in std::iter::once(&alone).chain(root.iter()) {
            for d in &o.deps {
                self.dependents.entry(*d).or_default().insert(e);
            }
            for c in &o.cell_deps {
                self.cell_dependents.entry(*c).or_default().insert(e);
            }
            self.known.extend(o.body.keys().copied());
        }
        for o in std::iter::once(&alone).chain(root.iter()) {
            self.table_log.extend(o.tables.iter().copied());
        }
        let mut new_calls = Vec::new();
        let mut changed_cells = Vec::new();
        for o in std::iter::once(&alone).chain(root.iter()) {
            for c in &o.calls {
                new_calls.push((c.target, Origin::Call(c.kind)));
            }
            for s in &o.stores {
                match s {
                    Store::Word { addr, elem, .. } => {
                        if self
                            .cells
                            .entry(*addr)
                            .or_default()
                            .elems
                            .insert(elem.clone())
                        {
                            changed_cells.push(*addr);
                        }
                    }
                    Store::Byte { addr, .. } => {
                        let c = self.cells.entry(*addr).or_default();
                        if !c.loose {
                            c.loose = true;
                            changed_cells.push(*addr);
                            changed_cells.push(addr.wrapping_sub(1));
                        }
                    }
                }
            }
        }
        for (t, o) in new_calls {
            if self.roots.contains_key(&t) && self.called.insert(t) {
                self.mark(t);
            }
            self.add_entry(t, o);
        }
        for c in changed_cells {
            let deps: Vec<Loc> = self
                .cell_dependents
                .get(&c)
                .map(|d| d.iter().copied().collect())
                .unwrap_or_default();
            for d in deps {
                self.mark(d);
            }
        }
        self.results.insert(e, (alone, root));
        self.inline_bodies.insert(e, inline_body);
    }

    fn solve(&mut self) {
        let mut rounds = 0;
        while let Some(e) = self.dirty.pop() {
            self.queued.remove(&e);
            rounds += 1;
            if rounds > MAX_PROCESS {
                break;
            }
            self.process(e);
        }
    }

    fn report(self, kind: MapperKind, banks: usize) -> Report {
        let offset = |l: Loc| {
            if l.addr < 0x4000 {
                usize::from(l.addr)
            } else {
                usize::from(l.bank) * ROM_BANK_SIZE + usize::from(l.addr - 0x4000)
            }
        };
        let mut covered = vec![false; banks * ROM_BANK_SIZE];
        let mut functions = BTreeMap::new();
        let mut edges = Vec::new();
        let mut unresolved = BTreeSet::new();
        let mut dynamic_writes = BTreeSet::new();
        let mut copies: Vec<(Loc, CopyLoop)> = Vec::new();
        let mut ram_sites = Vec::new();
        let mut taken = BTreeSet::new();
        let mut tables = BTreeSet::new();
        let mut ram_pointers = BTreeSet::new();
        let mut inlined: FxSet<Loc> = FxSet::default();
        let mut called: FxSet<Loc> = FxSet::default();
        for (alone, root) in self.results.values() {
            for o in std::iter::once(alone).chain(root.iter()) {
                inlined.extend(o.inlined.iter().copied());
                called.extend(o.calls.iter().map(|c| c.target));
            }
        }
        let mut reach: FxSet<Loc> = self.roots.keys().copied().collect();
        let mut stack: Vec<Loc> = reach.iter().copied().collect();
        while let Some(l) = stack.pop() {
            let Some((alone, root)) = self.results.get(&l) else {
                continue;
            };
            for o in std::iter::once(alone).chain(root.iter()) {
                for c in &o.calls {
                    if reach.insert(c.target) {
                        stack.push(c.target);
                    }
                }
            }
        }
        let mut extra: FxMap<Loc, BTreeMap<Loc, u8>> = FxMap::default();
        for (e, m) in &self.inline_bodies {
            if reach.contains(e) {
                for (f, b) in m {
                    extra
                        .entry(*f)
                        .or_default()
                        .extend(b.iter().map(|(l, n)| (*l, *n)));
                }
            }
        }
        for (loc, origin) in &self.entries {
            if !reach.contains(loc) {
                continue;
            }
            let Some((alone, root)) = self.results.get(loc) else {
                continue;
            };
            let mut body = alone.body.clone();
            if let Some(r) = root {
                body.extend(r.body.iter().map(|(l, n)| (*l, *n)));
            }
            if let Some(b) = extra.get(loc) {
                body.extend(b.iter().map(|(l, n)| (*l, *n)));
            }
            let mut own = BTreeSet::new();
            for (l, len) in &body {
                let base = offset(*l);
                for b in base..base + usize::from(*len) {
                    if let Some(c) = covered.get_mut(b) {
                        *c = true;
                    }
                    own.insert(b);
                }
            }
            let sum = self.summaries.get(loc);
            let trampoline = sum.filter(|s| s.ctx).and_then(|s| {
                (s.bank_reg.is_some() || s.target_regs.is_some()).then(|| Trampoline {
                    bank_reg: s.bank_reg.map(reg_name),
                    target_regs: s.target_regs.map(|(h, l)| (reg_name(h), reg_name(l))),
                })
            });
            functions.insert(
                *loc,
                Function {
                    entry: *loc,
                    origin: *origin,
                    body,
                    bytes: own.len(),
                    switches_bank: sum.is_some_and(|s| s.touches),
                    trampoline,
                },
            );
            let rst = loc.addr < 0x40;
            let root_only = root.is_some() && !called.contains(loc) && !rst;
            let mut pick = |o: &Outcome, all: bool| {
                for (u, dep) in &o.unresolved {
                    if all || *dep == Dep::No || !(inlined.contains(loc) || rst) {
                        let u: Unresolved = u.clone();
                        unresolved.insert(u);
                    }
                }
            };
            match root {
                Some(r) if root_only => pick(r, true),
                _ => pick(alone, false),
            }
            for o in std::iter::once(alone).chain(root.iter()) {
                for c in &o.calls {
                    edges.push(Edge {
                        from: c.site,
                        to: c.target,
                        kind: c.kind,
                    });
                }
                dynamic_writes.extend(o.dyn_writes.iter().copied());
                for c in &o.copies {
                    copies.push((*loc, c.clone()));
                }
                taken.extend(o.taken.iter().copied());
                for t in &o.ram {
                    ram_sites.push((*loc, *t));
                }
                tables.extend(o.tables.iter().copied());
                ram_pointers.extend(o.cells.iter().copied());
            }
        }
        let unresolved: Vec<Unresolved> = {
            let mut seen = FxSet::default();
            unresolved
                .into_iter()
                .filter(|u| u.kind != UnresolvedKind::DynamicBankWrite)
                .filter(|u| seen.insert((u.at, u.kind)))
                .collect()
        };
        edges.sort();
        edges.dedup();
        let mut coverage: BTreeMap<u16, BankCoverage> = BTreeMap::new();
        for b in 0..banks {
            let bank = u16::try_from(b).unwrap_or(u16::MAX);
            let n = covered[b * ROM_BANK_SIZE..(b + 1) * ROM_BANK_SIZE]
                .iter()
                .filter(|c| **c)
                .count();
            coverage.insert(
                bank,
                BankCoverage {
                    functions: 0,
                    bytes: n,
                },
            );
        }
        for loc in functions.keys() {
            if let Some(c) = coverage.get_mut(&loc.bank) {
                c.functions += 1;
            }
        }
        ram_sites.sort();
        ram_sites.dedup();
        copies.sort_by_key(|(l, c)| (*l, c.at));
        copies.dedup();
        let ram_code = ram_sites
            .into_iter()
            .map(|(function, transfer)| {
                let copied_by = copies
                    .iter()
                    .filter(|(_, c)| {
                        let len = c
                            .counts
                            .iter()
                            .max()
                            .map_or(0x100, |n| if *n == 0 { 0x100 } else { u16::from(*n) });
                        c.dests.iter().any(|d| {
                            transfer.target >= *d
                                && u32::from(transfer.target) < u32::from(*d) + u32::from(len)
                        })
                    })
                    .cloned()
                    .collect();
                RamCode {
                    transfer,
                    function,
                    copied_by,
                }
            })
            .collect();
        let known: FxSet<Loc> = functions
            .values()
            .flat_map(|f| f.body.keys().copied())
            .collect();
        let unreached_pointers = taken
            .into_iter()
            .filter(|t| t.target.is_none_or(|l| !known.contains(&l)))
            .collect();
        Report {
            unreached_pointers,
            mapper: kind.name(),
            banks,
            functions,
            edges,
            coverage,
            unresolved,
            dynamic_writes: dynamic_writes.into_iter().collect(),
            ram_code,
            tables: tables.into_iter().collect(),
            ram_pointers: ram_pointers.into_iter().collect(),
        }
    }
}

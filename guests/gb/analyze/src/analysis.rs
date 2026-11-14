//! The driver: roots, the call graph fixpoint and the report.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fmt;

use relift_gb_formats::banks::ROM_BANK_SIZE;
use relift_gb_formats::{Cartridge, Error as FormatError, MapperKind};

use crate::bankmodel::BankModel;
use crate::func::{self, CopyLoop, Env, FnRun, Summary};
use crate::image::Image;
use crate::loc::Loc;
use crate::report::{
    BankCoverage, Edge, Function, Origin, RamCode, Report, Trampoline, Unresolved, UnresolvedKind,
};
use crate::state::{State, Val};

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
const MAX_ROUNDS: usize = 64;

struct Entry {
    state: State,
    origin: Origin,
}

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
    let image = Image::new(rom);
    let mut d = Driver {
        model,
        image,
        entries: BTreeMap::new(),
        results: HashMap::new(),
        summaries: HashMap::new(),
        touching: HashSet::new(),
        dirty: Vec::new(),
    };
    d.seed(rom);
    d.solve();
    Ok(d.report(kind, banks))
}

struct Driver<'a> {
    model: BankModel,
    image: Image<'a>,
    entries: BTreeMap<Loc, Entry>,
    results: HashMap<Loc, (FnRun, BTreeSet<Unresolved>)>,
    summaries: HashMap<Loc, Summary>,
    touching: HashSet<Loc>,
    dirty: Vec<Loc>,
}

impl Driver<'_> {
    fn add_entry(&mut self, loc: Loc, state: State, origin: Origin) {
        match self.entries.get_mut(&loc) {
            Some(e) => {
                if e.state.merge(&state) {
                    self.dirty.push(loc);
                }
            }
            None => {
                self.entries.insert(loc, Entry { state, origin });
                self.dirty.push(loc);
            }
        }
    }

    fn seed(&mut self, rom: &[u8]) {
        let mut start = State::reset(&self.model);
        start.regs = [Val::Top; 7];
        self.add_entry(Loc::fixed(0x100), start, Origin::Root);
        for v in VECTORS {
            if rom.get(usize::from(v)).is_some_and(|b| *b != 0xFF) {
                let mut s = State::reset(&self.model);
                s.lo = None;
                s.hi = None;
                self.add_entry(Loc::fixed(v), s, Origin::Root);
            }
        }
    }

    fn process(&mut self, e: Loc) -> bool {
        let state = self.entries[&e].state;
        let env = Env {
            model: &self.model,
            image: &self.image,
            summaries: &self.summaries,
            touching: &self.touching,
        };
        let real = func::run(&env, e, state);
        let mut unresolved: BTreeSet<Unresolved> = real.unresolved.iter().cloned().collect();
        let mut global_change = false;
        if real.parametric || !real.unresolved.is_empty() {
            let sym = func::run(&env, e, State::symbolic());
            let still: BTreeSet<_> = sym.unresolved.iter().map(|u| (u.at, u.kind)).collect();
            unresolved.retain(|u| still.contains(&(u.at, u.kind)));
            let old = self.summaries.get(&e).copied().unwrap_or_default();
            let merged = Summary {
                bank: sym.summary.bank.or(old.bank),
                target: sym.summary.target.or(old.target),
            };
            if merged != old {
                self.summaries.insert(e, merged);
                global_change = true;
            }
        }
        if real.touches && self.touching.insert(e) {
            global_change = true;
        }
        for c in &real.calls {
            self.add_entry(c.target, c.state, Origin::Call(c.kind));
        }
        self.results.insert(e, (real, unresolved));
        global_change
    }

    fn solve(&mut self) {
        let mut rounds = 0;
        while let Some(e) = self.dirty.pop() {
            rounds += 1;
            if rounds > MAX_ROUNDS * 4096 {
                break;
            }
            if self.process(e) {
                self.dirty = self.entries.keys().copied().collect();
            }
        }
    }

    fn report(self, kind: MapperKind, banks: usize) -> Report {
        let mut covered = vec![false; banks * ROM_BANK_SIZE];
        let mut mark = |l: Loc, len: u8| {
            let base = if l.addr < 0x4000 {
                usize::from(l.addr)
            } else {
                usize::from(l.bank) * ROM_BANK_SIZE + usize::from(l.addr - 0x4000)
            };
            for b in base..base + usize::from(len) {
                if let Some(c) = covered.get_mut(b) {
                    *c = true;
                }
            }
        };
        let mut functions = BTreeMap::new();
        let mut edges = Vec::new();
        let mut unresolved = BTreeSet::new();
        let mut dynamic_writes = BTreeSet::new();
        let mut copies: Vec<(Loc, CopyLoop)> = Vec::new();
        let mut ram_sites = Vec::new();
        for (loc, entry) in &self.entries {
            let Some((run, unres)) = self.results.get(loc) else {
                continue;
            };
            let mut own = BTreeSet::new();
            for (l, len) in &run.body {
                mark(*l, *len);
                let base = if l.addr < 0x4000 {
                    usize::from(l.addr)
                } else {
                    usize::from(l.bank) * ROM_BANK_SIZE + usize::from(l.addr - 0x4000)
                };
                own.extend(base..base + usize::from(*len));
            }
            let trampoline =
                self.summaries
                    .get(loc)
                    .filter(|s| !s.is_empty())
                    .map(|s| Trampoline {
                        bank_reg: s.bank.map(reg_name),
                        target_regs: s.target.map(|(h, l)| (reg_name(h), reg_name(l))),
                    });
            functions.insert(
                *loc,
                Function {
                    entry: *loc,
                    origin: entry.origin,
                    body: run.body.clone(),
                    bytes: own.len(),
                    switches_bank: self.touching.contains(loc),
                    trampoline,
                },
            );
            for c in &run.calls {
                edges.push(Edge {
                    from: c.site,
                    to: c.target,
                    kind: c.kind,
                });
            }
            for u in unres {
                if u.kind == UnresolvedKind::DynamicBankWrite {
                    dynamic_writes.insert(u.at);
                } else {
                    unresolved.insert(u.clone());
                }
            }
            for c in &run.copies {
                copies.push((*loc, c.clone()));
            }
            for t in &run.ram {
                ram_sites.push((*loc, *t));
            }
        }
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
        Report {
            mapper: kind.name(),
            banks,
            functions,
            edges,
            coverage,
            unresolved: unresolved.into_iter().collect(),
            dynamic_writes: dynamic_writes.into_iter().collect(),
            ram_code,
        }
    }
}

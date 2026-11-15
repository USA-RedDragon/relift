//! Which lifted functions produce exact FPSR flags.
//!
//! While the build lifts each module it keeps a small summary of every
//! function for the flags-live region analysis (`relift_cpu_arm64::flags::
//! region`): its blocks with FP use and FPSR writes, its calls, its live
//! FPSR reads, and the computed transfers that go through a loader-filled
//! slot. The module's analysis is then dropped and its shards emitted in
//! two parts ([`relift_emit::Options::parts`]). Once every module is in,
//! the decision says which functions must raise exact flags; each module's
//! `relift_parts.h` moves those into part 1 of their shards, which the
//! build compiles with `-DRL_FP_FLAGS -ffp-exception-behavior=strict`.
//! When a region cannot be bounded, every function produces flags, as
//! before.
//!
//! What calls a function from code the analysis cannot follow (its
//! address escapes) decides how far a region reaches:
//!
//! - a relative relocation's value, `DT_INIT` and `DT_FINI`;
//! - a code address the code computes;
//! - a hot-load hint (a root a dev run reached from code the analysis
//!   cannot follow);
//! - a module's entry, except the first module's, where the runtime starts
//!   the main thread with clear flags and never enters again (it does not
//!   deliver exceptions through it);
//! - every definition of a symbol named by a GOT or data word whose value
//!   or address escapes. A PLT word is read only by its PLT stub (the
//!   AArch64 ELF convention), so a call through its stub goes to the
//!   definitions of its symbol and nothing escapes.
//!
//! A symbol that no module names is reachable only by a run-time lookup by
//! name, which the program's modules do not do through the loader; a
//! module loaded at run time is outside the program the build sees.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use relift_analyze::{Function, Insn, Layout, Outline};
use relift_cpu_arm64::flags::{self, region};
use relift_guest::{Frontend, Module, RootKind};
use relift_switch::program::{self, Placed, SlotKind};

use crate::import::BoxError;

/// The size of a loader-filled slot.
const SLOT: u64 = 8;

/// A shard file and the functions it defines, by entry and host name.
type Shard = (String, Vec<(u64, String)>);

/// One function, for the report.
struct Named {
    module: usize,
    name: String,
}

/// The program-wide FPSR flags analysis of one build.
pub struct Fpsr {
    /// Whether any module holds an `MRS FPSR`; without one nothing runs.
    on: bool,
    /// Every definition of each exported name.
    defs: HashMap<String, Vec<u64>>,
    /// The symbol slots by address.
    slots: BTreeMap<u64, (String, SlotKind)>,
    /// Escaped entries and addresses.
    escaped: BTreeSet<u64>,
    /// Names whose definitions escape.
    escaped_names: BTreeSet<String>,
    /// The program start.
    starts: BTreeSet<u64>,
    fns: Vec<region::Func>,
    names: BTreeMap<u64, Named>,
    modules: Vec<String>,
    /// Per module: its directory and shard files with their functions.
    shards: Vec<(PathBuf, Vec<Shard>)>,
}

/// What [`Fpsr::begin_module`] found in one module for its functions.
pub struct ModuleFlags {
    index: usize,
    base: u64,
    live: BTreeSet<u64>,
    via: BTreeMap<u64, u64>,
    resume: BTreeSet<u64>,
    start: Option<u64>,
    entries: BTreeSet<u64>,
}

/// What [`Fpsr::visit`] keeps of one function: the addresses it takes and
/// its uses of the module's slots.
pub type Visit = (BTreeSet<u64>, Option<relift_analyze::SlotUses>);

/// One function's summary, from [`Fpsr::function`].
pub struct FnFlags {
    func: region::Func,
    entry: u64,
    name: String,
}

fn view<'a>(f: &'a Function, raw: &'a dyn Fn(u64) -> u32) -> flags::Function<'a> {
    flags::Function {
        entry: f.entry,
        insts: f
            .insts
            .iter()
            .map(|(&pc, i)| {
                let b = match i {
                    Insn::Lifted(b) => Some(&**b),
                    Insn::Failed(_) => None,
                };
                (pc, b)
            })
            .collect(),
        raw,
    }
}

/// What the build does with the decision.
#[derive(Clone, Debug, Default)]
pub struct Plan {
    /// Some function holds a live read: the runtime produces flags.
    pub flags: bool,
    /// Every lifted function produces flags (a region is unbounded).
    pub program_wide: bool,
    /// The shard sources that need a part-1 compile.
    pub part1: Vec<PathBuf>,
    /// The census, for the build log.
    pub report: String,
}

fn words_have_mrs_fpsr(p: &Placed) -> bool {
    p.nso
        .image
        .as_chunks::<4>()
        .0
        .iter()
        .any(|w| flags::mrs_fpsr(u32::from_le_bytes(*w)).is_some())
}

impl Fpsr {
    /// Starts the analysis of the program `placed`.
    pub fn new(placed: &[Placed]) -> Fpsr {
        let on = placed.iter().any(words_have_mrs_fpsr);
        let mut f = Fpsr {
            on,
            defs: HashMap::new(),
            slots: BTreeMap::new(),
            escaped: BTreeSet::new(),
            escaped_names: BTreeSet::new(),
            starts: BTreeSet::new(),
            fns: Vec::new(),
            names: BTreeMap::new(),
            modules: placed.iter().map(|p| p.name.clone()).collect(),
            shards: Vec::new(),
        };
        if !on {
            return f;
        }
        for p in placed {
            let l = program::linkage(p);
            for (n, a) in l.exports {
                f.defs.entry(n).or_default().push(a);
            }
            for s in l.slots {
                if s.kind == SlotKind::Data {
                    f.escaped_names.insert(s.name.clone());
                }
                f.slots.insert(s.addr, (s.name, s.kind));
            }
            f.escaped.extend(l.values);
        }
        f
    }

    /// Whether modules are emitted in parts.
    pub fn parts(&self) -> bool {
        self.on
    }

    /// The loader-filled slots of module `p`, for [`Fpsr::visit`].
    pub fn slots_of(&self, p: &Placed) -> BTreeSet<u64> {
        let end = p.base + p.nso.image.len() as u64 + u64::from(p.nso.bss_size);
        self.slots.range(p.base..end).map(|(&a, _)| a).collect()
    }

    /// What the analysis needs of function `f` of a module (`module`, whose
    /// slots are `mine` from [`Fpsr::slots_of`]) before every function of
    /// the module is known: give it to [`Fpsr::begin_module`] with the
    /// others, in entry order.
    pub fn visit(&self, module: &Module, mine: &BTreeSet<u64>, f: &Function) -> Visit {
        if !self.on {
            return (BTreeSet::new(), None);
        }
        let taken = relift_analyze::address_taken(module, 4, f);
        if mine.is_empty() {
            return (taken, None);
        }
        (taken, Some(relift_analyze::slot_uses(f, mine, SLOT)))
    }

    /// Starts module `i` (`module`, outlined as `layout` with `visits` of its
    /// functions from [`Fpsr::visit`]), whose host overrides are
    /// `overrides`; `fe` lifts its functions again. Every function then goes
    /// through [`Fpsr::function`] and the results, in entry order, to
    /// [`Fpsr::end_module`].
    pub fn begin_module(
        &mut self,
        i: usize,
        module: &Module,
        layout: &Layout,
        visits: Vec<Visit>,
        fe: &(dyn Frontend + Sync),
        overrides: &[relift_emit::Override],
    ) -> ModuleFlags {
        let uses = visits;
        let mut m = ModuleFlags {
            index: i,
            base: module.base,
            live: BTreeSet::new(),
            via: BTreeMap::new(),
            resume: BTreeSet::new(),
            start: None,
            entries: layout.functions.keys().copied().collect(),
        };
        if !self.on {
            return m;
        }
        let raw = |pc: u64| {
            module
                .bytes_at(pc)
                .and_then(|b| b.get(..4))
                .map_or(0, |w| u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
        };
        let sites: BTreeSet<u64> = layout
            .functions
            .values()
            .filter(|o| o.pcs().any(|pc| flags::mrs_fpsr(raw(pc)).is_some()))
            .map(|o| o.entry)
            .collect();
        if !sites.is_empty() {
            let mut callers: BTreeMap<u64, BTreeSet<u64>> = BTreeMap::new();
            for o in layout.functions.values() {
                for &(t, _) in &o.calls {
                    callers.entry(t).or_default().insert(o.entry);
                }
            }
            let mut need = sites.clone();
            let mut todo: Vec<u64> = sites.into_iter().collect();
            while let Some(e) = todo.pop() {
                for &c in callers.get(&e).into_iter().flatten() {
                    if need.insert(c) {
                        todo.push(c);
                    }
                }
            }
            let need: Vec<&Outline> = need.iter().map(|e| &layout.functions[e]).collect();
            let lifted = relift_analyze::each_lifted(fe, module, &need, |_, f| f.clone());
            let views: Vec<flags::Function<'_>> = lifted.iter().map(|f| view(f, &raw)).collect();
            m.live = flags::live_fpsr_reads(&views).into_iter().collect();
        }
        for (taken, u) in uses {
            self.escaped.extend(taken);
            let Some(u) = u else {
                continue;
            };
            m.via.extend(u.transfers);
            for s in u.escaped {
                if let Some((n, kind)) = self.slots.get(&s)
                    && *kind != SlotKind::Plt
                {
                    self.escaped_names.insert(n.clone());
                }
            }
        }
        for r in &module.roots {
            if (i != 0 && r.kind == RootKind::Entry) || r.kind == RootKind::Hint {
                self.escaped.insert(r.addr);
            }
        }
        m.resume = overrides
            .iter()
            .filter(|o| o.resume_point)
            .map(|o| o.entry)
            .collect();
        m.start = module
            .roots
            .iter()
            .find(|r| i == 0 && r.kind == RootKind::Entry)
            .map(|r| r.addr);
        if let Some(e) = m.start {
            self.starts.insert(e);
        }
        m
    }

    /// The summary of function `f` (lifted from `o`) of the module `m`
    /// started, from `module`.
    pub fn function(
        &self,
        m: &ModuleFlags,
        module: &Module,
        o: &Outline,
        f: &Function,
    ) -> Option<FnFlags> {
        if !self.on {
            return None;
        }
        let raw = |pc: u64| {
            module
                .bytes_at(pc)
                .and_then(|b| b.get(..4))
                .map_or(0, |w| u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
        };
        let is_entry = |a: u64| m.entries.contains(&a);
        let resolved = |pc: u64| {
            let s = m.via.get(&pc)?;
            let (n, _) = self.slots.get(s)?;
            Some(self.defs.get(n).cloned().unwrap_or_default())
        };
        let resume_point = |a: u64| m.resume.contains(&a);
        let regs: Vec<(relift_ir::Reg, u128)> = program::START_REGS
            .iter()
            .map(|&(n, v)| (relift_cpu_arm64::regs::x(n), u128::from(v)))
            .collect();
        let cx = flags::Context {
            is_entry: &is_entry,
            resolved: &resolved,
            resume_point: &resume_point,
            start: m.start.map(|e| (e, regs.as_slice())),
        };
        let reads: BTreeSet<u64> = m
            .live
            .range(f.entry..f.entry + o.span())
            .copied()
            .filter(|r| f.insts.contains_key(r))
            .collect();
        Some(FnFlags {
            func: flags::summarize(&view(f, &raw), &f.tables, &reads, &cx),
            entry: f.entry,
            name: f
                .name
                .clone()
                .unwrap_or_else(|| format!("fn_{:x}", f.entry - m.base)),
        })
    }

    /// Takes the summaries of every function of the module `m` started.
    pub fn end_module(&mut self, m: ModuleFlags, fns: Vec<FnFlags>) {
        for f in fns {
            self.names.insert(
                f.entry,
                Named {
                    module: m.index,
                    name: f.name,
                },
            );
            self.fns.push(f.func);
        }
    }

    /// Takes module `i` (`module`, outlined as `layout` with `visits`) whole,
    /// as [`Fpsr::begin_module`], [`Fpsr::function`] and
    /// [`Fpsr::end_module`] do it.
    pub fn add_module(
        &mut self,
        i: usize,
        module: &Module,
        layout: &Layout,
        visits: Vec<Visit>,
        fe: &(dyn Frontend + Sync),
        overrides: &[relift_emit::Override],
    ) {
        let m = self.begin_module(i, module, layout, visits, fe, overrides);
        let me = &*self;
        let all: Vec<&Outline> = layout.functions.values().collect();
        let fns =
            relift_analyze::each_lifted(fe, module, &all, |o, f| me.function(&m, module, o, f));
        self.end_module(m, fns.into_iter().flatten().collect());
    }

    /// Records the files emitted for a module into `dir`.
    pub fn add_files(&mut self, dir: &Path, files: &[relift_emit::File]) {
        if !self.on {
            return;
        }
        let shards = files
            .iter()
            .filter(|f| !f.fns.is_empty())
            .map(|f| (f.name.clone(), f.fns.clone()))
            .collect();
        self.shards.push((dir.to_path_buf(), shards));
    }

    /// Decides, writes each module's parts header and gives the plan.
    ///
    /// # Errors
    ///
    /// A header that cannot be written.
    pub fn finish(mut self) -> Result<Plan, BoxError> {
        if !self.on {
            return Ok(Plan {
                flags: false,
                program_wide: false,
                part1: Vec::new(),
                report: String::new(),
            });
        }
        for n in std::mem::take(&mut self.escaped_names) {
            if let Some(d) = self.defs.get(&n) {
                self.escaped.extend(d);
            }
        }
        let p = region::Program {
            fns: std::mem::take(&mut self.fns),
            escaped: std::mem::take(&mut self.escaped),
            starts: std::mem::take(&mut self.starts),
        };
        let d = region::decide(&p);
        let wide = d.program_wide();
        let mut part1 = Vec::new();
        for (dir, shards) in &self.shards {
            let mut list = Vec::new();
            for (file, fns) in shards {
                let names: Vec<&str> = if wide {
                    Vec::new()
                } else {
                    fns.iter()
                        .filter(|(e, _)| d.flagged.contains_key(e))
                        .map(|(_, n)| n.as_str())
                        .collect()
                };
                if !names.is_empty() {
                    part1.push(dir.join(file));
                }
                list.push((file.as_str(), names));
            }
            relift_cache::write_if_changed(
                &dir.join(relift_emit::PARTS_NAME),
                relift_emit::parts_header(&list).as_bytes(),
            )?;
        }
        Ok(Plan {
            flags: !d.flagged.is_empty(),
            program_wide: wide,
            part1,
            report: self.report(&d, p.fns.len()),
        })
    }

    fn name(&self, e: u64) -> String {
        self.names.get(&e).map_or_else(
            || format!("{e:#x}"),
            |n| format!("{}:{}", self.modules[n.module], n.name),
        )
    }

    /// The census: per module the flagged functions and why, then each
    /// read's region.
    fn report(&self, d: &region::Decision, total: usize) -> String {
        let mut t = String::new();
        let mut per: BTreeMap<usize, (usize, usize)> = BTreeMap::new();
        for (e, r) in &d.flagged {
            let m = self.names.get(e).map_or(usize::MAX, |n| n.module);
            let c = per.entry(m).or_default();
            match r {
                region::Reason::Read => c.0 += 1,
                region::Reason::Region => c.1 += 1,
            }
        }
        let _ = writeln!(
            t,
            "FPSR flags: {} of {total} functions, {} live reads, {}",
            d.flagged.len(),
            d.regions.len(),
            if d.program_wide() {
                "a region is unbounded: every function produces flags"
            } else {
                "every region is bounded"
            }
        );
        for (m, (reads, region)) in per {
            let name = self.modules.get(m).map_or("?", String::as_str);
            let _ = writeln!(
                t,
                "  {name}: {} with a read, {} with FP in a region",
                reads, region
            );
        }
        for r in &d.regions {
            if !r.entered {
                let _ = writeln!(
                    t,
                    "  read at {:#x} in {}: never entered",
                    r.read,
                    self.name(r.func)
                );
                continue;
            }
            let fp: Vec<String> = r.fp.iter().map(|&e| self.name(e)).collect();
            let _ = writeln!(
                t,
                "  read at {:#x} in {}: {} functions in the region, FP in {}{}",
                r.read,
                self.name(r.func),
                r.touched,
                if fp.is_empty() {
                    "none".to_string()
                } else {
                    fp.join(", ")
                },
                r.unbounded.as_ref().map_or_else(String::new, |u| {
                    let via: Vec<String> = r.chain.iter().map(|&e| self.name(e)).collect();
                    format!("; unbounded: {u}, reached through {}", via.join(" <- "))
                })
            );
        }
        t
    }
}

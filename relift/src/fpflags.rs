//! Which lifted functions produce exact FPSR flags.
//!
//! While the build lifts each module it keeps a small summary of every
//! function for the flags-live region analysis (`relift_cpu_a64::flags::
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

use relift_analyze::{Analysis, Insn};
use relift_cpu_a64::flags::{self, region};
use relift_guest::{Module, RootKind};
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

    /// Takes module `i` (`p`, analyzed as `analysis`), whose host overrides
    /// are `overrides`, before its analysis is dropped.
    pub fn add_module(
        &mut self,
        i: usize,
        p: &Placed,
        module: &Module,
        analysis: &Analysis,
        overrides: &[relift_emit::Override],
    ) {
        if !self.on {
            return;
        }
        let raw = |pc: u64| {
            module
                .bytes_at(pc)
                .and_then(|b| b.get(..4))
                .map_or(0, |w| u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
        };
        let views: Vec<flags::Function<'_>> = analysis
            .functions
            .values()
            .map(|f| flags::Function {
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
                raw: &raw,
            })
            .collect();
        let live: BTreeSet<u64> = flags::live_fpsr_reads(&views).into_iter().collect();
        let end = p.base + p.nso.image.len() as u64 + u64::from(p.nso.bss_size);
        let mine: BTreeSet<u64> = self.slots.range(p.base..end).map(|(&a, _)| a).collect();
        let mut via: BTreeMap<u64, u64> = BTreeMap::new();
        for f in analysis.functions.values() {
            self.escaped
                .extend(relift_analyze::address_taken(module, 4, f));
            if mine.is_empty() {
                continue;
            }
            let u = relift_analyze::slot_uses(f, &mine, SLOT);
            via.extend(u.transfers);
            for s in u.escaped {
                if let Some((n, kind)) = self.slots.get(&s)
                    && *kind != SlotKind::Plt
                {
                    self.escaped_names.insert(n.clone());
                }
            }
        }
        if i != 0 {
            for r in &module.roots {
                if r.kind == RootKind::Entry {
                    self.escaped.insert(r.addr);
                }
            }
        }
        let resume: BTreeSet<u64> = overrides
            .iter()
            .filter(|o| o.resume_point)
            .map(|o| o.entry)
            .collect();
        let is_entry = |a: u64| analysis.functions.contains_key(&a);
        let resolved = |pc: u64| {
            let s = via.get(&pc)?;
            let (n, _) = self.slots.get(s)?;
            Some(self.defs.get(n).cloned().unwrap_or_default())
        };
        let resume_point = |a: u64| resume.contains(&a);
        let regs: Vec<(relift_ir::Reg, u128)> = program::START_REGS
            .iter()
            .map(|&(n, v)| (relift_cpu_a64::regs::x(n), u128::from(v)))
            .collect();
        let start = module
            .roots
            .iter()
            .find(|r| i == 0 && r.kind == RootKind::Entry)
            .map(|r| (r.addr, regs.as_slice()));
        if let Some((e, _)) = start {
            self.starts.insert(e);
        }
        let cx = flags::Context {
            is_entry: &is_entry,
            resolved: &resolved,
            resume_point: &resume_point,
            start,
        };
        let mut fns = Vec::with_capacity(views.len());
        for (v, f) in views.iter().zip(analysis.functions.values()) {
            let reads: BTreeSet<u64> = live.range(f.entry..f.entry + f.span(4)).copied().collect();
            let reads = reads
                .into_iter()
                .filter(|r| f.insts.contains_key(r))
                .collect();
            fns.push(flags::summarize(v, &f.tables, &reads, &cx));
            self.names.insert(
                f.entry,
                Named {
                    module: i,
                    name: f
                        .name
                        .clone()
                        .unwrap_or_else(|| format!("fn_{:x}", f.entry - p.base)),
                },
            );
        }
        self.fns.extend(fns);
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
                r.unbounded
                    .as_ref()
                    .map_or_else(String::new, |u| format!("; unbounded: {u}"))
            );
        }
        t
    }
}

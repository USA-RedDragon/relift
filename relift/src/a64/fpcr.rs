//! Whether the build may drop the FPCR guards of lifted code.
//!
//! The FPCR proof (`relift_cpu_arm64::fpcr`) runs once every module is
//! lifted, over the whole program: every `MSR FPCR` must be shown to write
//! the default mode. The build then compiles lifted code with
//! `-DRL_FPCR_CONST`, which folds every FPCR guard and its slow path away.
//! `emit.fpcr-guards` keeps them, for debugging and for the reference
//! exact-flags mode (`RELIFT_FP_EXACT=1`), which runs through them.
//!
//! What may call a function, for the proof's climb from a write to its
//! callers: a direct branch (`BL`, `B`, a conditional branch, `CBZ`,
//! `TBZ` and their negations) anywhere in the program, a function whose
//! extent holds the entry or the instruction before it (which may fall
//! into it), and a computed transfer through a PLT or GOT
//! word naming one of the entry's symbols, and a jump-table target. A
//! function whose address escapes ([`crate::a64::fpflags::Linkage::escaped`])
//! or that starts the program may be entered from anywhere, and the climb
//! stops there; one with no callers is never entered (a symbol that no
//! module names is reachable only by a run-time lookup by name, which the
//! program's modules do not do through the loader).

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fmt::Write as _;
use std::sync::Arc;

use relift_analyze::{Layout, Outline};
use relift_cpu_arm64::cpu::Cpu;
use relift_cpu_arm64::fpcr::{self, Code, Host};
use relift_ir::Block;
use relift_switch::program::Placed;

use crate::a64::fpflags::{Linkage, ModuleFlags};

/// What the proof keeps of one module.
struct Mod {
    base: u64,
    image: Vec<u8>,
    text_end: u64,
    outlines: Vec<Outline>,
    hosts: BTreeMap<u64, Host>,
    via: BTreeMap<u64, u64>,
    pads: Vec<relift_guest::Pad>,
}

/// The FPCR proof of one build.
#[derive(Default)]
pub struct Fpcr {
    mods: Vec<Mod>,
    sites: usize,
}

/// What the build does with the proof.
#[derive(Clone, Debug, Default)]
pub struct Plan {
    /// Whether lifted code is compiled without FPCR guards.
    pub constant: bool,
    /// The census, for the build log.
    pub report: String,
}

/// The longest call-site range of an unwind table looked through: one
/// function's code.
const CALL_SITE_SPAN: u64 = 1 << 24;

/// The functions that find a function by its name at run time.
const LOOKUPS: [&str; 4] = [
    "_ZN2nn2ro12LookupSymbolEPmPKc",
    "_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc",
    "dlsym",
    "dlvsym",
];

/// Whether the program may look a function up by name at run time: some
/// definition of a [`LOOKUPS`] function escapes or `called` says it has a
/// caller. Every exported function may then be entered from anywhere.
/// The loader's own binding (lazy PLT binding included) fills only the
/// slots that name a symbol, which the climb follows.
fn lookup_reachable(linkage: &Linkage, called: &dyn Fn(u64) -> bool) -> bool {
    LOOKUPS
        .iter()
        .filter_map(|n| linkage.defs.get(*n))
        .flatten()
        .any(|&d| linkage.escaped.contains(&d) || called(d))
}

fn word(image: &[u8], off: u64) -> Option<u32> {
    let o = usize::try_from(off).ok()?;
    let b = image.get(o..o.checked_add(4)?)?;
    Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// The target of the direct branch `w` at `pc`.
fn branch_target(w: u32, pc: u64) -> Option<u64> {
    let sx = |v: u32, bits: u32| -> i64 { i64::from((v << (32 - bits)) as i32 >> (32 - bits)) };
    let off = if w & 0x7c00_0000 == 0x1400_0000 {
        sx(w & 0x03ff_ffff, 26)
    } else if w & 0xff00_0010 == 0x5400_0000 || w & 0x7e00_0000 == 0x3400_0000 {
        sx((w >> 5) & 0x7_ffff, 19)
    } else if w & 0x7e00_0000 == 0x3600_0000 {
        sx((w >> 5) & 0x3fff, 14)
    } else {
        return None;
    };
    Some(pc.wrapping_add_signed(off * 4))
}

impl Fpcr {
    /// Keeps what the proof needs of module `p` (as `module`, outlined as
    /// `layout`, with `flags` from the FPSR analysis's linkage pass), whose
    /// host overrides are `overrides` and whose functions in `swapped` run mod
    /// variants instead.
    pub fn add_module(
        &mut self,
        p: &Placed,
        module: &relift_guest::Module,
        layout: &Layout,
        flags: &ModuleFlags,
        overrides: &[relift_emit::Override],
        swapped: &BTreeSet<u64>,
    ) {
        let image = &p.nso.image;
        let text = &p.nso.segments[0];
        let text_start = u64::from(text.memory_offset);
        let text_end = text_start + u64::from(text.size);
        let has = (text_start..text_end)
            .step_by(4)
            .any(|o| word(image, o).is_some_and(fpcr::msr_fpcr));
        let mut hosts = BTreeMap::new();
        for o in overrides {
            let h = if o.resume_point || swapped.contains(&o.entry) {
                Host::Unknown
            } else {
                Host::Override
            };
            hosts.insert(o.entry, h);
        }
        self.mods.push(Mod {
            base: p.base,
            image: image.clone(),
            text_end: p.base + text_end,
            outlines: layout.functions.values().cloned().collect(),
            hosts,
            via: flags.via().clone(),
            pads: module.pads.clone(),
        });
        if has {
            self.sites += 1;
        }
    }

    fn module_of(&self, pc: u64) -> Option<&Mod> {
        self.mods
            .iter()
            .rev()
            .find(|m| pc >= m.base && pc - m.base < m.image.len() as u64)
    }

    /// Runs the proof over every module added, with the program's
    /// `linkage`; `guards` (`emit.fpcr-guards`) keeps the guards anyway.
    pub fn finish(&self, cpu: &Cpu, linkage: &Linkage, guards: bool) -> Plan {
        let mut sites: BTreeMap<u64, BTreeSet<u64>> = BTreeMap::new();
        for m in &self.mods {
            for o in (m.base..m.text_end).step_by(4) {
                if word(&m.image, o - m.base).is_some_and(fpcr::msr_fpcr) {
                    let fns = m
                        .outlines
                        .iter()
                        .filter(|f| f.entry <= o && o < f.reach && f.contains(o))
                        .map(|f| f.entry)
                        .collect();
                    sites.insert(o, fns);
                }
            }
        }
        let mut t = String::new();
        if sites.is_empty() {
            return Plan {
                constant: !guards,
                report: t,
            };
        }
        let entries: BTreeSet<u64> = self
            .mods
            .iter()
            .flat_map(|m| m.outlines.iter().map(|o| o.entry))
            .collect();
        let mut pads: Vec<relift_guest::Pad> = self
            .mods
            .iter()
            .flat_map(|m| m.pads.iter().copied())
            .collect();
        pads.sort_unstable();
        let tables: HashMap<u64, Vec<u64>> = self
            .mods
            .iter()
            .flat_map(|m| m.outlines.iter())
            .flat_map(|o| o.tables.iter().map(|(&pc, ts)| (pc, ts.clone())))
            .collect();
        let lifted: RefCell<HashMap<u64, Option<Arc<Block>>>> = RefCell::new(HashMap::new());
        let block = |pc: u64| -> Option<Arc<Block>> {
            if let Some(b) = lifted.borrow().get(&pc) {
                return b.clone();
            }
            let b = self.lift(pc, cpu);
            lifted.borrow_mut().insert(pc, b.clone());
            b
        };
        let slot = |a: u64| -> Option<Vec<u64>> {
            let n = linkage.words.get(&a)?;
            linkage.defs.get(n).cloned()
        };
        let table = |pc: u64| tables.get(&pc).cloned();
        let is_entry = |a: u64| entries.contains(&a);
        let pads_of = |pc: u64| -> Vec<u64> {
            let end = pads.partition_point(|p| p.site <= pc);
            pads[..end]
                .iter()
                .rev()
                .take_while(|p| pc - p.site < CALL_SITE_SPAN)
                .filter(|p| pc - p.site < p.len)
                .map(|p| p.addr)
                .collect()
        };
        let host = |e: u64| {
            self.module_of(e)
                .and_then(|m| m.hosts.get(&e).copied())
                .unwrap_or(Host::Guest)
        };
        let code = Code {
            block: &block,
            slot: &slot,
            table: &table,
            is_entry: &is_entry,
            pads: &pads_of,
            host: &host,
        };
        let branches = self.branches(&entries);
        let direct = |f: u64| -> Vec<u64> {
            let mut out: BTreeSet<u64> = BTreeSet::new();
            let mut at: Vec<u64> = branches.get(&f).cloned().unwrap_or_default();
            for m in &self.mods {
                for (&pc, s) in &m.via {
                    let Some(n) = linkage.words.get(s) else {
                        continue;
                    };
                    if linkage.defs.get(n).is_some_and(|d| d.contains(&f)) {
                        at.push(pc);
                    }
                }
            }
            at.push(f);
            at.push(f.wrapping_sub(4));
            for pc in at {
                if let Some(m) = self.module_of(pc) {
                    out.extend(
                        m.outlines
                            .iter()
                            .filter(|o| o.entry != f && o.entry <= pc && pc < o.reach)
                            .filter(|o| o.contains(pc))
                            .map(|o| o.entry),
                    );
                }
            }
            out.into_iter().collect()
        };
        let lookup = lookup_reachable(linkage, &|f| !direct(f).is_empty());
        let exported: BTreeSet<u64> = if lookup {
            linkage.defs.values().flatten().copied().collect()
        } else {
            BTreeSet::new()
        };
        let callers = |f: u64| -> Option<Vec<u64>> {
            if linkage.escaped.contains(&f) || linkage.starts.contains(&f) || exported.contains(&f)
            {
                return None;
            }
            Some(direct(f))
        };
        let proof = fpcr::prove(&code, &callers, &sites);
        let _ = writeln!(
            t,
            "FPCR: {} MSR FPCR in {} module{}, {} root function{} proven, {} climbed through, {} never called: {}",
            sites.len(),
            self.sites,
            if self.sites == 1 { "" } else { "s" },
            proof.roots.len(),
            if proof.roots.len() == 1 { "" } else { "s" },
            proof.climbed.len(),
            proof.unreachable.len(),
            match (&proof.reason, guards) {
                (Some(r), _) => format!("the guards stay ({r})"),
                (None, true) => "the FP mode is constant; emit.fpcr-guards keeps the guards".into(),
                (None, false) => "the FP mode is constant, lifted code has no FPCR guards".into(),
            }
        );
        Plan {
            constant: proof.constant && !guards,
            report: t,
        }
    }

    fn lift(&self, pc: u64, cpu: &Cpu) -> Option<Arc<Block>> {
        let m = self.module_of(pc)?;
        if pc >= m.text_end {
            return None;
        }
        let w = word(&m.image, pc - m.base)?;
        relift_cpu_arm64::lift::lift_for_emit(w, pc, cpu)
            .ok()
            .map(Arc::new)
    }

    /// The direct branches to each function entry, by the branch's address.
    fn branches(&self, entries: &BTreeSet<u64>) -> HashMap<u64, Vec<u64>> {
        let mut out: HashMap<u64, Vec<u64>> = HashMap::new();
        for m in &self.mods {
            for o in m.outlines.iter() {
                for (&pc, ts) in &o.tables {
                    for t in ts.iter().filter(|t| entries.contains(t)) {
                        out.entry(*t).or_default().push(pc);
                    }
                }
                for pc in o.pcs() {
                    let Some(w) = word(&m.image, pc - m.base) else {
                        continue;
                    };
                    if let Some(t) = branch_target(w, pc)
                        && entries.contains(&t)
                    {
                        out.entry(t).or_default().push(pc);
                    }
                }
            }
        }
        for v in out.values_mut() {
            v.sort_unstable();
            v.dedup();
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::{branch_target, lookup_reachable};
    use crate::a64::fpflags::Linkage;

    fn linkage(lookup_escapes: bool) -> Linkage {
        let mut l = Linkage::default();
        l.defs.insert("feupdateenv".into(), vec![0x2000]);
        l.defs
            .insert("_ZN2nn2ro12LookupSymbolEPmPKc".into(), vec![0x3000]);
        if lookup_escapes {
            l.escaped.insert(0x3000);
        }
        l
    }

    #[test]
    fn exports_are_reachable_when_a_lookup_by_name_is() {
        assert!(!lookup_reachable(&linkage(false), &|_| false));
        assert!(lookup_reachable(&linkage(true), &|_| false));
        assert!(lookup_reachable(&linkage(false), &|f| f == 0x3000));
        assert!(!lookup_reachable(&linkage(false), &|f| f == 0x2000));
    }

    #[test]
    fn direct_branches_give_their_targets() {
        assert_eq!(branch_target(0x9400_0004, 0x1000), Some(0x1010));
        assert_eq!(branch_target(0x97ff_ffff, 0x1000), Some(0xffc));
        assert_eq!(branch_target(0x1400_0002, 0x1000), Some(0x1008));
        assert_eq!(branch_target(0x5400_0040, 0x1000), Some(0x1008));
        assert_eq!(branch_target(0xb400_0060, 0x1000), Some(0x100c));
        assert_eq!(branch_target(0x3608_0040, 0x1000), Some(0x1008));
        assert_eq!(branch_target(0xd63f_0100, 0x1000), None);
        assert_eq!(branch_target(0xd65f_03c0, 0x1000), None);
    }
}

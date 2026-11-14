//! The build side of bundled mods: which functions of a module they patch,
//! each such function lifted once per combination of its mods, and the C
//! the runtime switches between at start (`relift/mods.h`).

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use relift_analyze::{Analysis, EntryKind, Function, Insn};
use relift_guest::{Frontend, Module};
use relift_ir::{Flow, Next};

use super::bundled::{self, Bundled, ModImage, Swap, SwapC};
use crate::import::BoxError;

/// The bundled mods' part of one module of the build.
#[derive(Clone, Debug, Default)]
pub struct ModulePlan {
    /// Each mod alone applied to the module, when it patches it.
    pub per_mod: Vec<Option<ModImage>>,
    /// The swapped functions.
    pub swaps: Vec<Swap>,
}

/// The entries of `a` each mod's writes touch, by entry.
pub fn touched_by_mods(
    a: &Analysis,
    granule: u64,
    base: u64,
    per_mod: &[Option<ModImage>],
) -> BTreeMap<u64, Vec<usize>> {
    let mut out: BTreeMap<u64, Vec<usize>> = BTreeMap::new();
    for (k, m) in per_mod.iter().enumerate() {
        let Some(m) = m else { continue };
        let guest: Vec<_> = m
            .ranges
            .iter()
            .map(|r| base + r.start..base + r.end)
            .collect();
        for e in super::conflicts::touched(a, granule, &guest) {
            out.entry(e).or_default().push(k);
        }
    }
    out
}

/// The name of the switch a swapped function's callers and dispatch entry
/// use.
pub fn switch_name(module: usize, entry: u64) -> String {
    format!("rl_modsw_{module}_{entry:x}")
}

/// The constant targets of `f`'s calls and jumps that leave it.
fn targets(f: &Function) -> BTreeSet<u64> {
    let mut out = BTreeSet::new();
    for i in f.insts.values() {
        let Insn::Lifted(b) = i else { continue };
        match b.exit.next {
            Next::Const(t) => {
                out.insert(t);
            }
            Next::Select {
                taken, not_taken, ..
            } => {
                out.insert(taken);
                out.insert(not_taken);
            }
            Next::Dynamic(_) => {}
        }
        if let Flow::Call { ret } = b.exit.flow {
            out.insert(ret);
        }
    }
    out.retain(|t| !f.insts.contains_key(t));
    out
}

/// What one module's variants need from the build.
pub struct Variants<'a> {
    /// The CPU.
    pub fe: &'a (dyn Frontend + Sync),
    /// The module index.
    pub index: usize,
    /// The module with the user mods applied.
    pub module: &'a Module,
    /// Its analysis.
    pub analysis: &'a Analysis,
    /// The host name of each function of the main emit, and the switch of
    /// each swapped one, by entry.
    pub names: &'a BTreeMap<u64, String>,
    /// The runtime's overrides, by entry.
    pub overrides: &'a [relift_emit::Override],
    /// The bundled mods.
    pub list: &'a [Bundled],
    /// The module's plan.
    pub plan: &'a ModulePlan,
    /// The image id of the build.
    pub image_id: [u8; 32],
}

fn raw(m: &Module, pc: u64) -> u64 {
    m.bytes_at(pc).and_then(|b| b.get(..4)).map_or(0, |w| {
        u64::from(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
    })
}

/// Lifts and emits every variant of the module's swapped functions into
/// `dir`; returns the C files and the swaps for `mods.c`.
///
/// # Errors
///
/// A variant reaches a function that is not in the module's analysis
/// (patched code calling new code), which is not built yet.
pub fn emit(v: &Variants<'_>, dir: &Path) -> Result<(Vec<PathBuf>, Vec<SwapC>), BoxError> {
    let mut files = Vec::new();
    let mut out = Vec::new();
    let known: BTreeMap<u64, EntryKind> = v
        .analysis
        .functions
        .values()
        .map(|f| (f.entry, f.kind))
        .collect();
    for s in &v.plan.swaps {
        let mut variants = Vec::new();
        for mask in 0..1usize << s.mods.len() {
            let Some(on) = s.on(v.list, mask) else {
                variants.push(None);
                continue;
            };
            let mut m = v.module.clone();
            m.image = bundled::combine(&v.module.image, &v.plan.per_mod, &on);
            let mut skip = known.clone();
            skip.remove(&s.entry);
            let va = relift_analyze::discover_more(v.fe, &m, &skip, &[s.entry]);
            if let Some(extra) = va.functions.keys().find(|&&e| e != s.entry) {
                return Err(format!(
                    "a bundled mod makes {:#x} call new code at {extra:#x}; mods that add functions cannot be bundled yet",
                    s.entry
                )
                .into());
            }
            let f = va.functions.get(&s.entry).ok_or_else(|| {
                format!("no function lifts at {:#x} with the bundled mods", s.entry)
            })?;
            let host = format!(
                "{}_v{mask}",
                v.names[&s.entry].replace("rl_modsw_", "rl_modfn_")
            );
            let fns = [relift_emit::Function {
                entry: f.entry,
                name: host.clone(),
                guest_name: f.name.clone(),
                span: f.span(v.fe.granule()),
                insts: f
                    .insts
                    .iter()
                    .map(|(&pc, i)| {
                        let e = match i {
                            Insn::Lifted(b) => relift_emit::Insn::Lifted {
                                block: b,
                                raw: raw(&m, pc),
                            },
                            Insn::Failed(e) => relift_emit::Insn::Failed(e),
                        };
                        (pc, e)
                    })
                    .collect(),
                tables: f.tables.clone(),
                pads: f.pads.clone(),
            }];
            let mut ovs: Vec<relift_emit::Override> = Vec::new();
            for t in targets(f) {
                if let Some(o) = v.overrides.iter().find(|o| o.entry == t) {
                    ovs.push(o.clone());
                } else if let Some(n) = v.names.get(&t) {
                    ovs.push(relift_emit::Override {
                        entry: t,
                        host_fn: n.clone(),
                        may_unwind: true,
                        resume_point: false,
                        declare: true,
                    });
                }
            }
            let sym = format!("rl_modunit_{}_{:x}_{mask}", v.index, s.entry);
            let emitted = relift_emit::emit(
                v.fe,
                &fns,
                &ovs,
                &relift_emit::Options {
                    unit_name: format!("{}+mod{mask}", v.module.name),
                    unit_symbol: sym,
                    module_base: v.module.base,
                    image_id: v.image_id,
                    ..relift_emit::Options::default()
                },
            );
            let d = dir.join(format!("{:x}_{mask}", s.entry));
            std::fs::create_dir_all(&d)?;
            for file in &emitted.files {
                let path = d.join(&file.name);
                relift_cache::write_if_changed(&path, file.text.as_bytes())?;
                if file.name.ends_with(".c") {
                    files.push(path);
                }
            }
            variants.push(Some(host));
        }
        out.push(SwapC {
            entry: s.entry,
            switch: v.names[&s.entry].clone(),
            mods: s.mods.clone(),
            variants,
        });
    }
    Ok((files, out))
}

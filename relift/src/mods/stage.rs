//! The code mods' part of one `relift build`: the user mods applied to the
//! loaded modules, the incremental analysis of each patched module, the
//! conflict checks, and the bundled mods' plan, variants and `mods.c`.

use std::collections::BTreeMap;
use std::ops::Range;
use std::path::{Path, PathBuf};
use std::time::Instant;

use relift_analyze::{Function, Layout, Progress, Reuse};
use relift_guest::{Frontend, Module};
use relift_switch_formats::nso::Nso;

use super::bundled::{self, Bundled, SwapC};
use super::conflicts::{self, ModWrites};
use super::user::{self, Loaded, UserMod};
use super::variants::{self, ModulePlan, Variants};
use super::{cache, hash_files};
use crate::import::BoxError;

/// The mods of a build and what they did.
pub struct Stage {
    /// The user mods, in order.
    pub user: Vec<UserMod>,
    /// The bundled mods, in declared order.
    pub bundled: Vec<Bundled>,
    /// Per module: the module before the user mods, when a patch changed it.
    pub originals: Vec<Option<Nso>>,
    /// Per module: whether a user mod replaced it.
    pub replaced: Vec<bool>,
    /// Per module: each user mod's image ranges.
    pub writes: Vec<Vec<ModWrites>>,
    /// Notes to show.
    pub notes: Vec<String>,
    /// Where analyses are cached.
    pub cache_dir: PathBuf,
    /// This relift's hash.
    pub tool: [u8; 32],
    /// The swapped functions of every module.
    pub swaps: Vec<SwapC>,
    /// Per bundled mod: its data writes (guest address, bytes).
    pub data: Vec<Vec<(u64, Vec<u8>)>>,
    /// The variants' C files.
    pub files: Vec<PathBuf>,
}

/// How the analysis of one module was found.
#[derive(Clone, Debug)]
pub enum Found {
    /// Discovery from scratch.
    Full,
    /// The module is unchanged since a build that kept its outlines.
    Cached,
    /// Only the functions a user mod changed were lifted again.
    Incremental {
        /// What was reused.
        reuse: Reuse,
        /// Milliseconds the incremental analysis took.
        ms: f64,
        /// Milliseconds reading the cached stock analysis took (or finding
        /// it, when there was none).
        base_ms: f64,
    },
}

/// The module to analyze.
#[derive(Clone, Copy)]
pub struct Input<'a> {
    /// The module with the user mods applied.
    pub module: &'a Module,
    /// The module before them, when they changed it.
    pub original: Option<&'a Module>,
}

/// The analysis of one module.
pub struct Analyzed<T> {
    /// Its functions without their code.
    pub layout: Layout,
    /// `visit` of each function, in entry order.
    pub visits: Vec<T>,
    /// How it was found.
    pub found: Found,
}

impl Stage {
    /// Reads `mods` and the bundled mods of `game`, and applies the user
    /// mods to `modules` in order.
    ///
    /// # Errors
    ///
    /// A mod does not read, or the user mods conflict (see [`user::apply`]).
    pub fn new(
        mods: &[PathBuf],
        game: &toml::Table,
        game_dir: &Path,
        title_id: &str,
        modules: &mut [Loaded],
        cache_dir: PathBuf,
    ) -> Result<Stage, BoxError> {
        let user: Vec<UserMod> = mods
            .iter()
            .map(|m| user::read(m, title_id))
            .collect::<Result<_, _>>()?;
        let bundled = bundled::read(game, game_dir, title_id)?;
        let before: Vec<Nso> = modules.iter().map(|m| m.nso.clone()).collect();
        let applied = user::apply(&user, modules)?;
        let n = modules.len();
        let mut s = Stage {
            originals: vec![None; n],
            replaced: vec![false; n],
            writes: vec![Vec::new(); n],
            notes: applied.notes,
            data: vec![Vec::new(); bundled.len()],
            user,
            bundled,
            cache_dir,
            tool: cache::tool_hash()?,
            swaps: Vec::new(),
            files: Vec::new(),
        };
        for w in &applied.writes {
            if w.replaced {
                s.replaced[w.module] = true;
            }
            s.writes[w.module].push(ModWrites {
                name: s.user[w.user_mod].name.clone(),
                ranges: w.ranges.clone(),
                replaced: w.replaced,
            });
        }
        for (i, b) in before.into_iter().enumerate() {
            if !s.replaced[i] && !s.writes[i].is_empty() {
                s.originals[i] = Some(b);
            }
        }
        Ok(s)
    }

    /// Whether module `i`'s bundled mods or user mods need [`Stage::plan`].
    pub fn needs_plan(&self, i: usize) -> bool {
        !self.bundled.is_empty() || !self.writes[i].is_empty()
    }

    /// The outlines of module `i` (`module`, at `base`) with `visit` of
    /// each function: incremental from the cached outlines of `original`
    /// (the module before the user mods) when a patch changed it, taken
    /// from the cache when the module is unchanged since a build that kept
    /// them, else from scratch, and then kept for later builds.
    ///
    /// # Errors
    ///
    /// The cache cannot be written.
    pub fn analyze<T: Send>(
        &self,
        fe: &(dyn Frontend + Sync),
        i: usize,
        input: &Input<'_>,
        on: &mut dyn FnMut(Progress),
        visit: &(dyn Fn(&Function) -> T + Sync),
    ) -> Result<Analyzed<T>, BoxError> {
        let Input { module, original } = *input;
        let path = self.cache_dir.join(format!("{i}.zst"));
        if let Some(orig) = original {
            let key = cache::key(&self.tool, orig);
            let t0 = Instant::now();
            let base = match cache::read(&path, &key) {
                Some(l) => l,
                None => {
                    let l = relift_analyze::outline_with(fe, orig, &mut |_| {});
                    cache::write(&path, &key, &l)?;
                    l
                }
            };
            let changed: Vec<Range<u64>> = self.writes[i]
                .iter()
                .flat_map(|w| w.ranges.iter())
                .map(|r| module.base + r.start..module.base + r.end)
                .collect();
            let base_ms = t0.elapsed().as_secs_f64() * 1e3;
            let t0 = Instant::now();
            let (layout, visits, reuse) =
                relift_analyze::rediscover(fe, module, orig, &base, &changed, on, visit);
            let ms = t0.elapsed().as_secs_f64() * 1e3;
            return Ok(Analyzed {
                layout,
                visits,
                found: Found::Incremental { reuse, ms, base_ms },
            });
        }
        let key = cache::key(&self.tool, module);
        if let Some(l) = cache::read(&path, &key) {
            let (layout, visits, _) =
                relift_analyze::rediscover(fe, module, module, &l, &[], on, visit);
            return Ok(Analyzed {
                layout,
                visits,
                found: Found::Cached,
            });
        }
        let (layout, visits) = relift_analyze::outline_visit(fe, module, on, visit);
        cache::write(&path, &key, &layout)?;
        Ok(Analyzed {
            layout,
            visits,
            found: Found::Full,
        })
    }

    /// The superset functions of module `i` over its outlines `hot`
    /// ([`crate::total::superset`]), from the cache when the module and
    /// `hot` are unchanged since a build that kept them; gives whether they
    /// came from the cache.
    ///
    /// # Errors
    ///
    /// The cache cannot be written.
    pub(crate) fn superset(
        &self,
        fe: &(dyn Frontend + Sync),
        i: usize,
        module: &Module,
        hot: &Layout,
    ) -> Result<(crate::total::Superset, bool), BoxError> {
        let path = self.cache_dir.join(format!("{i}.cold.zst"));
        let mut h = std::hash::DefaultHasher::new();
        for o in hot.functions.values() {
            std::hash::Hash::hash(&(o.entry, &o.runs), &mut h);
        }
        let key = format!(
            "cold {} {:016x}",
            cache::key(&self.tool, module),
            std::hash::Hasher::finish(&h)
        );
        if let Some(layout) = cache::read(&path, &key) {
            return Ok((crate::total::Superset { layout, roots: 0 }, true));
        }
        let s = crate::total::superset(fe, module, hot);
        cache::write(&path, &key, &s.layout)?;
        Ok((s, false))
    }

    /// Checks module `i`'s user mods against its overrides and its bundled
    /// mods, and plans its bundled mods: their swapped functions and data
    /// writes. Returns the plan and the notes to show.
    ///
    /// # Errors
    ///
    /// A conflict the build refuses.
    pub fn plan(
        &mut self,
        fe: &(dyn Frontend + Sync),
        i: usize,
        module: &Module,
        build_id: &[u8; 32],
        layout: &Layout,
        overrides: &BTreeMap<u64, String>,
    ) -> Result<(ModulePlan, Vec<String>), BoxError> {
        let name = &module.name;
        let granule = fe.granule();
        let per_mod = bundled::per_mod(&self.bundled, build_id, &module.image)?;
        let guest = |r: &Range<u64>| module.base + r.start..module.base + r.end;
        let ranges: Vec<Range<u64>> = self.writes[i]
            .iter()
            .flat_map(|w| w.ranges.iter())
            .chain(per_mod.iter().flatten().flat_map(|m| m.ranges.iter()))
            .map(guest)
            .collect();
        let a = &relift_analyze::touching(fe, module, layout, &ranges);
        let touched = variants::touched_by_mods(a, granule, module.base, &per_mod);
        let label = |e: u64| conflicts::label(name, a.functions.get(&e), e);
        let swaps = bundled::swaps(&self.bundled, &touched, &label)?;
        let by_fn: BTreeMap<u64, Vec<String>> = touched
            .iter()
            .map(|(e, ks)| {
                (
                    *e,
                    ks.iter().map(|&k| self.bundled[k].name.clone()).collect(),
                )
            })
            .collect();
        let guest_writes: Vec<ModWrites> = self.writes[i]
            .iter()
            .map(|w| ModWrites {
                name: w.name.clone(),
                ranges: w.ranges.iter().map(guest).collect(),
                replaced: w.replaced,
            })
            .collect();
        let notes = conflicts::check(name, a, granule, &guest_writes, overrides, &by_fn)?;
        for (k, m) in per_mod.iter().enumerate() {
            let Some(m) = m else { continue };
            for r in &m.ranges {
                let g = guest(r);
                if conflicts::touched(a, granule, std::slice::from_ref(&g)).is_empty() {
                    let bytes = m.image[r.start as usize..r.end as usize].to_vec();
                    self.data[k].push((g.start, bytes));
                }
            }
        }
        Ok((ModulePlan { per_mod, swaps }, notes))
    }

    /// Lifts and emits the variants of a module's swapped functions into
    /// `dir`.
    ///
    /// # Errors
    ///
    /// As [`variants::emit`].
    pub fn emit_variants(&mut self, v: &Variants<'_>, dir: &Path) -> Result<(), BoxError> {
        let (files, swaps) = variants::emit(v, dir)?;
        self.files.extend(files);
        self.swaps.extend(swaps);
        Ok(())
    }

    /// `mods.c` for the bundled mods, when there are any.
    pub fn mods_c(&self, headers: &[&str]) -> Option<String> {
        (!self.bundled.is_empty())
            .then(|| bundled::mods_c(headers, &self.bundled, &self.swaps, &self.data))
    }

    /// The bundled mods' bool settings.
    pub fn settings(&self) -> Vec<relift_info::Setting> {
        super::info_section(&self.user, &self.bundled).1
    }

    /// The self-description's `mods` section.
    pub fn info(&self) -> relift_info::Mods {
        super::info_section(&self.user, &self.bundled).0
    }

    /// The hash of the whole mod set, for the build's image id.
    pub fn hash(&self) -> String {
        let mut all = BTreeMap::new();
        for (k, m) in self.user.iter().enumerate() {
            all.insert(format!("user/{k}/{}", m.name), m.hash.as_bytes().to_vec());
        }
        for (k, b) in self.bundled.iter().enumerate() {
            all.insert(
                format!("bundled/{k}/{}", b.name),
                b.hash.as_bytes().to_vec(),
            );
        }
        hash_files(&all)
    }
}

/// The text of a reuse for the build log.
pub fn reuse_line(module: &str, reuse: &Reuse, ms: f64, base_ms: f64) -> String {
    format!(
        "{module}: code mods changed {} function{}; {} reused from the stock analysis ({ms:.0} ms, stock analysis read in {base_ms:.0} ms)",
        reuse.lifted.len(),
        if reuse.lifted.len() == 1 { "" } else { "s" },
        reuse.reused.len()
    )
}

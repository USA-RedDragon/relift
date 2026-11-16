//! `relift build` and `relift run`: lift, emit and compile the project's
//! modules for this host, then run the result.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use relift_analyze::{EntryKind, Insn, Outline, Progress};
use relift_build::{Compile, Event, Plan};
use relift_cpu_arm64::frontend::Arm64;
use relift_guest::{Frontend, Root, RootKind};
use relift_progress::{Line, Metrics, Paint, Verbosity, count, duration, noun};
use relift_switch::program::{self, Placed};
use relift_switch_formats::npdm::Npdm;
use relift_switch_formats::nso::Nso;

use crate::hints::Hint;
use crate::import::{BoxError, cache_root};
use crate::mods::stage::{Analyzed, Found, Input, Stage};
use crate::mods::variants::ModulePlan;
use crate::settings::Settings;
use crate::ui::Ui;

/// The runtime sources this binary was built beside, used when the
/// `runtime-dir` setting is empty.
pub(crate) const RUNTIME_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../runtime");

/// The order the Switch loader maps exefs slots in.
fn slot_rank(slot: &str) -> u32 {
    match slot {
        "rtld" => 0,
        "main" => 1,
        "sdk" => 20,
        s => s
            .strip_prefix("subsdk")
            .and_then(|n| n.parse::<u32>().ok())
            .map_or(30, |n| 2 + n),
    }
}

fn read_toml(path: &Path) -> Result<toml::Table, BoxError> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    text.parse::<toml::Table>()
        .map_err(|e| format!("{}: {e}", path.display()).into())
}

/// The import directory game.toml in the working directory names.
pub(crate) fn import_dir(settings: &Settings) -> Result<PathBuf, BoxError> {
    let game = read_toml(Path::new("game.toml"))?;
    let dir = game
        .get("game")
        .and_then(|g| g.get("import_dir"))
        .and_then(|d| d.as_str())
        .ok_or("game.toml has no game.import_dir; run relift import first")?;
    let dir = Path::new(dir);
    if dir.is_absolute() {
        Ok(dir.to_path_buf())
    } else {
        Ok(cache_root(settings)?.join(dir))
    }
}

/// The modules of an import in load order, named, with its NPDM.
type Loaded = (Vec<(String, Nso)>, Option<Npdm>);

/// The modules of the import in load order, with the NPDM.
pub(crate) fn load(import: &Path) -> Result<Loaded, BoxError> {
    let (mods, npdm) = load_slots(import)?;
    Ok((mods.into_iter().map(|m| (m.name, m.nso)).collect(), npdm))
}

/// As [`load`], with each module's exefs slot.
fn load_slots(import: &Path) -> Result<(Vec<crate::mods::user::Loaded>, Option<Npdm>), BoxError> {
    let table = read_toml(&import.join("modules.toml"))?;
    let mut mods = Vec::new();
    for m in table
        .get("module")
        .and_then(|m| m.as_array())
        .ok_or("modules.toml lists no module")?
    {
        let field = |k: &str| m.get(k).and_then(|v| v.as_str()).unwrap_or_default();
        let path = import.join(field("path"));
        let bytes = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
        let nso = Nso::parse(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
        mods.push(crate::mods::user::Loaded {
            slot: field("slot").to_string(),
            name: field("name").to_string(),
            nso,
        });
    }
    mods.sort_by_key(|m| slot_rank(&m.slot));
    let npdm = std::fs::read(import.join("exefs").join("main.npdm"))
        .ok()
        .and_then(|b| Npdm::parse(&b).ok());
    Ok((mods, npdm))
}

fn c_ident(s: &str) -> String {
    s.chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect()
}

/// What lifting and emitting the modules gave.
struct Emitted {
    files: Vec<PathBuf>,
    units: Vec<String>,
    functions: usize,
    unlifted: usize,
    unlifted_list: String,
    /// Which functions produce FPSR flags.
    fpsr: crate::fpflags::Plan,
    /// Whether lifted code drops the FPCR guards.
    fpcr: crate::fpcr::Plan,
    /// The entries of every module, one `<module> <address> <kind>` line
    /// each ([`entries_line`]), for the dev server's later discovery.
    entries: String,
    /// The C files of the cold copies and code maps.
    cold_files: Vec<PathBuf>,
    /// The symbols of the cold units.
    cold_units: Vec<String>,
    /// The symbols of the code maps.
    code_maps: Vec<String>,
    /// One line per module: the class of each word of its executable pages.
    total: String,
}

/// One line of the entry list: the module index, the address, and `S` for
/// an entry other functions stop at or `A` for an address-taken one.
fn entries_line(module: usize, addr: u64, kind: EntryKind) -> String {
    let k = if kind == EntryKind::AddressTaken {
        'A'
    } else {
        'S'
    };
    format!("{module} {addr:#x} {k}\n")
}

/// The entries of an entry list, per module index.
pub(crate) fn parse_entries(text: &str) -> Result<Vec<BTreeMap<u64, EntryKind>>, BoxError> {
    let mut out: Vec<BTreeMap<u64, EntryKind>> = Vec::new();
    for line in text.lines() {
        let mut f = line.split(' ');
        let (Some(m), Some(a), Some(k)) = (f.next(), f.next(), f.next()) else {
            return Err(format!("a bad entry list line: {line}").into());
        };
        let m: usize = m.parse()?;
        let a = u64::from_str_radix(a.trim_start_matches("0x"), 16)?;
        let k = if k == "A" {
            EntryKind::AddressTaken
        } else {
            EntryKind::Called
        };
        if out.len() <= m {
            out.resize(m + 1, BTreeMap::new());
        }
        out[m].insert(a, k);
    }
    Ok(out)
}

/// The identity of the guest image set: the modules in load order, by
/// name and bytes, and `game.lock` when there is one.
pub(crate) fn image_id(placed: &[Placed]) -> [u8; 32] {
    let mut all = Vec::new();
    for p in placed {
        all.extend_from_slice(p.name.as_bytes());
        all.push(0);
        all.extend_from_slice(&p.base.to_le_bytes());
        all.extend_from_slice(&relift_switch_formats::sha256(&p.nso.image));
    }
    if let Ok(lock) = std::fs::read("game.lock") {
        all.extend_from_slice(&relift_switch_formats::sha256(&lock));
    }
    relift_switch_formats::sha256(&all)
}

/// The module `p` as analysis sees it, with the hints that name it as roots.
pub(crate) fn module_with_hints(p: &Placed, hints: &BTreeSet<Hint>) -> relift_guest::Module {
    let mut module = program::module(p);
    for h in hints.iter().filter(|h| h.module == p.name) {
        module.roots.push(Root {
            addr: p.base.wrapping_add(h.offset),
            kind: RootKind::Hint,
            name: None,
        });
    }
    module
}

fn unlifted_line(module: &str, pc: u64, word: u64, message: &str) -> String {
    format!("{module}\t{pc:#x}\t{word:08x}\t{message}\n")
}

/// `-I` of the build directory, where generated C files name their data
/// files as `c/<name>`: an assembler `.incbin` and a compiler's `#embed`
/// both find them there, and a dependency file records the path as
/// written instead of the directory of the including file.
pub(crate) fn build_dir_flag(out: &Path) -> String {
    format!("-I{}", out.display())
}

/// Lifts and emits `placed` into `cdir`, one directory per module; returns
/// the emitted C files and the unit symbols.
fn emit_modules(
    settings: &Settings,
    placed: &[Placed],
    cdir: &Path,
    ui: &mut Ui,
    rows: &[usize],
    hints: &BTreeSet<Hint>,
    stage: &mut Stage,
) -> Result<Emitted, BoxError> {
    let mut done = Emitted {
        files: Vec::new(),
        units: Vec::new(),
        functions: 0,
        unlifted: 0,
        unlifted_list: String::new(),
        fpsr: crate::fpflags::Plan::default(),
        fpcr: crate::fpcr::Plan::default(),
        entries: String::new(),
        cold_files: Vec::new(),
        cold_units: Vec::new(),
        code_maps: Vec::new(),
        total: String::new(),
    };
    let mut census = crate::total::Census::default();
    let mut fpsr = crate::fpflags::Fpsr::new(placed);
    let mut fpcr = crate::fpcr::Fpcr::default();
    let cpu = Arm64 {
        features: relift_switch::CPU_FEATURES,
    };
    for (i, (p, &row)) in placed.iter().zip(rows).enumerate() {
        let module = module_with_hints(p, hints);
        ui.begin(row, "discover");
        let mut lifting = false;
        let mut found = 0;
        let mine = fpsr.slots_of(p);
        let visit = |f: &relift_analyze::Function| fpsr.visit(&module, &mine, f);
        let original = stage.originals[i].clone().map(|nso| {
            module_with_hints(
                &Placed {
                    name: p.name.clone(),
                    base: p.base,
                    nso,
                },
                hints,
            )
        });
        let analyzed = stage.analyze(
            &cpu,
            i,
            &Input {
                module: &module,
                original: original.as_ref(),
            },
            &mut |event| match event {
                Progress::Found { entries, .. } => {
                    found = entries;
                    ui.update(
                        row,
                        Metrics {
                            done: entries as u64,
                            unit: "functions found",
                            ..Metrics::default()
                        },
                    );
                }
                Progress::Lifted {
                    done,
                    total,
                    unlifted,
                } => {
                    if !lifting {
                        lifting = true;
                        ui.done(row, Line::new().text(noun(total as u64, "function")));
                        ui.begin(row, "lift");
                    }
                    ui.update(
                        row,
                        Metrics {
                            done: done as u64,
                            total: Some(total as u64),
                            unit: "functions",
                            unlifted: Some(unlifted as u64),
                            ..Metrics::default()
                        },
                    );
                }
            },
            &visit,
        )?;
        let Analyzed {
            layout,
            visits,
            found: how,
        } = analyzed;
        if let Found::Incremental { reuse, ms, base_ms } = &how {
            ui.note(&Line::new().text(crate::mods::stage::reuse_line(
                &p.name, reuse, *ms, *base_ms,
            )));
        }
        if !lifting {
            ui.done(row, Line::new().text(noun(found as u64, "function")));
            ui.begin(row, "lift");
        }
        let mut failed = BTreeMap::new();
        for o in layout.functions.values() {
            for (pc, e) in &o.failed {
                failed.insert(*pc, e);
            }
        }
        for (&pc, e) in &failed {
            done.unlifted_list
                .push_str(&unlifted_line(&p.name, pc, e.insn, &e.message));
            ui.detail(
                &Line::new()
                    .text(format!("  {}  {pc:#x}  {:08x}  ", p.name, e.insn))
                    .paint(Paint::Yellow, e.message.clone()),
            );
        }
        for o in layout.functions.values() {
            done.entries.push_str(&entries_line(i, o.entry, o.kind));
        }
        done.unlifted += failed.len();
        done.functions += layout.functions.len();
        ui.done(row, lift_summary(layout.functions.len(), failed.len()));
        let all: Vec<&Outline> = layout.functions.values().collect();
        let raw = |pc: u64| {
            module.bytes_at(pc).and_then(|b| b.get(..4)).map_or(0, |w| {
                u64::from(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
            })
        };
        let head = |o: &Outline| relift_emit::Head {
            entry: o.entry,
            name: program::host_name(p, o.name.as_deref(), o.entry),
            guest_name: o.name.clone(),
            span: o.span(),
            pads: o.pads.clone(),
        };
        let known_overrides: BTreeMap<u64, String> = layout
            .functions
            .keys()
            .filter_map(|&a| Some((a, program::known(module.bytes_at(a)?)?.host_fn.to_string())))
            .collect();
        let (plan, notes) = if stage.needs_plan(i) {
            stage.plan(&cpu, i, &module, &p.nso.build_id, &layout, &known_overrides)?
        } else {
            (ModulePlan::default(), Vec::new())
        };
        for n in notes {
            ui.note(&Line::new().text(n));
        }
        let swapped: BTreeSet<u64> = plan.swaps.iter().map(|s| s.entry).collect();
        let keep: Vec<usize> = (0..all.len())
            .filter(|&k| !swapped.contains(&all[k].entry))
            .collect();
        let heads: Vec<relift_emit::Head> = keep.iter().map(|&k| head(all[k])).collect();
        let mut overrides: Vec<relift_emit::Override> = layout
            .functions
            .keys()
            .filter_map(|&a| {
                let k = program::known(module.bytes_at(a)?)?;
                Some(relift_emit::Override {
                    entry: a,
                    host_fn: k.host_fn.into(),
                    may_unwind: k.may_unwind,
                    resume_point: k.resume_point,
                    declare: false,
                })
            })
            .collect();
        if !swapped.is_empty() {
            let known: BTreeMap<u64, EntryKind> = layout
                .functions
                .values()
                .map(|o| (o.entry, o.kind))
                .collect();
            let mut names: BTreeMap<u64, String> =
                heads.iter().map(|h| (h.entry, h.name.clone())).collect();
            for &e in &swapped {
                let sw = crate::mods::variants::switch_name(i, e);
                names.insert(e, sw.clone());
                overrides.retain(|o| o.entry != e);
                overrides.push(relift_emit::Override {
                    entry: e,
                    host_fn: sw,
                    may_unwind: true,
                    resume_point: false,
                    declare: true,
                });
            }
            stage.emit_variants(
                &crate::mods::variants::Variants {
                    fe: &cpu,
                    index: i,
                    module: &module,
                    known: &known,
                    names: &names,
                    overrides: &overrides,
                    list: &stage.bundled.clone(),
                    plan: &plan,
                },
                &cdir.join("mods").join(i.to_string()),
            )?;
        }
        let flags = fpsr.begin_module(i, &module, &layout, visits, &cpu, &overrides);
        if fpsr.has_linkage() {
            fpcr.add_module(p, &module, &layout, &flags, &overrides, &swapped);
        }
        let summaries = std::sync::Mutex::new(Vec::new());
        let summarized: Vec<std::sync::atomic::AtomicBool> = heads
            .iter()
            .map(|_| std::sync::atomic::AtomicBool::new(false))
            .collect();
        let fpsr_now = &fpsr;
        let lend = |ks: &[usize], sink: &mut dyn FnMut(usize, &relift_emit::Function<'_>)| {
            let group: Vec<&Outline> = ks.iter().map(|&k| all[keep[k]]).collect();
            let mut n = 0;
            relift_analyze::lift_each(&cpu, &module, &group, &mut |_, f| {
                let k = ks[n];
                n += 1;
                let e = emit_function(f, &heads[k], &raw);
                let first = !summarized[k].swap(true, std::sync::atomic::Ordering::Relaxed);
                if first && let Some(s) = fpsr_now.function(&flags, &module, all[keep[k]], f) {
                    summaries
                        .lock()
                        .unwrap_or_else(std::sync::PoisonError::into_inner)
                        .push((keep[k], s));
                }
                sink(k, &e);
            });
        };
        let parts = fpsr.parts();
        let unit = format!("rl_unit_{i}_{}", c_ident(&p.name));
        ui.begin(row, "emit");
        let dir = cdir.join(format!("{i}_{}", c_ident(&p.name)));
        std::fs::create_dir_all(&dir)?;
        let mut files: Vec<relift_emit::File> = Vec::new();
        let window = (kib(settings.emit.shard_kib) / WINDOW_RATIO).max(1) as u64;
        let store = crate::emitstore::EmitStore::open(
            &cdir.with_file_name("emit").join(i.to_string()),
            stage.tool,
            p.base,
            window,
        )?;
        let from = relift_emit::Stream {
            heads: &heads,
            lend: &lend,
            store: Some(&store),
        };
        let (undefined, written) = with_writers(&mut |write| {
            relift_emit::emit_reusing(
                &cpu,
                &from,
                &overrides,
                &relift_emit::Options {
                    shard_bytes: kib(settings.emit.shard_kib),
                    piece_bytes: kib(settings.emit.piece_kib),
                    unit_name: p.name.clone(),
                    unit_symbol: unit.clone(),
                    module_base: p.base,
                    parts,
                    test_fault: test_fault(),
                    window,
                    ..relift_emit::Options::default()
                },
                &mut |d, total| {
                    ui.update(
                        row,
                        Metrics {
                            done: d as u64,
                            total: Some(total as u64),
                            unit: "functions",
                            ..Metrics::default()
                        },
                    );
                },
                &mut |f| {
                    let relift_emit::File { name, text, fns } = f;
                    write(dir.join(&name), text);
                    files.push(relift_emit::File {
                        name,
                        text: String::new(),
                        fns,
                    });
                },
            )
        });
        written?;
        let counts = store.finish()?;
        remove_stale_shards(&dir, &files)?;
        let mut summaries = summaries
            .into_inner()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if parts {
            let mut seen = vec![false; all.len()];
            for (k, _) in &summaries {
                seen[*k] = true;
            }
            let rest: Vec<usize> = (0..all.len()).filter(|&k| !seen[k]).collect();
            let outlines: Vec<&Outline> = rest.iter().map(|&k| all[k]).collect();
            let more = relift_analyze::each_lifted(&cpu, &module, &outlines, |o, f| {
                fpsr.function(&flags, &module, o, f)
            });
            summaries.extend(
                rest.into_iter()
                    .zip(more)
                    .filter_map(|(k, s)| Some((k, s?))),
            );
        }
        summaries.sort_unstable_by_key(|s| s.0);
        fpsr.end_module(flags, summaries.into_iter().map(|s| s.1).collect());
        let out = relift_emit::Output { files, undefined };
        let shards = out
            .files
            .iter()
            .filter(|f| f.name.starts_with("shard"))
            .count();
        fpsr.add_files(&dir, &out.files);
        for f in &out.files {
            let path = dir.join(&f.name);
            if f.name.ends_with(".c") {
                done.files.push(path);
            }
        }
        ui.done(
            row,
            Line::new().text(format!(
                "{}, {}, {} of {} reused",
                noun(shards as u64, "shard"),
                noun(overrides.len() as u64, "override"),
                count(counts.reused as u64),
                count(counts.total as u64)
            )),
        );
        if settings.emit.cold {
            let heads_by: BTreeMap<u64, &relift_emit::Head> =
                heads.iter().map(|h| (h.entry, h)).collect();
            let cold = ColdModule {
                cpu: &cpu,
                index: i,
                p,
                module: &module,
                layout: &layout,
                heads: &heads_by,
                overrides: &overrides,
                hot_unit: &unit,
                dir: &dir,
                window,
                shard_bytes: kib(settings.emit.shard_kib),
                piece_bytes: kib(settings.emit.cold_piece_kib),
                first: settings.emit.cold_first,
            };
            let made = emit_cold(&cold, stage, ui, row)?;
            census.add(&made.census);
            done.total
                .push_str(&format!("{} {}\n", p.name, made.census.line()));
            done.cold_files.extend(made.files);
            done.cold_units.push(made.unit);
            done.code_maps.push(made.map);
        }
        done.units.push(unit);
    }
    if settings.emit.cold {
        done.total.push_str(&format!("all {}\n", census.line()));
    }
    done.fpcr = fpcr.finish(&fpsr.linkage(), settings.emit.fpcr_guards);
    drop(fpcr);
    done.fpsr = fpsr.finish()?;
    Ok(done)
}

/// The C function of the lifted function `f`, under the name and span of
/// `head`; `raw` gives the raw instruction at an address.
fn emit_function<'a>(
    f: &'a relift_analyze::Function,
    head: &relift_emit::Head,
    raw: &dyn Fn(u64) -> u64,
) -> relift_emit::Function<'a> {
    relift_emit::Function {
        entry: f.entry,
        name: head.name.clone(),
        guest_name: f.name.clone(),
        span: head.span,
        insts: f
            .insts
            .iter()
            .map(|(&pc, i)| {
                let e = match i {
                    Insn::Lifted(b) => relift_emit::Insn::Lifted {
                        block: b,
                        raw: raw(pc),
                    },
                    Insn::Failed(e) => relift_emit::Insn::Failed(e),
                };
                (pc, e)
            })
            .collect::<BTreeMap<_, _>>(),
        tables: f.tables.clone(),
        pads: f.pads.clone(),
    }
}

/// Whether the cold copy is built with the large code model and linked after
/// `.bss` ([`COLD_SCRIPT`]): on x86-64 ELF, where hot code reaches its data
/// and the runtime's only within 2 GiB, so the cold copy must not sit
/// between them.
const COLD_FAR: bool =
    cfg!(target_arch = "x86_64") && !cfg!(windows) && !cfg!(target_vendor = "apple");

/// The linker script fragment that puts the cold copy's text after `.bss`,
/// in a segment of its own (GNU ld and lld).
const COLD_SCRIPT: &str = "SECTIONS { . = ALIGN(CONSTANT(MAXPAGESIZE)); relift_cold : { *(relift_cold) } } INSERT AFTER .bss;\n";

/// What [`emit_cold`] takes of one module.
struct ColdModule<'a> {
    cpu: &'a Arm64,
    index: usize,
    p: &'a Placed,
    module: &'a relift_guest::Module,
    /// The module's hot functions.
    layout: &'a relift_analyze::Layout,
    /// The hot functions emitted, by entry.
    heads: &'a BTreeMap<u64, &'a relift_emit::Head>,
    /// The hot unit's overrides.
    overrides: &'a [relift_emit::Override],
    hot_unit: &'a str,
    /// The module's C directory; the cold copy goes in its `cold`.
    dir: &'a Path,
    window: u64,
    shard_bytes: usize,
    piece_bytes: usize,
    /// The cold copy runs every function: it calls only itself.
    first: bool,
}

/// What [`emit_cold`] made of one module.
struct ColdMade {
    files: Vec<PathBuf>,
    unit: String,
    map: String,
    census: crate::total::Census,
}

/// Emits the cold copy of a module: every function of its hot layout and
/// its superset functions as small pieces dispatch enters at any
/// instruction, calling the hot functions by name, in shards of their own,
/// with the module's code map, checked to be total.
fn emit_cold(
    m: &ColdModule<'_>,
    stage: &Stage,
    ui: &mut Ui,
    row: usize,
) -> Result<ColdMade, BoxError> {
    let ColdModule { cpu, p, module, .. } = *m;
    let i = m.index;
    ui.begin(row, "cold");
    let (sup, cached) = stage.superset(cpu, i, module, m.layout)?;
    let mut hot: BTreeSet<u64> = m.overrides.iter().map(|o| o.entry).collect();
    for h in m.heads.values() {
        hot.insert(h.entry);
        hot.extend(h.pads.iter().copied());
    }
    let mut all: Vec<&Outline> = m
        .layout
        .functions
        .values()
        .chain(sup.layout.functions.values())
        .filter(|o| {
            m.first
                || o.runs
                    .iter()
                    .any(|&(first, n)| (0..u64::from(n)).any(|k| !hot.contains(&(first + 4 * k))))
        })
        .collect();
    all.sort_by_key(|o| o.entry);
    let heads: Vec<relift_emit::Head> = all
        .iter()
        .map(|o| relift_emit::Head {
            entry: o.entry,
            name: format!("{}_cold", program::host_name(p, o.name.as_deref(), o.entry)),
            guest_name: o.name.clone(),
            span: o.span(),
            pads: o.pads.clone(),
        })
        .collect();
    let mut calls: Vec<relift_emit::Override> = m.overrides.to_vec();
    calls.extend(
        m.heads
            .values()
            .filter(|_| !m.first)
            .map(|h| relift_emit::Override {
                entry: h.entry,
                host_fn: h.name.clone(),
                may_unwind: true,
                resume_point: false,
                declare: true,
            }),
    );
    let raw = |pc: u64| {
        module.bytes_at(pc).and_then(|b| b.get(..4)).map_or(0, |w| {
            u64::from(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
        })
    };
    let lend = |ks: &[usize], sink: &mut dyn FnMut(usize, &relift_emit::Function<'_>)| {
        let group: Vec<&Outline> = ks.iter().map(|&k| all[k]).collect();
        let mut n = 0;
        relift_analyze::lift_each(cpu, module, &group, &mut |_, f| {
            let k = ks[n];
            n += 1;
            sink(k, &emit_function(f, &heads[k], &raw));
        });
    };
    let unit = format!("rl_cold_{i}_{}", c_ident(&p.name));
    let dir = m.dir.join("cold");
    std::fs::create_dir_all(&dir)?;
    let store = crate::emitstore::EmitStore::open(
        &m.dir
            .parent()
            .unwrap_or(m.dir)
            .with_file_name("emit")
            .join(format!("{i}c")),
        stage.tool,
        p.base,
        m.window,
    )?;
    let from = relift_emit::Stream {
        heads: &heads,
        lend: &lend,
        store: Some(&store),
    };
    let mut files: Vec<relift_emit::File> = Vec::new();
    let (_, written) = with_writers(&mut |write| {
        relift_emit::emit_reusing(
            cpu,
            &from,
            &[],
            &relift_emit::Options {
                shard_bytes: m.shard_bytes,
                piece_bytes: m.piece_bytes,
                unit_name: format!("{} (cold)", p.name),
                unit_symbol: unit.clone(),
                module_base: p.base,
                window: m.window,
                enter_anywhere: Some(u64::MAX),
                calls: calls.clone(),
                entry_ranges: true,
                piece_syms: false,
                ..relift_emit::Options::default()
            },
            &mut |d, total| {
                ui.update(
                    row,
                    Metrics {
                        done: d as u64,
                        total: Some(total as u64),
                        unit: "functions",
                        ..Metrics::default()
                    },
                );
            },
            &mut |f| {
                let relift_emit::File { name, text, fns } = f;
                write(dir.join(&name), text);
                files.push(relift_emit::File {
                    name,
                    text: String::new(),
                    fns,
                });
            },
        )
    });
    written?;
    store.finish()?;
    remove_stale_shards(&dir, &files)?;
    let map = crate::total::code_map(cpu, module, &hot, &all)?;
    let sym = format!("rl_code_{i}_{}", c_ident(&p.name));
    let map_c = dir.join("codemap.c");
    relift_cache::write_if_changed(
        &map_c,
        crate::total::code_map_c(cpu.abi_headers(), &map, &sym, m.hot_unit, &unit, &p.name)
            .as_bytes(),
    )?;
    let mut out: Vec<PathBuf> = files
        .iter()
        .filter(|f| f.name.ends_with(".c"))
        .map(|f| dir.join(&f.name))
        .collect();
    out.push(map_c);
    let shards = files.iter().filter(|f| f.name.starts_with("shard")).count();
    ui.done(
        row,
        Line::new().text(format!(
            "{}, {} superset functions{}",
            noun(shards as u64, "cold shard"),
            count(sup.layout.functions.len() as u64),
            if cached || sup.roots == 0 {
                String::new()
            } else {
                format!(" from {}", noun(sup.roots as u64, "root"))
            }
        )),
    );
    ui.note(&Line::new().text(format!("{}: {}", p.name, map.census.line())));
    Ok(ColdMade {
        files: out,
        unit,
        map: sym,
        census: map.census,
    })
}

/// The guest bytes of a shard window per byte of the shard budget: lifted
/// AArch64 takes about a hundred bytes of C per guest byte, so a window
/// of the budget over this fills most of a shard.
const WINDOW_RATIO: usize = 64;

/// Runs `emit` with a `write` that writes files on threads of their own
/// (each only when changed); returns what `emit` gave and the first
/// error writing.
fn with_writers<R>(emit: &mut dyn FnMut(&mut Write<'_>) -> R) -> (R, std::io::Result<()>) {
    let (tx, rx) = std::sync::mpsc::sync_channel::<(PathBuf, String)>(WRITERS * 2);
    let shared = std::sync::Mutex::new(rx);
    let rx = &shared;
    std::thread::scope(move |sc| {
        let mut workers = Vec::new();
        for _ in 0..relift_proc::workers::jobs().clamp(1, WRITERS) {
            let spawned = std::thread::Builder::new().spawn_scoped(sc, move || {
                let mut r = Ok(());
                loop {
                    let next = rx
                        .lock()
                        .unwrap_or_else(std::sync::PoisonError::into_inner)
                        .recv();
                    let Ok((path, text)) = next else { break };
                    if r.is_ok() {
                        r = relift_cache::write_if_changed(&path, text.as_bytes()).map(|_| ());
                    }
                }
                r
            });
            match spawned {
                Ok(h) => workers.push(h),
                Err(_) => break,
            }
        }
        let inline = workers.is_empty();
        let mut r = Ok(());
        let out = emit(&mut |path, text| {
            if !inline {
                let _ = tx.send((path, text));
            } else if r.is_ok() {
                r = relift_cache::write_if_changed(&path, text.as_bytes()).map(|_| ());
            }
        });
        drop(tx);
        for w in workers {
            let done = w
                .join()
                .unwrap_or_else(|_| Err(std::io::Error::other("a file writer panicked")));
            if r.is_ok() {
                r = done;
            }
        }
        (out, r)
    })
}

/// Writes one file: its path and its text.
type Write<'w> = dyn FnMut(PathBuf, String) + 'w;

/// How many threads write emitted files.
const WRITERS: usize = 4;

/// Removes the shards of `dir` that the emit just done did not write.
fn remove_stale_shards(dir: &Path, files: &[relift_emit::File]) -> Result<(), BoxError> {
    let kept: BTreeSet<&str> = files.iter().map(|f| f.name.as_str()).collect();
    for e in std::fs::read_dir(dir)?.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if name.starts_with("shard") && name.ends_with(".c") && !kept.contains(name.as_str()) {
            std::fs::remove_file(e.path())?;
        }
    }
    Ok(())
}

/// The guest address in `RELIFT_EMIT_TEST_FAULT`, the emitter's test hook.
fn test_fault() -> Option<u64> {
    let v = std::env::var("RELIFT_EMIT_TEST_FAULT").ok()?;
    u64::from_str_radix(v.trim_start_matches("0x"), 16).ok()
}

fn kib(n: u32) -> usize {
    (n as usize).max(1) << 10
}

fn lift_summary(functions: usize, unlifted: usize) -> Line {
    let paint = if unlifted > 0 {
        Paint::Yellow
    } else {
        Paint::Plain
    };
    Line::new()
        .text(format!("{}, ", noun(functions as u64, "function")))
        .paint(paint, format!("{} unlifted", count(unlifted as u64)))
}

/// Bounds the compilers' memory, with the budget at three fifths of the
/// memory available now: light compiles run first, the rest in stages of
/// similar size, each in a pool sized so its members fit together
/// ([`relift_build::schedule`]). Sorts `compiles` largest source first
/// within a stage, so the long compiles start early. Returns the pools.
pub(crate) fn memory_pools(compiles: &mut [Compile], jobs: u32) -> Vec<(String, u32)> {
    let size = |c: &Compile| std::fs::metadata(&c.src).map_or(0, |m| m.len());
    compiles.sort_by_cached_key(|c| std::cmp::Reverse(size(c)));
    let Some(avail) = relift_build::available_memory() else {
        return Vec::new();
    };
    let needs: Vec<u64> = compiles
        .iter()
        .map(|c| relift_build::compile_memory(size(c)))
        .collect();
    let s = relift_build::schedule(&needs, jobs, avail / 5 * 3);
    let mut pools = Vec::new();
    for (k, st) in s.stages.iter().enumerate() {
        let name = format!("mem{}", k + 1);
        for &i in &st.members {
            compiles[i].pool = Some(name.clone());
            compiles[i].stage = u32::try_from(k + 1).unwrap_or(u32::MAX);
        }
        pools.push((name, st.depth));
    }
    pools
}

fn runtime_sources(rt: &Path) -> Result<Vec<PathBuf>, BoxError> {
    let mut out = Vec::new();
    for layer in ["core", "cpus/arm64", "guests/switch"] {
        let dir = rt.join(layer);
        let mut v: Vec<PathBuf> = std::fs::read_dir(&dir)
            .map_err(|e| format!("{}: {e}", dir.display()))?
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| p.extension().is_some_and(|x| x == "c" || x == "S"))
            .collect();
        v.sort();
        out.extend(v);
    }
    Ok(out)
}

/// The C compiler command: the configured or `PATH` clang, else `zig cc`
/// from the configured or `PATH` zig.
pub(crate) fn c_compiler(t: &crate::settings::Toolchain) -> Result<Vec<String>, BoxError> {
    if let Some(clang) = tool(&t.clang, "clang") {
        return Ok(vec![clang.display().to_string()]);
    }
    let zig = tool(&t.zig, "zig").ok_or("no clang or zig on PATH")?;
    Ok(vec![zig.display().to_string(), "cc".into()])
}

/// The glibc the pinned SDK was built for (`runtime/tools/linux-sdk.sh`
/// writes it beside its SDL3), when the SDK is there.
pub(crate) fn pinned_glibc(cache_root: &Path) -> Option<String> {
    let arch = std::env::consts::ARCH;
    let file = cache_root
        .join("sdk")
        .join("sdl3")
        .join(format!("{arch}-linux"))
        .join("glibc");
    let v = std::fs::read_to_string(file).ok()?;
    let v = v.trim();
    (!v.is_empty() && v.chars().all(|c| c.is_ascii_digit() || c == '.')).then(|| v.to_string())
}

/// The compiler of a release build. On Linux, with the pinned SDK in the
/// cache and zig on `PATH`, `zig cc` targeting that SDK's glibc, so the
/// program asks no newer glibc than the libraries it ships with (a host
/// compiler binds the host's newest symbol versions). Elsewhere, and
/// whenever `toolchain.clang` is set, [`c_compiler`].
pub(crate) fn release_compiler(settings: &Settings) -> Result<Vec<String>, BoxError> {
    if cfg!(target_os = "linux")
        && settings.toolchain.clang.as_os_str().is_empty()
        && settings.package.appimage != "never"
        && let (Some(v), Some(zig)) = (
            pinned_glibc(&cache_root(settings)?),
            tool(&settings.toolchain.zig, "zig"),
        )
    {
        return Ok(vec![
            zig.display().to_string(),
            "cc".into(),
            "-target".into(),
            format!("{}-linux-gnu.{v}", std::env::consts::ARCH),
        ]);
    }
    c_compiler(&settings.toolchain)
}

fn cc_name(cc: &[String]) -> String {
    let stem = |s: &String| {
        Path::new(s)
            .file_stem()
            .map_or_else(|| s.clone(), |f| f.to_string_lossy().into_owned())
    };
    cc.iter().map(stem).collect::<Vec<_>>().join(" ")
}

pub(crate) fn tool(setting: &Path, name: &str) -> Option<PathBuf> {
    if setting.as_os_str().is_empty() {
        relift_build::find_tool(name)
    } else {
        Some(setting.to_path_buf())
    }
}

/// The environment variable that turns a `relift build` or `relift run`
/// into the lifting child: it lifts and emits, writes the record of what it
/// emitted to the named file and exits. Not a setting (no `RELIFT_` prefix).
const EMIT_RECORD: &str = "_RELIFT_EMIT_RECORD";

/// The entry list the lifting child writes into the build directory
/// ([`entries_line`]).
pub(crate) const ENTRIES: &str = "entries.txt";

/// What the lifting child emitted, as the parent reads it back.
struct EmitRecord {
    modules: Vec<String>,
    lifted: Vec<PathBuf>,
    prog: Vec<PathBuf>,
    functions: usize,
    unlifted: usize,
    /// Whether every lifted file is built with FPSR flags.
    fp_program_wide: bool,
    /// The files built a second time with FPSR flags (`RL_PART=1`).
    fp_part1: Vec<PathBuf>,
    /// Whether any function produces FPSR flags.
    fp_flags: bool,
    /// Whether lifted code is built without FPCR guards (`RL_FPCR_CONST`).
    fpcr_const: bool,
    /// The C files of the cold copies, built for size rather than speed.
    cold: Vec<PathBuf>,
}

impl EmitRecord {
    fn to_text(&self) -> String {
        let mut t = format!("functions {}\nunlifted {}\n", self.functions, self.unlifted);
        for m in &self.modules {
            t.push_str(&format!("module {m}\n"));
        }
        for p in &self.lifted {
            t.push_str(&format!("lifted {}\n", p.display()));
        }
        for p in &self.prog {
            t.push_str(&format!("prog {}\n", p.display()));
        }
        if self.fp_program_wide {
            t.push_str("fp_program_wide 1\n");
        }
        if self.fp_flags {
            t.push_str("fp_flags 1\n");
        }
        if self.fpcr_const {
            t.push_str("fpcr_const 1\n");
        }
        for p in &self.fp_part1 {
            t.push_str(&format!("fp_part1 {}\n", p.display()));
        }
        for p in &self.cold {
            t.push_str(&format!("cold {}\n", p.display()));
        }
        t
    }

    fn parse(t: &str) -> Result<EmitRecord, BoxError> {
        let mut r = EmitRecord {
            modules: Vec::new(),
            lifted: Vec::new(),
            prog: Vec::new(),
            functions: 0,
            unlifted: 0,
            fp_program_wide: false,
            fp_part1: Vec::new(),
            fp_flags: false,
            fpcr_const: false,
            cold: Vec::new(),
        };
        for line in t.lines() {
            let (k, v) = line.split_once(' ').ok_or("a bad emit record line")?;
            match k {
                "functions" => r.functions = v.parse()?,
                "unlifted" => r.unlifted = v.parse()?,
                "module" => r.modules.push(v.into()),
                "lifted" => r.lifted.push(v.into()),
                "prog" => r.prog.push(v.into()),
                "fp_program_wide" => r.fp_program_wide = v == "1",
                "fp_part1" => r.fp_part1.push(v.into()),
                "fp_flags" => r.fp_flags = v == "1",
                "fpcr_const" => r.fpcr_const = v == "1",
                "cold" => r.cold.push(v.into()),
                _ => return Err(format!("unknown emit record key {k}").into()),
            }
        }
        Ok(r)
    }
}

/// The project's name and its host build directory.
pub(crate) fn project_out(settings: &Settings) -> Result<(String, PathBuf), BoxError> {
    let name = crate::paths::canonical(".")?
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "project".into());
    let root = cache_root(settings)?;
    std::fs::create_dir_all(&root)?;
    let out = crate::paths::canonical(root)?
        .join("projects")
        .join(&name)
        .join("build")
        .join("host");
    Ok((name, out))
}

/// Lifts and emits the project (the lifting child's whole job): the C of
/// every module, the unlifted list and the program files.
fn lift_and_emit(
    settings: &Settings,
    name: &str,
    out: &Path,
    mods: &[PathBuf],
    touchpad: bool,
    ui: &mut Ui,
) -> Result<EmitRecord, BoxError> {
    let import = import_dir(settings)?;
    let (mut loaded, npdm) = load_slots(&import)?;
    let closed = crate::closed::check(npdm.as_ref(), &[import.join("romfs"), import.join("dlc")])?;
    if let Some(n) = closed {
        ui.note(&Line::new().text(n));
    }
    let game = read_toml(Path::new("game.toml"))?;
    let title_id = npdm
        .as_ref()
        .map(|n| format!("{:016x}", n.program_id))
        .unwrap_or_default();
    let mut stage = Stage::new(
        mods,
        &game,
        Path::new("."),
        &title_id,
        &mut loaded,
        out.join("analysis"),
    )?;
    for n in &stage.notes {
        ui.note(&Line::new().text(n.clone()));
    }
    let placed = program::place(loaded.into_iter().map(|m| (m.name, m.nso)).collect());
    ui.title(&format!(
        "relift build: {name} ({} module{}, {}-{})",
        placed.len(),
        if placed.len() == 1 { "" } else { "s" },
        std::env::consts::OS,
        std::env::consts::ARCH
    ));
    let rows: Vec<usize> = placed.iter().map(|p| ui.row(&p.name)).collect();
    let cdir = out.join("c");
    let hints = crate::hints::read(Path::new("game.toml"))?;
    let mut game_settings = crate::game_settings::read(Path::new("game.toml"))?;
    game_settings.extend(stage.settings());
    crate::game_settings::check_dir(Path::new("overrides"), &game_settings)?;
    let emitted = emit_modules(settings, &placed, &cdir, ui, &rows, &hints, &mut stage)?;
    let mut emitted = emitted;
    let cpu = Arm64 {
        features: relift_switch::CPU_FEATURES,
    };
    let mods_c = stage.mods_c(cpu.abi_headers());
    if let Some(text) = &mods_c {
        let path = cdir.join("mods.c");
        relift_cache::write_if_changed(&path, text.as_bytes())?;
        emitted.files.push(path);
        emitted.files.extend(stage.files.iter().cloned());
    }
    relift_cache::write_if_changed(&out.join(ENTRIES), emitted.entries.as_bytes())?;
    relift_cache::write_if_changed(&out.join("total.txt"), emitted.total.as_bytes())?;
    let units: Vec<String> = if settings.emit.cold_first {
        emitted
            .cold_units
            .iter()
            .chain(&emitted.units)
            .cloned()
            .collect()
    } else {
        emitted
            .units
            .iter()
            .chain(&emitted.cold_units)
            .cloned()
            .collect()
    };
    let unlifted_file = out.join("unlifted.txt");
    relift_cache::write_if_changed(&unlifted_file, emitted.unlifted_list.as_bytes())?;
    let fpsr = emitted.fpsr;
    let fp_flags = fpsr.flags;
    for l in fpsr.report.lines().chain(emitted.fpcr.report.lines()) {
        ui.note(&Line::new().text(l));
    }
    let fpcr_const = emitted.fpcr.constant;
    let mut prog = Vec::new();
    let game = read_toml(Path::new("game.toml"))?;
    let mut ports = game_ports(Path::new("game.toml"))?;
    if touchpad && let Some(t) = relift_input::touchpad_port("switch") {
        ports.add_port(t)?;
    }
    let doc = crate::info::switch_document(
        &import,
        &game,
        &placed,
        name,
        game_settings.clone(),
        &ports,
        stage.info(),
    )?;
    let info_c = cdir.join(crate::info::C_FILE);
    relift_cache::write_if_changed(
        &info_c,
        (crate::info::c_file(&doc) + &crate::info::image_id_c(&image_id(&placed))).as_bytes(),
    )?;
    prog.push(info_c);
    prog.extend(crate::winres::write(
        &cdir,
        doc.identity.title(&[]).unwrap_or(name),
        doc.identity.icon.as_ref(),
    )?);
    let settings_c = cdir.join("game_settings.c");
    relift_cache::write_if_changed(
        &settings_c,
        crate::game_settings::generate_c(&game_settings).as_bytes(),
    )?;
    prog.push(settings_c);
    let ports_c = cdir.join("game_ports.c");
    relift_cache::write_if_changed(&ports_c, game_ports_c(Path::new("game.toml"))?.as_bytes())?;
    prog.push(ports_c);
    for f in program::program_files(
        &placed,
        npdm.as_ref(),
        &units,
        &emitted.code_maps,
        program::Fp {
            flags: fp_flags,
            fpcr_const,
        },
        mods_c.as_ref().map(|_| crate::mods::bundled::SYMBOL),
        true,
    ) {
        let path = cdir.join(&f.name);
        relift_cache::write_if_changed(&path, &f.bytes)?;
        if f.is_c() {
            prog.push(path);
        }
    }
    Ok(EmitRecord {
        modules: placed.iter().map(|p| p.name.clone()).collect(),
        lifted: emitted.files,
        prog,
        functions: emitted.functions,
        unlifted: emitted.unlifted,
        fp_program_wide: fpsr.program_wide,
        fp_part1: fpsr.part1,
        fp_flags: fp_flags || fpsr.program_wide,
        fpcr_const,
        cold: emitted.cold_files,
    })
}

/// Runs the lift and emit in a child relift (this executable, the same
/// arguments, [`EMIT_RECORD`] set), so the memory they use is gone before
/// the compilers start, and reads back what it emitted.
fn lift_in_child(out: &Path) -> Result<EmitRecord, BoxError> {
    std::fs::create_dir_all(out)?;
    let rec = out.join("emit.txt");
    let _ = std::fs::remove_file(&rec);
    let status = relift_proc::command(&std::env::current_exe()?)
        .args(std::env::args_os().skip(1))
        .env(EMIT_RECORD, &rec)
        .status()?;
    if !status.success() {
        return Err(format!("lifting failed ({status})").into());
    }
    EmitRecord::parse(&std::fs::read_to_string(&rec)?)
}

/// How the program is built: shipped, with the runtime linked in and no
/// code loaded at run time, or dev (`relift run`), with the runtime as a
/// shared library and the miss path that hot-loads code
/// (`-DRL_HOTLOAD`, docs/abi.md section 6).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Mode {
    Shipped,
    Dev,
}

/// The link flags that make a shared library on this host.
fn shlib_flags(name: &str) -> Vec<String> {
    if cfg!(windows) {
        vec!["-shared".into()]
    } else if cfg!(target_os = "macos") {
        vec![
            "-dynamiclib".into(),
            format!("-Wl,-install_name,@rpath/{name}"),
        ]
    } else {
        vec![
            "-shared".into(),
            "-Wl,--no-undefined".into(),
            format!("-Wl,-soname,{name}"),
        ]
    }
}

/// The link flag that finds the shared library in `dir` beside the program,
/// wherever the pair lives.
pub(crate) fn rpath_here(dir: &str) -> Option<String> {
    if cfg!(windows) {
        None
    } else if cfg!(target_os = "macos") {
        Some(format!("-Wl,-rpath,@loader_path/{dir}"))
    } else {
        Some(format!("-Wl,-rpath,$ORIGIN/{dir}"))
    }
}

/// The file name of the runtime library on this host.
pub(crate) fn runtime_lib_name() -> &'static str {
    if cfg!(windows) {
        "relift_rt.dll"
    } else if cfg!(target_os = "macos") {
        "librelift_rt.dylib"
    } else {
        "librelift_rt.so"
    }
}

/// How to compile and link code for this build, kept for the dev server.
#[derive(Clone, Debug)]
pub struct BuildTools {
    /// The C compiler command: clang, or `zig cc`.
    pub cc: Vec<String>,
    /// Ninja, when found.
    pub ninja: Option<PathBuf>,
    /// Compile jobs.
    pub jobs: u32,
    /// The flags of hot-loaded code: those of lifted code with FPSR flags
    /// when any function produces them, as position-independent code with
    /// hidden symbols.
    pub hot_flags: Vec<String>,
    /// The link flags of a hot-loaded library.
    pub hot_link: Vec<String>,
}

/// What one build produced.
pub(crate) struct Built {
    pub(crate) exe: PathBuf,
    pub(crate) out: PathBuf,
    pub(crate) toolchain: BuildTools,
    pub(crate) cross: Option<crate::cross::Build>,
}

/// Builds `plans` in order, each in its own directory, showing progress
/// on `row`; returns how many steps ran.
pub(crate) fn build_plans(
    ui: &mut Ui,
    row: usize,
    plans: &[(Plan, PathBuf)],
    ninja: Option<&Path>,
    jobs: u32,
    note: &str,
) -> Result<usize, BoxError> {
    let compile = Metrics {
        unit: "files",
        note: Some(note.to_string()),
        elapsed: true,
        ..Metrics::default()
    };
    ui.update(row, compile.clone());
    let verbose = ui.verbosity() == Verbosity::Verbose;
    let mut finished = 0;
    for (plan, dir) in plans {
        std::fs::create_dir_all(dir)?;
        let base = finished;
        relift_build::build_with(plan, dir, ninja, jobs, &mut |e| match e {
            Event::Tick => ui.tick(),
            Event::Step { done, total, what } => {
                finished = base + done;
                ui.update(
                    row,
                    Metrics {
                        done: done as u64,
                        total: Some(total as u64),
                        ..compile.clone()
                    },
                );
                if verbose {
                    ui.detail(&Line::new().text(format!("  {what}")));
                }
            }
        })?;
    }
    Ok(finished)
}

/// The runtime sources: the `runtime-dir` setting, or the ones this
/// binary was built beside.
pub(crate) fn runtime_dir(settings: &Settings) -> PathBuf {
    if settings.runtime_dir.as_os_str().is_empty() {
        PathBuf::from(RUNTIME_DIR)
    } else {
        settings.runtime_dir.clone()
    }
}

/// The runtime sources a build compiles, copied under the project's build
/// directory (`layers` of `src` into `dst`), so the compile commands name
/// files inside the cache and read the same wherever the cache or relift
/// lives. Only what changed is written, so a copy of another version of
/// the runtime rebuilds only what differs.
pub(crate) fn mirror_runtime(src: &Path, dst: &Path, layers: &[&str]) -> Result<PathBuf, BoxError> {
    for layer in layers {
        relift_cache::sync_tree(
            &src.join(layer),
            &dst.join(layer),
            &|rel| match rel.extension() {
                Some(e) => e == "c" || e == "h" || e == "S",
                None => !rel
                    .file_name()
                    .is_some_and(|n| n == "tests" || n == "devtests"),
            },
        )
        .map_err(|e| format!("{}: {e}", src.join(layer).display()))?;
    }
    Ok(dst.to_path_buf())
}

pub(crate) fn build_project(
    settings: &Settings,
    targets: &[String],
    mods: &[PathBuf],
    mode: Mode,
    ui: &mut Ui,
) -> Result<Built, BoxError> {
    let (name, out) = project_out(settings)?;
    let cross = match targets {
        [] => None,
        [t] => match crate::cross::target(t)? {
            None => None,
            Some(c) if mode == Mode::Shipped => {
                let mut b = crate::cross::Build::prepare(settings, c, &name)?;
                if let crate::cross::Build::Ios(i) = &mut b {
                    i.for_switch()?;
                }
                Some(b)
            }
            Some(_) => return Err(format!("relift run does not run on {t} yet").into()),
        },
        [t, ..] => return Err(format!("target {t}: only one target builds at a time").into()),
    };
    if let Some(rec) = std::env::var_os(EMIT_RECORD) {
        let r = lift_and_emit(settings, &name, &out, mods, cross.is_some(), ui)?;
        relift_cache::write_if_changed(Path::new(&rec), r.to_text().as_bytes())?;
        std::process::exit(0);
    }
    let emitted = lift_in_child(&out)?;
    // The child printed the module rows; registering them again keeps the
    // compile row's columns in line with them.
    for m in &emitted.modules {
        ui.row(m);
    }
    let compile_row = ui.row("");
    let cdir = out.join("c");
    let unlifted_file = out.join("unlifted.txt");
    let (lifted, prog) = (emitted.lifted, emitted.prog);
    let croot = crate::paths::canonical(cache_root(settings)?)?;
    let rt = mirror_runtime(
        &runtime_dir(settings),
        &out.join("runtime"),
        &["core", "cpus/arm64", "guests/switch", "host"],
    )?;
    let dev = mode == Mode::Dev;
    let hotload: Vec<String> = if dev {
        vec!["-DRL_HOTLOAD".into()]
    } else if cfg!(windows) {
        vec!["-DRL_STATIC_RUNTIME".into()]
    } else {
        Vec::new()
    };
    let inc = |p: &str| format!("-I{}", rt.join(p).display());
    let cc = match &cross {
        Some(c) => c.cc(),
        None if dev => c_compiler(&settings.toolchain)?,
        None => release_compiler(settings)?,
    };
    let arch: Vec<String> = if cross.is_some() {
        Vec::new()
    } else if cfg!(target_arch = "x86_64") {
        vec!["-mcx16".into()]
    } else {
        Vec::new()
    };
    let zig = cross.is_none() && cc.len() > 1;
    let base: Vec<String> = ["-std=c11", "-O2", "-g", "-ffp-contract=off"]
        .iter()
        .map(|s| s.to_string())
        .chain(arch)
        .chain(zig.then_some("-fno-sanitize=undefined".to_string()))
        .chain(
            cross
                .as_ref()
                .filter(|c| c.pic())
                .map(|_| "-fPIC".to_string()),
        )
        .collect();
    // Lifted code gets line tables only: full -g makes clang track every
    // local of functions with tens of thousands of them, which made one
    // large shard take minutes instead of seconds. Crash mapping needs only
    // the function maps and line tables (docs/abi.md section 11).
    let lifted_flags: Vec<String> = base
        .iter()
        .map(|f| {
            if f == "-g" {
                "-gline-tables-only".to_string()
            } else {
                f.clone()
            }
        })
        .chain(["-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer"].map(String::from))
        .chain([inc("core/include"), inc("cpus/arm64/include")])
        .chain(hotload.iter().cloned())
        .collect();
    // Code that produces FPSR flags gets them from host FP operations,
    // which the compiler must then neither fold nor reorder.
    let lifted_flags: Vec<String> = lifted_flags
        .into_iter()
        .chain(emitted.fpcr_const.then(|| "-DRL_FPCR_CONST".to_string()))
        .collect();
    let strict = ["-DRL_FP_FLAGS", "-ffp-exception-behavior=strict"].map(String::from);
    let flag_flags: Vec<String> = lifted_flags.iter().cloned().chain(strict).collect();
    let pic: Vec<String> = if cfg!(windows) && cross.is_none() {
        Vec::new()
    } else {
        ["-fPIC", "-fvisibility=hidden"].map(String::from).into()
    };
    let hot_flags: Vec<String> = if emitted.fp_flags {
        flag_flags.clone()
    } else {
        lifted_flags.clone()
    }
    .into_iter()
    .chain(pic.iter().cloned())
    .collect();
    let cold_flags: Vec<String> = if emitted.fp_flags {
        &flag_flags
    } else {
        &lifted_flags
    }
    .iter()
    .filter_map(|f| match f.as_str() {
        "-O2" => Some("-Os".into()),
        "-gline-tables-only" => None,
        _ => Some(f.clone()),
    })
    .chain(
        [
            "-g0",
            "-fno-asynchronous-unwind-tables",
            "-fno-unwind-tables",
            "-DRL_COLD_TEXT",
        ]
        .map(String::from),
    )
    .chain(pic.iter().cloned())
    .chain((COLD_FAR && cross.is_none()).then(|| "-mcmodel=large".to_string()))
    .collect();
    let lifted_flags = if emitted.fp_program_wide {
        flag_flags.clone()
    } else {
        lifted_flags
    };
    let extras = match &cross {
        Some(c) => c.extras(&rt, true)?,
        None => crate::runtime_link::extras(&rt, &croot)?,
    };
    ui.note(&Line::new().text(extras.note()));
    let rt_flags: Vec<String> = base
        .iter()
        .cloned()
        .chain(
            [
                "-fno-omit-frame-pointer",
                "-D_GNU_SOURCE",
                "-D_DARWIN_C_SOURCE",
                "-DRL_BUILDING_RUNTIME",
                "-DRL_CHECKS",
            ]
            .map(String::from),
        )
        .chain([
            inc("core/include"),
            inc("core"),
            inc("cpus/arm64/include"),
            inc("cpus/arm64"),
            inc("guests/switch"),
        ])
        .chain(extras.defines())
        .chain(hotload.iter().cloned())
        .collect();
    let lib_flags: Vec<String> = if dev || cross.as_ref().is_some_and(crate::cross::Build::pic) {
        rt_flags
            .iter()
            .cloned()
            .chain(pic.iter().cloned())
            .collect()
    } else {
        rt_flags.clone()
    };
    let bdir = match &cross {
        Some(c) => out.join(c.name()),
        None if dev => out.join("dev"),
        None => out.clone(),
    };
    let obj = |src: &Path, tag: &str| {
        let stem = src
            .strip_prefix(&cdir)
            .or_else(|_| src.strip_prefix(&rt))
            .unwrap_or(src);
        bdir.join("obj").join(tag).join(stem).with_extension("o")
    };
    let mut compiles: Vec<Compile> = lifted
        .iter()
        .map(|s| Compile {
            src: s.clone(),
            obj: obj(s, "lifted"),
            flags: lifted_flags.clone(),
            pool: None,
            stage: 0,
        })
        .collect();
    for s in &emitted.cold {
        compiles.push(Compile {
            src: s.clone(),
            obj: obj(s, "lifted"),
            flags: cold_flags.clone(),
            pool: None,
            stage: 0,
        });
    }
    for s in &emitted.fp_part1 {
        compiles.push(Compile {
            src: s.clone(),
            obj: obj(s, "lifted").with_extension("p1.o"),
            flags: flag_flags
                .iter()
                .cloned()
                .chain(["-DRL_PART=1".to_string()])
                .collect(),
            pool: None,
            stage: 0,
        });
    }
    let prog_flags: Vec<String> = rt_flags
        .iter()
        .filter(|f| !dev || *f != "-DRL_BUILDING_RUNTIME")
        .cloned()
        .collect();
    let (prog, resources) = crate::winres::split(&prog);
    for s in &prog {
        compiles.push(Compile {
            src: s.clone(),
            obj: obj(s, "lifted"),
            flags: prog_flags
                .iter()
                .cloned()
                .chain([build_dir_flag(&out)])
                .collect(),
            pool: None,
            stage: 0,
        });
    }
    let mut rt_compiles: Vec<Compile> = runtime_sources(&rt)?
        .into_iter()
        .map(|s| Compile {
            obj: obj(&s, "rt"),
            src: s,
            flags: lib_flags.clone(),
            pool: None,
            stage: 0,
        })
        .collect();
    rt_compiles.extend(extras.compiles(&rt, &lib_flags, &|s| obj(s, "rt")));
    let exe = match &cross {
        Some(c) => bdir.join(c.program()),
        None => bdir.join(format!("game{}", std::env::consts::EXE_SUFFIX)),
    };
    let ninja = tool(&settings.toolchain.ninja, "ninja");
    let jobs = relift_build::job_count(settings.jobs);
    let mut libs: Vec<String> = if let Some(c) = &cross {
        c.libs()
    } else if cfg!(windows) {
        ["-lpthread", "-lm", "-lws2_32"].map(String::from).into()
    } else {
        ["-lpthread", "-lm"].map(String::from).into()
    };
    if COLD_FAR && cross.is_none() && !emitted.cold.is_empty() {
        let script = out.join("cold.ld");
        relift_cache::write_if_changed(&script, COLD_SCRIPT.as_bytes())?;
        libs.push("-T".into());
        libs.push(script.display().to_string());
    }
    let rt_lib = if cfg!(windows) {
        bdir.join(runtime_lib_name())
    } else {
        bdir.join("rt").join(runtime_lib_name())
    };
    let mut plans = Vec::new();
    if dev {
        let pools = memory_pools(&mut rt_compiles, jobs);
        plans.push((
            Plan {
                cc: cc.clone(),
                compiles: rt_compiles,
                output: rt_lib.clone(),
                link_flags: shlib_flags(runtime_lib_name())
                    .into_iter()
                    .chain(libs.iter().cloned())
                    .chain(extras.libs())
                    .collect(),
                pools,
                root: Some(croot.clone()),
            },
            bdir.join("rt"),
        ));
        let pools = memory_pools(&mut compiles, jobs);
        plans.push((
            Plan {
                cc: cc.clone(),
                compiles,
                output: exe.clone(),
                link_flags: [rt_lib.display().to_string()]
                    .into_iter()
                    .chain(resources.iter().cloned())
                    .chain(rpath_here("rt"))
                    .chain(libs.iter().cloned())
                    .collect(),
                pools,
                root: Some(croot.clone()),
            },
            bdir.clone(),
        ));
    } else {
        compiles.extend(rt_compiles);
        let pools = memory_pools(&mut compiles, jobs);
        plans.push((
            Plan {
                cc: cc.clone(),
                compiles,
                output: exe.clone(),
                link_flags: resources
                    .iter()
                    .cloned()
                    .chain(libs.iter().cloned())
                    .chain(extras.libs())
                    .collect(),
                pools,
                root: Some(croot.clone()),
            },
            bdir.clone(),
        ));
    }
    let note = format!(
        "{} {}, -j{jobs}",
        cc_name(&cc),
        lifted_flags
            .iter()
            .find(|f| f.starts_with("-O"))
            .map_or("", String::as_str)
    );
    ui.begin(compile_row, "compile");
    let finished = build_plans(ui, compile_row, &plans, ninja.as_deref(), jobs, &note)?;
    ui.done(
        compile_row,
        if finished == 0 {
            Line::new().text(format!("up to date, {note}"))
        } else {
            Line::new().text(format!("{}, {note}", noun(finished as u64, "file")))
        },
    );
    summarize(
        ui,
        emitted.functions,
        emitted.unlifted,
        &unlifted_file,
        &exe,
    );
    Ok(Built {
        exe,
        out,
        cross,
        toolchain: BuildTools {
            cc,
            ninja,
            jobs,
            hot_flags,
            hot_link: shlib_flags("unit")
                .into_iter()
                .filter(|f| !f.contains("soname") && !f.contains("install_name"))
                .chain([rt_lib.display().to_string()])
                .collect(),
        },
    })
}

fn summarize(ui: &mut Ui, functions: usize, unlifted: usize, list: &Path, exe: &Path) {
    let paint = if unlifted > 0 {
        Paint::Yellow
    } else {
        Paint::Plain
    };
    ui.summary(
        &Line::new()
            .paint(Paint::Green, "built")
            .text(format!(" {}, ", noun(functions as u64, "function")))
            .paint(
                paint,
                format!("{} instructions not lifted", count(unlifted as u64)),
            )
            .text(", ")
            .text(duration(ui.elapsed()))
            .text(" in all"),
    );
    if unlifted > 0 {
        ui.summary(&Line::new().text(format!("  not lifted, with addresses: {}", list.display())));
    }
    ui.summary(&Line::new().text(format!("  output: {}", exe.display())));
}

/// `relift info`.
///
/// # Errors
///
/// No binary to read, or it holds no self-description.
pub fn run_info(settings: &Settings, binary: Option<&Path>) -> Result<(), BoxError> {
    let exes = match project_out(settings) {
        Ok((_, out)) => vec![out.join("game"), out.join("dev").join("game")],
        Err(_) => Vec::new(),
    };
    crate::info::run(binary, &exes)
}

/// `relift build`.
///
/// # Errors
///
/// No game.toml or import, a module that does not parse, or a failed
/// compile or link.
pub fn run_build(
    settings: &Settings,
    targets: &[String],
    mods: &[PathBuf],
    dev: bool,
    ui: &mut Ui,
) -> Result<(), BoxError> {
    if dev && !crate::gb::is_project() {
        return Err(
            "--dev is for a Game Boy project; `relift run` makes the dev build of others".into(),
        );
    }
    let handheld = if crate::gb::is_project() {
        Some("Game Boy")
    } else if crate::gba::is_project() {
        Some("Game Boy Advance")
    } else {
        None
    };
    if let Some(what) = handheld {
        if targets.len() > 1 {
            return Err(format!("one target at a time for a {what} build").into());
        }
        let target = match targets.first() {
            Some(t) => crate::cross::target(t)?,
            None => None,
        };
        let built = if crate::gb::is_project() {
            crate::gb::build(settings, mods, !dev, target, ui)?
        } else {
            crate::gba::build(settings, mods, true, target, ui)?
        };
        let (name, _) = project_out(settings)?;
        let request = PackageRequest {
            name: &name,
            label: &built.title,
            exe: &built.exe,
            import: None,
            android: match &built.cross {
                Some(crate::cross::Build::Android(a)) => Some(a.as_ref()),
                _ => None,
            },
            ios: match &built.cross {
                Some(crate::cross::Build::Ios(i)) => Some(i.as_ref()),
                _ => None,
            },
        };
        return package(settings, &request, ui);
    }
    let built = build_project(settings, targets, mods, Mode::Shipped, ui)?;
    let (name, _) = project_out(settings)?;
    let import = import_dir(settings)?;
    let request = PackageRequest {
        name: &name,
        label: "",
        exe: &built.exe,
        import: Some(&import),
        android: match &built.cross {
            Some(crate::cross::Build::Android(a)) => Some(a.as_ref()),
            _ => None,
        },
        ios: match &built.cross {
            Some(crate::cross::Build::Ios(i)) => Some(i.as_ref()),
            _ => None,
        },
    };
    package(settings, &request, ui)
}

/// What the packaging step of a release build works from.
struct PackageRequest<'a> {
    name: &'a str,
    /// The title the game goes by; empty to use `name`.
    label: &'a str,
    exe: &'a Path,
    /// The import directory of a Switch game, whose `romfs/` and `dlc/` go
    /// with the program.
    import: Option<&'a Path>,
    /// The Android build, when the target is one.
    android: Option<&'a crate::android::AndroidBuild>,
    /// The iOS build, when the target is one.
    ios: Option<&'a crate::ios::IosBuild>,
}

/// The release output of the target: an APK for Android, an app bundle for
/// iOS, a signed `.app` on macOS, on Linux an AppImage of the program.
fn package(settings: &Settings, req: &PackageRequest<'_>, ui: &mut Ui) -> Result<(), BoxError> {
    let label = if req.label.is_empty() {
        req.name
    } else {
        req.label
    };
    let data_manifest = match (req.import, req.ios.is_some() || req.android.is_some()) {
        (Some(import), true) => {
            let work = req.exe.parent().ok_or("the program has no folder")?;
            let e = crate::datamanifest::entries(import, work)?;
            (!e.is_empty()).then(|| crate::datamanifest::text(&e).into_bytes())
        }
        _ => None,
    };
    if let Some(i) = req.ios {
        let home = crate::ios::home(req.exe, req.name, label)?;
        let data = req.import.filter(|_| settings.package.ios_data == "bundle");
        let app = crate::ios::package(
            i,
            &crate::ios::Contents {
                exe: req.exe,
                name: req.name,
                home: &home,
                data_manifest: data_manifest.as_deref(),
                data,
            },
            ui,
        )?;
        ui.summary(&Line::new().text(format!("  app: {}", app.display())));
        if let crate::ios::Signing::Unsigned(why) = &i.signing {
            ui.summary(&Line::new().paint(
                Paint::Yellow,
                format!("  not signed: {why}; a device installs only a signed app"),
            ));
        }
        return Ok(());
    }
    if let Some(a) = req.android {
        let l = crate::android::launcher(req.exe, req.name, label)?;
        let apk = crate::android::package(a, req.exe, req.name, &l, data_manifest.as_deref(), ui)?;
        ui.summary(&Line::new().text(format!("  apk: {}", apk.display())));
        return Ok(());
    }
    let request = crate::appimage::Request {
        name: req.name,
        exe: req.exe,
        import: req.import,
    };
    if let Some(p) = crate::macapp::package(settings, &request, ui)? {
        ui.summary(&Line::new().text(format!("  app: {}", p.path.display())));
        ui.summary(&Line::new().text(format!(
            "  bundled: {}; {}",
            if p.bundled.is_empty() {
                "no libraries".to_string()
            } else {
                p.bundled.join(", ")
            },
            match (&p.signed_with, p.notarized) {
                (None, _) => "ad-hoc signature".to_string(),
                (Some(_), false) => "Developer ID signature".to_string(),
                (Some(_), true) => "Developer ID signature, notarized and stapled".to_string(),
            }
        )));
    }
    if let Some(p) = crate::appimage::package(settings, &request, ui)? {
        let libs: Vec<&str> = p.bundled.iter().map(|b| b.soname.as_str()).collect();
        ui.summary(&Line::new().text(format!("  appimage: {}", p.path.display())));
        ui.summary(&Line::new().text(format!(
            "  bundled: {}; needs glibc {} or newer",
            if libs.is_empty() {
                "no libraries".to_string()
            } else {
                libs.join(", ")
            },
            p.glibc.map_or("(none)".to_string(), |(a, b)| format!("{a}.{b}"))
        )));
    }
    Ok(())
}

/// `relift run`: a dev build, then the program in the working directory,
/// with the SD card in `sdmc/` there and the import's `romfs/` and `dlc/`
/// as its data storage and add-on content, and the dev server
/// ([`crate::dev`]) on `RELIFT_DEV_SOCKET`, which hot-loads the code the
/// program misses. Returns the program's exit code.
///
/// # Errors
///
/// As [`run_build`], or the program cannot start.
pub fn run_run(
    settings: &Settings,
    target: Option<&str>,
    trace_os: bool,
    ui: &mut Ui,
) -> Result<i32, BoxError> {
    if crate::gb::is_project() {
        if target.is_some_and(|t| t != "host") {
            return Err("only the host target runs so far".into());
        }
        return crate::gb::run(settings, ui);
    }
    if crate::gba::is_project() {
        if target.is_some_and(|t| t != "host") {
            return Err("only the host target runs so far".into());
        }
        return crate::gba::run(settings, ui);
    }
    let targets: Vec<String> = target.map(String::from).into_iter().collect();
    let built = build_project(settings, &targets, &[], Mode::Dev, ui)?;
    let import = import_dir(settings)?;
    let (mods, _) = load(&import)?;
    let placed = program::place(mods);
    let known = parse_entries(&std::fs::read_to_string(built.out.join(ENTRIES))?)?;
    let rt = if settings.runtime_dir.as_os_str().is_empty() {
        PathBuf::from(RUNTIME_DIR)
    } else {
        settings.runtime_dir.clone()
    };
    let abi = crate::dev::abi_version(&rt.join("core/include/relift/abi_core.h"))?;
    let hot = built.out.join("dev").join("hot");
    let _ = std::fs::remove_dir_all(&hot);
    let mut server = crate::dev::Server::new(
        placed,
        known,
        abi,
        built.toolchain.clone(),
        hot,
        crate::paths::canonical("game.toml")?,
    );
    let sock = crate::dev::socket_path(&built.out);
    let _ = std::fs::remove_file(&sock);
    let listener =
        crate::devsock::bind(&sock).map_err(|e| format!("dev socket {}: {e}", sock.display()))?;
    let mut cmd = relift_proc::command(&built.exe);
    cmd.env(crate::dev::SOCKET_ENV, &sock);
    let romfs = import.join("romfs");
    if romfs.is_dir() {
        cmd.env("RELIFT_ROMFS", &romfs);
    }
    let dlc = import.join("dlc");
    if dlc.is_dir() {
        cmd.env("RELIFT_DLC", &dlc);
    }
    if trace_os {
        cmd.env("RELIFT_TRACE_OS", "1");
    }
    let stop = std::sync::atomic::AtomicBool::new(false);
    let quiet = ui.verbosity() == Verbosity::Quiet;
    let (status, loads) = std::thread::scope(|sc| {
        let server = &mut server;
        let stop = &stop;
        let listener = &listener;
        let worker = sc.spawn(move || {
            crate::dev::accept_loop(
                listener,
                server,
                stop,
                &mut |l| {
                    if !quiet {
                        eprintln!(
                            "relift run: hot-loaded {:#x} ({}): {}, {} unlifted, lift {:.0} ms, compile {:.0} ms",
                            l.target,
                            l.hint.text(),
                            noun(l.functions as u64, "function"),
                            l.unlifted,
                            l.lift_ms,
                            l.compile_ms
                        );
                    }
                },
                &mut |e| eprintln!("relift run: dev server: {e}"),
            );
            server.loads.clone()
        });
        let status = cmd.status();
        stop.store(true, std::sync::atomic::Ordering::Release);
        let _ = crate::devsock::connect(&sock);
        let loads = worker.join().unwrap_or_default();
        (status, loads)
    });
    let _ = std::fs::remove_file(&sock);
    let status = status?;
    if !loads.is_empty() {
        ui.summary(&Line::new().text(format!(
            "hot-loaded {}; added to the hints in game.toml, so the next relift build lifts them",
            noun(loads.len() as u64, "target")
        )));
    }
    Ok(status.code().unwrap_or(128))
}

/// The C that declares the extra input ports of a game to the runtime: the
/// `[input.port.*]` tables of `game.toml` on top of the Switch's native ports.
fn game_ports(game_toml: &Path) -> Result<relift_input::PortSet, BoxError> {
    let mut set = relift_input::native_ports("switch")?;
    match std::fs::read_to_string(game_toml) {
        Ok(text) => set.extend_from_toml("game.toml", relift_input::Origin::Game, &text)?,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => return Err(format!("{}: {e}", game_toml.display()).into()),
    }
    Ok(set)
}

fn game_ports_c(game_toml: &Path) -> Result<String, BoxError> {
    Ok(relift_input::extras_c_file(&game_ports(game_toml)?))
}

#[cfg(test)]
mod ports_tests {
    use super::game_ports_c;

    #[test]
    fn a_missing_game_toml_declares_no_extras() {
        let c = game_ports_c(std::path::Path::new("/nonexistent/game.toml")).unwrap();
        assert!(c.contains("rl_game_ports_count = 0;"));
    }

    #[test]
    fn game_toml_extras_reach_the_c() {
        let dir = std::env::temp_dir().join(format!("relift-ports-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("game.toml");
        relift_cache::write_if_changed(
            &path,
            b"[input.port.dial]\nkind = \"Dial\"\n[input.port.dial.control.turn]\nkind = \"axis\"\n[input.port.p2.control.turbo]\n",
        )
        .unwrap();
        let c = game_ports_c(&path).unwrap();
        assert!(c.contains("{ \"dial\", \"Dial\", 1,"));
        assert!(c.contains("{ \"p2\", \"Switch Pro Controller\", 1,"));
        assert!(c.contains("\"turbo\""));
        assert!(!c.contains("\"lstick_x\""));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}

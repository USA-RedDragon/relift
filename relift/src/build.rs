//! `relift build` and `relift run`: lift, emit and compile the project's
//! modules for this host, then run the result.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use relift_analyze::{EntryKind, Insn, Outline, Progress, outline_visit};
use relift_build::{Compile, Event, Plan};
use relift_cpu_a64::frontend::A64;
use relift_guest::{Root, RootKind};
use relift_progress::{Line, Metrics, Paint, Verbosity, count, duration, noun};
use relift_switch::program::{self, Placed};
use relift_switch_formats::npdm::Npdm;
use relift_switch_formats::nso::Nso;

use crate::hints::Hint;
use crate::import::{BoxError, cache_root};
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
pub(crate) fn import_dir() -> Result<PathBuf, BoxError> {
    let game = read_toml(Path::new("game.toml"))?;
    let dir = game
        .get("game")
        .and_then(|g| g.get("import_dir"))
        .and_then(|d| d.as_str())
        .ok_or("game.toml has no game.import_dir; run relift import first")?;
    Ok(PathBuf::from(dir))
}

/// The modules of an import in load order, named, with its NPDM.
type Loaded = (Vec<(String, Nso)>, Option<Npdm>);

/// The modules of the import in load order, with the NPDM.
pub(crate) fn load(import: &Path) -> Result<Loaded, BoxError> {
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
        mods.push((slot_rank(field("slot")), field("name").to_string(), nso));
    }
    mods.sort_by_key(|m| m.0);
    let npdm = std::fs::read(import.join("exefs").join("main.npdm"))
        .ok()
        .and_then(|b| Npdm::parse(&b).ok());
    Ok((mods.into_iter().map(|(_, n, m)| (n, m)).collect(), npdm))
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
    /// The entries of every module, one `<module> <address> <kind>` line
    /// each ([`entries_line`]), for the dev server's later discovery.
    entries: String,
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

/// Lifts and emits `placed` into `cdir`, one directory per module; returns
/// the emitted C files and the unit symbols.
fn emit_modules(
    settings: &Settings,
    placed: &[Placed],
    cdir: &Path,
    ui: &mut Ui,
    rows: &[usize],
    image_id: [u8; 32],
    hints: &BTreeSet<Hint>,
) -> Result<Emitted, BoxError> {
    let mut done = Emitted {
        files: Vec::new(),
        units: Vec::new(),
        functions: 0,
        unlifted: 0,
        unlifted_list: String::new(),
        fpsr: crate::fpflags::Plan::default(),
        entries: String::new(),
    };
    let mut fpsr = crate::fpflags::Fpsr::new(placed);
    let cpu = A64 {
        features: relift_switch::CPU_FEATURES,
    };
    for (i, (p, &row)) in placed.iter().zip(rows).enumerate() {
        let module = module_with_hints(p, hints);
        ui.begin(row, "discover");
        let mut lifting = false;
        let mut found = 0;
        let mine = fpsr.slots_of(p);
        let visit = |f: &relift_analyze::Function| fpsr.visit(&module, &mine, f);
        let (layout, visits) = outline_visit(
            &cpu,
            &module,
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
        );
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
        let heads: Vec<relift_emit::Head> = all
            .iter()
            .map(|o| relift_emit::Head {
                entry: o.entry,
                name: program::host_name(p, o.name.as_deref(), o.entry),
                guest_name: o.name.clone(),
                span: o.span(),
                pads: o.pads.clone(),
            })
            .collect();
        let overrides: Vec<relift_emit::Override> = layout
            .functions
            .keys()
            .filter_map(|&a| {
                let k = program::known(module.bytes_at(a)?)?;
                Some(relift_emit::Override {
                    entry: a,
                    host_fn: k.host_fn.into(),
                    may_unwind: k.may_unwind,
                    resume_point: k.resume_point,
                })
            })
            .collect();
        let flags = fpsr.begin_module(i, &module, &layout, visits, &cpu, &overrides);
        let summaries = std::sync::Mutex::new(Vec::new());
        let fpsr_now = &fpsr;
        let lend = |ks: &[usize], sink: &mut dyn FnMut(usize, &relift_emit::Function<'_>)| {
            let group: Vec<&Outline> = ks.iter().map(|&k| all[k]).collect();
            let mut n = 0;
            relift_analyze::lift_each(&cpu, &module, &group, &mut |_, f| {
                let k = ks[n];
                n += 1;
                let e = relift_emit::Function {
                    entry: f.entry,
                    name: heads[k].name.clone(),
                    guest_name: f.name.clone(),
                    span: heads[k].span,
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
                };
                if let Some(s) = fpsr_now.function(&flags, &module, all[k], f) {
                    summaries
                        .lock()
                        .unwrap_or_else(std::sync::PoisonError::into_inner)
                        .push((k, s));
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
        let mut written: Result<(), BoxError> = Ok(());
        let undefined = relift_emit::emit_streamed(
            &cpu,
            &heads,
            &lend,
            &overrides,
            &relift_emit::Options {
                shard_bytes: kib(settings.emit.shard_kib),
                piece_bytes: kib(settings.emit.piece_kib),
                unit_name: p.name.clone(),
                unit_symbol: unit.clone(),
                module_base: p.base,
                parts,
                image_id,
                test_fault: test_fault(),
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
                if written.is_ok() {
                    written = relift_cache::write_if_changed(&dir.join(&f.name), f.text.as_bytes())
                        .map(|_| ())
                        .map_err(Into::into);
                }
                files.push(relift_emit::File {
                    text: String::new(),
                    ..f
                });
            },
        );
        written?;
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
                "{}, {}",
                noun(shards as u64, "shard"),
                noun(overrides.len() as u64, "override")
            )),
        );
        done.units.push(unit);
    }
    done.fpsr = fpsr.finish()?;
    Ok(done)
}

/// `n` KiB in bytes, at least one.
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
fn memory_pools(compiles: &mut [Compile], jobs: u32) -> Vec<(String, u32)> {
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
    for layer in ["core", "cpus/a64", "guests/switch"] {
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
fn c_compiler(t: &crate::settings::Toolchain) -> Result<Vec<String>, BoxError> {
    if let Some(clang) = tool(&t.clang, "clang") {
        return Ok(vec![clang.display().to_string()]);
    }
    let zig = tool(&t.zig, "zig").ok_or("no clang or zig on PATH")?;
    Ok(vec![zig.display().to_string(), "cc".into()])
}

fn cc_name(cc: &[String]) -> String {
    let stem = |s: &String| {
        Path::new(s)
            .file_stem()
            .map_or_else(|| s.clone(), |f| f.to_string_lossy().into_owned())
    };
    cc.iter().map(stem).collect::<Vec<_>>().join(" ")
}

fn tool(setting: &Path, name: &str) -> Option<PathBuf> {
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
        for p in &self.fp_part1 {
            t.push_str(&format!("fp_part1 {}\n", p.display()));
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
                _ => return Err(format!("unknown emit record key {k}").into()),
            }
        }
        Ok(r)
    }
}

/// The project's name and its host build directory.
fn project_out(settings: &Settings) -> Result<(String, PathBuf), BoxError> {
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
    ui: &mut Ui,
) -> Result<EmitRecord, BoxError> {
    let import = import_dir()?;
    let (mods, npdm) = load(&import)?;
    let placed = program::place(mods);
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
    let game_settings = crate::game_settings::read(Path::new("game.toml"))?;
    crate::game_settings::check_dir(Path::new("overrides"), &game_settings)?;
    let emitted = emit_modules(
        settings,
        &placed,
        &cdir,
        ui,
        &rows,
        image_id(&placed),
        &hints,
    )?;
    relift_cache::write_if_changed(&out.join(ENTRIES), emitted.entries.as_bytes())?;
    let unlifted_file = out.join("unlifted.txt");
    relift_cache::write_if_changed(&unlifted_file, emitted.unlifted_list.as_bytes())?;
    let fpsr = emitted.fpsr;
    let fp_flags = fpsr.flags;
    for l in fpsr.report.lines() {
        ui.note(&Line::new().text(l));
    }
    let mut prog = Vec::new();
    let game = read_toml(Path::new("game.toml"))?;
    let doc = crate::info::switch_document(&import, &game, &placed, name)?;
    let info_c = cdir.join(crate::info::C_FILE);
    relift_cache::write_if_changed(&info_c, crate::info::c_file(&doc).as_bytes())?;
    prog.push(info_c);
    let settings_c = cdir.join("game_settings.c");
    relift_cache::write_if_changed(
        &settings_c,
        crate::game_settings::generate_c(&game_settings).as_bytes(),
    )?;
    prog.push(settings_c);
    for f in program::program_files(&placed, npdm.as_ref(), &emitted.units, fp_flags) {
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
pub struct Toolchain {
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
    pub(crate) toolchain: Toolchain,
}

/// Builds `plans` in order, each in its own directory, showing progress
/// on `row`; returns how many steps ran.
fn build_plans(
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

pub(crate) fn build_project(
    settings: &Settings,
    targets: &[String],
    mode: Mode,
    ui: &mut Ui,
) -> Result<Built, BoxError> {
    if let Some(t) = targets.iter().find(|t| t.as_str() != "host") {
        return Err(format!("target {t}: only the host target builds so far").into());
    }
    let (name, out) = project_out(settings)?;
    if let Some(rec) = std::env::var_os(EMIT_RECORD) {
        let r = lift_and_emit(settings, &name, &out, ui)?;
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
    let rt = if settings.runtime_dir.as_os_str().is_empty() {
        PathBuf::from(RUNTIME_DIR)
    } else {
        settings.runtime_dir.clone()
    };
    let dev = mode == Mode::Dev;
    let hotload: Vec<String> = if dev {
        vec!["-DRL_HOTLOAD".into()]
    } else if cfg!(windows) {
        vec!["-DRL_STATIC_RUNTIME".into()]
    } else {
        Vec::new()
    };
    let inc = |p: &str| format!("-I{}", rt.join(p).display());
    let cc = c_compiler(&settings.toolchain)?;
    let arch: Vec<String> = if cfg!(target_arch = "x86_64") {
        vec!["-mcx16".into()]
    } else {
        Vec::new()
    };
    let zig = cc.len() > 1;
    let base: Vec<String> = ["-std=c11", "-O2", "-g", "-ffp-contract=off"]
        .iter()
        .map(|s| s.to_string())
        .chain(arch)
        .chain(zig.then_some("-fno-sanitize=undefined".to_string()))
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
        .chain([inc("core/include"), inc("cpus/a64/include")])
        .chain(hotload.iter().cloned())
        .collect();
    // Code that produces FPSR flags gets them from host FP operations,
    // which the compiler must then neither fold nor reorder.
    let strict = ["-DRL_FP_FLAGS", "-ffp-exception-behavior=strict"].map(String::from);
    let flag_flags: Vec<String> = lifted_flags.iter().cloned().chain(strict).collect();
    let pic: Vec<String> = if cfg!(windows) {
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
    let lifted_flags = if emitted.fp_program_wide {
        flag_flags.clone()
    } else {
        lifted_flags
    };
    let extras = crate::runtime_link::extras(&rt, &cache_root(settings)?)?;
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
            inc("cpus/a64/include"),
            inc("cpus/a64"),
            inc("guests/switch"),
        ])
        .chain(extras.defines())
        .chain(hotload.iter().cloned())
        .collect();
    let lib_flags: Vec<String> = if dev {
        rt_flags
            .iter()
            .cloned()
            .chain(pic.iter().cloned())
            .collect()
    } else {
        rt_flags.clone()
    };
    let bdir = if dev { out.join("dev") } else { out.clone() };
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
    for s in &prog {
        compiles.push(Compile {
            src: s.clone(),
            obj: obj(s, "lifted"),
            flags: prog_flags.clone(),
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
    let exe = bdir.join(format!("game{}", std::env::consts::EXE_SUFFIX));
    let ninja = tool(&settings.toolchain.ninja, "ninja");
    let jobs = relift_build::job_count(settings.jobs);
    let libs: Vec<String> = if cfg!(windows) {
        ["-lpthread", "-lm", "-lws2_32"].map(String::from).into()
    } else {
        ["-lpthread", "-lm"].map(String::from).into()
    };
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
                    .chain(
                        (!cfg!(windows))
                            .then(|| format!("-Wl,-rpath,{}", bdir.join("rt").display())),
                    )
                    .chain(libs.iter().cloned())
                    .collect(),
                pools,
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
                link_flags: libs.iter().cloned().chain(extras.libs()).collect(),
                pools,
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
        toolchain: Toolchain {
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
pub fn run_build(settings: &Settings, targets: &[String], ui: &mut Ui) -> Result<(), BoxError> {
    build_project(settings, targets, Mode::Shipped, ui).map(|_| ())
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
    let targets: Vec<String> = target.map(String::from).into_iter().collect();
    let built = build_project(settings, &targets, Mode::Dev, ui)?;
    let import = import_dir()?;
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

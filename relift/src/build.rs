//! `relift build` and `relift run`: lift, emit and compile the project's
//! modules for this host, then run the result.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_analyze::{Insn, Progress, discover_with};
use relift_build::{Compile, Event, Plan};
use relift_cpu_a64::frontend::A64;
use relift_progress::{Line, Metrics, Paint, Verbosity, count, duration, noun};
use relift_switch::program::{self, Placed};
use relift_switch_formats::npdm::Npdm;
use relift_switch_formats::nso::Nso;

use crate::import::{BoxError, cache_root};
use crate::settings::Settings;
use crate::ui::Ui;

/// The runtime sources this binary was built beside, used when the
/// `runtime-dir` setting is empty.
const RUNTIME_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../runtime");

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
fn import_dir() -> Result<PathBuf, BoxError> {
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
fn load(import: &Path) -> Result<Loaded, BoxError> {
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

/// What one build produced.
struct Built {
    exe: PathBuf,
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
    /// The `MRS FPSR` sites whose result the program can use, as
    /// (module, address); any at all turns on FPSR flag production.
    fpsr_reads: Vec<(String, u64)>,
}

fn unlifted_line(module: &str, pc: u64, word: u64, message: &str) -> String {
    format!("{module}\t{pc:#x}\t{word:08x}\t{message}\n")
}

/// Lifts and emits `placed` into `cdir`, one directory per module; returns
/// the emitted C files and the unit symbols.
fn emit_modules(
    placed: &[Placed],
    cdir: &Path,
    ui: &mut Ui,
    rows: &[usize],
) -> Result<Emitted, BoxError> {
    let mut done = Emitted {
        files: Vec::new(),
        units: Vec::new(),
        functions: 0,
        unlifted: 0,
        unlifted_list: String::new(),
        fpsr_reads: Vec::new(),
    };
    for (i, (p, &row)) in placed.iter().zip(rows).enumerate() {
        let module = program::module(p);
        ui.begin(row, "discover");
        let mut lifting = false;
        let mut found = 0;
        let analysis = discover_with(&A64, &module, &mut |event| match event {
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
        });
        if !lifting {
            ui.done(row, Line::new().text(noun(found as u64, "function")));
            ui.begin(row, "lift");
        }
        let mut failed = BTreeMap::new();
        for f in analysis.functions.values() {
            for (&pc, i) in &f.insts {
                if let Insn::Failed(e) = i {
                    failed.insert(pc, e);
                }
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
        done.unlifted += failed.len();
        done.functions += analysis.functions.len();
        ui.done(row, lift_summary(analysis.functions.len(), failed.len()));
        let raw = |pc: u64| {
            module.bytes_at(pc).and_then(|b| b.get(..4)).map_or(0, |w| {
                u64::from(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
            })
        };
        let raw32 = |pc: u64| raw(pc) as u32;
        let census: Vec<relift_cpu_a64::flags::Function<'_>> = analysis
            .functions
            .values()
            .map(|f| relift_cpu_a64::flags::Function {
                entry: f.entry,
                insts: f
                    .insts
                    .iter()
                    .map(|(&pc, i)| {
                        let b = match i {
                            Insn::Lifted(b) => Some(b),
                            Insn::Failed(_) => None,
                        };
                        (pc, b)
                    })
                    .collect(),
                raw: &raw32,
            })
            .collect();
        for a in relift_cpu_a64::flags::live_fpsr_reads(&census) {
            done.fpsr_reads.push((p.name.clone(), a));
        }
        let fns: Vec<relift_emit::Function<'_>> = analysis
            .functions
            .values()
            .map(|f| relift_emit::Function {
                entry: f.entry,
                name: program::host_name(p, f.name.as_deref(), f.entry),
                guest_name: f.name.clone(),
                span: f.span(4),
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
            })
            .collect();
        let overrides: Vec<relift_emit::Override> = analysis
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
        let unit = format!("rl_unit_{i}_{}", c_ident(&p.name));
        ui.begin(row, "emit");
        let mut shards = 0;
        let out = relift_emit::emit_with(
            &A64,
            &fns,
            &overrides,
            &relift_emit::Options {
                shard_size: 64,
                unit_name: p.name.clone(),
                unit_symbol: unit.clone(),
                module_base: p.base,
                parts: false,
            },
            &mut |d, total| {
                shards = total;
                ui.update(
                    row,
                    Metrics {
                        done: d as u64,
                        total: Some(total as u64),
                        unit: "shards",
                        ..Metrics::default()
                    },
                );
            },
        );
        let dir = cdir.join(format!("{i}_{}", c_ident(&p.name)));
        std::fs::create_dir_all(&dir)?;
        for f in &out.files {
            let path = dir.join(&f.name);
            relift_cache::write_if_changed(&path, f.text.as_bytes())?;
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
    Ok(done)
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

fn tool(setting: &Path, name: &str) -> Option<PathBuf> {
    if setting.as_os_str().is_empty() {
        relift_build::find_tool(name)
    } else {
        Some(setting.to_path_buf())
    }
}

fn build_project(settings: &Settings, targets: &[String], ui: &mut Ui) -> Result<Built, BoxError> {
    if let Some(t) = targets.iter().find(|t| t.as_str() != "host") {
        return Err(format!("target {t}: only the host target builds so far").into());
    }
    let import = import_dir()?;
    let (mods, npdm) = load(&import)?;
    let placed = program::place(mods);
    let name = std::fs::canonicalize(".")?
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "project".into());
    ui.title(&format!(
        "relift build: {name} ({} module{}, {}-{})",
        placed.len(),
        if placed.len() == 1 { "" } else { "s" },
        std::env::consts::OS,
        std::env::consts::ARCH
    ));
    let rows: Vec<usize> = placed.iter().map(|p| ui.row(&p.name)).collect();
    let compile_row = ui.row("");
    let root = cache_root(settings)?;
    std::fs::create_dir_all(&root)?;
    let out = std::fs::canonicalize(root)?
        .join("projects")
        .join(name)
        .join("build")
        .join("host");
    let cdir = out.join("c");
    let emitted = emit_modules(&placed, &cdir, ui, &rows)?;
    let (lifted, units) = (emitted.files, emitted.units);
    let unlifted_file = out.join("unlifted.txt");
    relift_cache::write_if_changed(&unlifted_file, emitted.unlifted_list.as_bytes())?;
    let fpsr_reads = emitted.fpsr_reads;
    let fp_flags = !fpsr_reads.is_empty();
    if let Some((m, a)) = fpsr_reads.first() {
        ui.note(&Line::new().text(format!(
            "FPSR flags on: {}, first in {m} at {a:#x}",
            noun(fpsr_reads.len() as u64, "usable FPSR read")
        )));
    }
    let prog = cdir.join("program.c");
    relift_cache::write_if_changed(
        &prog,
        program::program_c(&placed, npdm.as_ref(), &units, fp_flags).as_bytes(),
    )?;
    let rt = if settings.runtime_dir.as_os_str().is_empty() {
        PathBuf::from(RUNTIME_DIR)
    } else {
        settings.runtime_dir.clone()
    };
    let inc = |p: &str| format!("-I{}", rt.join(p).display());
    let arch: Vec<String> = if cfg!(target_arch = "x86_64") {
        vec!["-mcx16".into()]
    } else {
        Vec::new()
    };
    let base: Vec<String> = ["-std=c11", "-O2", "-g", "-ffp-contract=off"]
        .iter()
        .map(|s| s.to_string())
        .chain(arch)
        .collect();
    let lifted_flags: Vec<String> = base
        .iter()
        .cloned()
        .chain(["-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer"].map(String::from))
        .chain([inc("core/include"), inc("cpus/a64/include")])
        // A program that reads FPSR gets its flags from host FP operations,
        // which the compiler must then neither fold nor reorder.
        .chain(
            fp_flags
                .then_some(["-DRL_FP_FLAGS", "-ffp-exception-behavior=strict"])
                .into_iter()
                .flatten()
                .map(String::from),
        )
        .collect();
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
        .collect();
    let obj = |src: &Path, tag: &str| {
        let stem = src
            .strip_prefix(&cdir)
            .or_else(|_| src.strip_prefix(&rt))
            .unwrap_or(src);
        out.join("obj").join(tag).join(stem).with_extension("o")
    };
    let mut compiles: Vec<Compile> = lifted
        .iter()
        .map(|s| Compile {
            src: s.clone(),
            obj: obj(s, "lifted"),
            flags: lifted_flags.clone(),
        })
        .collect();
    compiles.push(Compile {
        src: prog.clone(),
        obj: obj(&prog, "lifted"),
        flags: rt_flags.clone(),
    });
    for s in runtime_sources(&rt)? {
        compiles.push(Compile {
            obj: obj(&s, "rt"),
            src: s,
            flags: rt_flags.clone(),
        });
    }
    let clang = tool(&settings.toolchain.clang, "clang").ok_or("no clang on PATH")?;
    let exe = out.join("game");
    let plan = Plan {
        cc: vec![clang.display().to_string()],
        compiles,
        output: exe.clone(),
        link_flags: vec!["-lpthread".into(), "-lm".into()],
    };
    let ninja = tool(&settings.toolchain.ninja, "ninja");
    let jobs = if ninja.is_some() {
        relift_build::job_count(settings.jobs)
    } else {
        1
    };
    let note = format!(
        "{} {}, -j{jobs}",
        clang
            .file_stem()
            .map_or_else(|| "clang".to_string(), |s| s.to_string_lossy().into_owned()),
        lifted_flags
            .iter()
            .find(|f| f.starts_with("-O"))
            .map_or("", String::as_str)
    );
    let compile = Metrics {
        unit: "files",
        note: Some(note.clone()),
        elapsed: true,
        ..Metrics::default()
    };
    ui.begin(compile_row, "compile");
    ui.update(compile_row, compile.clone());
    let mut finished = 0;
    let verbose = ui.verbosity() == Verbosity::Verbose;
    relift_build::build_with(
        &plan,
        &out,
        ninja.as_deref(),
        settings.jobs,
        &mut |e| match e {
            Event::Tick => ui.tick(),
            Event::Step { done, total, what } => {
                finished = done;
                ui.update(
                    compile_row,
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
        },
    )?;
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
    Ok(Built { exe })
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

/// `relift build`.
///
/// # Errors
///
/// No game.toml or import, a module that does not parse, or a failed
/// compile or link.
pub fn run_build(settings: &Settings, targets: &[String], ui: &mut Ui) -> Result<(), BoxError> {
    build_project(settings, targets, ui).map(|_| ())
}

/// `relift run`: build, then run the program in the working directory,
/// with the SD card in `sdmc/` there and the import's `romfs/` and `dlc/`
/// as its data storage and add-on content. Returns the program's exit code.
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
    let built = build_project(settings, &targets, ui)?;
    let mut cmd = relift_proc::command(&built.exe);
    let import = import_dir()?;
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
    let status = cmd.status()?;
    Ok(status.code().unwrap_or(128))
}

//! The Linux stage: `relift import` of a statically linked aarch64 Linux
//! executable, `relift build` of a native program from it, and `relift run`.
//!
//! The import copies the executable into the project's import directory and
//! writes `game.toml` with `guest = "linux"`. The build lifts the program's
//! code into one unit (`relift-linux`) and links it with the runtime's core,
//! A64 and Linux layers, which are the kernel the program runs on.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use relift_linux::{lift, program};
use relift_objfile::{Machine, elf};
use relift_progress::{Line, Paint, duration, noun};

use crate::cross;
use crate::gba::{Layers, compile_layers};
use crate::import::BoxError;
use crate::settings::Settings;
use crate::ui::Ui;

/// The executable's file name in the import directory.
const PROGRAM_FILE: &str = "program";

/// The runtime directories the program compiles, below the runtime root.
const LAYERS: [&str; 3] = ["core", "cpus/arm64", "guests/linux"];

/// The optimization of lifted code in a dev build (`relift run`); a release
/// build uses [`crate::build::RELEASE_LIFTED_OPT`].
const LIFTED_OPT: &str = "-O0";

const LINUX_LAYERS: Layers = Layers {
    dirs: &LAYERS,
    lifted_inc: &["core/include", "cpus/arm64/include", "guests/linux"],
    rt_inc: &[
        "core/include",
        "core",
        "cpus/arm64/include",
        "cpus/arm64",
        "guests/linux",
    ],
    lifted_opt: LIFTED_OPT,
    cache: "linux-runtime",
    cx16: true,
};

/// The largest file read to check whether it is an executable.
const MAX_IMAGE: u64 = 1 << 30;

/// Whether `path` is an aarch64 ELF executable or shared object.
#[must_use]
pub fn is_executable(path: &Path) -> bool {
    if !path.is_file() || std::fs::metadata(path).is_ok_and(|m| m.len() > MAX_IMAGE) {
        return false;
    }
    std::fs::read(path).is_ok_and(|b| {
        elf::is_elf(&b) && elf::parse(&b).is_ok_and(|o| o.machine == Machine::Aarch64)
    })
}

/// Whether the project's `game.toml` names this guest.
#[must_use]
pub fn is_project() -> bool {
    project_table()
        .and_then(|g| {
            g.get("guest")
                .and_then(|v| v.as_str())
                .map(|g| g == "linux")
        })
        .unwrap_or(false)
}

fn project_table() -> Option<toml::Table> {
    let t: toml::Table = std::fs::read_to_string("game.toml").ok()?.parse().ok()?;
    t.get("game")?.as_table().cloned()
}

fn quote(s: &str) -> String {
    toml::Value::String(s.into()).to_string()
}

/// `relift import` of the executable at `path` into the project in the
/// working directory.
///
/// # Errors
///
/// The file cannot be read or is not an executable this guest takes, or the
/// import directory or `game.toml` cannot be written.
pub fn import(settings: &Settings, path: &Path, ui: &mut Ui) -> Result<(), BoxError> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let object = elf::parse(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    program::place("program", object).map_err(|e| format!("{}: {e}", path.display()))?;
    let out = crate::import::import_dir(settings, Path::new("."))?;
    std::fs::create_dir_all(&out)?;
    relift_cache::write_if_changed(&out.join(PROGRAM_FILE), &bytes)?;
    let source = crate::paths::canonical(path)?;
    let sha: String = relift_switch_formats::sha256(&bytes)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    let title = path
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("program")
        .to_string();
    let text = format!(
        "# game.toml, written by relift import.\n\n[game]\nguest = \"linux\"\ntitle = {}\nimport_dir = {}\nprogram = {}\nprogram_sha256 = \"{sha}\"\n",
        quote(&title),
        quote(&out.display().to_string()),
        quote(&source.display().to_string()),
    );
    let manifest = Path::new("game.toml");
    let wrote = !manifest.exists() && relift_cache::write_if_changed(manifest, text.as_bytes())?;
    ui.summary(&Line::new().paint(Paint::Green, "imported").text(format!(
        " {title} ({} bytes, aarch64 Linux), into {}{}, {}",
        bytes.len(),
        out.display(),
        if wrote { "; wrote game.toml" } else { "" },
        duration(ui.elapsed())
    )));
    Ok(())
}

struct Project {
    bytes: Vec<u8>,
    title: String,
}

fn project() -> Result<Project, BoxError> {
    let g = project_table().ok_or("game.toml has no [game]")?;
    let s = |k: &str| g.get(k).and_then(|v| v.as_str()).map(String::from);
    let dir = PathBuf::from(s("import_dir").ok_or("game.toml has no game.import_dir")?);
    let path = dir.join(PROGRAM_FILE);
    let bytes = std::fs::read(&path)
        .map_err(|e| format!("{}: {e}; run relift import again", path.display()))?;
    Ok(Project {
        bytes,
        title: s("title").unwrap_or_else(|| "program".into()),
    })
}

/// What [`emit_cold`] made.
struct ColdMade {
    files: Vec<PathBuf>,
    unit: String,
    map: String,
}

/// Emits the cold copy of the program (`docs/abi.md` section 5.2): the
/// functions of the hot unit again, as pieces dispatch can enter at any
/// instruction, with every function the superset roots add for the words
/// discovery left out. The hot unit is untouched. Dispatch finds cold code
/// where an address is not a hot entry, so a target that only data names
/// (a return address Go computes from a table) runs lifted code too.
fn emit_cold(
    placed: &program::Placed,
    lifted: &lift::Lifted,
    cdir: &Path,
    ui: &mut Ui,
) -> Result<ColdMade, BoxError> {
    let row = ui.row("cold");
    ui.begin(row, "cold");
    let fe = lift::frontend();
    let granule = relift_guest::Frontend::granule(&fe);
    let module = &lifted.module;
    let hot_layout = relift_analyze::Layout::of(&lifted.analysis, granule);
    let sup = &lifted.superset;
    let mut hot: BTreeSet<u64> = lifted.inner.clone();
    for f in lifted.analysis.functions.values() {
        hot.insert(f.entry);
        hot.extend(f.pads.iter().copied());
    }
    let mut all: Vec<&relift_analyze::Outline> = hot_layout
        .functions
        .values()
        .filter(|o| {
            o.runs
                .iter()
                .any(|&(first, n)| (0..u64::from(n)).any(|k| !hot.contains(&(first + granule * k))))
        })
        .chain(sup.layout.functions.values())
        .collect();
    all.sort_by_key(|o| o.entry);
    let raw = |pc: u64| {
        module.bytes_at(pc).and_then(|b| b.get(..4)).map_or(0, |w| {
            u64::from(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
        })
    };
    let mut lifted_fns: Vec<relift_analyze::Function> = Vec::new();
    relift_analyze::lift_each(&fe, module, &all, &mut |_, f| lifted_fns.push(f.clone()));
    let fns: Vec<relift_emit::Function<'_>> = lifted_fns
        .iter()
        .map(|f| relift_emit::Function {
            entry: f.entry,
            name: format!("{}_cold", program::host_name(f.entry)),
            guest_name: f.name.clone(),
            span: f.span(granule),
            insts: f
                .insts
                .iter()
                .map(|(&pc, i)| {
                    let e = match i {
                        relift_analyze::Insn::Lifted(b) => relift_emit::Insn::Lifted {
                            block: b,
                            raw: raw(pc),
                        },
                        relift_analyze::Insn::Failed(e) => relift_emit::Insn::Failed(e),
                    };
                    (pc, e)
                })
                .collect(),
            tables: f.tables.clone(),
            pads: f.pads.clone(),
            guesses: BTreeMap::new(),
            entries: BTreeMap::new(),
        })
        .collect();
    let calls: Vec<relift_emit::Override> = lifted
        .analysis
        .functions
        .values()
        .map(|f| relift_emit::Override {
            entry: f.entry,
            host_fn: program::host_name(f.entry),
            may_unwind: true,
            resume_point: false,
            declare: true,
        })
        .collect();
    let unit = format!("{}_cold", lift::UNIT_SYMBOL);
    let output = relift_emit::emit(
        &fe,
        &fns,
        &[],
        &relift_emit::Options {
            unit_name: format!("{} (cold)", placed.name),
            unit_symbol: unit.clone(),
            module_base: placed.image.base,
            enter_anywhere: Some(u64::MAX),
            entry_ranges: true,
            piece_syms: false,
            calls,
            trampoline: lifted.trampoline,
            preempt_resume: true,
            count_insns: true,
            ..relift_emit::Options::default()
        },
    );
    let dir = cdir.join("cold");
    std::fs::create_dir_all(&dir)?;
    let mut files = Vec::new();
    for f in &output.files {
        let path = dir.join(&f.name);
        relift_cache::write_if_changed(&path, f.text.as_bytes())?;
        if f.name.ends_with(".c") {
            files.push(path);
        }
    }
    relift_cache::write_if_changed(
        &dir.join(relift_emit::PRELUDE_NAME),
        relift_emit::PRELUDE.as_bytes(),
    )?;
    let map = crate::total::code_map(&fe, module, &hot, &all)?;
    let sym = format!("rl_code_{}", lift::UNIT_SYMBOL);
    let map_c = dir.join("codemap.c");
    relift_cache::write_if_changed(
        &map_c,
        crate::total::code_map_c(
            relift_guest::Frontend::abi_headers(&fe),
            &map,
            &sym,
            lift::UNIT_SYMBOL,
            &unit,
            &placed.name,
        )
        .as_bytes(),
    )?;
    files.push(map_c);
    ui.done(
        row,
        Line::new().text(format!(
            "{} cold functions, {} superset from {}; {}",
            noun(all.len() as u64, "function"),
            sup.layout.functions.len(),
            noun(sup.roots as u64, "root"),
            map.census.line()
        )),
    );
    Ok(ColdMade {
        files,
        unit,
        map: sym,
    })
}

/// What a build made.
pub struct Built {
    /// The program.
    pub exe: PathBuf,
}

/// `relift build` of the project in the working directory, for this host or
/// for `target`; a `release` build for this host takes the release
/// compiler.
///
/// # Errors
///
/// No import, a file that does not parse, a target whose tools are missing,
/// or a failed compile or link.
pub fn build(
    settings: &Settings,
    release: bool,
    target: Option<cross::Target>,
    ui: &mut Ui,
) -> Result<Built, BoxError> {
    let p = project()?;
    let (name, host_out) = crate::build::project_out(settings)?;
    let cross = target
        .map(|t| cross::Build::prepare(settings, t, &name))
        .transpose()?;
    let out = match &cross {
        Some(c) => host_out.with_file_name(c.name()),
        None => host_out,
    };
    let cdir = out.join("c");
    std::fs::create_dir_all(&cdir)?;
    ui.title(&format!(
        "relift build: {name} ({}, {} KiB, {})",
        p.title,
        p.bytes.len() / 1024,
        cross.as_ref().map_or_else(
            || format!("{}-{}", std::env::consts::OS, std::env::consts::ARCH),
            |c| c.name().to_string()
        )
    ));
    let row = ui.row("program");
    ui.begin(row, "lift");
    let object = elf::parse(&p.bytes).map_err(|e| format!("{}: {e}", p.title))?;
    let placed = program::place(&p.title, object)?;
    let id = relift_switch_formats::sha256(&p.bytes);
    let lifted = lift::lift(&placed, id, settings.emit.trampoline, settings.emit.cold);
    let mut files = Vec::new();
    let udir = cdir.join("unit");
    std::fs::create_dir_all(&udir)?;
    for f in &lifted.output.files {
        let path = udir.join(&f.name);
        relift_cache::write_if_changed(&path, f.text.as_bytes())?;
        if f.name.ends_with(".c") {
            files.push(path);
        }
    }
    relift_cache::write_if_changed(
        &udir.join(relift_emit::PRELUDE_NAME),
        relift_emit::PRELUDE.as_bytes(),
    )?;
    ui.note(&Line::new().text(match &lifted.reason {
        Some(why) => format!("returns: trampoline mode ({why})"),
        None => {
            "returns: direct (every stack pointer write is relative to the entry value)".to_string()
        }
    }));
    let mut list = String::new();
    for u in &lifted.unlifted {
        list.push_str(&format!("{:08x}  {}\n", u.at, u.failure.message));
    }
    relift_cache::write_if_changed(&out.join("unlifted.txt"), list.as_bytes())?;
    ui.done(
        row,
        Line::new().text(format!(
            "{}, {}, {} not lifted",
            noun(lifted.functions as u64, "function"),
            noun(lifted.instructions as u64, "instruction"),
            lifted.unlifted.len()
        )),
    );
    let mut units = vec![lift::UNIT_SYMBOL.to_string()];
    let mut maps = Vec::new();
    if settings.emit.cold {
        let made = emit_cold(&placed, &lifted, &cdir, ui)?;
        files.extend(made.files);
        units.push(made.unit);
        maps.push(made.map);
    }
    let mut prog = Vec::new();
    for f in program::program_files(&placed, &units, id, lifted.trampoline, &maps) {
        let path = cdir.join(&f.name);
        relift_cache::write_if_changed(&path, &f.bytes)?;
        if f.name.ends_with(".c") {
            prog.push(path);
        }
    }
    compile_layers(
        settings,
        &out,
        &cdir,
        (&files, &prog),
        &LINUX_LAYERS,
        (release, cross.as_ref()),
        ui,
    )?;
    let exe = match &cross {
        Some(c) => out.join(c.program()),
        None => out.join(format!("game{}", std::env::consts::EXE_SUFFIX)),
    };
    ui.summary(&Line::new().paint(Paint::Green, "built").text(format!(
        " {} functions, {} instructions not lifted, {} in all",
        lifted.functions,
        lifted.unlifted.len(),
        duration(ui.elapsed())
    )));
    ui.summary(&Line::new().text(format!("  output: {}", exe.display())));
    Ok(Built { exe })
}

/// `relift run`: builds, then runs the program with `args`. Returns the
/// program's exit code.
///
/// # Errors
///
/// As [`build`], or the program cannot start.
pub fn run(settings: &Settings, args: &[String], ui: &mut Ui) -> Result<i32, BoxError> {
    let built = build(settings, false, None, ui)?;
    let status = relift_proc::command(&built.exe).args(args).status()?;
    Ok(status.code().unwrap_or(128))
}

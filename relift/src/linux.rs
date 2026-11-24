//! The Linux stage: `relift import` of a statically linked aarch64 Linux
//! executable, `relift build` of a native program from it, and `relift run`.
//!
//! The import copies the executable into the project's import directory and
//! writes `relift.toml` with `guest = "linux"`. A dynamically linked program
//! also brings its program interpreter and every library of its `DT_NEEDED`
//! closure, found in the sysroot the user names (`--sysroot`) the way the
//! interpreter searches, copied into the import directory and listed in
//! `modules.toml`, so a build needs the sysroot no more. The build lifts
//! every file's code into a unit of its own (`relift-linux`) and links them
//! with the runtime's core, A64 and Linux layers, which are the kernel the
//! program runs on.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use relift_linux::lift;
use relift_linux::program::{self, Input, Role};
use relift_linux::sysroot::{self, Sysroot};
use relift_objfile::{Machine, elf};
use relift_progress::{Line, Paint, duration, noun};

use crate::cross;
use crate::import::BoxError;
use crate::layers::{Layers, compile_layers};
use crate::settings::Settings;
use crate::ui::Ui;

/// The executable's file name in the import directory.
const PROGRAM_FILE: &str = "program";

/// The description of the program's other files in the import directory:
/// the guest path of the executable and, per file, its role, name, paths
/// and place in the directory.
const MODULES_FILE: &str = "modules.toml";

/// The directory of the import's copies of the interpreter and libraries.
const MODULES_DIR: &str = "modules";

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
    share_lifted: true,
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

/// Whether the project's `relift.toml` names this guest.
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
    let t: toml::Table = std::fs::read_to_string("relift.toml").ok()?.parse().ok()?;
    t.get("game")?.as_table().cloned()
}

fn quote(s: &str) -> String {
    toml::Value::String(s.into()).to_string()
}

/// The guest path of the executable: where the program was imported from.
fn guest_path(path: &Path) -> Result<String, BoxError> {
    let source = crate::paths::canonical(path)?;
    Ok(source.to_string_lossy().replace('\\', "/"))
}

fn sha_hex(bytes: &[u8]) -> String {
    relift_switch_formats::sha256(bytes)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// `relift import` of the executable at `path` into the project in the
/// working directory. A dynamically linked executable needs `sysroot`, a
/// directory that is the guest's root file system (or a toolchain's
/// sysroot), to find its interpreter and libraries.
///
/// # Errors
///
/// The file cannot be read or is not an executable this guest takes, a
/// dynamically linked one has no sysroot or a file it needs is not in it, or
/// the import directory or `relift.toml` cannot be written.
pub fn import(
    settings: &Settings,
    path: &Path,
    sysroot_dir: Option<&Path>,
    ui: &mut Ui,
) -> Result<(), BoxError> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let object = elf::parse(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    let exec = guest_path(path)?;
    let dynamic = object.interpreter.is_some()
        || object
            .dynamic
            .as_ref()
            .is_some_and(|d| !d.needed.is_empty());
    let closure = if dynamic {
        let dir = sysroot_dir.ok_or_else(|| {
            format!(
                "{}: a dynamically linked program needs its interpreter and libraries: pass --sysroot DIR, an arm64 Linux root file system or a toolchain's sysroot",
                path.display()
            )
        })?;
        let origin = sysroot::normalize(&exec);
        let origin = origin
            .rsplit_once('/')
            .map_or("/", |(d, _)| if d.is_empty() { "/" } else { d });
        let root =
            Sysroot::new(dir)?.with_overlay(origin, path.parent().unwrap_or(Path::new(".")))?;
        sysroot::closure(&object, &bytes, origin, &root)
            .map_err(|e| format!("{}: {e}", path.display()))?
    } else {
        sysroot::Closure::default()
    };
    let mut inputs = vec![Input {
        name: "program".into(),
        role: Role::Main,
        paths: Vec::new(),
        bytes: bytes.clone(),
    }];
    inputs.extend(closure.interpreter.iter().map(|l| Input {
        name: l.name.clone(),
        role: Role::Interpreter,
        paths: l.paths.clone(),
        bytes: l.bytes.clone(),
    }));
    inputs.extend(closure.libraries.iter().map(|l| Input {
        name: l.name.clone(),
        role: Role::Library,
        paths: l.paths.clone(),
        bytes: l.bytes.clone(),
    }));
    program::place(inputs, Vec::new()).map_err(|e| format!("{}: {e}", path.display()))?;
    let out = crate::import::import_dir(settings, Path::new("."))?;
    std::fs::create_dir_all(&out)?;
    relift_cache::write_if_changed(&out.join(PROGRAM_FILE), &bytes)?;
    let mods = out.join(MODULES_DIR);
    if mods.exists() {
        std::fs::remove_dir_all(&mods)?;
    }
    let mut manifest = format!("exec_path = {}\n", quote(&exec));
    let files = closure
        .interpreter
        .iter()
        .map(|l| ("interpreter", l))
        .chain(closure.libraries.iter().map(|l| ("library", l)))
        .map(|(role, l)| (role, l.name.clone(), &l.paths, &l.bytes))
        .chain(closure.data.iter().map(|d| {
            let name = d.paths.first().map_or("data", |p| p.as_str());
            ("data", name.to_string(), &d.paths, &d.bytes)
        }));
    let mut count = 0;
    let mut size = 0;
    for (i, (role, name, paths, bytes)) in files.enumerate() {
        std::fs::create_dir_all(&mods)?;
        let file = format!("{i:03}-{}", name.replace('/', "_"));
        relift_cache::write_if_changed(&mods.join(&file), bytes)?;
        let paths: Vec<String> = paths.iter().map(|p| quote(p)).collect();
        manifest.push_str(&format!(
            "\n[[module]]\nrole = \"{role}\"\nname = {}\nfile = {}\nsha256 = \"{}\"\npaths = [{}]\n",
            quote(&name),
            quote(&file),
            sha_hex(bytes),
            paths.join(", ")
        ));
        count += 1;
        size += bytes.len();
    }
    relift_cache::write_if_changed(&out.join(MODULES_FILE), manifest.as_bytes())?;
    let sha = sha_hex(&bytes);
    let title = path
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("program")
        .to_string();
    let entry = crate::kind::desktop_entry(path, sysroot_dir);
    let kind = match &entry {
        Some((file, game)) => {
            format!(
                "# From the desktop entry {}.\ngame = {game}\n",
                file.display()
            )
        }
        None => String::new(),
    };
    let text = format!(
        "# The project file, written by relift import.\n\n[game]\nguest = \"linux\"\ntitle = {}\nimport_dir = {}\nprogram = {}\nprogram_sha256 = \"{sha}\"\n{kind}",
        quote(&title),
        quote(&out.display().to_string()),
        quote(&exec),
    );
    let wrote = crate::projectfile::write_stub(Path::new(crate::projectfile::FILE), &text)?;
    ui.summary(&Line::new().paint(Paint::Green, "imported").text(format!(
        " {title} ({} bytes, aarch64 Linux{}), into {}{}, {}",
        bytes.len(),
        if count > 0 {
            format!(
                ", with {} and {} KiB of libraries",
                noun(count, "file"),
                size / 1024
            )
        } else {
            String::new()
        },
        out.display(),
        if wrote { "; wrote relift.toml" } else { "" },
        duration(ui.elapsed())
    )));
    ui.summary(&Line::new().text(match &entry {
        Some((file, true)) => format!("  kind: game (the desktop entry {} lists Game)", file.display()),
        Some((file, false)) => format!("  kind: application (the desktop entry {} does not list Game)", file.display()),
        None => "  kind: application (no desktop entry found; set game = true in [game] of relift.toml for a game)".to_string(),
    }));
    Ok(())
}

struct Project {
    inputs: Vec<Input>,
    data: Vec<program::Data>,
    exec_path: String,
    title: String,
}

fn project() -> Result<Project, BoxError> {
    let g = project_table().ok_or("relift.toml has no [game]")?;
    let s = |k: &str| g.get(k).and_then(|v| v.as_str()).map(String::from);
    let dir = PathBuf::from(s("import_dir").ok_or("relift.toml has no game.import_dir")?);
    let path = dir.join(PROGRAM_FILE);
    let bytes = std::fs::read(&path)
        .map_err(|e| format!("{}: {e}; run relift import again", path.display()))?;
    let title = s("title").unwrap_or_else(|| "program".into());
    let mut inputs = vec![Input {
        name: title.clone(),
        role: Role::Main,
        paths: Vec::new(),
        bytes,
    }];
    let mut data = Vec::new();
    let mut exec_path = s("program").unwrap_or_else(|| "/program".into());
    let list = dir.join(MODULES_FILE);
    if list.exists() {
        let text = std::fs::read_to_string(&list)?;
        let t: toml::Table = text
            .parse()
            .map_err(|e| format!("{}: {e}", list.display()))?;
        if let Some(p) = t.get("exec_path").and_then(|v| v.as_str()) {
            exec_path = p.into();
        }
        for m in t
            .get("module")
            .and_then(|v| v.as_array())
            .map(Vec::as_slice)
            .unwrap_or_default()
        {
            let field = |k: &str| {
                m.get(k)
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| format!("{}: a module has no {k}", list.display()))
            };
            let file = dir.join(MODULES_DIR).join(field("file")?);
            let bytes = std::fs::read(&file)
                .map_err(|e| format!("{}: {e}; run relift import again", file.display()))?;
            let want = field("sha256")?;
            if sha_hex(&bytes) != want {
                return Err(format!(
                    "{} changed since the import; run relift import again",
                    file.display()
                )
                .into());
            }
            let paths: Vec<String> = m
                .get("paths")
                .and_then(|v| v.as_array())
                .map(|a| {
                    a.iter()
                        .filter_map(|p| p.as_str().map(String::from))
                        .collect()
                })
                .unwrap_or_default();
            match field("role")? {
                "data" => data.push(program::Data { paths, bytes }),
                role => inputs.push(Input {
                    name: field("name")?.into(),
                    role: if role == "interpreter" {
                        Role::Interpreter
                    } else {
                        Role::Library
                    },
                    paths,
                    bytes,
                }),
            }
        }
    }
    Ok(Project {
        inputs,
        data,
        exec_path,
        title,
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
    index: usize,
    lifted: &lift::Lifted,
    cdir: &Path,
    ui: &mut Ui,
) -> Result<ColdMade, BoxError> {
    let row = ui.row(&format!("{} cold", placed.name));
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
    let unit = format!("{}_cold", lift::unit_symbol(index));
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
    let dir = cdir.join("cold").join(index.to_string());
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
    let sym = format!("rl_code_{}", lift::unit_symbol(index));
    let map_c = dir.join("codemap.c");
    relift_cache::write_if_changed(
        &map_c,
        crate::total::code_map_c(
            relift_guest::Frontend::abi_headers(&fe),
            &map,
            &sym,
            &lift::unit_symbol(index),
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
        p.inputs.iter().map(|i| i.bytes.len()).sum::<usize>() / 1024,
        cross.as_ref().map_or_else(
            || format!("{}-{}", std::env::consts::OS, std::env::consts::ARCH),
            |c| c.name().to_string()
        )
    ));
    let program = program::place(p.inputs, p.data).map_err(|e| format!("{}: {e}", p.title))?;
    let placed = &program.modules;
    let mut all = Vec::new();
    for m in placed {
        all.extend_from_slice(m.name.as_bytes());
        all.push(0);
        all.extend_from_slice(&relift_switch_formats::sha256(&m.file));
    }
    for d in &program.data {
        all.extend_from_slice(&relift_switch_formats::sha256(&d.bytes));
    }
    let id = relift_switch_formats::sha256(&all);
    let rows: Vec<usize> = placed.iter().map(|m| ui.row(&m.name)).collect();
    for &row in &rows {
        ui.begin(row, "lift");
    }
    let results = lift::lift_program(placed, id, settings.emit.trampoline, settings.emit.cold);
    let mut files = Vec::new();
    let mut units = Vec::new();
    let mut maps = Vec::new();
    let mut list = String::new();
    let (mut functions, mut instructions, mut not_lifted) = (0, 0, 0);
    for (i, lifted) in results.iter().enumerate() {
        let udir = cdir.join("unit").join(i.to_string());
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
        for u in &lifted.unlifted {
            list.push_str(&format!(
                "{}\t{:08x}  {}\n",
                placed[i].name, u.at, u.failure.message
            ));
        }
        units.push(lift::unit_symbol(i));
        if settings.emit.cold {
            let made = emit_cold(&placed[i], i, lifted, &cdir, ui)?;
            files.extend(made.files);
            units.push(made.unit);
            maps.push(made.map);
        }
        functions += lifted.functions;
        instructions += lifted.instructions;
        not_lifted += lifted.unlifted.len();
        ui.done(
            rows[i],
            Line::new().text(format!(
                "{}, {}, {} not lifted",
                noun(lifted.functions as u64, "function"),
                noun(lifted.instructions as u64, "instruction"),
                lifted.unlifted.len()
            )),
        );
    }
    relift_cache::write_if_changed(&out.join("unlifted.txt"), list.as_bytes())?;
    ui.note(
        &Line::new().text(match results.first().and_then(|l| l.reason.as_ref()) {
            Some(why) => format!("returns: trampoline mode ({why})"),
            None => "returns: direct (every stack pointer write is relative to the entry value)"
                .to_string(),
        }),
    );
    let mut prog = Vec::new();
    for f in program::program_files(
        &program,
        &p.exec_path,
        (&units, &maps),
        id,
        results.first().is_some_and(|l| l.trampoline),
    ) {
        let path = cdir.join(&f.name);
        relift_cache::write_if_changed(&path, &f.bytes)?;
        if f.name.ends_with(".c") {
            prog.push(path);
        }
    }
    let mut doc = crate::info::linux_document(&p.title);
    crate::kind::stamp(&mut doc)?;
    let info_c = cdir.join(crate::info::C_FILE);
    relift_cache::write_if_changed(&info_c, crate::info::c_file(&doc).as_bytes())?;
    prog.push(info_c);
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
        Some(c) => out.join(c.program(&crate::progname::stem())),
        None => out.join(crate::progname::program()),
    };
    ui.summary(&Line::new().paint(Paint::Green, "built").text(format!(
        " {functions} functions, {instructions} instructions lifted, {not_lifted} not lifted, {} in all",
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

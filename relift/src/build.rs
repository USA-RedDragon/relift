//! `relift build` and `relift run`: lift, emit and compile the project's
//! modules for this host, then run the result.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use relift_analyze::{Insn, discover};
use relift_build::{Compile, Plan};
use relift_cpu_a64::frontend::A64;
use relift_switch::program::{self, Placed};
use relift_switch_formats::npdm::Npdm;
use relift_switch_formats::nso::Nso;

use crate::import::{BoxError, cache_root};
use crate::settings::Settings;

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

/// Lifts and emits `placed` into `cdir`, one directory per module; returns
/// the emitted C files and the unit symbols.
fn emit_modules(placed: &[Placed], cdir: &Path) -> Result<(Vec<PathBuf>, Vec<String>), BoxError> {
    let mut files = Vec::new();
    let mut units = Vec::new();
    for (i, p) in placed.iter().enumerate() {
        let module = program::module(p);
        let analysis = discover(&A64, &module);
        let raw = |pc: u64| {
            module.bytes_at(pc).and_then(|b| b.get(..4)).map_or(0, |w| {
                u64::from(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
            })
        };
        let fns: Vec<relift_emit::Function<'_>> = analysis
            .functions
            .values()
            .map(|f| relift_emit::Function {
                entry: f.entry,
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
        let unit = format!("rl_unit_{i}_{}", c_ident(&p.name));
        let out = relift_emit::emit(
            &A64,
            &fns,
            &relift_emit::Options {
                shard_size: 64,
                unit_name: p.name.clone(),
                unit_symbol: unit.clone(),
            },
        );
        let dir = cdir.join(format!("{i}_{}", c_ident(&p.name)));
        std::fs::create_dir_all(&dir)?;
        for f in &out.files {
            let path = dir.join(&f.name);
            relift_cache::write_if_changed(&path, f.text.as_bytes())?;
            if f.name.ends_with(".c") {
                files.push(path);
            }
        }
        eprintln!(
            "relift build: {} at {:#x}: {} functions, {} instructions do not lift",
            p.name,
            p.base,
            analysis.functions.len(),
            out.undefined
        );
        units.push(unit);
    }
    Ok((files, units))
}

fn runtime_sources(rt: &Path) -> Result<Vec<PathBuf>, BoxError> {
    let mut out = Vec::new();
    for layer in ["core", "cpus/a64", "guests/switch"] {
        let dir = rt.join(layer);
        let mut v: Vec<PathBuf> = std::fs::read_dir(&dir)
            .map_err(|e| format!("{}: {e}", dir.display()))?
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| p.extension().is_some_and(|x| x == "c"))
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

fn build_project(settings: &Settings, targets: &[String]) -> Result<Built, BoxError> {
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
    let root = cache_root(settings)?;
    std::fs::create_dir_all(&root)?;
    let out = std::fs::canonicalize(root)?
        .join("projects")
        .join(name)
        .join("build")
        .join("host");
    let cdir = out.join("c");
    let (lifted, units) = emit_modules(&placed, &cdir)?;
    let prog = cdir.join("program.c");
    relift_cache::write_if_changed(
        &prog,
        program::program_c(&placed, npdm.as_ref(), &units).as_bytes(),
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
        .chain([inc("core/include"), inc("cpus/a64/include")])
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
    relift_build::build(&plan, &out, ninja.as_deref(), settings.jobs)?;
    eprintln!("relift build: {}", exe.display());
    Ok(Built { exe })
}

/// `relift build`.
///
/// # Errors
///
/// No game.toml or import, a module that does not parse, or a failed
/// compile or link.
pub fn run_build(settings: &Settings, targets: &[String]) -> Result<(), BoxError> {
    build_project(settings, targets).map(|_| ())
}

/// `relift run`: build, then run the program in the working directory,
/// with the SD card in `sdmc/` there. Returns the program's exit code.
///
/// # Errors
///
/// As [`run_build`], or the program cannot start.
pub fn run_run(settings: &Settings, target: Option<&str>, trace_os: bool) -> Result<i32, BoxError> {
    let targets: Vec<String> = target.map(String::from).into_iter().collect();
    let built = build_project(settings, &targets)?;
    let mut cmd = relift_proc::command(&built.exe);
    if trace_os {
        cmd.env("RELIFT_TRACE_OS", "1");
    }
    let status = cmd.status()?;
    Ok(status.code().unwrap_or(128))
}

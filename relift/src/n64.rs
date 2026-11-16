//! The Nintendo 64 stage: `relift import` of a cartridge ROM, `relift
//! build` of a native program from it, and `relift run`.
//!
//! The import normalizes the ROM to big-endian, copies it into the
//! project's import directory and writes `game.toml` with `guest = "n64"`.
//! The build finds the code (`relift-n64-analyze`), lifts each segment into
//! one unit (`relift-n64`), and links them with the runtime's core, VR4300
//! and N64 layers. The program binds each unit when it finds its bytes in
//! RDRAM. `relift run` opens the program's window.

use std::path::{Path, PathBuf};

use relift_build::{Compile, Plan};
use relift_n64_formats::{ByteOrder, Rom};
use relift_progress::{Line, Paint, duration, noun};

use crate::import::{BoxError, cache_root};
use crate::settings::Settings;
use crate::ui::Ui;

/// The optimization of lifted code: the bodies are small next to a run's
/// device and dispatch work, and -O0 keeps the build of a large ROM short.
const LIFTED_OPT: &str = "-O0";

/// The ROM's file name in the import directory (big-endian).
const ROM_FILE: &str = "rom.z64";

/// The runtime directories the program compiles, below the runtime root.
const LAYERS: [&str; 5] = [
    "core",
    "cpus/mips",
    "cpus/rsp",
    "guests/n64",
    "guests/n64/run",
];

/// Whether `path` is a Nintendo 64 cartridge image in one of the three byte
/// orders.
#[must_use]
pub fn is_rom(path: &Path) -> bool {
    if !path.is_file() {
        return false;
    }
    std::fs::read(path).is_ok_and(|rom| is_rom_image(&rom))
}

/// Whether `rom` is a cartridge image: a known byte order magic and a whole
/// header.
#[must_use]
pub fn is_rom_image(rom: &[u8]) -> bool {
    Rom::new(rom).is_ok()
}

/// Whether the project's `game.toml` names the Nintendo 64 guest.
#[must_use]
pub fn is_project() -> bool {
    std::fs::read_to_string("game.toml")
        .ok()
        .and_then(|t| t.parse::<toml::Table>().ok())
        .and_then(|t| {
            t.get("game")
                .and_then(|g| g.get("guest"))
                .and_then(|g| g.as_str())
                .map(|g| g == "n64")
        })
        .unwrap_or(false)
}

fn quote(s: &str) -> String {
    toml::Value::String(s.into()).to_string()
}

fn title_of(rom: &Rom) -> String {
    rom.header().name_text().trim().to_string()
}

/// `relift import` of the ROM at `path` into the project in the working
/// directory.
///
/// # Errors
///
/// The ROM cannot be read or is no N64 image, or the import directory or
/// `game.toml` cannot be written.
pub fn import(settings: &Settings, path: &Path, ui: &mut Ui) -> Result<(), BoxError> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let rom = Rom::new(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    let out = crate::import::import_dir(settings, Path::new("."))?;
    std::fs::create_dir_all(&out)?;
    relift_cache::write_if_changed(&out.join(ROM_FILE), &rom.bytes)?;
    let source = crate::paths::canonical(path)?;
    let sha: String = relift_switch_formats::sha256(&rom.bytes)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    let h = rom.header();
    let title = title_of(&rom);
    let game_id: String = h
        .game_id
        .iter()
        .map(|b| relift_n64_formats::Header::printable(*b))
        .collect::<String>()
        .trim()
        .to_string();
    let cic = rom.cic().map_or("unknown", |c| c.name());
    let text = format!(
        "# game.toml, written by relift import.\n\n[game]\nguest = \"n64\"\ntitle = {}\ngame_id = {}\nversion = {}\nboot = {}\nimport_dir = {}\nrom = {}\nrom_sha256 = \"{sha}\"\n",
        quote(&title),
        quote(&game_id),
        h.version,
        quote(cic),
        quote(&out.display().to_string()),
        quote(&source.display().to_string()),
    );
    let manifest = Path::new("game.toml");
    let wrote = !manifest.exists() && relift_cache::write_if_changed(manifest, text.as_bytes())?;
    let order = match rom.order {
        ByteOrder::Z64 => "big-endian",
        ByteOrder::V64 => "byte-swapped",
        ByteOrder::N64 => "little-endian",
    };
    ui.summary(&Line::new().paint(Paint::Green, "imported").text(format!(
        " {title} ({game_id}, {} bytes {order}, boot {cic}, Nintendo 64), into {}{}, {}",
        rom.bytes.len(),
        out.display(),
        if wrote { "; wrote game.toml" } else { "" },
        duration(ui.elapsed())
    )));
    Ok(())
}

fn project_rom() -> Result<(Vec<u8>, String, String), BoxError> {
    let text = std::fs::read_to_string("game.toml").map_err(|e| format!("game.toml: {e}"))?;
    let t: toml::Table = text.parse().map_err(|e| format!("game.toml: {e}"))?;
    let g = t.get("game").ok_or("game.toml has no [game]")?;
    let s = |k: &str| g.get(k).and_then(|v| v.as_str()).map(String::from);
    let dir = PathBuf::from(s("import_dir").ok_or("game.toml has no game.import_dir")?);
    let path = dir.join(ROM_FILE);
    let rom = std::fs::read(&path)
        .map_err(|e| format!("{}: {e}; run relift import again", path.display()))?;
    Ok((
        rom,
        s("title").unwrap_or_default(),
        s("rom").unwrap_or_default(),
    ))
}

fn c_files(dir: &Path) -> Result<Vec<PathBuf>, BoxError> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir)
        .map_err(|e| format!("{}: {e}", dir.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "c" || x == "S"))
        .collect();
    v.sort();
    Ok(v)
}

/// `relift build` of the Nintendo 64 project in the working directory.
/// Returns the program's path.
///
/// # Errors
///
/// No import, a ROM the analysis refuses, or a failed compile or link.
pub fn build(settings: &Settings, mods: &[PathBuf], ui: &mut Ui) -> Result<PathBuf, BoxError> {
    if !mods.is_empty() {
        return Err("mods are not supported for Nintendo 64 ROMs yet".into());
    }
    let (bytes, project_title, rom_path) = project_rom()?;
    let rom = Rom::new(&bytes).map_err(|e| format!("{ROM_FILE}: {e}"))?;
    let (name, out) = crate::build::project_out(settings)?;
    let cdir = out.join("c");
    std::fs::create_dir_all(&cdir)?;
    ui.title(&format!(
        "relift build: {name} ({}, {} KiB, {}-{})",
        title_of(&rom),
        rom.bytes.len() / 1024,
        std::env::consts::OS,
        std::env::consts::ARCH
    ));
    let row = ui.row("rom");
    ui.begin(row, "analyze");
    let report = relift_n64_analyze::analyze(&rom.bytes).map_err(|e| format!("analyze: {e}"))?;
    ui.done(
        row,
        Line::new().text(format!(
            "{}, {} segments, {} code runs, {} unresolved",
            noun(report.functions.len() as u64, "function"),
            report.segments.len(),
            report.units.len(),
            report.unresolved.len()
        )),
    );
    let mut notes = String::new();
    for u in &report.unresolved {
        notes.push_str(&format!(
            "{:08x}  {}  {}\n",
            u.site,
            u.kind.name(),
            u.detail
        ));
    }
    relift_cache::write_if_changed(&out.join("unresolved.txt"), notes.as_bytes())?;
    ui.begin(row, "lift");
    let lifted = relift_n64::program::lift(
        &rom.bytes,
        &report,
        relift_switch_formats::sha256(&rom.bytes),
    );
    let mut files = Vec::new();
    for u in &lifted.units {
        let udir = cdir.join(&u.dir);
        std::fs::create_dir_all(&udir)?;
        for f in &u.output.files {
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
    }
    let rrow = ui.row("rsp");
    ui.begin(rrow, "lift");
    let rsp = relift_n64::rsp::lift(
        &rom.bytes,
        &report,
        relift_switch_formats::sha256(&rom.bytes),
    );
    for im in &rsp.images {
        let udir = cdir.join(&im.dir);
        std::fs::create_dir_all(&udir)?;
        for f in &im.output.files {
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
    }
    let mut list = String::new();
    for u in &rsp.unlifted {
        list.push_str(&format!(
            "rsp{}  {:03x}  {:08x}  {}\n",
            u.image, u.pc as u32, u.failure.insn as u32, u.failure.message
        ));
    }
    for u in &lifted.unlifted {
        list.push_str(&format!(
            "seg{}  {:08x}  {:08x}  {}\n",
            u.segment, u.pc as u32, u.failure.insn as u32, u.failure.message
        ));
    }
    relift_cache::write_if_changed(&out.join("unlifted.txt"), list.as_bytes())?;
    ui.done(
        row,
        Line::new().text(format!(
            "{} in {}, {} instructions, {} not lifted",
            noun(lifted.functions() as u64, "function"),
            noun(lifted.units.len() as u64, "unit"),
            lifted.instructions,
            lifted.unlifted.len()
        )),
    );
    ui.done(
        rrow,
        Line::new().text(format!(
            "{} in {}, {} instructions, {} not lifted",
            noun(rsp.functions() as u64, "function"),
            noun(rsp.images.len() as u64, "image"),
            rsp.instructions,
            rsp.unlifted.len()
        )),
    );
    relift_cache::write_if_changed(&cdir.join("rom.bin"), &rom.bytes)?;
    let title = if project_title.is_empty() {
        title_of(&rom)
    } else {
        project_title
    };
    let symbols: Vec<String> = lifted.units.iter().map(|u| u.symbol.clone()).collect();
    let mut prog = Vec::new();
    for (n, text) in [
        ("rom.c", relift_n64::program::rom_c()),
        ("rsp_images.c", relift_n64::rsp::images_c(&rsp.images)),
        (
            "program.c",
            relift_n64::program::program_c(&relift_n64::program::Program {
                title: &title,
                rom_path: &rom_path,
                units: &symbols,
                code: &lifted.code,
                cic: relift_n64::program::cic_number(report.boot),
                entry: report.entry,
            }),
        ),
    ] {
        let path = cdir.join(n);
        relift_cache::write_if_changed(&path, text.as_bytes())?;
        prog.push(path);
    }
    let game_settings = crate::game_settings::read(Path::new("game.toml"))?;
    let ports = relift_input::native_ports("n64")?;
    let doc = crate::info::n64_document(&title, &rom, game_settings.clone(), &ports);
    let info_c = cdir.join(crate::info::C_FILE);
    relift_cache::write_if_changed(&info_c, crate::info::c_file(&doc).as_bytes())?;
    prog.push(info_c);
    let settings_c = cdir.join("game_settings.c");
    relift_cache::write_if_changed(
        &settings_c,
        crate::game_settings::generate_c(&game_settings).as_bytes(),
    )?;
    prog.push(settings_c);
    compile(settings, &out, &cdir, &files, &prog, ui)?;
    let exe = out.join(format!("game{}", std::env::consts::EXE_SUFFIX));
    ui.summary(&Line::new().paint(Paint::Green, "built").text(format!(
        " {}, {} instructions not lifted, {} in all",
        noun((lifted.functions() + rsp.functions()) as u64, "function"),
        lifted.unlifted.len() + rsp.unlifted.len(),
        duration(ui.elapsed())
    )));
    ui.summary(&Line::new().text(format!("  output: {}", exe.display())));
    Ok(exe)
}

fn compile(
    settings: &Settings,
    out: &Path,
    cdir: &Path,
    lifted: &[PathBuf],
    prog: &[PathBuf],
    ui: &mut Ui,
) -> Result<(), BoxError> {
    let rt = crate::build::runtime_dir(settings);
    let inc = |p: &str| format!("-I{}", rt.join(p).display());
    let cc = crate::build::c_compiler(&settings.toolchain)?;
    let zig = cc.len() > 1;
    let base: Vec<String> = ["-std=c11", "-O2", "-ffp-contract=off"]
        .iter()
        .map(|s| s.to_string())
        .chain(zig.then_some("-fno-sanitize=undefined".to_string()))
        .collect();
    let static_rt = cfg!(windows).then_some("-DRL_STATIC_RUNTIME".to_string());
    let trace = std::env::var_os("RELIFT_N64_TRACE")
        .is_some_and(|v| !v.is_empty() && v != "0")
        .then(|| "-DRL_N64_TRACE".to_string());
    let lifted_flags: Vec<String> = base
        .iter()
        .map(|f| {
            if f == "-O2" {
                LIFTED_OPT.into()
            } else {
                f.clone()
            }
        })
        .chain(["-gline-tables-only", "-fno-omit-frame-pointer"].map(String::from))
        .chain([
            inc("core/include"),
            inc("cpus/mips/include"),
            inc("cpus/rsp/include"),
            inc("guests/n64/run/include"),
        ])
        .chain(static_rt.clone())
        .chain(trace.clone())
        .collect();
    let mut extras = crate::runtime_link::extras(&rt, &cache_root(settings)?)?;
    extras.vk.clear();
    ui.note(&Line::new().text(extras.note()));
    let rt_flags: Vec<String> = base
        .iter()
        .cloned()
        .chain(
            [
                "-g",
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
            inc("cpus/mips/include"),
            inc("cpus/mips"),
            inc("cpus/rsp/include"),
            inc("cpus/rsp"),
            inc("guests/n64"),
            inc("guests/n64/run"),
            inc("guests/n64/run/include"),
        ])
        .chain(extras.defines())
        .chain(static_rt)
        .chain(trace)
        .collect();
    let obj = |src: &Path, tag: &str| {
        let stem = src
            .strip_prefix(cdir)
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
            pool: None,
            stage: 0,
        })
        .collect();
    let mut rt_compiles = Vec::new();
    for layer in LAYERS {
        rt_compiles.extend(c_files(&rt.join(layer))?);
    }
    for s in prog {
        compiles.push(Compile {
            src: s.clone(),
            obj: obj(s, "prog"),
            flags: rt_flags.clone(),
            pool: None,
            stage: 0,
        });
    }
    let jobs = relift_build::job_count(settings.jobs);
    let ninja = crate::build::tool(&settings.toolchain.ninja, "ninja");
    let shared = !cfg!(windows);
    let key: String =
        relift_switch_formats::sha256(format!("{cc:?}\n{rt_flags:?}\n{}", rt.display()).as_bytes())
            [..8]
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect();
    let rt_dir = cache_root(settings)?.join("n64-runtime").join(&key);
    let rt_obj = |s: &Path| {
        let stem = s.strip_prefix(&rt).unwrap_or(s);
        rt_dir.join("obj").join(stem).with_extension("o")
    };
    let mut rt_plan_compiles: Vec<Compile> = rt_compiles
        .iter()
        .map(|s| Compile {
            src: s.clone(),
            obj: if shared { rt_obj(s) } else { obj(s, "rt") },
            flags: rt_flags.clone(),
            pool: None,
            stage: 0,
        })
        .collect();
    rt_plan_compiles.extend(extras.compiles(&rt, &rt_flags, &|s| {
        if shared { rt_obj(s) } else { obj(s, "rt") }
    }));
    let row = ui.row("");
    ui.begin(row, "compile");
    let mut link: Vec<String> = Vec::new();
    let mut n = 0;
    let exe = out.join(format!("game{}", std::env::consts::EXE_SUFFIX));
    if shared {
        std::fs::create_dir_all(&rt_dir)?;
        let lock_path = rt_dir.join("lock");
        relift_cache::write_if_changed(&lock_path, b"")?;
        let lock = std::fs::File::open(&lock_path)?;
        lock.lock()?;
        let rt_o = rt_dir.join("runtime.o");
        let pools = crate::build::memory_pools(&mut rt_plan_compiles, jobs);
        let plan = Plan {
            cc: cc.clone(),
            compiles: rt_plan_compiles,
            output: rt_o.clone(),
            link_flags: ["-r", "-nostdlib"].map(String::from).into(),
            pools,
            root: None,
        };
        n += crate::build::build_plans(
            ui,
            row,
            &[(plan, rt_dir.clone())],
            ninja.as_deref(),
            jobs,
            "",
        )?;
        drop(lock);
        let time = |p: &Path| std::fs::metadata(p).and_then(|m| m.modified()).ok();
        if time(&rt_o) > time(&exe) && exe.exists() {
            std::fs::remove_file(&exe)?;
        }
        link.push(rt_o.display().to_string());
    } else {
        compiles.extend(rt_plan_compiles);
    }
    let pools = crate::build::memory_pools(&mut compiles, jobs);
    let libs: Vec<String> = if cfg!(windows) {
        [
            "-lpthread",
            "-lm",
            "-lws2_32",
            "-luserenv",
            "-lntdll",
            "-lbcrypt",
        ]
        .map(String::from)
        .into()
    } else {
        ["-lpthread", "-lm", "-ldl"].map(String::from).into()
    };
    let plan = Plan {
        cc: cc.clone(),
        compiles,
        output: exe.clone(),
        link_flags: link.into_iter().chain(libs).chain(extras.libs()).collect(),
        pools,
        root: None,
    };
    n += crate::build::build_plans(
        ui,
        row,
        &[(plan, out.to_path_buf())],
        ninja.as_deref(),
        jobs,
        "",
    )?;
    ui.done(row, Line::new().text(noun(n as u64, "file")));
    Ok(())
}

/// `relift run` of the Nintendo 64 project: builds, then runs the program.
/// Returns the exit code.
///
/// # Errors
///
/// As [`build`], or the program cannot start.
pub fn run(settings: &Settings, ui: &mut Ui) -> Result<i32, BoxError> {
    let exe = build(settings, &[], ui)?;
    let status = relift_proc::command(&exe).status()?;
    Ok(status.code().unwrap_or(128))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bytes_without_a_magic_are_not_a_rom() {
        assert!(!is_rom_image(&[0u8; 0x1000]));
        assert!(!is_rom_image(&[0x80, 0x37, 0x12, 0x40]));
        let rom = relift_n64_formats::synth::rom(&relift_n64_formats::synth::Spec::default());
        assert!(is_rom_image(&rom));
        let v64 = ByteOrder::V64.encode(&rom).expect("encodes");
        assert!(is_rom_image(&v64));
    }
}

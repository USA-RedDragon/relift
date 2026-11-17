//! The Master System and Game Gear stage: `relift import` of a cartridge
//! ROM, `relift build` of a native program from it, and `relift run`.
//!
//! The import copies the ROM (less a copier header) into the project's
//! import directory and writes `game.toml` with `guest = "sms"` and the
//! console. The build lifts the ROM's code into one unit per bank and slot
//! and one per group of code in RAM (`relift-sms`) and links it with the
//! runtime's core, Z80 and SMS layers. Until discovery gives the code, a
//! build takes it from the executed instructions of an ares oracle trace
//! named by `RELIFT_SMS_TRACE` (`exec.tsv`), a development path.

use std::path::{Path, PathBuf};

use relift_progress::{Line, Paint, duration, noun};
use relift_sms::program::{self, Console, Region};
use relift_sms_formats::{Cartridge, System};

use crate::cross;
use crate::gb::Built;
use crate::gba::{Layers, compile_layers};
use crate::import::BoxError;
use crate::settings::Settings;
use crate::ui::Ui;

/// The ROM's file name in the import directory.
const ROM_FILE: &str = "rom.bin";

/// The runtime directories the program compiles, below the runtime root.
const LAYERS: [&str; 4] = ["core", "cpus/z80", "guests/sms", "guests/sms/run"];

const SMS_LAYERS: Layers = Layers {
    dirs: &LAYERS,
    lifted_inc: &["core/include", "cpus/z80/include", "guests/sms/run/include"],
    rt_inc: &[
        "core/include",
        "core",
        "cpus/z80/include",
        "cpus/z80",
        "guests/sms",
        "guests/sms/run",
        "guests/sms/run/include",
    ],
    lifted_opt: "-O0",
    cache: "sms-runtime",
};

/// The extensions of the cartridge images the import takes.
const EXTENSIONS: [&str; 2] = ["sms", "gg"];

/// Whether `path` is a Master System or Game Gear cartridge image: a
/// `.sms` or `.gg` file whose layout the formats crate takes, or any file
/// with a `TMR SEGA` header.
#[must_use]
pub fn is_rom(path: &Path) -> bool {
    if !path.is_file() {
        return false;
    }
    let ext = path
        .extension()
        .and_then(|e| e.to_str())
        .map(str::to_ascii_lowercase);
    let named = ext.as_deref().is_some_and(|e| EXTENSIONS.contains(&e));
    std::fs::read(path).is_ok_and(|rom| {
        Cartridge::parse(&rom, ext.as_deref()).is_ok_and(|c| {
            (named || c.header.is_some()) && !matches!(c.system, Some((System::Sg1000, _)))
        })
    })
}

/// Whether the project's `game.toml` names this guest.
#[must_use]
pub fn is_project() -> bool {
    project_table()
        .and_then(|g| g.get("guest").and_then(|v| v.as_str()).map(|g| g == "sms"))
        .unwrap_or(false)
}

fn project_table() -> Option<toml::Table> {
    let t: toml::Table = std::fs::read_to_string("game.toml").ok()?.parse().ok()?;
    t.get("game")?.as_table().cloned()
}

fn quote(s: &str) -> String {
    toml::Value::String(s.into()).to_string()
}

/// The console of a cartridge: a Game Gear cartridge runs as a Game Gear,
/// every other as a Master System.
fn console_of(c: &Cartridge) -> &'static str {
    match c.system {
        Some((System::Gg, _)) => "gg",
        _ => "sms",
    }
}

/// `relift import` of the ROM at `path` into the project in the working
/// directory.
///
/// # Errors
///
/// The ROM cannot be read or its layout is not a cartridge's, or the
/// import directory or `game.toml` cannot be written.
pub fn import(settings: &Settings, path: &Path, ui: &mut Ui) -> Result<(), BoxError> {
    let image = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let ext = path.extension().and_then(|e| e.to_str());
    let cart = Cartridge::parse(&image, ext).map_err(|e| format!("{}: {e}", path.display()))?;
    let rom = &image[cart.layout.rom_start()..];
    let out = crate::import::import_dir(settings, Path::new("."))?;
    std::fs::create_dir_all(&out)?;
    relift_cache::write_if_changed(&out.join(ROM_FILE), rom)?;
    let source = crate::paths::canonical(path)?;
    let sha: String = relift_switch_formats::sha256(rom)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    let title = path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("game")
        .to_string();
    let console = console_of(&cart);
    let text = format!(
        "# game.toml, written by relift import.\n\n[game]\nguest = \"sms\"\nconsole = \"{console}\"\nregion = \"ntsc-u\"\nmapper = \"{}\"\ncopier_header = {}\ntitle = {}\nimport_dir = {}\nrom = {}\nrom_sha256 = \"{sha}\"\n",
        cart.mapper.kind.name(),
        cart.layout.rom_start(),
        quote(&title),
        quote(&out.display().to_string()),
        quote(&source.display().to_string()),
    );
    let manifest = Path::new("game.toml");
    let wrote = !manifest.exists() && relift_cache::write_if_changed(manifest, text.as_bytes())?;
    ui.summary(&Line::new().paint(Paint::Green, "imported").text(format!(
        " {title} ({} bytes, {}, {} mapper), into {}{}, {}",
        rom.len(),
        if console == "gg" {
            "Game Gear"
        } else {
            "Master System"
        },
        cart.mapper.kind.name(),
        out.display(),
        if wrote { "; wrote game.toml" } else { "" },
        duration(ui.elapsed())
    )));
    Ok(())
}

/// The project's ROM and settings: the ROM bytes, the title, the console,
/// the region and the mapper.
struct Project {
    rom: Vec<u8>,
    title: String,
    console: Console,
    region: Region,
    mapper: program::Mapper,
    copier_header: u32,
}

fn project() -> Result<Project, BoxError> {
    let g = project_table().ok_or("game.toml has no [game]")?;
    let s = |k: &str| g.get(k).and_then(|v| v.as_str()).map(String::from);
    let dir = PathBuf::from(s("import_dir").ok_or("game.toml has no game.import_dir")?);
    let path = dir.join(ROM_FILE);
    let rom = std::fs::read(&path)
        .map_err(|e| format!("{}: {e}; run relift import again", path.display()))?;
    let console = match s("console").as_deref() {
        Some("gg") => Console::Gg,
        Some("gg-ms") => Console::GgMs,
        Some("sms") | None => Console::Ms,
        Some(o) => return Err(format!("game.toml: console {o:?} is not sms, gg or gg-ms").into()),
    };
    let region = match s("region").as_deref() {
        Some("ntsc-j") => Region::NtscJ,
        Some("pal") => Region::Pal,
        Some("ntsc-u") | None => Region::NtscU,
        Some(o) => {
            return Err(format!("game.toml: region {o:?} is not ntsc-u, ntsc-j or pal").into());
        }
    };
    let mapper = match s("mapper").as_deref() {
        Some("codemasters") => program::Mapper::Codemasters,
        Some("korea") => program::Mapper::Korea,
        _ => program::Mapper::Sega,
    };
    Ok(Project {
        rom,
        title: s("title").unwrap_or_default(),
        console,
        region,
        mapper,
        copier_header: g
            .get("copier_header")
            .and_then(toml::Value::as_integer)
            .and_then(|v| u32::try_from(v).ok())
            .unwrap_or(0),
    })
}

/// The code to lift: the executed instructions of the ares trace that
/// `RELIFT_SMS_TRACE` names, whose ROM offsets count the copier header the
/// import dropped.
fn code(copier_header: u32) -> Result<relift_sms::code::Code, BoxError> {
    let Some(path) = std::env::var_os("RELIFT_SMS_TRACE").filter(|v| !v.is_empty()) else {
        return Err(
            "Master System code discovery is not in this build yet; set RELIFT_SMS_TRACE to an ares oracle exec.tsv to build from a trace"
                .into(),
        );
    };
    let text = std::fs::read_to_string(&path)
        .map_err(|e| format!("{}: {e}", Path::new(&path).display()))?;
    Ok(relift_sms::code::Code::from_exec_tsv(&text, copier_header)?)
}

/// `relift build` of the project in the working directory, for this host
/// or for `target`; a `release` build for this host takes the release
/// compiler.
///
/// # Errors
///
/// No import, no code, a target whose tools are missing, or a failed
/// compile or link.
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
        p.rom.len() / 1024,
        cross.as_ref().map_or_else(
            || format!("{}-{}", std::env::consts::OS, std::env::consts::ARCH),
            |c| c.name().to_string()
        )
    ));
    let row = ui.row("rom");
    ui.begin(row, "lift");
    let code = code(p.copier_header)?;
    let lifted = program::lift(&p.rom, &code, relift_switch_formats::sha256(&p.rom));
    let mut files = Vec::new();
    for u in std::iter::once(&lifted.entries).chain(&lifted.units) {
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
    let mut list = String::new();
    for u in &lifted.unlifted {
        list.push_str(&format!(
            "{:04x}  {:?}  {}\n",
            u.at.addr, u.at.src, u.failure.message
        ));
    }
    relift_cache::write_if_changed(&out.join("unlifted.txt"), list.as_bytes())?;
    ui.done(
        row,
        Line::new().text(format!(
            "{} in {}, {} RAM groups, {} not lifted",
            noun(code.instructions.len() as u64, "instruction"),
            noun(lifted.units.len() as u64, "unit"),
            lifted.ram_groups.len(),
            lifted.unlifted.len()
        )),
    );
    let entry = cross
        .as_ref()
        .map_or(program::Entry::Main, cross::Build::entry);
    let prog_desc = program::Program {
        title: &p.title,
        console: p.console,
        region: p.region,
        mapper: p.mapper,
        entry,
    };
    relift_cache::write_if_changed(&cdir.join("rom.bin"), &p.rom)?;
    let mut prog = Vec::new();
    for (n, text) in [
        ("rom.c", program::rom_c(&p.rom)),
        ("program.c", program::program_c(&prog_desc, &lifted)),
    ] {
        let path = cdir.join(n);
        relift_cache::write_if_changed(&path, text.as_bytes())?;
        prog.push(path);
    }
    let platform = if p.console == Console::Gg {
        "gg"
    } else {
        "sms"
    };
    let ports = relift_input::native_ports(platform)?;
    let game_settings = crate::game_settings::read(Path::new("game.toml"))?;
    let doc = crate::info::sms_document(&p.title, platform, game_settings.clone(), &ports);
    let info_c = cdir.join(crate::info::C_FILE);
    relift_cache::write_if_changed(&info_c, crate::info::c_file(&doc).as_bytes())?;
    prog.push(info_c);
    let label = doc.identity.title(&[]).unwrap_or(&p.title).to_string();
    prog.extend(crate::winres::write(
        &cdir,
        &label,
        doc.identity.icon.as_ref(),
    )?);
    let settings_c = cdir.join("game_settings.c");
    relift_cache::write_if_changed(
        &settings_c,
        crate::game_settings::generate_c(&game_settings).as_bytes(),
    )?;
    prog.push(settings_c);
    compile_layers(
        settings,
        &out,
        &cdir,
        (&files, &prog),
        &SMS_LAYERS,
        (release, cross.as_ref()),
        ui,
    )?;
    let exe = match &cross {
        Some(c) => out.join(c.program()),
        None => out.join(format!("game{}", std::env::consts::EXE_SUFFIX)),
    };
    ui.summary(&Line::new().paint(Paint::Green, "built").text(format!(
        " {} units, {} instructions not lifted, {} in all",
        lifted.units.len(),
        lifted.unlifted.len(),
        duration(ui.elapsed())
    )));
    ui.summary(&Line::new().text(format!("  output: {}", exe.display())));
    Ok(Built {
        exe,
        source: PathBuf::new(),
        title: label,
        cross,
    })
}

/// `relift run`: builds, then runs the program. Returns the exit code.
///
/// # Errors
///
/// As [`build`], or the program cannot start.
pub fn run(settings: &Settings, ui: &mut Ui) -> Result<i32, BoxError> {
    let built = build(settings, false, None, ui)?;
    let status = relift_proc::command(&built.exe).status()?;
    Ok(status.code().unwrap_or(128))
}

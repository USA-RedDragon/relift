//! The Game Boy Advance stage: `relift import` of a cartridge ROM, `relift
//! build` of a native program from it, and `relift run`.
//!
//! The import copies the ROM into the project's import directory and writes
//! `relift.toml` with `guest = "gba"`. The build finds the code
//! (`relift-gba-analyze`), lifts it into one unit (`relift-gba`), and links
//! it with the runtime's core, ARMv4T and GBA layers, for this host or for
//! another target (`relift build --target`), where it enters from the app
//! glue and shows the on-screen controller on a touch screen. `relift run`
//! opens the program's window.

use std::path::{Path, PathBuf};

use relift_progress::{Line, Paint, duration, noun};

use crate::cross;
use crate::gb::Built;
use crate::import::BoxError;
use crate::layers::{Layers, compile_layers};
use crate::settings::Settings;
use crate::ui::Ui;

/// The optimization of lifted code in a dev build (`relift run`); a release
/// build uses [`crate::build::RELEASE_LIFTED_OPT`]. Every access is a bus
/// call, so the lifted bodies are a small part of a run, and -O0 compiles
/// fastest.
const LIFTED_OPT: &str = "-O0";

/// The ROM's file name in the import directory.
const ROM_FILE: &str = "rom.gba";

/// The runtime directories the program compiles, below the runtime root.
const LAYERS: [&str; 6] = [
    "core",
    "cpus/arm32",
    "guests/gba",
    "guests/gba/bios",
    "guests/gba/ppu",
    "guests/gba/run",
];

/// The largest cartridge image (the format's 32 MiB, with room for
/// oddities): a larger file is not read to check, so a many-GB game dump is
/// not loaded into memory.
const MAX_ROM: u64 = 64 << 20;

/// Whether `path` is a Game Boy Advance cartridge image: a file whose
/// header has the fixed byte and a branch at the entry, with the logo or a
/// `.gba` name.
#[must_use]
pub fn is_rom(path: &Path) -> bool {
    if !path.is_file() || std::fs::metadata(path).is_ok_and(|m| m.len() > MAX_ROM) {
        return false;
    }
    let named = path
        .extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| e.eq_ignore_ascii_case("gba"));
    std::fs::read(path).is_ok_and(|rom| is_rom_image(&rom, named))
}

/// Whether `rom` is a cartridge image: its header decodes, and it has the
/// logo or comes from a file named `.gba` (`named`).
#[must_use]
pub fn is_rom_image(rom: &[u8], named: bool) -> bool {
    relift_gba_formats::Header::parse(rom)
        .is_ok_and(|h| h.entry_target().is_some() && (named || h.checks().logo))
}

/// The module name of the cartridge ROM in hints and stop reports.
pub const ROM_MODULE: &str = "rom";

/// The roots of the hints that name the ROM (`rom+0x<offset>`, the offset
/// from `0x08000000`; an odd offset is Thumb code).
#[must_use]
pub fn rom_hints(
    hints: &std::collections::BTreeSet<crate::hints::Hint>,
) -> Vec<relift_gba_analyze::Loc> {
    hints
        .iter()
        .filter(|h| h.module == ROM_MODULE)
        .filter_map(|h| {
            let a = u32::try_from(0x0800_0000 + h.offset).ok()?;
            Some(relift_gba_analyze::Loc::rom(a & !1, a & 1 == 1))
        })
        .collect()
}

fn title(rom: &[u8]) -> String {
    relift_gba_formats::Header::parse(rom)
        .map(|h| h.title)
        .unwrap_or_default()
}

fn quote(s: &str) -> String {
    toml::Value::String(s.into()).to_string()
}

/// `relift import` of the ROM at `path` into the project in the working
/// directory.
///
/// # Errors
///
/// The ROM cannot be read or its header does not decode, or the import
/// directory or `relift.toml` cannot be written.
pub fn import(settings: &Settings, path: &Path, ui: &mut Ui) -> Result<(), BoxError> {
    let rom = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let h =
        relift_gba_formats::Header::parse(&rom).map_err(|e| format!("{}: {e}", path.display()))?;
    let out = crate::import::import_dir(settings, Path::new("."))?;
    std::fs::create_dir_all(&out)?;
    relift_cache::write_if_changed(&out.join(ROM_FILE), &rom)?;
    let source = crate::paths::canonical(path)?;
    let sha = relift_gba_formats::checksum::hex(&relift_switch_formats::sha256(&rom));
    let text = format!(
        "# The project file, written by relift import.\n\n[game]\nguest = \"gba\"\ntitle = {}\ngame_code = {}\nversion = {}\nimport_dir = {}\nrom = {}\nrom_sha256 = \"{sha}\"\n",
        quote(&h.title),
        quote(&h.game_code),
        h.version,
        quote(&out.display().to_string()),
        quote(&source.display().to_string()),
    );
    let manifest = Path::new("relift.toml");
    let wrote = crate::projectfile::write_stub(manifest, &text)?;
    ui.summary(&Line::new().paint(Paint::Green, "imported").text(format!(
        " {} ({}, {} bytes, Game Boy Advance), into {}{}, {}",
        h.title,
        h.game_code,
        rom.len(),
        out.display(),
        if wrote { "; wrote relift.toml" } else { "" },
        duration(ui.elapsed())
    )));
    Ok(())
}

fn project_rom() -> Result<(Vec<u8>, String, String), BoxError> {
    let text = std::fs::read_to_string("relift.toml").map_err(|e| format!("relift.toml: {e}"))?;
    let t: toml::Table = text.parse().map_err(|e| format!("relift.toml: {e}"))?;
    let g = t.get("game").ok_or("relift.toml has no [game]")?;
    let s = |k: &str| g.get(k).and_then(|v| v.as_str()).map(String::from);
    let dir = PathBuf::from(s("import_dir").ok_or("relift.toml has no game.import_dir")?);
    let path = dir.join(ROM_FILE);
    let rom = std::fs::read(&path)
        .map_err(|e| format!("{}: {e}; run relift import again", path.display()))?;
    Ok((
        rom,
        s("title").unwrap_or_default(),
        s("rom").unwrap_or_default(),
    ))
}

/// `relift build` of the Game Boy Advance project in the working directory,
/// for this host or for `target`; a `release` build for this host takes the
/// release compiler.
///
/// # Errors
///
/// No import, a target whose tools are missing, or a failed compile or
/// link.
pub fn build(
    settings: &Settings,
    mods: &[PathBuf],
    release: bool,
    target: Option<cross::Target>,
    ui: &mut Ui,
) -> Result<Built, BoxError> {
    if !mods.is_empty() {
        return Err("mods are not supported for Game Boy Advance ROMs yet".into());
    }
    let (rom, project_title, rom_path) = project_rom()?;
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
        title(&rom),
        rom.len() / 1024,
        cross.as_ref().map_or_else(
            || format!("{}-{}", std::env::consts::OS, std::env::consts::ARCH),
            |c| c.name().to_string()
        )
    ));
    let row = ui.row("rom");
    ui.begin(row, "analyze");
    let hints = rom_hints(&crate::hints::read(Path::new("relift.toml"))?);
    let report =
        relift_gba_analyze::analyze_with(&rom, &hints).map_err(|e| format!("analyze: {e}"))?;
    ui.done(
        row,
        Line::new().text(format!(
            "{}, {} RAM copies, {} IRQ handlers, {} unresolved",
            noun(report.functions.len() as u64, "function"),
            report.copies.len(),
            report.irq_handlers.len(),
            report.unresolved.len()
        )),
    );
    let mut notes = String::new();
    for u in &report.unresolved {
        notes.push_str(&format!("{}  {}  {}\n", u.at, u.kind.name(), u.detail));
    }
    relift_cache::write_if_changed(&out.join("unresolved.txt"), notes.as_bytes())?;
    ui.begin(row, "lift");
    let lifted = relift_gba::program::lift(&rom, &report, relift_switch_formats::sha256(&rom));
    let mut files = Vec::new();
    let outputs = std::iter::once(&lifted.output).chain(&lifted.variants);
    for (k, output) in outputs.enumerate() {
        let udir = cdir.join(if k == 0 {
            "rom".to_owned()
        } else {
            format!("rom_v{k}")
        });
        std::fs::create_dir_all(&udir)?;
        for f in &output.files {
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
    for k in lifted.variants.len() + 1.. {
        let stale = cdir.join(format!("rom_v{k}"));
        if !stale.exists() {
            break;
        }
        std::fs::remove_dir_all(&stale)?;
    }
    let mut list = String::new();
    for u in &lifted.unlifted {
        list.push_str(&format!(
            "{}  {:08x}  {}\n",
            u.at, u.failure.insn as u32, u.failure.message
        ));
    }
    relift_cache::write_if_changed(&out.join("unlifted.txt"), list.as_bytes())?;
    ui.done(
        row,
        Line::new().text(format!(
            "{} in 1 unit, {} instructions, {} not lifted",
            noun(lifted.functions as u64, "function"),
            lifted.instructions,
            lifted.unlifted.len()
        )),
    );
    let mut prog = Vec::new();
    relift_cache::write_if_changed(&cdir.join("rom.bin"), &rom)?;
    let title = if project_title.is_empty() {
        title(&rom)
    } else {
        project_title
    };
    let entry = cross
        .as_ref()
        .map_or(relift_guest::Entry::Main, cross::Build::entry);
    let (rom_path, save) = if cross.is_some() {
        ("", "battery.sav")
    } else {
        (rom_path.as_str(), "")
    };
    for (n, text) in [
        ("rom.c", relift_gba::program::rom_c(&rom)),
        (
            "program.c",
            relift_gba::program::program_c(&relift_gba::program::Program {
                title: &title,
                rom_path,
                save,
                entry,
                variant_units: lifted.variants.len(),
                ram: &lifted.ram,
            }),
        ),
    ] {
        let path = cdir.join(n);
        relift_cache::write_if_changed(&path, text.as_bytes())?;
        prog.push(path);
    }
    let mut ports = relift_input::native_ports("gba")?;
    if cross.is_some()
        && let Some(t) = relift_input::touchpad_port("gba")
    {
        ports.add_port(t)?;
    }
    let game_settings = crate::game_settings::read(Path::new("relift.toml"))?;
    let mut doc = crate::info::handheld_document(
        &title,
        &rom,
        true,
        game_settings.clone(),
        &ports,
        relift_info::Mods::default(),
        None,
    )?;
    crate::kind::stamp(&mut doc)?;
    let info_c = cdir.join(crate::info::C_FILE);
    relift_cache::write_if_changed(&info_c, crate::info::c_file(&doc).as_bytes())?;
    prog.push(info_c);
    let label = doc.identity.title(&[]).unwrap_or(&title).to_string();
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
    compile(
        settings,
        &out,
        &cdir,
        &files,
        &prog,
        (release, cross.as_ref()),
        ui,
    )?;
    let exe = match &cross {
        Some(c) => out.join(c.program(&crate::progname::stem())),
        None => out.join(crate::progname::program()),
    };
    ui.summary(&Line::new().paint(Paint::Green, "built").text(format!(
        " {}, {} instructions not lifted, {} in all",
        noun(lifted.functions as u64, "function"),
        lifted.unlifted.len(),
        duration(ui.elapsed())
    )));
    ui.summary(&Line::new().text(format!("  output: {}", exe.display())));
    Ok(Built {
        exe,
        source: PathBuf::from(rom_path),
        title: label,
        cross,
    })
}

const GBA_LAYERS: Layers = Layers {
    dirs: &LAYERS,
    lifted_inc: &[
        "core/include",
        "cpus/arm32/include",
        "guests/gba/run/include",
    ],
    rt_inc: &[
        "core/include",
        "core",
        "cpus/arm32/include",
        "cpus/arm32",
        "guests/gba",
        "guests/gba/run",
        "guests/gba/run/include",
    ],
    lifted_opt: LIFTED_OPT,
    cache: "gba-runtime",
    cx16: false,
    share_lifted: false,
    ..Layers::BASE
};

fn compile(
    settings: &Settings,
    out: &Path,
    cdir: &Path,
    lifted: &[PathBuf],
    prog: &[PathBuf],
    (release, cross): (bool, Option<&cross::Build>),
    ui: &mut Ui,
) -> Result<(), BoxError> {
    compile_layers(
        settings,
        out,
        cdir,
        (lifted, prog),
        &GBA_LAYERS,
        (release, cross),
        ui,
    )
}

/// `relift run` of the Game Boy Advance project: builds, then runs the
/// program. Returns the exit code.
///
/// # Errors
///
/// As [`build`], or the program cannot start.
pub fn run(settings: &Settings, ui: &mut Ui) -> Result<i32, BoxError> {
    let built = build(settings, &[], false, None, ui)?;
    let status = relift_proc::command(&built.exe).status()?;
    Ok(status.code().unwrap_or(128))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bytes_without_a_header_are_not_a_rom() {
        assert!(!is_rom_image(&[0u8; 0x200], true));
        assert!(!is_rom_image(&[0u8; 4], true));
    }
}

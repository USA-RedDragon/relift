//! The Nintendo DS stage: `relift import` of a decrypted cartridge image,
//! `relift build` of a native program from it, and `relift run`.
//!
//! The import copies the image into the project's import directory and
//! writes `game.toml` with `guest = "nds"` and an `[nds]` section. The build
//! reads the units of both processors and finds their code
//! (`relift-nds-analyze`), lifts each unit into a C unit of its own
//! (`relift-nds`), and links them with the runtime's core, ARM32 and NDS
//! layers. The program boots the cartridge without a BIOS image and runs
//! headless.

use std::path::{Path, PathBuf};

use relift_progress::{Line, Paint, duration, noun};

use crate::gba::{Layers, compile_layers};
use crate::import::BoxError;
use crate::settings::Settings;
use crate::ui::Ui;

/// The image's file name in the import directory.
const ROM_FILE: &str = "rom.nds";

/// What the program compiles. Lifted code is built at -O0 as on the GBA:
/// every access is a bus call.
const NDS_LAYERS: Layers = Layers {
    dirs: &["core", "cpus/arm32", "guests/nds", "guests/nds/run"],
    lifted_inc: &[
        "core/include",
        "cpus/arm32/include",
        "guests/nds/run/include",
    ],
    rt_inc: &[
        "core/include",
        "core",
        "cpus/arm32/include",
        "cpus/arm32",
        "guests/nds",
        "guests/nds/run",
        "guests/nds/run/include",
    ],
    lifted_opt: "-O0",
    cache: "nds-runtime",
};

/// Whether `path` is a decrypted Nintendo DS cartridge image: a file whose
/// header has a correct CRC and whose name ends in `.nds`, or whose logo
/// checks.
#[must_use]
pub fn is_rom(path: &Path) -> bool {
    if !path.is_file() {
        return false;
    }
    let named = path
        .extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| e.eq_ignore_ascii_case("nds"));
    std::fs::read(path).is_ok_and(|rom| is_rom_image(&rom, named))
}

/// Whether `rom` is a cartridge image: its header decodes with a correct
/// CRC, and it has the logo or comes from a file named `.nds` (`named`).
#[must_use]
pub fn is_rom_image(rom: &[u8], named: bool) -> bool {
    relift_nds_formats::Header::parse(rom).is_ok_and(|h| {
        let c = h.checks();
        c.header_crc && (named || c.logo)
    })
}

/// Whether the project's `game.toml` names the Nintendo DS guest.
#[must_use]
pub fn is_project() -> bool {
    std::fs::read_to_string("game.toml")
        .ok()
        .and_then(|t| t.parse::<toml::Table>().ok())
        .and_then(|t| {
            t.get("game")
                .and_then(|g| g.get("guest"))
                .and_then(|g| g.as_str())
                .map(|g| g == "nds")
        })
        .unwrap_or(false)
}

fn quote(s: &str) -> String {
    toml::Value::String(s.into()).to_string()
}

/// `relift import` of the image at `path` into the project in the working
/// directory.
///
/// # Errors
///
/// The image cannot be read or its header does not decode, or the import
/// directory or `game.toml` cannot be written.
pub fn import(settings: &Settings, path: &Path, ui: &mut Ui) -> Result<(), BoxError> {
    let rom = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let h =
        relift_nds_formats::Header::parse(&rom).map_err(|e| format!("{}: {e}", path.display()))?;
    let out = crate::import::import_dir(settings, Path::new("."))?;
    std::fs::create_dir_all(&out)?;
    relift_cache::write_if_changed(&out.join(ROM_FILE), &rom)?;
    let source = crate::paths::canonical(path)?;
    let sha = relift_nds_formats::checksum::hex(&relift_nds_formats::checksum::sha256(&rom));
    let text = format!(
        "# game.toml, written by relift import.\n\n[game]\nguest = \"nds\"\ntitle = {}\ngame_code = {}\nversion = {}\nimport_dir = {}\nrom = {}\nrom_sha256 = \"{sha}\"\n\n# The run: ticks of the 67 MHz ARM9 clock after which the program stops\n# with a report (0: never), and ARM9 words it prints at the end.\n[nds]\nmax_ticks = 0\npeek = []\n",
        quote(&h.title),
        quote(&h.game_code),
        h.version,
        quote(&out.display().to_string()),
        quote(&source.display().to_string()),
    );
    let manifest = Path::new("game.toml");
    let wrote = !manifest.exists() && relift_cache::write_if_changed(manifest, text.as_bytes())?;
    ui.summary(&Line::new().paint(Paint::Green, "imported").text(format!(
        " {} ({}, {} bytes, Nintendo DS), into {}{}, {}",
        h.title,
        h.game_code,
        rom.len(),
        out.display(),
        if wrote { "; wrote game.toml" } else { "" },
        duration(ui.elapsed())
    )));
    Ok(())
}

/// The project: the image, its title and the run options of `[nds]`.
struct Project {
    rom: Vec<u8>,
    title: String,
    args: Vec<String>,
}

fn project() -> Result<Project, BoxError> {
    let text = std::fs::read_to_string("game.toml").map_err(|e| format!("game.toml: {e}"))?;
    let t: toml::Table = text.parse().map_err(|e| format!("game.toml: {e}"))?;
    let g = t.get("game").ok_or("game.toml has no [game]")?;
    let s = |k: &str| g.get(k).and_then(|v| v.as_str()).map(String::from);
    let dir = PathBuf::from(s("import_dir").ok_or("game.toml has no game.import_dir")?);
    let path = dir.join(ROM_FILE);
    let rom = std::fs::read(&path)
        .map_err(|e| format!("{}: {e}; run relift import again", path.display()))?;
    Ok(Project {
        rom,
        title: s("title").unwrap_or_default(),
        args: run_args(t.get("nds")),
    })
}

/// The program's options from an `[nds]` table.
fn run_args(nds: Option<&toml::Value>) -> Vec<String> {
    let mut args = Vec::new();
    let Some(nds) = nds else {
        return args;
    };
    if let Some(n) = nds.get("max_ticks").and_then(toml::Value::as_integer)
        && n > 0
    {
        args.extend(["--max-ticks".to_string(), n.to_string()]);
    }
    for p in nds
        .get("peek")
        .and_then(toml::Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(toml::Value::as_integer)
    {
        args.extend(["--peek".to_string(), format!("{p:#x}")]);
    }
    args
}

/// `relift build` of the Nintendo DS project in the working directory.
/// Returns the program's path.
///
/// # Errors
///
/// No import, an image whose units do not read, or a failed compile or
/// link.
pub fn build(settings: &Settings, mods: &[PathBuf], ui: &mut Ui) -> Result<PathBuf, BoxError> {
    if !mods.is_empty() {
        return Err("mods are not supported for Nintendo DS images yet".into());
    }
    let p = project()?;
    let rom = &p.rom;
    let (name, out) = crate::build::project_out(settings)?;
    let cdir = out.join("c");
    std::fs::create_dir_all(&cdir)?;
    let parsed = relift_nds_formats::Rom::parse(rom).map_err(|e| format!("image: {e}"))?;
    ui.title(&format!(
        "relift build: {name} ({}, {} KiB, {}-{})",
        parsed.header.title,
        rom.len() / 1024,
        std::env::consts::OS,
        std::env::consts::ARCH
    ));
    let row = ui.row("units");
    ui.begin(row, "analyze");
    let units = relift_nds_analyze::Program::read(&parsed).map_err(|e| format!("units: {e}"))?;
    let reports = units.analyze(relift_build::job_count(settings.jobs) as usize, &|_| {});
    let found: usize = reports.iter().map(|r| r.report.functions.len()).sum();
    let unresolved: usize = reports.iter().map(|r| r.report.unresolved.len()).sum();
    ui.done(
        row,
        Line::new().text(format!(
            "{} in {}, {unresolved} unresolved",
            noun(found as u64, "function"),
            noun(units.units.len() as u64, "unit"),
        )),
    );
    let mut notes = String::new();
    for r in &reports {
        for u in &r.report.unresolved {
            notes.push_str(&format!(
                "{}  {}  {}  {}\n",
                r.id.name(),
                u.at,
                u.kind.name(),
                u.detail
            ));
        }
    }
    relift_cache::write_if_changed(&out.join("unresolved.txt"), notes.as_bytes())?;
    ui.begin(row, "lift");
    let image_id = relift_nds_formats::checksum::sha256(rom);
    let lifted = relift_nds::program::lift(&units, &reports, image_id);
    let mut files = Vec::new();
    let mut dirs = Vec::new();
    for (k, u) in lifted.units.iter().enumerate() {
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
        relift_cache::write_if_changed(&cdir.join(relift_nds::program::blob_file(k)), &u.blob)?;
        dirs.push(u.dir.clone());
    }
    for e in std::fs::read_dir(&cdir)?.flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        if e.path().is_dir() && !dirs.contains(&n) {
            std::fs::remove_dir_all(e.path())?;
        }
    }
    let mut list = String::new();
    for u in &lifted.unlifted {
        list.push_str(&format!(
            "{}  {}  {:08x}  {}\n",
            u.unit.name(),
            u.at,
            u.failure.insn as u32,
            u.failure.message
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
    let title = if p.title.is_empty() {
        parsed.header.title.clone()
    } else {
        p.title.clone()
    };
    relift_cache::write_if_changed(&cdir.join("rom.bin"), rom)?;
    let mut prog = Vec::new();
    for (n, text) in [
        ("data.c", relift_nds::program::data_c(lifted.units.len())),
        (
            "program.c",
            relift_nds::program::program_c(&title, &lifted.units, &p.args),
        ),
    ] {
        let path = cdir.join(n);
        relift_cache::write_if_changed(&path, text.as_bytes())?;
        prog.push(path);
    }
    let game_settings = crate::game_settings::read(Path::new("game.toml"))?;
    let doc = crate::info::nds_document(&title, rom, game_settings.clone())?;
    let info_c = cdir.join(crate::info::C_FILE);
    relift_cache::write_if_changed(&info_c, crate::info::c_file(&doc).as_bytes())?;
    prog.push(info_c);
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
        &NDS_LAYERS,
        (false, None),
        ui,
    )?;
    let exe = out.join(format!("game{}", std::env::consts::EXE_SUFFIX));
    ui.summary(&Line::new().paint(Paint::Green, "built").text(format!(
        " {} in {}, {} instructions not lifted, {} in all",
        noun(lifted.functions() as u64, "function"),
        noun(lifted.units.len() as u64, "unit"),
        lifted.unlifted.len(),
        duration(ui.elapsed())
    )));
    ui.summary(&Line::new().text(format!("  output: {}", exe.display())));
    Ok(exe)
}

/// `relift run` of the Nintendo DS project: builds, then runs the program.
/// Returns the exit code.
///
/// # Errors
///
/// As [`build`], or the program cannot start.
pub fn run(settings: &Settings, ui: &mut Ui) -> Result<i32, BoxError> {
    let exe = build(settings, &[], ui)?;
    let status = relift_proc::command(&exe).arg("--stats").status()?;
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

    #[test]
    fn a_synthetic_image_is_a_rom_by_its_name() {
        let rom = relift_nds_formats::synth::rom(&relift_nds_formats::synth::Spec::default());
        assert!(is_rom_image(&rom, true));
        assert!(!is_rom_image(&rom, false));
    }

    #[test]
    fn the_nds_table_gives_the_run_options() {
        let t: toml::Table = "[nds]\nmax_ticks = 5\npeek = [0x2100000, 16]\n"
            .parse()
            .unwrap();
        assert_eq!(
            run_args(t.get("nds")),
            ["--max-ticks", "5", "--peek", "0x2100000", "--peek", "0x10"]
        );
        let none: toml::Table = "[nds]\nmax_ticks = 0\n".parse().unwrap();
        assert!(run_args(none.get("nds")).is_empty());
    }
}

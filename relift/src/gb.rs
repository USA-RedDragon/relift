//! The Game Boy and Game Boy Color stage: `relift import` of a cartridge
//! ROM, `relift build` of a native program from it, and `relift run`.
//!
//! The import copies the ROM into the project's import directory and
//! writes `relift.toml` with `guest = "gb"` and where the ROM came from. The
//! build applies the ROM-hack patches given as mods, finds the functions
//! with `relift-gb-analyze`, lifts them into one unit per bank
//! (`relift-gb`), and links them with the runtime's core, SM83 and Game
//! Boy layers and the device library (`guests/gb/devices`, built with
//! cargo into the cache). The battery save lives next to the imported ROM
//! as a raw `.sav`.

use std::path::{Path, PathBuf};

use relift_progress::{Line, Paint, duration, noun};

use crate::cross;
use crate::import::{BoxError, cache_root, import_dir_value};
use crate::layers::{Layers, compile_layers};
use crate::settings::Settings;
use crate::ui::Ui;

/// The optimization of lifted code in a dev build (`relift build --dev`,
/// `relift run`). In the exact mode the device library runs at every access
/// and boundary, so the lifted bodies are a small part of the run: a Game
/// Boy Color game with 256 MBC5 banks runs 2400 frames in 8.1 s with its
/// lifted code at -O0, -O1 or -O2 alike, while -O0 compiles in a quarter of
/// the time of -O2 (1.7 s against 6.8 s on 8 jobs). A release build still
/// uses [`crate::build::RELEASE_LIFTED_OPT`], for the smaller program.
const LIFTED_OPT: &str = "-O0";

/// The ROM's file name in the import directory.
const ROM_FILE: &str = "rom.gb";

/// The file in the import directory where the program adds the ROM
/// locations it ran without lifted code; the next build lifts them.
const LEARNED_FILE: &str = "learned.txt";

/// The largest cartridge image (the format's 8 MiB, with room for oddities):
/// a larger file is not read to check, so a many-GB game dump is not loaded
/// into memory.
const MAX_ROM: u64 = 64 << 20;

/// Whether `path` is a Game Boy cartridge image: a file whose header
/// decodes, with the Nintendo logo or a `.gb`, `.gbc` or `.sgb` name.
#[must_use]
pub fn is_rom(path: &Path) -> bool {
    if !path.is_file() || std::fs::metadata(path).is_ok_and(|m| m.len() > MAX_ROM) {
        return false;
    }
    let named = path
        .extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| ["gb", "gbc", "sgb", "cgb"].contains(&e.to_ascii_lowercase().as_str()));
    let Ok(rom) = std::fs::read(path) else {
        return false;
    };
    named && rom.len() >= 0x150 || relift_gb_formats::Header::parse(&rom).is_ok_and(|h| h.logo_ok)
}

/// Whether the project's `relift.toml` names the Game Boy guest.
#[must_use]
pub fn is_project() -> bool {
    std::fs::read_to_string("relift.toml")
        .ok()
        .and_then(|t| t.parse::<toml::Table>().ok())
        .and_then(|t| {
            t.get("game")
                .and_then(|g| g.get("guest"))
                .and_then(|g| g.as_str())
                .map(|g| g == "gb")
        })
        .unwrap_or(false)
}

/// The hardware a ROM starts on: a Game Boy Color for a cartridge whose
/// CGB flag (`$0143`) knows it, else the original Game Boy.
#[must_use]
pub fn default_model(rom: &[u8]) -> &'static str {
    if is_color(rom) { "cgb" } else { "dmg" }
}

fn is_color(rom: &[u8]) -> bool {
    rom.get(0x143).is_some_and(|b| b & 0x80 != 0)
}

/// The header title, or the raw title bytes when the header does not
/// decode (an unknown cartridge type).
fn title(rom: &[u8]) -> String {
    match relift_gb_formats::Header::parse(rom) {
        Ok(h) => h.title,
        Err(_) => rom
            .get(0x134..0x143)
            .unwrap_or_default()
            .iter()
            .take_while(|b| **b != 0)
            .map(|&b| {
                if (0x20..0x7F).contains(&b) {
                    char::from(b)
                } else {
                    '?'
                }
            })
            .collect(),
    }
}

/// The battery save path of the ROM at `rom`: the same name with `.sav`.
#[must_use]
pub fn save_path(rom: &Path) -> PathBuf {
    rom.with_extension("sav")
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
    let h = relift_gb_formats::Header::parse(&rom);
    if let Err(e) = &h {
        ui.note(&Line::new().paint(
            Paint::Yellow,
            format!("ROM header: {e}; the ROM runs without discovery"),
        ));
    }
    let out = crate::import::import_dir(settings, Path::new("."))?;
    std::fs::create_dir_all(&out)?;
    relift_cache::write_if_changed(&out.join(ROM_FILE), &rom)?;
    let source = crate::paths::canonical(path)?;
    let sha: String = relift_switch_formats::sha256(&rom)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    let text = format!(
        "# The project file, written by relift import.\n\n[game]\nguest = \"gb\"\ntitle = {}\nversion = {}\nimport_dir = {}\nrom = {}\nrom_sha256 = \"{sha}\"\nmodel = \"{}\"\n",
        quote(&title(&rom)),
        rom.get(0x14C).copied().unwrap_or(0),
        quote(&import_dir_value(settings, &out)?),
        quote(&source.display().to_string()),
        default_model(&rom)
    );
    let manifest = Path::new("relift.toml");
    let wrote = crate::projectfile::write_stub(manifest, &text)?;
    ui.summary(&Line::new().paint(Paint::Green, "imported").text(format!(
        " {} ({}, {} bytes, {}), into {}{}, {}",
        title(&rom),
        h.as_ref().map_or("unknown cartridge type", |h| h.mapper.kind.name()),
        rom.len(),
        if is_color(&rom) {
            "Game Boy Color"
        } else {
            "Game Boy"
        },
        out.display(),
        if wrote { "; wrote relift.toml" } else { "" },
        duration(ui.elapsed())
    )));
    Ok(())
}

struct Project {
    rom: Vec<u8>,
    dir: PathBuf,
    source: PathBuf,
    model: String,
}

fn project(settings: &Settings) -> Result<Project, BoxError> {
    let text = std::fs::read_to_string("relift.toml").map_err(|e| format!("relift.toml: {e}"))?;
    let t: toml::Table = text.parse().map_err(|e| format!("relift.toml: {e}"))?;
    let g = t.get("game").ok_or("relift.toml has no [game]")?;
    let s = |k: &str| g.get(k).and_then(|v| v.as_str()).map(String::from);
    let dir = PathBuf::from(s("import_dir").ok_or("relift.toml has no game.import_dir")?);
    let dir = if dir.is_absolute() {
        dir
    } else {
        cache_root(settings)?.join(dir)
    };
    let rom = std::fs::read(dir.join(ROM_FILE)).map_err(|e| {
        format!(
            "{}: {e}; run relift import again",
            dir.join(ROM_FILE).display()
        )
    })?;
    Ok(Project {
        rom,
        dir,
        source: s("rom").map(PathBuf::from).unwrap_or_default(),
        model: s("model").unwrap_or_else(|| "cgb".into()),
    })
}

/// The learned file of the Game Boy project in the working directory,
/// which the next build lifts from.
///
/// # Errors
///
/// No `relift.toml` or no import.
pub fn learned_file(settings: &Settings) -> Result<PathBuf, BoxError> {
    Ok(project(settings)?.dir.join(LEARNED_FILE))
}

/// The ROM with the ROM-hack patches of `mods` applied in order, and the
/// last hack's record.
fn patched(
    rom: Vec<u8>,
    mods: &[PathBuf],
    ui: &mut Ui,
) -> Result<(Vec<u8>, Option<crate::mods::rom::RomHack>), BoxError> {
    let mut data = rom;
    let mut last = None;
    for m in mods {
        let patch = std::fs::read(m).map_err(|e| format!("{}: {e}", m.display()))?;
        let name = m
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        let hack =
            crate::mods::rom::apply(&data, &name, &patch, None, &crate::mods::rom::gb_identity)?;
        for w in &hack.warnings {
            ui.note(&Line::new().paint(Paint::Yellow, w.clone()));
        }
        data.clone_from(&hack.data);
        last = Some(hack);
    }
    Ok((data, last))
}

/// The cache folder of a device library build: the release flavor, which
/// leaves rustyboi's CPU interpreter out, is a build of its own.
fn device_target_dir(
    settings: &Settings,
    triple: Option<&str>,
    release: bool,
) -> Result<PathBuf, BoxError> {
    let flavor = if release { "-lifted" } else { "" };
    Ok(cache_root(settings)?.join(match triple {
        Some(t) => format!("gb-devices{flavor}-{t}"),
        None => format!("gb-devices{flavor}"),
    }))
}

fn device_library_path(
    settings: &Settings,
    triple: Option<&str>,
    release: bool,
) -> Result<PathBuf, BoxError> {
    let dir = device_target_dir(settings, triple, release)?;
    let dir = triple.map_or(dir.clone(), |t| dir.join(t));
    Ok(dir.join("release").join(device_library_name(
        cfg!(all(windows, target_env = "msvc")) && triple.is_none(),
    )))
}

fn device_library_name(msvc: bool) -> &'static str {
    if msvc {
        "relift_gb_devices.lib"
    } else {
        "librelift_gb_devices.a"
    }
}

/// The device library, built with cargo from `guests/gb/devices` into the
/// cache the first time and whenever its sources change. A release build's
/// has no CPU interpreter (`--no-default-features`): every instruction of a
/// release program is lifted, and the first one that is not stops it.
fn device_library(
    settings: &Settings,
    rt: &Path,
    triple: Option<&str>,
    release: bool,
) -> Result<PathBuf, BoxError> {
    let manifest = rt
        .join("..")
        .join("guests")
        .join("gb")
        .join("devices")
        .join("Cargo.toml");
    if !manifest.is_file() {
        return Err(format!(
            "{}: the device library sources are missing",
            manifest.display()
        )
        .into());
    }
    let target = device_target_dir(settings, triple, release)?;
    let lib = device_library_path(settings, triple, release)?;
    let mut cmd = relift_proc::command("cargo");
    cmd.args([
        "build",
        "--release",
        "--locked",
        "--quiet",
        "--manifest-path",
    ])
    .arg(&manifest)
    .current_dir(manifest.parent().unwrap_or(rt))
    .env("CARGO_TARGET_DIR", &target);
    if let Some(t) = triple {
        cmd.args(["--target", t]);
    }
    if release {
        cmd.arg("--no-default-features");
    }
    let out = cmd
        .output()
        .map_err(|e| format!("cargo (for the Game Boy device library): {e}"))?;
    if !out.status.success() || !lib.is_file() {
        let err = String::from_utf8_lossy(&out.stderr);
        let hint = match triple {
            Some(t)
                if err.contains("can't find crate for `std`")
                    || err.contains("may not be installed") =>
            {
                format!("; the Rust std for {t} is missing: run `rustup target add {t}`")
            }
            _ => String::new(),
        };
        return Err(format!(
            "building the Game Boy device library failed ({}){hint}\n{}",
            out.status,
            err.trim_end()
        )
        .into());
    }
    Ok(lib)
}

/// What a build produced.
pub struct Built {
    /// The program.
    pub exe: PathBuf,
    /// The ROM the save belongs next to.
    pub source: PathBuf,
    /// The title the game goes by, empty when it has none.
    pub title: String,
    /// The build for another target, when there is one.
    pub cross: Option<cross::Build>,
}

/// `relift build` of the Game Boy project in the working directory, with
/// the ROM-hack patches `mods` (IPS, UPS or BPS) applied in order. A
/// release build lifts what the analysis finds and nothing more: it leaves
/// the learned file of earlier dev runs out.
///
/// # Errors
///
/// No import, a patch that does not apply, a ROM the analysis refuses, or
/// a failed compile or link.
pub fn build(
    settings: &Settings,
    mods: &[PathBuf],
    release: bool,
    target: Option<cross::Target>,
    ui: &mut Ui,
) -> Result<Built, BoxError> {
    let p = project(settings)?;
    let (name, host_out) = crate::build::project_out(settings)?;
    let cross = target
        .map(|t| cross::Build::prepare(settings, t, &name))
        .transpose()?;
    let (rom, hack) = patched(p.rom, mods, ui)?;
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
    let learn = p.dir.join(LEARNED_FILE);
    let learned = if release {
        relift_gb::program::Learned::default()
    } else {
        std::fs::read_to_string(&learn)
            .map(|t| relift_gb::program::read_learned(&t))
            .unwrap_or_default()
    };
    let seen = &learned.rom;
    let report = match relift_gb_analyze::analyze_with(&rom, seen) {
        Ok(r) => {
            ui.done(
                row,
                Line::new().text(format!(
                    "{}, {} unresolved{}",
                    noun(r.functions.len() as u64, "function"),
                    r.unresolved.len(),
                    if seen.is_empty() {
                        String::new()
                    } else {
                        format!(", {} seen at run time", seen.len())
                    }
                )),
            );
            Some(r)
        }
        Err(e) => {
            ui.done(
                row,
                Line::new().paint(
                    Paint::Yellow,
                    format!("{e}; every instruction runs on the device library's CPU"),
                ),
            );
            None
        }
    };
    ui.begin(row, "lift");
    let image_id = relift_switch_formats::sha256(&rom);
    let lifted = match &report {
        Some(r) => {
            let keys: Vec<u32> = match std::env::var("RELIFT_GB_SUPERSET").as_deref() {
                Ok("all") => std::iter::once(u32::MAX)
                    .chain(1..u32::try_from(rom.len() / 0x4000).unwrap_or(0))
                    .collect(),
                Ok("none") => Vec::new(),
                _ => std::iter::once(u32::MAX)
                    .chain(r.lift_banks.iter().map(|b| u32::from(*b)))
                    .collect(),
            };
            let ram: Vec<(u16, u16)> = learned
                .ram
                .iter()
                .copied()
                .chain(r.ram_ops.ops.iter().map(|o| (o.addr, o.key)))
                .collect::<std::collections::BTreeSet<_>>()
                .into_iter()
                .collect();
            let ram = relift_gb::program::complete_ram_slots(&ram, relift_gb_analyze::MAX_KEYS);
            let mut l = relift_gb::program::lift_superset(&rom, r, &ram, image_id, &keys);
            let live: Vec<_> = r
                .ram_ops
                .live
                .iter()
                .map(|&(bank, addr, k)| (relift_gb_analyze::Loc { bank, addr }, k))
                .collect();
            relift_gb::program::lift_live(&live, image_id, &mut l);
            l.sleds.extend(
                r.ram_ops
                    .sleds
                    .iter()
                    .map(|&(s, e)| (relift_gb::program::RAM_UNIT, s, e)),
            );
            l
        }
        None => relift_gb::program::lift_nothing(image_id),
    };
    let mut files = Vec::new();
    let mut unlifted_list = String::new();
    for u in &lifted.unlifted {
        unlifted_list.push_str(&format!(
            "{:03x}:{:04x}  {:06x}  {}\n",
            u.at.bank, u.at.addr, u.failure.insn, u.failure.message
        ));
    }
    for u in &lifted.units {
        let dir = cdir.join(&u.dir);
        std::fs::create_dir_all(&dir)?;
        for f in &u.output.files {
            let path = dir.join(&f.name);
            relift_cache::write_if_changed(&path, f.text.as_bytes())?;
            if f.name.ends_with(".c") {
                files.push(path);
            }
        }
        let prelude = dir.join(relift_emit::PRELUDE_NAME);
        relift_cache::write_if_changed(&prelude, relift_emit::PRELUDE.as_bytes())?;
    }
    let undefined: usize = lifted.units.iter().map(|u| u.output.undefined).sum();
    ui.done(
        row,
        Line::new().text(format!(
            "{} in {}, {} not lifted, {} left to dispatch, {}",
            noun(
                lifted.units.iter().map(|u| u.functions as u64).sum(),
                "function"
            ),
            noun(lifted.units.len() as u64, "unit"),
            lifted.unlifted.len().max(undefined),
            lifted.filler,
            noun(lifted.sleds.len() as u64, "nop sled")
        )),
    );
    let unlifted_file = out.join("unlifted.txt");
    relift_cache::write_if_changed(&unlifted_file, unlifted_list.as_bytes())?;
    let save = if cross.is_some() {
        "battery.sav".to_string()
    } else if p.source.as_os_str().is_empty() {
        String::new()
    } else {
        save_path(&p.source).display().to_string()
    };
    let mut prog = Vec::new();
    relift_cache::write_if_changed(&cdir.join("rom.bin"), &rom)?;
    for (n, text) in [
        ("rom.c", relift_gb::program::rom_c(&rom)),
        (
            "program.c",
            relift_gb::program::program_c(
                &lifted.units,
                &lifted.sleds,
                &lifted.ram,
                &lifted.halt_bug,
                &lifted.live,
                &relift_gb::program::Program {
                    title: &name,
                    model: &p.model,
                    save: &save,
                    entry: cross
                        .as_ref()
                        .map_or(relift_guest::Entry::Main, cross::Build::entry),
                    learn: &learn.display().to_string(),
                },
            ),
        ),
    ] {
        let path = cdir.join(n);
        relift_cache::write_if_changed(&path, text.as_bytes())?;
        prog.push(path);
    }
    let platform = if is_color(&rom) { "gbc" } else { "gb" };
    let mut ports = relift_input::native_ports(platform)?;
    if cross.is_some()
        && let Some(t) = relift_input::touchpad_port(platform)
    {
        ports.add_port(t)?;
    }
    let game_settings = crate::game_settings::read(Path::new("relift.toml"))?;
    let mods_section = relift_info::Mods {
        code: hack
            .iter()
            .filter_map(|h| h.identity.patch.as_ref())
            .map(|p| relift_info::CodeMod {
                name: p.name.clone(),
                hash: p.hash.clone(),
            })
            .collect(),
        ..relift_info::Mods::default()
    };
    let mut doc = crate::info::handheld_document(
        &name,
        &rom,
        false,
        game_settings.clone(),
        &ports,
        mods_section.clone(),
        hack.as_ref(),
    )
    .or_else(|_| {
        let mut fixed = rom.clone();
        if let Some(b) = fixed.get_mut(0x147) {
            *b = 0;
        }
        crate::info::handheld_document(
            &name,
            &fixed,
            false,
            game_settings.clone(),
            &ports,
            mods_section,
            hack.as_ref(),
        )
    })?;
    crate::kind::stamp(&mut doc)?;
    let info_c = cdir.join(crate::info::C_FILE);
    relift_cache::write_if_changed(&info_c, crate::info::c_file(&doc).as_bytes())?;
    prog.push(info_c);
    prog.extend(crate::winres::write(
        &cdir,
        doc.identity.title(&[]).unwrap_or(&name),
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
        noun(
            lifted.units.iter().map(|u| u.functions as u64).sum(),
            "function"
        ),
        lifted.unlifted.len(),
        duration(ui.elapsed())
    )));
    ui.summary(&Line::new().text(format!("  output: {}", exe.display())));
    Ok(Built {
        exe,
        source: p.source,
        title: doc.identity.title(&[]).unwrap_or(&name).to_string(),
        cross,
    })
}

/// The Game Boy runtime: compiled from a copy in the cache, with the
/// device library the program links.
const GB_LAYERS: Layers = Layers {
    dirs: &["core", "cpus/sm83", "guests/gb/exact"],
    files: &["guests/gb/ports_gen.c", "guests/gb/touchpad.c"],
    mirror: &[
        "core",
        "cpus/sm83",
        "guests/gb",
        "host",
        "third_party/renderdoc",
    ],
    lifted_inc: &[
        "core/include",
        "cpus/sm83/include",
        "guests/gb/exact/include",
    ],
    rt_inc: &[
        "core/include",
        "core",
        "cpus/sm83/include",
        "cpus/sm83",
        "guests/gb",
        "guests/gb/exact",
        "guests/gb/exact/include",
    ],
    lifted_opt: LIFTED_OPT,
    cache: "gb-runtime",
    rt_defines: lifted_only,
    libraries: Some(devices),
    ..Layers::BASE
};

/// A release program's runtime runs lifted code only.
fn lifted_only(release: bool) -> Vec<String> {
    release
        .then(|| "-DRL_GB_LIFTED_ONLY".to_string())
        .into_iter()
        .collect()
}

fn devices(
    settings: &Settings,
    rt: &Path,
    triple: Option<&str>,
    release: bool,
) -> Result<Vec<PathBuf>, BoxError> {
    Ok(vec![device_library(settings, rt, triple, release)?])
}

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
        &GB_LAYERS,
        (release, cross),
        ui,
    )
}

/// `relift run` of the Game Boy project: builds, then runs the program
/// with its save next to the imported ROM. Returns the exit code.
///
/// # Errors
///
/// As [`build`], or the program cannot start.
pub fn run(settings: &Settings, ui: &mut Ui) -> Result<i32, BoxError> {
    let built = build(settings, &[], false, None, ui)?;
    let mut cmd = relift_proc::command(&built.exe);
    if !built.source.as_os_str().is_empty() {
        cmd.arg("--save").arg(save_path(&built.source));
    }
    let status = cmd.status()?;
    Ok(status.code().unwrap_or(128))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_device_library_is_a_lib_only_for_the_msvc_abi() {
        assert_eq!(device_library_name(true), "relift_gb_devices.lib");
        assert_eq!(device_library_name(false), "librelift_gb_devices.a");
    }

    #[test]
    fn the_save_sits_next_to_the_rom() {
        assert_eq!(
            save_path(Path::new("/games/Some Game (USA).gbc")),
            Path::new("/games/Some Game (USA).sav")
        );
    }

    #[test]
    fn a_header_without_a_known_name_or_the_logo_is_not_a_rom() {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../target/tmp")
            .join(format!("gb-is-rom-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let mut rom = vec![0u8; 0x8000];
        rom[0x147] = 0;
        let bin = d.join("x.bin");
        let gb = d.join("x.gb");
        relift_cache::write_if_changed(&bin, &rom).unwrap();
        relift_cache::write_if_changed(&gb, &rom).unwrap();
        assert!(!is_rom(&bin));
        assert!(is_rom(&gb));
        assert!(!is_rom(&d));
        std::fs::remove_dir_all(&d).unwrap();
    }
}

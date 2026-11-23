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

use relift_build::{Compile, Plan};
use relift_progress::{Line, Paint, duration, noun};

use crate::cross;
use crate::import::{BoxError, cache_root, import_dir_value};
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

fn c_files(dir: &Path) -> Result<Vec<PathBuf>, BoxError> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir)
        .map_err(|e| format!("{}: {e}", dir.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "c" || x == "S"))
        .collect();
    v.sort();
    Ok(v)
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
    let doc = crate::info::handheld_document(
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

fn compile(
    settings: &Settings,
    out: &Path,
    cdir: &Path,
    lifted: &[PathBuf],
    prog: &[PathBuf],
    (release, cross): (bool, Option<&cross::Build>),
    ui: &mut Ui,
) -> Result<(), BoxError> {
    let rt_real = crate::build::runtime_dir(settings);
    let croot = crate::paths::canonical(cache_root(settings)?)?;
    let rt = {
        let base = croot.join("gb-runtime");
        std::fs::create_dir_all(&base)?;
        let lock_path = base.join("lock");
        relift_cache::write_if_changed(&lock_path, b"")?;
        let lock = std::fs::File::open(&lock_path)?;
        lock.lock()?;
        crate::build::mirror_runtime(
            &rt_real,
            &base.join("src"),
            &[
                "core",
                "cpus/sm83",
                "guests/gb",
                "host",
                "third_party/renderdoc",
            ],
        )?
    };
    let inc = |p: &str| format!("-I{}", rt.join(p).display());
    let cc = match cross {
        Some(c) => c.cc(),
        None if release => crate::build::release_compiler(settings)?,
        None => crate::build::c_compiler(&settings.toolchain)?,
    };
    let zig = cc.len() > 1;
    let base: Vec<String> = ["-std=c11", "-O2", "-ffp-contract=off"]
        .iter()
        .map(|s| s.to_string())
        .chain(zig.then_some("-fno-sanitize=undefined".to_string()))
        .chain(cross.filter(|c| c.pic()).map(|_| "-fPIC".to_string()))
        .collect();
    let static_rt = cfg!(windows).then_some("-DRL_STATIC_RUNTIME".to_string());
    let lifted_flags: Vec<String> = base
        .iter()
        .map(|f| {
            if f == "-O2" {
                crate::build::lifted_opt(release, LIFTED_OPT).into()
            } else {
                f.clone()
            }
        })
        .chain(
            [
                if settings.emit.line_tables {
                    "-gline-tables-only"
                } else {
                    "-g0"
                },
                "-fno-omit-frame-pointer",
            ]
            .map(String::from),
        )
        .chain([
            inc("core/include"),
            inc("cpus/sm83/include"),
            inc("guests/gb/exact/include"),
        ])
        .chain(static_rt.clone())
        .collect();
    let mut extras = match cross {
        Some(c) => c.extras(&rt, false)?,
        None => crate::runtime_link::extras(&rt, &croot)?,
    };
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
            inc("cpus/sm83/include"),
            inc("cpus/sm83"),
            inc("guests/gb"),
            inc("guests/gb/exact"),
            inc("guests/gb/exact/include"),
        ])
        .chain(extras.defines())
        .chain(static_rt)
        .chain((!release && cross.is_none()).then(|| "-DRL_DEV_RUN".to_string()))
        .chain(release.then(|| "-DRL_GB_LIFTED_ONLY".to_string()))
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
    for layer in ["core", "cpus/sm83", "guests/gb/exact"] {
        rt_compiles.extend(c_files(&rt.join(layer))?);
    }
    rt_compiles.push(rt.join("guests/gb/ports_gen.c"));
    rt_compiles.push(rt.join("guests/gb/touchpad.c"));
    let (prog, resources) = crate::winres::split(prog);
    for s in &prog {
        compiles.push(Compile {
            src: s.clone(),
            obj: obj(s, "prog"),
            flags: rt_flags
                .iter()
                .cloned()
                .chain([crate::build::build_dir_flag(out)])
                .collect(),
            pool: None,
            stage: 0,
        });
    }
    let jobs = relift_build::job_count(settings.jobs);
    let ninja = crate::build::tool(&settings.toolchain.ninja, "ninja");
    let shared = cross.is_some() || !cfg!(windows);
    let key: String = relift_switch_formats::sha256(
        format!(
            "{cc:?}\n{:?}",
            rt_flags
                .iter()
                .filter(|f| !f.starts_with("-I"))
                .collect::<Vec<_>>()
        )
        .as_bytes(),
    )[..8]
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    let rt_dir = croot.join("gb-runtime").join(&key);
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
    let triple = cross.map(cross::Build::triple);
    let devices = device_library(settings, &rt_real, triple, release)?;
    let output = out.join(match cross {
        Some(c) => c.program(&crate::progname::stem()),
        None => crate::progname::program(),
    });
    let row = ui.row("");
    ui.begin(row, "compile");
    let mut link: Vec<String> = Vec::new();
    let mut n = 0;
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
            asm: None,
            compiles: rt_plan_compiles,
            output: rt_o.clone(),
            link_flags: ["-r", "-nostdlib"].map(String::from).into(),
            pools,
            root: Some(croot.clone()),
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
        let exe = output.clone();
        let time = |p: &Path| std::fs::metadata(p).and_then(|m| m.modified()).ok();
        let newer = [
            rt_o.as_path(),
            &device_library_path(settings, triple, release)?,
        ]
        .iter()
        .any(|p| time(p) > time(&exe));
        if newer && exe.exists() {
            std::fs::remove_file(&exe)?;
        }
        link.push(rt_o.display().to_string());
    } else {
        compiles.extend(rt_plan_compiles);
    }
    let pools = crate::build::memory_pools(&mut compiles, jobs);
    let libs: Vec<String> = if let Some(c) = cross {
        c.libs(&crate::progname::stem())
    } else if cfg!(windows) {
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
        let mut l: Vec<String> = ["-lpthread", "-lm", "-ldl"].map(String::from).into();
        if cc.len() > 1 && cfg!(target_os = "linux") {
            l.push("-lunwind".into());
        }
        l
    };
    let plan = Plan {
        cc: cc.clone(),
        asm: None,
        compiles,
        output,
        link_flags: link
            .into_iter()
            .chain(resources)
            .chain(std::iter::once(devices.display().to_string()))
            .chain(libs)
            .chain(extras.libs())
            .collect(),
        pools,
        root: Some(croot.clone()),
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

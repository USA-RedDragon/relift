//! The Game Boy Advance stage: `relift import` of a cartridge ROM, `relift
//! build` of a native program from it, and `relift run`.
//!
//! The import copies the ROM into the project's import directory and writes
//! `game.toml` with `guest = "gba"`. The build finds the code
//! (`relift-gba-analyze`), lifts it into one unit (`relift-gba`), and links
//! it with the runtime's core, ARMv4T and GBA layers, for this host or for
//! another target (`relift build --target`), where it enters from the app
//! glue and shows the on-screen controller on a touch screen. `relift run`
//! opens the program's window.

use std::path::{Path, PathBuf};

use relift_build::{Compile, Plan};
use relift_progress::{Line, Paint, duration, noun};

use crate::cross;
use crate::gb::Built;
use crate::import::{BoxError, cache_root};
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

/// Whether the project's `game.toml` names the Game Boy Advance guest.
#[must_use]
pub fn is_project() -> bool {
    std::fs::read_to_string("game.toml")
        .ok()
        .and_then(|t| t.parse::<toml::Table>().ok())
        .and_then(|t| {
            t.get("game")
                .and_then(|g| g.get("guest"))
                .and_then(|g| g.as_str())
                .map(|g| g == "gba")
        })
        .unwrap_or(false)
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
/// directory or `game.toml` cannot be written.
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
        "# game.toml, written by relift import.\n\n[game]\nguest = \"gba\"\ntitle = {}\ngame_code = {}\nversion = {}\nimport_dir = {}\nrom = {}\nrom_sha256 = \"{sha}\"\n",
        quote(&h.title),
        quote(&h.game_code),
        h.version,
        quote(&out.display().to_string()),
        quote(&source.display().to_string()),
    );
    let manifest = Path::new("game.toml");
    let wrote = !manifest.exists() && relift_cache::write_if_changed(manifest, text.as_bytes())?;
    ui.summary(&Line::new().paint(Paint::Green, "imported").text(format!(
        " {} ({}, {} bytes, Game Boy Advance), into {}{}, {}",
        h.title,
        h.game_code,
        rom.len(),
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
    let hints = rom_hints(&crate::hints::read(Path::new("game.toml"))?);
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
    let game_settings = crate::game_settings::read(Path::new("game.toml"))?;
    let doc = crate::info::handheld_document(
        &title,
        &rom,
        true,
        game_settings.clone(),
        &ports,
        relift_info::Mods::default(),
        None,
    )?;
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
        Some(c) => out.join(c.program()),
        None => out.join(format!("game{}", std::env::consts::EXE_SUFFIX)),
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

/// What a handheld guest's program compiles: the runtime directories, the
/// include directories of lifted code and of the runtime and program files
/// (below the runtime root), the optimization of lifted code in a dev build
/// (a release build uses [`crate::build::RELEASE_LIFTED_OPT`]) and the name
/// of the shared runtime object's cache.
pub(crate) struct Layers {
    pub(crate) dirs: &'static [&'static str],
    pub(crate) lifted_inc: &'static [&'static str],
    pub(crate) rt_inc: &'static [&'static str],
    pub(crate) lifted_opt: &'static str,
    pub(crate) cache: &'static str,
    /// Whether the layers include the A64 header, which needs 16-byte
    /// compare-and-swap on an x86-64 host.
    pub(crate) cx16: bool,
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

/// Compiles the lifted files `lifted` and the program files `prog` of
/// `cdir` with the runtime layers `layers` and links `game` in `out`.
pub(crate) fn compile_layers(
    settings: &Settings,
    out: &Path,
    cdir: &Path,
    (lifted, prog): (&[PathBuf], &[PathBuf]),
    layers: &Layers,
    (release, cross): (bool, Option<&cross::Build>),
    ui: &mut Ui,
) -> Result<(), BoxError> {
    let rt = crate::build::runtime_dir(settings);
    let croot = crate::paths::canonical(cache_root(settings)?)?;
    let inc = |p: &str| format!("-I{}", rt.join(p).display());
    let cc = match cross {
        Some(c) => c.cc(),
        None if release => crate::build::release_compiler(settings)?,
        None => crate::build::c_compiler(&settings.toolchain)?,
    };
    let zig = cc.len() > 1;
    let cx16 = layers.cx16 && cross.is_none() && cfg!(target_arch = "x86_64");
    let base: Vec<String> = ["-std=c11", "-O2", "-ffp-contract=off"]
        .iter()
        .map(|s| s.to_string())
        .chain(cx16.then(|| "-mcx16".to_string()))
        .chain(zig.then_some("-fno-sanitize=undefined".to_string()))
        .chain(cross.filter(|c| c.pic()).map(|_| "-fPIC".to_string()))
        .collect();
    let static_rt = (cross.is_none() && cfg!(windows)).then_some("-DRL_STATIC_RUNTIME".to_string());
    let lifted_flags: Vec<String> = base
        .iter()
        .map(|f| {
            if f == "-O2" {
                crate::build::lifted_opt(release, layers.lifted_opt).into()
            } else {
                f.clone()
            }
        })
        .chain(
            ["-fno-omit-frame-pointer"]
                .into_iter()
                .chain(cross.is_none().then_some("-gline-tables-only"))
                .map(String::from),
        )
        .chain(layers.lifted_inc.iter().map(|p| inc(p)))
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
            cross
                .is_none()
                .then_some("-g")
                .into_iter()
                .chain([
                    "-fno-omit-frame-pointer",
                    "-D_GNU_SOURCE",
                    "-D_DARWIN_C_SOURCE",
                    "-DRL_BUILDING_RUNTIME",
                    "-DRL_CHECKS",
                ])
                .map(String::from),
        )
        .chain(layers.rt_inc.iter().map(|p| inc(p)))
        .chain(extras.defines())
        .chain(static_rt)
        .chain((!release && cross.is_none()).then(|| "-DRL_DEV_RUN".to_string()))
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
    for layer in layers.dirs {
        rt_compiles.extend(c_files(&rt.join(layer))?);
    }
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
    let key: String =
        relift_switch_formats::sha256(format!("{cc:?}\n{rt_flags:?}\n{}", rt.display()).as_bytes())
            [..8]
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect();
    let rt_dir = croot.join(layers.cache).join(&key);
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
    let exe = out.join(match cross {
        Some(c) => c.program().to_string(),
        None => format!("game{}", std::env::consts::EXE_SUFFIX),
    });
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
    let libs: Vec<String> = if let Some(c) = cross {
        c.libs()
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
        if zig && cfg!(target_os = "linux") {
            l.push("-lunwind".into());
        }
        l
    };
    let plan = Plan {
        cc: cc.clone(),
        asm: None,
        compiles,
        output: exe,
        link_flags: link
            .into_iter()
            .chain(resources)
            .chain(libs)
            .chain(extras.libs())
            .collect(),
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

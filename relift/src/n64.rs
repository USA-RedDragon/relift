//! The Nintendo 64 stage: `relift import` of a cartridge ROM, `relift
//! build` of a native program from it, and `relift run`.
//!
//! The import normalizes the ROM to big-endian, copies it into the
//! project's import directory and writes `relift.toml` with `guest = "n64"`.
//! The build finds the code (`relift-n64-analyze`), lifts each segment into
//! one unit (`relift-n64`), and links them with the runtime's core, VR4300
//! and N64 layers. The program binds each unit when it finds its bytes in
//! RDRAM. `relift run` opens the program's window.

use std::path::{Path, PathBuf};

use relift_build::Compile;
use relift_n64_formats::{ByteOrder, Rom};
use relift_progress::{Line, Paint, duration, noun};

use crate::cross;
use crate::gb::Built;
use crate::import::{BoxError, cache_root};
use crate::layers::{Layers, RuntimeCtx, compile_layers};
use crate::settings::Settings;
use crate::ui::Ui;

/// The optimization of lifted CPU code in a dev build (`relift run`): -O1
/// runs a frame about twice as fast as -O0 and within a few percent of -O2,
/// which builds the most. A release build uses
/// [`crate::build::RELEASE_LIFTED_OPT`].
const LIFTED_OPT: &str = "-O1";

/// The largest cartridge image (the cartridge domain's 252 MiB, rounded up):
/// a larger file is not read to check, so a many-GB game dump is not loaded
/// into memory.
const MAX_ROM: u64 = 256 << 20;

/// The optimization of lifted RSP code: its vector instructions are lane
/// helpers that cost nothing only once inlined, and an RSP task runs them
/// for every vertex and every audio sample.
const RSP_OPT: &str = "-O2";

/// Whether the lifted C file at `src` holds RSP code (`c/rsp<k>/...`).
fn rsp_unit(src: &Path, cdir: &Path) -> bool {
    src.strip_prefix(cdir)
        .ok()
        .and_then(|p| p.components().next())
        .and_then(|c| c.as_os_str().to_str())
        .is_some_and(|c| c.starts_with("rsp"))
}

/// The parallel-rdp pipeline variants of a project, as a run records them
/// in its cache folder: copied into the project, a build warms them on the
/// first launch.
const RDP_VARIANTS: &str = "rdp-variants.txt";

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
    if !path.is_file() || std::fs::metadata(path).is_ok_and(|m| m.len() > MAX_ROM) {
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
/// `relift.toml` cannot be written.
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
        "# The project file, written by relift import.\n\n[game]\nguest = \"n64\"\ntitle = {}\ngame_id = {}\nversion = {}\nboot = {}\nimport_dir = {}\nrom = {}\nrom_sha256 = \"{sha}\"\n",
        quote(&title),
        quote(&game_id),
        h.version,
        quote(cic),
        quote(&out.display().to_string()),
        quote(&source.display().to_string()),
    );
    let manifest = Path::new("relift.toml");
    let wrote = crate::projectfile::write_stub(manifest, &text)?;
    let order = match rom.order {
        ByteOrder::Z64 => "big-endian",
        ByteOrder::V64 => "byte-swapped",
        ByteOrder::N64 => "little-endian",
    };
    ui.summary(&Line::new().paint(Paint::Green, "imported").text(format!(
        " {title} ({game_id}, {} bytes {order}, boot {cic}, Nintendo 64), into {}{}, {}",
        rom.bytes.len(),
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

/// `relift build` of the Nintendo 64 project in the working directory, for
/// this host or for `target`; a `release` build compiles lifted CPU code at
/// [`crate::build::RELEASE_LIFTED_OPT`], a dev build at [`LIFTED_OPT`].
///
/// # Errors
///
/// No import, a ROM the analysis refuses, a target whose tools are missing,
/// or a failed compile or link.
pub fn build(
    settings: &Settings,
    mods: &[PathBuf],
    release: bool,
    target: Option<cross::Target>,
    ui: &mut Ui,
) -> Result<Built, BoxError> {
    if !mods.is_empty() {
        return Err("mods are not supported for Nintendo 64 ROMs yet".into());
    }
    let (bytes, project_title, rom_path) = project_rom()?;
    let rom = Rom::new(&bytes).map_err(|e| format!("{ROM_FILE}: {e}"))?;
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
        title_of(&rom),
        rom.bytes.len() / 1024,
        cross.as_ref().map_or_else(
            || format!("{}-{}", std::env::consts::OS, std::env::consts::ARCH),
            |c| c.name().to_string()
        )
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
    let game_settings = crate::game_settings::read(Path::new("relift.toml"))?;
    let picture = crate::n64_picture::defaults(
        &std::fs::read_to_string("relift.toml").map_err(|e| format!("relift.toml: {e}"))?,
    )?;
    let rdp_variants = match std::fs::read_to_string(RDP_VARIANTS) {
        Ok(s) => s,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => String::new(),
        Err(e) => return Err(format!("{RDP_VARIANTS}: {e}").into()),
    };
    let mut prog = Vec::new();
    for (n, text) in [
        ("rom.c", relift_n64::program::rom_c(&rom.bytes)),
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
                picture: &picture,
                rdp_variants: &rdp_variants,
                launch: cross
                    .as_ref()
                    .map_or(relift_guest::Entry::Main, cross::Build::entry),
            }),
        ),
    ] {
        let path = cdir.join(n);
        relift_cache::write_if_changed(&path, text.as_bytes())?;
        prog.push(path);
    }
    let ports = relift_input::native_ports("n64")?;
    let mut doc = crate::info::n64_document(&title, &rom, game_settings.clone(), &picture, &ports);
    crate::kind::stamp(&mut doc)?;
    let info_c = cdir.join(crate::info::C_FILE);
    relift_cache::write_if_changed(&info_c, crate::info::c_file(&doc).as_bytes())?;
    prog.push(info_c);
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
    let notice = crate::build::runtime_dir(settings).join("guests/n64/rdp/NOTICE");
    std::fs::create_dir_all(out.join("licenses"))?;
    relift_cache::write_if_changed(
        &out.join("licenses").join("parallel-rdp.txt"),
        &std::fs::read(&notice).map_err(|e| format!("{}: {e}", notice.display()))?,
    )?;
    let exe = match &cross {
        Some(c) => out.join(c.program(&crate::progname::stem())),
        None => out.join(crate::progname::program()),
    };
    ui.summary(&Line::new().paint(Paint::Green, "built").text(format!(
        " {}, {} instructions not lifted, {} in all",
        noun((lifted.functions() + rsp.functions()) as u64, "function"),
        lifted.unlifted.len() + rsp.unlifted.len(),
        duration(ui.elapsed())
    )));
    ui.summary(&Line::new().text(format!("  output: {}", exe.display())));
    Ok(Built {
        exe,
        source: PathBuf::from(rom_path),
        title: doc.identity.title(&[]).unwrap_or(&title).to_string(),
        cross,
    })
}

/// The Nintendo 64 runtime, with parallel-rdp's C++ in its object.
const N64_LAYERS: Layers = Layers {
    dirs: &LAYERS,
    lifted_inc: &[
        "core/include",
        "cpus/mips/include",
        "cpus/rsp/include",
        "guests/n64/run/include",
    ],
    rt_inc: &[
        "core/include",
        "core",
        "cpus/mips/include",
        "cpus/mips",
        "cpus/rsp/include",
        "cpus/rsp",
        "guests/n64",
        "guests/n64/run",
        "guests/n64/run/include",
    ],
    lifted_opt: LIFTED_OPT,
    cache: "n64-runtime",
    release_compiler: false,
    cross_vk: true,
    cxx: true,
    lifted_defines: trace,
    rt_defines: trace,
    file_opt: rsp_opt,
    runtime_extra: Some(parallel_rdp),
    ..Layers::BASE
};

/// `RELIFT_N64_TRACE` set (and not `0`) builds the CPU trace.
fn trace(_: bool) -> Vec<String> {
    std::env::var_os("RELIFT_N64_TRACE")
        .is_some_and(|v| !v.is_empty() && v != "0")
        .then(|| "-DRL_N64_TRACE".to_string())
        .into_iter()
        .collect()
}

fn rsp_opt(src: &Path, cdir: &Path) -> Option<&'static str> {
    rsp_unit(src, cdir).then_some(RSP_OPT)
}

/// parallel-rdp's sources, its glue and volk, compiled into the runtime
/// object with its symbols renamed.
fn parallel_rdp(ctx: &RuntimeCtx<'_>) -> Result<Vec<Compile>, BoxError> {
    let RuntimeCtx {
        settings,
        rt,
        rt_dir,
        zig,
        ref pic,
        ref static_rt,
    } = *ctx;
    let pic = pic.clone();
    let inc = |p: &str| format!("-I{}", rt.join(p).display());
    let mut rt_plan_compiles = Vec::new();
    let prdp = crate::parallel_rdp::ensure(&cache_root(settings)?)?;
    let pinc = crate::parallel_rdp::INCLUDES.map(|p| format!("-I{}", prdp.join(p).display()));
    let cxx: Vec<String> = ["-x", "c++", "-std=c++17", "-O2", "-g0", "-pthread"]
        .map(String::from)
        .into_iter()
        .chain(
            zig.then(|| ["-fno-sanitize=undefined", "-lc++"].map(String::from))
                .into_iter()
                .flatten(),
        )
        .chain(pic.clone())
        .chain(pinc.clone())
        .chain([inc("core/include"), inc("guests/n64")])
        .chain(static_rt.clone())
        .collect();
    let names = rt_dir.join("prdp_names.h");
    relift_cache::write_if_changed(
        &names,
        crate::parallel_rdp::renames_header(&prdp)?.as_bytes(),
    )?;
    let renamed = vec!["-include".to_string(), names.display().to_string()];
    let no_load: Vec<String> = [
        "-include".to_string(),
        rt.join("guests/n64/rdp/noload.h").display().to_string(),
    ]
    .into_iter()
    .chain(renamed.clone())
    .collect();
    let pobj = |s: &Path, root: &Path| {
        let stem = s.strip_prefix(root).unwrap_or(s);
        rt_dir
            .join("obj")
            .join("prdp")
            .join(stem)
            .with_extension("o")
    };
    for src in crate::parallel_rdp::SOURCES.iter().map(|p| prdp.join(p)) {
        rt_plan_compiles.push(Compile {
            obj: pobj(&src, &prdp),
            src,
            flags: cxx.iter().cloned().chain(no_load.clone()).collect(),
            pool: None,
            stage: 0,
        });
    }
    let glue = rt.join("guests/n64/rdp/prdp.cpp");
    rt_plan_compiles.push(Compile {
        obj: pobj(&glue, rt),
        src: glue,
        flags: cxx.into_iter().chain(renamed).collect(),
        pool: None,
        stage: 0,
    });
    let volk = prdp.join(crate::parallel_rdp::VOLK);
    rt_plan_compiles.push(Compile {
        obj: pobj(&volk, &prdp),
        src: volk,
        flags: ["-std=c11", "-O2"]
            .map(String::from)
            .into_iter()
            .chain(no_load)
            .chain(zig.then_some("-fno-sanitize=undefined".to_string()))
            .chain(pic)
            .chain(pinc)
            .collect(),
        pool: None,
        stage: 0,
    });
    Ok(rt_plan_compiles)
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
        &N64_LAYERS,
        (release, cross),
        ui,
    )
}

/// `relift run` of the Nintendo 64 project: builds, then runs the program.
/// Returns the exit code.
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
    fn bytes_without_a_magic_are_not_a_rom() {
        assert!(!is_rom_image(&[0u8; 0x1000]));
        assert!(!is_rom_image(&[0x80, 0x37, 0x12, 0x40]));
        let rom = relift_n64_formats::synth::rom(&relift_n64_formats::synth::Spec::default());
        assert!(is_rom_image(&rom));
        let v64 = ByteOrder::V64.encode(&rom).expect("encodes");
        assert!(is_rom_image(&v64));
    }
}

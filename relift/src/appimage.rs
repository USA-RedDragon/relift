//! The Linux release output: an AppImage of the game that runs on a
//! mainstream distribution with nothing installed beyond glibc and the
//! system's graphics stack.
//!
//! `relift build` makes the program as before, then lays out an AppDir
//! (`AppRun`, the desktop file, the icon, `usr/bin/<game>`, the shared
//! libraries it needs under `usr/lib`, the self-description, the licenses
//! of what is bundled, and the romfs and DLC images a Switch game reads) and
//! gives it to `appimagetool`, which adds the type 2 runtime and a squashfs
//! image.
//!
//! Never bundled, always the system's: glibc, the Vulkan loader and the GPU
//! drivers, X11, Wayland, the audio servers (SDL3 loads those itself when it
//! runs). Saves, settings and the Switch SD card go to the per-user folder,
//! never into the AppImage.

use std::path::{Path, PathBuf};

use relift_info::Info;
use relift_progress::{Line, Paint};

use crate::elfdeps;
use crate::import::BoxError;
use crate::settings::Settings;
use crate::ui::Ui;

/// Where the self-description sits inside the AppImage.
pub const INFO_PATH: &str = "usr/share/relift/info.json";

const LICENSE_RELIFT: &str = include_str!("../../LICENSE");

/// Libraries that always come from the system: the C library family, the
/// compiler runtimes, the graphics stack and the display servers' clients.
const SYSTEM: &[&str] = &[
    "ld-linux",
    "ld64.so",
    "libc.so",
    "libm.so",
    "libdl.so",
    "libpthread.so",
    "librt.so",
    "libutil.so",
    "libresolv.so",
    "libnsl.so",
    "libcrypt.so",
    "libanl.so",
    "libgcc_s.so",
    "libstdc++.so",
    "libvulkan.so",
    "libGL.so",
    "libGLX.so",
    "libGLdispatch.so",
    "libOpenGL.so",
    "libEGL.so",
    "libGLESv2.so",
    "libdrm.so",
    "libgbm.so",
    "libX11.so",
    "libXext.so",
    "libxcb.so",
    "libwayland-client.so",
    "libwayland-cursor.so",
    "libwayland-egl.so",
    "libasound.so",
    "libpulse.so",
    "libpipewire-0.3.so",
    "libudev.so",
    "libdbus-1.so",
];

/// A bundled library the licenses file can name: file name prefix,
/// component, SPDX license, home page, and the directories where a
/// distribution keeps its license texts.
struct Known {
    prefix: &'static str,
    component: &'static str,
    license: &'static str,
    url: &'static str,
    texts: &'static [&'static str],
}

const KNOWN: &[Known] = &[
    Known {
        prefix: "libSDL3",
        component: "SDL3",
        license: "Zlib",
        url: "https://libsdl.org",
        texts: &[
            "/usr/share/licenses/sdl3",
            "/usr/share/doc/libsdl3-0",
            "/usr/share/doc/libsdl3",
        ],
    },
    Known {
        prefix: "libopus",
        component: "Opus",
        license: "BSD-3-Clause",
        url: "https://opus-codec.org",
        texts: &[
            "/usr/share/licenses/opus",
            "/usr/share/doc/libopus0",
            "/usr/share/doc/libopus",
        ],
    },
];

/// What `relift build` asks of packaging.
pub struct Request<'a> {
    /// The project name.
    pub name: &'a str,
    /// The built program.
    pub exe: &'a Path,
    /// The import directory of a Switch game, whose romfs and DLC go into
    /// the AppImage as plain images (crate::gamedata).
    pub import: Option<&'a Path>,
}

/// When to build the AppImage.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Policy {
    /// When `appimagetool` is found; say so when it is not.
    Auto,
    /// Always: no `appimagetool` is an error.
    Always,
    /// Never.
    Never,
}

impl Policy {
    /// The setting's value.
    ///
    /// # Errors
    ///
    /// The value is none of `auto`, `always`, `never`.
    pub fn parse(s: &str) -> Result<Policy, String> {
        match s {
            "auto" => Ok(Policy::Auto),
            "always" => Ok(Policy::Always),
            "never" => Ok(Policy::Never),
            other => Err(format!(
                "package.appimage: {other:?} is not auto, always or never"
            )),
        }
    }
}

pub(crate) fn slug(name: &str) -> String {
    let s: String = name
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.') {
                c
            } else {
                '-'
            }
        })
        .collect();
    let s = s.trim_matches(|c| c == '-' || c == '.').to_string();
    if s.is_empty() { "game".into() } else { s }
}

pub(crate) fn display_name(info: &Info, fallback: &str) -> String {
    let pick = info.identity.title(&[]).unwrap_or(fallback);
    let clean: String = pick.chars().filter(|c| !c.is_control()).collect();
    let clean = clean.trim();
    if clean.is_empty() {
        fallback.to_string()
    } else {
        clean.to_string()
    }
}

fn is_system(soname: &str) -> bool {
    SYSTEM.iter().any(|p| soname.starts_with(p))
}

fn default_search() -> Vec<PathBuf> {
    let multiarch = format!("{}-linux-gnu", std::env::consts::ARCH);
    [
        "/lib64".to_string(),
        "/usr/lib64".to_string(),
        format!("/lib/{multiarch}"),
        format!("/usr/lib/{multiarch}"),
        "/usr/lib".to_string(),
        "/lib".to_string(),
        "/usr/local/lib".to_string(),
    ]
    .into_iter()
    .map(PathBuf::from)
    .collect()
}

/// A shared library to bundle: the name the loader asks for and the file.
#[derive(Debug, PartialEq, Eq)]
pub struct Bundled {
    pub soname: String,
    pub path: PathBuf,
}

/// The libraries `exe` needs that the system cannot be relied on to have,
/// transitively, found along the search path of each file, then
/// `LD_LIBRARY_PATH`, then the standard directories.
///
/// # Errors
///
/// A file is not a readable ELF file, or a needed library is nowhere.
pub fn resolve(exe: &Path, extra: &[PathBuf]) -> Result<Vec<Bundled>, BoxError> {
    let mut found: Vec<Bundled> = Vec::new();
    let mut todo = vec![exe.to_path_buf()];
    let mut seen = std::collections::BTreeSet::new();
    while let Some(file) = todo.pop() {
        let bytes = std::fs::read(&file).map_err(|e| format!("{}: {e}", file.display()))?;
        let deps = elfdeps::read(&bytes).map_err(|e| format!("{}: {e}", file.display()))?;
        let mut dirs: Vec<PathBuf> = deps.search.iter().map(PathBuf::from).collect();
        dirs.extend(extra.iter().cloned());
        dirs.extend(default_search());
        for need in deps.needed {
            if is_system(&need) || !seen.insert(need.clone()) {
                continue;
            }
            let path = dirs
                .iter()
                .map(|d| d.join(&need))
                .find(|p| p.is_file())
                .ok_or_else(|| {
                    format!(
                        "{} needs {need}, which is not installed here; install it, or point the build at it with LD_LIBRARY_PATH",
                        file.display()
                    )
                })?;
            let real = std::fs::canonicalize(&path)?;
            todo.push(real.clone());
            found.push(Bundled {
                soname: need,
                path: real,
            });
        }
    }
    found.sort_by(|a, b| a.soname.cmp(&b.soname));
    Ok(found)
}

fn shell_quote(s: &str) -> String {
    format!("'{}'", s.replace('\'', "'\\''"))
}

/// The `AppRun` script: puts the bundled libraries on the loader's path,
/// points the program at the per-user folder for saves and at the shipped
/// data, and starts it with the user's arguments.
#[must_use]
pub fn app_run(slug: &str, platform: &str, has_romfs: bool, has_dlc: bool) -> String {
    let q = shell_quote(slug);
    let mut s = String::from("#!/bin/sh\n");
    s.push_str("HERE=$(dirname \"$(readlink -f \"$0\")\")\n");
    s.push_str("export LD_LIBRARY_PATH=\"$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n");
    s.push_str(&format!(
        "case \"${{1:-}}\" in --relift-info) exec \"$HERE/usr/bin/\"{q} \"$@\" ;; esac\n"
    ));
    s.push_str(&format!(
        "DATA=\"${{XDG_DATA_HOME:-${{HOME:-.}}/.local/share}}/\"{q}\n"
    ));
    let mut args = String::new();
    match platform {
        "switch" => {
            s.push_str("mkdir -p \"$DATA/save\" \"$DATA/sdmc\"\n");
            s.push_str(": \"${RELIFT_SAVE:=$DATA/save}\"\n");
            s.push_str(": \"${RELIFT_SDMC:=$DATA/sdmc}\"\n");
            s.push_str("export RELIFT_SAVE RELIFT_SDMC\n");
        }
        "gba" => {
            s.push_str("mkdir -p \"$DATA/save\"\n");
            s.push_str(": \"${RELIFT_SAVE:=$DATA/save}\"\n");
            s.push_str("export RELIFT_SAVE\n");
        }
        "gb" | "gbc" => {
            s.push_str("mkdir -p \"$DATA\"\n");
            args.push_str(&format!(" --save \"$DATA/\"{q}\".sav\""));
        }
        _ => {}
    }
    if has_romfs {
        s.push_str(
            ": \"${RELIFT_ROMFS:=$HERE/usr/share/relift/romfs.img}\"\nexport RELIFT_ROMFS\n",
        );
    }
    if has_dlc {
        s.push_str(": \"${RELIFT_DLC:=$HERE/usr/share/relift/dlc}\"\nexport RELIFT_DLC\n");
    }
    s.push_str(&format!("exec \"$HERE/usr/bin/\"{q}{args} \"$@\"\n"));
    s
}

/// The `.desktop` file.
#[must_use]
pub fn desktop_file(slug: &str, name: &str, platform: &str) -> String {
    let comment = match platform {
        "switch" => "Nintendo Switch game",
        "gb" => "Game Boy game",
        "gbc" => "Game Boy Color game",
        "gba" => "Game Boy Advance game",
        "sms" => "Master System game",
        "gg" => "Game Gear game",
        _ => "Game",
    };
    format!(
        "[Desktop Entry]\nType=Application\nName={name}\nComment={comment}, built with relift\nExec={slug}\nIcon={slug}\nCategories=Game;\nTerminal=false\n"
    )
}

fn licenses_file(name: &str, bundled: &[Bundled]) -> String {
    let mut s = format!(
        "{name}\n\nThis AppImage holds a program built with relift and these third-party components.\nThe game's own code and data belong to their rights holders.\n\n- relift runtime and the code relift generated: MIT (the text is in relift-LICENSE)\n- AppImage type 2 runtime ({}), the start of this file: MIT, {} (the text is in type2-runtime-LICENSE)\n",
        crate::appimage_runtime::TAG,
        crate::appimage_runtime::HOME
    );
    for b in bundled {
        match KNOWN.iter().find(|k| b.soname.starts_with(k.prefix)) {
            Some(k) => s.push_str(&format!(
                "- {} ({}): {}, {}\n",
                k.component, b.soname, k.license, k.url
            )),
            None => s.push_str(&format!(
                "- {} (from {}): license not known to relift; see the package that provides it\n",
                b.soname,
                b.path.display()
            )),
        }
    }
    s.push_str("\nNot bundled, always the system's: the C library, the Vulkan loader, the GPU drivers, X11, Wayland and the audio servers.\n");
    s
}

fn write_licenses(
    root: &Path,
    slug: &str,
    name: &str,
    bundled: &[Bundled],
) -> Result<(), BoxError> {
    let dir = root.join("usr/share/licenses").join(slug);
    std::fs::create_dir_all(&dir)?;
    relift_cache::write_if_changed(
        &dir.join("BUNDLED.txt"),
        licenses_file(name, bundled).as_bytes(),
    )?;
    relift_cache::write_if_changed(&dir.join("relift-LICENSE"), LICENSE_RELIFT.as_bytes())?;
    relift_cache::write_if_changed(
        &dir.join("type2-runtime-LICENSE"),
        crate::appimage_runtime::LICENSE.as_bytes(),
    )?;
    for b in bundled {
        let Some(k) = KNOWN.iter().find(|k| b.soname.starts_with(k.prefix)) else {
            continue;
        };
        let Some(src) = k.texts.iter().map(Path::new).find(|d| d.is_dir()) else {
            continue;
        };
        for e in std::fs::read_dir(src)?.flatten() {
            let n = e.file_name().to_string_lossy().to_lowercase();
            if e.path().is_file()
                && ["license", "copying", "copyright", "notice"]
                    .iter()
                    .any(|w| n.contains(w))
            {
                relift_cache::write_if_changed(
                    &dir.join(format!("{}-{}", k.component, n)),
                    &std::fs::read(e.path())?,
                )?;
            }
        }
    }
    Ok(())
}

/// What a packaged build says about itself.
#[derive(Debug)]
pub struct Packaged {
    pub path: PathBuf,
    pub bundled: Vec<Bundled>,
    pub glibc: Option<(u32, u32)>,
}

fn arch() -> &'static str {
    std::env::consts::ARCH
}

/// Lays out the AppDir of `req` under `dir` and returns what it bundled.
///
/// # Errors
///
/// A needed library is missing, or a file cannot be written.
pub fn layout(req: &Request, info: &Info, dir: &Path) -> Result<Packaged, BoxError> {
    let slug = slug(req.name);
    let name = display_name(info, req.name);
    let platform = info.identity.platform.as_str();
    let _ = std::fs::remove_dir_all(dir);
    let bin = dir.join("usr/bin");
    let lib = dir.join("usr/lib");
    std::fs::create_dir_all(&bin)?;
    std::fs::create_dir_all(&lib)?;
    let exe_dest = bin.join(&slug);
    relift_cache::write_if_changed(&exe_dest, &std::fs::read(req.exe)?)?;
    let ld: Vec<PathBuf> = std::env::var_os("LD_LIBRARY_PATH")
        .map(|p| std::env::split_paths(&p).collect())
        .unwrap_or_default();
    let bundled = resolve(req.exe, &ld)?;
    let mut glibc = elfdeps::read(&std::fs::read(req.exe)?)?.glibc;
    for b in &bundled {
        relift_cache::write_if_changed(&lib.join(&b.soname), &std::fs::read(&b.path)?)?;
        glibc = glibc.max(elfdeps::read(&std::fs::read(&b.path)?)?.glibc);
    }
    let share = dir.join("usr/share/relift");
    std::fs::create_dir_all(&share)?;
    let (romfs, dlc) = match req.import {
        Some(i) => crate::gamedata::write_images(i, &share, false)?,
        None => (false, false),
    };
    relift_cache::write_if_changed(&dir.join(INFO_PATH), &info.to_bytes())?;
    write_licenses(dir, &slug, &name, &bundled)?;
    let run = dir.join("AppRun");
    relift_cache::write_if_changed(&run, app_run(&slug, platform, romfs, dlc).as_bytes())?;
    set_mode(&run, 0o755)?;
    set_mode(&exe_dest, 0o755)?;
    relift_cache::write_if_changed(
        &dir.join(format!("{slug}.desktop")),
        desktop_file(&slug, &name, platform).as_bytes(),
    )?;
    let png = crate::icon::png(info.identity.icon.as_ref(), &name);
    relift_cache::write_if_changed(&dir.join(format!("{slug}.png")), &png)?;
    relift_cache::write_if_changed(&dir.join(".DirIcon"), &png)?;
    Ok(Packaged {
        path: PathBuf::new(),
        bundled,
        glibc,
    })
}

#[cfg(unix)]
pub(crate) fn set_mode(p: &Path, mode: u32) -> std::io::Result<()> {
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(p, std::fs::Permissions::from_mode(mode))
}

#[cfg(not(unix))]
pub(crate) fn set_mode(_: &Path, _: u32) -> std::io::Result<()> {
    Ok(())
}

/// The self-description of the AppImage at `path`, without running the
/// game: the AppImage's own runtime extracts the one file (it needs no FUSE
/// for that).
///
/// # Errors
///
/// The file cannot be run, or holds no description.
pub fn read_info(path: &Path) -> Result<Vec<u8>, BoxError> {
    let dir = std::env::temp_dir().join(format!("relift-info-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir)?;
    let abs = std::fs::canonicalize(path)?;
    let run = relift_proc::command(&abs)
        .args(["--appimage-extract", INFO_PATH])
        .current_dir(&dir)
        .output();
    let doc = std::fs::read(dir.join("squashfs-root").join(INFO_PATH));
    let _ = std::fs::remove_dir_all(&dir);
    match (run, doc) {
        (_, Ok(d)) => Ok(d),
        (Err(e), _) => Err(format!("{}: {e}", path.display()).into()),
        _ => Err(format!(
            "{}: no relift self-description in the AppImage",
            path.display()
        )
        .into()),
    }
}

/// The message for a machine without `appimagetool`.
#[must_use]
pub fn missing_tool() -> String {
    "no AppImage built: appimagetool is not on PATH (get it from https://github.com/AppImage/appimagetool/releases or your package manager, or set toolchain.appimagetool; package.appimage=never silences this)".into()
}

/// Packages the built program of `req` as `<project>.AppImage` beside it.
/// Returns `None` when packaging is off, not for this host, or the tool is
/// missing under the `auto` policy.
///
/// # Errors
///
/// The policy is `always` and the tool is missing, a library cannot be
/// found, or `appimagetool` fails.
pub fn package(
    settings: &Settings,
    req: &Request,
    ui: &mut Ui,
) -> Result<Option<Packaged>, BoxError> {
    let policy = Policy::parse(&settings.package.appimage)?;
    if policy == Policy::Never || !cfg!(target_os = "linux") {
        return Ok(None);
    }
    let Some(tool) = crate::build::tool(&settings.toolchain.appimagetool, "appimagetool") else {
        if policy == Policy::Always {
            return Err(missing_tool().into());
        }
        ui.summary(&Line::new().paint(Paint::Yellow, missing_tool()));
        return Ok(None);
    };
    let image = std::fs::read(req.exe)?;
    let doc =
        relift_info::section::read(&image).map_err(|e| format!("{}: {e}", req.exe.display()))?;
    let info = Info::from_bytes(doc)?;
    let runtime = crate::appimage_runtime::ensure(&crate::import::cache_root(settings)?, arch())?;
    let out = req.exe.parent().ok_or("the program has no folder")?;
    let scratch = out
        .join("appimage")
        .join(format!("build-{}", crate::appimage_runtime::unique()));
    let processors = relift_build::job_count(settings.jobs).clamp(1, 8);
    let result = assemble(&tool, &runtime, req, &info, out, &scratch, processors);
    let _ = std::fs::remove_dir_all(&scratch);
    Ok(Some(result?))
}

/// Lays out and packs one AppImage inside `scratch`, a folder no other build
/// uses (the AppDir, the tool's `TMPDIR` and the unfinished image), then
/// renames the finished image over `<project>.AppImage` in one step.
/// `mksquashfs` gets as many threads as the build has jobs: left alone it
/// starts one set per CPU, and builds running at once exhaust the system's
/// threads (`FATAL ERROR: Failed to create thread`).
fn assemble(
    tool: &Path,
    runtime: &Path,
    req: &Request,
    info: &Info,
    out: &Path,
    scratch: &Path,
    processors: u32,
) -> Result<Packaged, BoxError> {
    let dir = scratch.join("AppDir");
    let tmp = scratch.join("tmp");
    std::fs::create_dir_all(&tmp)?;
    let mut packaged = layout(req, info, &dir)?;
    let slug = slug(req.name);
    let image = scratch.join(format!("{slug}.AppImage"));
    let output = relift_proc::command(tool)
        .args(["--no-appstream", "--comp", "zstd", "--runtime-file"])
        .arg(runtime)
        .args(["--mksquashfs-opt", "-processors", "--mksquashfs-opt"])
        .arg(processors.to_string())
        .arg(&dir)
        .arg(&image)
        .env("ARCH", arch())
        .env("TMPDIR", &tmp)
        .output()
        .map_err(|e| format!("{}: {e}", tool.display()))?;
    if !output.status.success() || !image.is_file() {
        let log = String::from_utf8_lossy(&output.stderr);
        let tail: Vec<&str> = log.lines().rev().take(12).collect();
        return Err(format!(
            "{} failed ({}):\n{}",
            tool.display(),
            output.status,
            tail.into_iter().rev().collect::<Vec<_>>().join("\n")
        )
        .into());
    }
    let target = out.join(format!("{slug}.AppImage"));
    std::fs::rename(&image, &target).map_err(|e| format!("{}: {e}", target.display()))?;
    packaged.path = target;
    Ok(packaged)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_policy_is_one_of_three_words() {
        assert_eq!(Policy::parse("auto"), Ok(Policy::Auto));
        assert_eq!(Policy::parse("always"), Ok(Policy::Always));
        assert_eq!(Policy::parse("never"), Ok(Policy::Never));
        assert!(Policy::parse("yes").is_err());
    }

    #[test]
    fn a_name_becomes_a_safe_file_name() {
        assert_eq!(slug("Some Game (USA)"), "Some-Game--USA");
        assert_eq!(slug("../x"), "x");
        assert_eq!(slug("..."), "game");
    }

    #[test]
    fn the_title_is_the_default_language_then_the_first_present() {
        let mut i = Info::new("0");
        assert_eq!(display_name(&i, "proj"), "proj");
        i.identity.names.insert("default".into(), "D".into());
        assert_eq!(display_name(&i, "proj"), "D");
        i.identity.names.insert("French".into(), "F".into());
        i.identity.default_language = i.identity.first_language();
        assert_eq!(display_name(&i, "proj"), "F");
        i.identity
            .names
            .insert("AmericanEnglish".into(), "A\n".into());
        i.identity.default_language = i.identity.first_language();
        assert_eq!(display_name(&i, "proj"), "A");
    }

    #[test]
    fn system_libraries_are_not_bundled() {
        for s in [
            "libc.so.6",
            "libm.so.6",
            "ld-linux-x86-64.so.2",
            "libvulkan.so.1",
        ] {
            assert!(is_system(s), "{s}");
        }
        for s in ["libSDL3.so.0", "libopus.so.0"] {
            assert!(!is_system(s), "{s}");
        }
    }

    #[test]
    fn app_run_sets_the_per_user_folder_by_platform() {
        let sw = app_run("g", "switch", true, false);
        assert!(sw.contains("RELIFT_SAVE:=$DATA/save"));
        assert!(sw.contains("RELIFT_ROMFS:=$HERE/usr/share/relift/romfs.img"));
        assert!(!sw.contains("RELIFT_DLC"));
        let gb = app_run("g", "gbc", false, false);
        assert!(gb.contains("--save \"$DATA/\"'g'\".sav\""));
        assert!(gb.ends_with("\"$@\"\n"));
        assert!(gb.contains("--relift-info) exec"));
        let odd = app_run("it's", "gba", false, false);
        assert!(odd.contains("'it'\\''s'"));
    }

    #[test]
    fn the_desktop_file_names_the_icon_and_the_command() {
        let d = desktop_file("g", "Game", "gb");
        assert!(d.contains("Exec=g\n") && d.contains("Icon=g\n") && d.contains("Name=Game\n"));
    }

    #[test]
    fn the_licenses_file_names_known_and_unknown_libraries() {
        let b = [
            Bundled {
                soname: "libSDL3.so.0".into(),
                path: "/x/libSDL3.so.0".into(),
            },
            Bundled {
                soname: "libfoo.so.1".into(),
                path: "/x/libfoo.so.1".into(),
            },
        ];
        let t = licenses_file("G", &b);
        assert!(t.contains("SDL3 (libSDL3.so.0): Zlib"));
        assert!(t.contains("libfoo.so.1") && t.contains("not known"));
    }

    #[cfg(all(target_os = "linux", target_env = "gnu"))]
    #[test]
    fn the_layout_holds_the_program_the_launcher_files_and_the_description() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../target/tmp/appimage-layout");
        let _ = std::fs::remove_dir_all(&root);
        let data = root.join("import");
        std::fs::create_dir_all(data.join("romfs")).unwrap();
        relift_cache::write_if_changed(&data.join("romfs/a.bin"), b"x").unwrap();
        let mut info = Info::new("0");
        info.identity.platform = "switch".into();
        info.identity
            .names
            .insert("default".into(), "My Game".into());
        let exe = std::env::current_exe().unwrap();
        let req = Request {
            name: "my game",
            exe: &exe,
            import: Some(&data),
        };
        let out = root.join("AppDir");
        let p = layout(&req, &info, &out).unwrap();
        assert!(p.bundled.is_empty());
        for f in [
            "AppRun",
            "my-game.desktop",
            "my-game.png",
            ".DirIcon",
            "usr/bin/my-game",
            "usr/share/relift/romfs.img",
            "usr/share/licenses/my-game/BUNDLED.txt",
            "usr/share/licenses/my-game/relift-LICENSE",
            "usr/share/licenses/my-game/type2-runtime-LICENSE",
            INFO_PATH,
        ] {
            assert!(out.join(f).exists(), "{f}");
        }
        assert_eq!(std::fs::read(out.join(INFO_PATH)).unwrap(), info.to_bytes());
        let d = std::fs::read_to_string(out.join("my-game.desktop")).unwrap();
        assert!(d.contains("Name=My Game"));
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[cfg(all(target_os = "linux", target_env = "gnu"))]
    #[test]
    fn the_test_binary_needs_nothing_to_bundle() {
        let exe = std::env::current_exe().unwrap();
        assert!(resolve(&exe, &[]).unwrap().is_empty());
    }
}

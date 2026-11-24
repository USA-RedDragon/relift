//! The macOS release output: `<project>.app`, signed and optionally
//! notarized.
//!
//! The program goes into `Contents/MacOS` behind a launcher script that sets
//! the per-user folders, the libraries it links (SDL3, MoltenVK and what
//! they link, from outside `/usr/lib` and `/System`) into
//! `Contents/Frameworks` with their install names rewritten to `@rpath`, and
//! the import's romfs and DLC as plain images (crate::gamedata) into
//! `Contents/Resources`, beside the
//! game's icon (`AppIcon.icns`); `Info.plist` names the bundle with the
//! game's title. Everything is
//! then signed inside out: with the configured identity under the hardened
//! runtime and a secure timestamp, or ad hoc when there is none. The program
//! needs no entitlement: it never generates code, and the libraries inside
//! the bundle carry the same signature, so library validation holds.
//! Notarization submits a zip of the bundle and staples the ticket to the
//! bundle itself, so it opens offline.

use std::path::{Path, PathBuf};
use std::process::Command;

use relift_info::Info;
use relift_progress::{Line, Paint};

use crate::import::BoxError;
use crate::settings::Settings;
use crate::ui::Ui;

pub use crate::appimage::Request;

/// What a packaged build says about itself.
#[derive(Debug)]
pub struct Packaged {
    pub path: PathBuf,
    pub bundled: Vec<String>,
    pub signed_with: Option<String>,
    pub notarized: bool,
}

/// How to sign.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Signing {
    pub identity: Option<String>,
    pub keychain: Option<PathBuf>,
}

/// The App Store Connect API key notarization signs in with.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Notary {
    pub key: PathBuf,
    pub key_id: String,
    pub issuer: String,
}

/// The signing and notarization the settings ask for.
///
/// # Errors
///
/// Notarization is on without an identity or without the API key.
pub fn plan(settings: &Settings) -> Result<(Signing, Option<Notary>), String> {
    let p = &settings.package;
    let t = &settings.toolchain;
    plan_of(
        &t.codesign_identity,
        &t.codesign_keychain,
        p.notarize
            .then_some((&p.notarize_key, &p.notarize_key_id, &p.notarize_issuer)),
    )
}

fn plan_of(
    identity: &str,
    keychain: &Path,
    notarize: Option<(&PathBuf, &String, &String)>,
) -> Result<(Signing, Option<Notary>), String> {
    let identity = Some(identity.trim().to_string()).filter(|s| !s.is_empty() && s != "-");
    let keychain = Some(keychain.to_path_buf()).filter(|p| !p.as_os_str().is_empty());
    let signing = Signing { identity, keychain };
    let Some((key, key_id, issuer)) = notarize else {
        return Ok((signing, None));
    };
    if signing.identity.is_none() {
        return Err("package.notarize needs toolchain.codesign-identity (a Developer ID)".into());
    }
    if key.as_os_str().is_empty() || key_id.is_empty() || issuer.is_empty() {
        return Err(
            "package.notarize needs package.notarize-key, package.notarize-key-id and package.notarize-issuer"
                .into(),
        );
    }
    let notary = Notary {
        key: key.clone(),
        key_id: key_id.clone(),
        issuer: issuer.clone(),
    };
    Ok((signing, Some(notary)))
}

fn xml(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

/// The game's icon in the bundle's `Contents/Resources`.
const ICON_FILE: &str = "AppIcon.icns";

/// The bundle's `Info.plist`. A game gets the games category
/// (`LSApplicationCategoryType` `public.app-category.games`) and
/// `LSSupportsGameMode`, which makes macOS turn Game Mode on when the game
/// runs full screen (macOS 26 and later read the key; earlier macOS uses the
/// category). An application gets neither and no category.
/// Sources: <https://developer.apple.com/documentation/bundleresources/information-property-list/lsapplicationcategorytype>
/// and <https://developer.apple.com/documentation/bundleresources/information-property-list/lssupportsgamemode>.
pub fn info_plist(slug: &str, name: &str, game: bool) -> String {
    let id: String = slug
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    let mut rows = vec![
        ("CFBundleName", xml(name)),
        ("CFBundleDisplayName", xml(name)),
        ("CFBundleIdentifier", format!("relift.game.{id}")),
        ("CFBundleExecutable", xml(slug)),
        ("CFBundleIconFile", ICON_FILE.into()),
        ("CFBundlePackageType", "APPL".into()),
        ("CFBundleInfoDictionaryVersion", "6.0".into()),
        ("CFBundleShortVersionString", "1.0".into()),
        ("CFBundleVersion", "1".into()),
        ("LSMinimumSystemVersion", "11.0".into()),
    ];
    if game {
        rows.push((
            "LSApplicationCategoryType",
            "public.app-category.games".into(),
        ));
    }
    let mut s = String::from(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n",
    );
    for (k, v) in rows {
        s.push_str(&format!("\t<key>{k}</key>\n\t<string>{v}</string>\n"));
    }
    if game {
        s.push_str("\t<key>LSSupportsGameMode</key>\n\t<true/>\n");
    }
    s.push_str("\t<key>NSHighResolutionCapable</key>\n\t<true/>\n</dict>\n</plist>\n");
    s
}

fn quote(s: &str) -> String {
    format!("'{}'", s.replace('\'', "'\\''"))
}

/// The script that starts the program: per-user folders, the bundled data
/// and the save file, as the AppImage's `AppRun` does on Linux.
pub fn launcher(
    slug: &str,
    platform: &str,
    has_romfs: bool,
    has_dlc: bool,
    has_shaders: bool,
) -> String {
    let q = quote(slug);
    let real = quote(&format!("{slug}.bin"));
    let mut s = String::from("#!/bin/sh\nHERE=$(cd \"$(dirname \"$0\")\" && pwd)\n");
    s.push_str(&format!(
        "case \"${{1:-}}\" in --relift-info) exec \"$HERE\"/{real} \"$@\" ;; esac\n"
    ));
    s.push_str(&format!(
        "DATA=\"${{HOME:-.}}/Library/Application Support/\"{q}\n"
    ));
    let mut args = String::new();
    match platform {
        "switch" => {
            s.push_str("mkdir -p \"$DATA/save\" \"$DATA/sdmc\"\n");
            s.push_str(": \"${RELIFT_SAVE:=$DATA/save}\"\n: \"${RELIFT_SDMC:=$DATA/sdmc}\"\nexport RELIFT_SAVE RELIFT_SDMC\n");
            s.push_str(": \"${RELIFT_NVN_SHADERS:=$DATA/shaders}\"\nexport RELIFT_NVN_SHADERS\n");
        }
        "gba" => {
            s.push_str(
                "mkdir -p \"$DATA/save\"\n: \"${RELIFT_SAVE:=$DATA/save}\"\nexport RELIFT_SAVE\n",
            );
        }
        "gb" | "gbc" => {
            s.push_str("mkdir -p \"$DATA\"\n");
            args.push_str(&format!(" --save \"$DATA/\"{q}\".sav\""));
        }
        _ => {}
    }
    if has_romfs {
        s.push_str(": \"${RELIFT_ROMFS:=$HERE/../Resources/romfs.img}\"\nexport RELIFT_ROMFS\n");
    }
    if has_dlc {
        s.push_str(": \"${RELIFT_DLC:=$HERE/../Resources/dlc}\"\nexport RELIFT_DLC\n");
    }
    if has_shaders {
        s.push_str(
            ": \"${RELIFT_NVN_TABLES:=$HERE/../Resources/shaders}\"\nexport RELIFT_NVN_TABLES\n",
        );
    }
    s.push_str(&format!("exec \"$HERE\"/{real}{args} \"$@\"\n"));
    s
}

/// The libraries `otool -L` output names, minus the file's own install name
/// and the system's.
pub fn parse_deps(otool_l: &str) -> Vec<String> {
    otool_l
        .lines()
        .skip(1)
        .filter_map(|l| l.trim().split(" (").next())
        .filter(|p| !p.is_empty() && !p.starts_with("/usr/lib/") && !p.starts_with("/System/"))
        .map(String::from)
        .collect()
}

/// The run-path entries of `otool -l` output.
pub fn parse_rpaths(otool_big_l: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut lines = otool_big_l.lines();
    while let Some(l) = lines.next() {
        if l.trim() == "cmd LC_RPATH" {
            for l in lines.by_ref() {
                if let Some(rest) = l.trim().strip_prefix("path ") {
                    if let Some(p) = rest.split(" (offset").next() {
                        out.push(p.to_string());
                    }
                    break;
                }
            }
        }
    }
    out
}

fn run(cmd: &mut Command, what: &str) -> Result<String, BoxError> {
    let out = cmd.output().map_err(|e| format!("{what}: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "{what} failed ({}):\n{}{}",
            out.status,
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        )
        .into());
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

fn resolve(dep: &str, rpaths: &[String], owner: &Path, exe_dir: &Path) -> Option<PathBuf> {
    let owner_dir = owner.parent()?;
    let expand = |s: &str| -> String {
        s.replace("@loader_path", &owner_dir.to_string_lossy())
            .replace("@executable_path", &exe_dir.to_string_lossy())
    };
    if let Some(rest) = dep.strip_prefix("@rpath/") {
        return rpaths
            .iter()
            .map(|r| Path::new(&expand(r)).join(rest))
            .find(|p| p.is_file());
    }
    let p = PathBuf::from(expand(dep));
    p.is_file().then_some(p)
}

/// Copies the libraries `target` links into `frameworks`, each reference
/// rewritten to `@rpath/<file>`, and returns their file names.
fn bundle_libs(
    exe_src: &Path,
    frameworks: &Path,
    target: &Path,
    done: &mut Vec<String>,
    exe_dir: &Path,
) -> Result<(), BoxError> {
    let deps = parse_deps(&run(
        relift_proc::command("otool").arg("-L").arg(target),
        "otool -L",
    )?);
    let rpaths = parse_rpaths(&run(
        relift_proc::command("otool").arg("-l").arg(target),
        "otool -l",
    )?);
    for dep in deps {
        if dep.starts_with("@rpath/") && frameworks.join(&dep["@rpath/".len()..]).is_file() {
            continue;
        }
        let src = resolve(&dep, &rpaths, target, exe_dir)
            .or_else(|| resolve(&dep, &rpaths, exe_src, exe_dir))
            .ok_or_else(|| format!("{}: cannot find {dep}", target.display()))?;
        let file = src
            .file_name()
            .ok_or("a library without a name")?
            .to_string_lossy()
            .into_owned();
        let new = format!("@rpath/{file}");
        let dest = frameworks.join(&file);
        if !done.contains(&file) {
            done.push(file.clone());
            relift_cache::write_if_changed(
                &dest,
                &std::fs::read(&src).map_err(|e| format!("{}: {e}", src.display()))?,
            )?;
            crate::appimage::set_mode(&dest, 0o755)?;
            run(
                relift_proc::command("install_name_tool")
                    .args(["-id", &new])
                    .arg(&dest),
                "install_name_tool -id",
            )?;
            bundle_libs(&src, frameworks, &dest, done, exe_dir)?;
        }
        run(
            relift_proc::command("install_name_tool")
                .args(["-change", &dep, &new])
                .arg(target),
            "install_name_tool -change",
        )?;
    }
    Ok(())
}

fn sign_args(signing: &Signing, file: &Path) -> Vec<std::ffi::OsString> {
    let mut a: Vec<std::ffi::OsString> = vec!["--force".into(), "--sign".into()];
    a.push(signing.identity.as_deref().unwrap_or("-").into());
    if let Some(k) = &signing.keychain {
        a.push("--keychain".into());
        a.push(k.into());
    }
    if signing.identity.is_some() {
        a.extend(["--options".into(), "runtime".into(), "--timestamp".into()]);
    }
    a.push(file.into());
    a
}

fn sign(signing: &Signing, file: &Path) -> Result<(), BoxError> {
    run(
        relift_proc::command("codesign").args(sign_args(signing, file)),
        &format!("codesign {}", file.display()),
    )?;
    Ok(())
}

/// Submits `zip`, waits, and fails with Apple's log when it is not accepted.
fn notarize(notary: &Notary, zip: &Path) -> Result<String, BoxError> {
    let creds = |c: &mut Command| {
        c.arg("--key").arg(&notary.key).args([
            "--key-id",
            &notary.key_id,
            "--issuer",
            &notary.issuer,
        ]);
    };
    let mut submit = relift_proc::command("xcrun");
    submit.args(["notarytool", "submit"]).arg(zip).args([
        "--wait",
        "--timeout",
        "30m",
        "--output-format",
        "json",
    ]);
    creds(&mut submit);
    let out = submit.output().map_err(|e| format!("notarytool: {e}"))?;
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    let doc: serde_json::Value = serde_json::from_str(&text).map_err(|_| {
        format!(
            "notarytool submit failed ({}):\n{text}{}",
            out.status,
            String::from_utf8_lossy(&out.stderr)
        )
    })?;
    let id = doc["id"].as_str().unwrap_or("").to_string();
    if doc["status"].as_str() == Some("Accepted") {
        return Ok(id);
    }
    let mut log = relift_proc::command("xcrun");
    log.args(["notarytool", "log", &id]);
    creds(&mut log);
    let detail = run(&mut log, "notarytool log").unwrap_or_default();
    Err(format!(
        "notarization {id} was not accepted ({}):\n{detail}",
        doc["status"].as_str().unwrap_or("no status")
    )
    .into())
}

/// Packages the built program of `req` as `<project>.app` beside it. `None`
/// on hosts other than macOS.
///
/// # Errors
///
/// A library is missing, signing or notarization fails, or the settings ask
/// for notarization without what it needs.
pub fn package(
    settings: &Settings,
    req: &Request,
    ui: &mut Ui,
) -> Result<Option<Packaged>, BoxError> {
    let (signing, notary) = plan(settings)?;
    if !cfg!(target_os = "macos") || settings.package.appimage == "never" {
        return Ok(None);
    }
    let image = std::fs::read(req.exe)?;
    let doc =
        relift_info::section::read(&image).map_err(|e| format!("{}: {e}", req.exe.display()))?;
    let info = Info::from_bytes(doc)?;
    let out = req.exe.parent().ok_or("the program has no folder")?;
    let slug = crate::appimage::slug(req.name);
    let name = crate::appimage::display_name(&info, req.name);
    let scratch = out
        .join("app")
        .join(format!("build-{}", crate::appimage_runtime::unique()));
    let job = Job {
        signing: &signing,
        notary: notary.as_ref(),
        req,
        info: &info,
        slug: &slug,
        name: &name,
    };
    let result = assemble(&job, out, &scratch, ui);
    let _ = std::fs::remove_dir_all(&scratch);
    result.map(Some)
}

struct Job<'a> {
    signing: &'a Signing,
    notary: Option<&'a Notary>,
    req: &'a Request<'a>,
    info: &'a Info,
    slug: &'a str,
    name: &'a str,
}

fn assemble(job: &Job, out: &Path, scratch: &Path, ui: &mut Ui) -> Result<Packaged, BoxError> {
    let Job {
        signing,
        notary,
        req,
        info,
        slug,
        name,
    } = *job;
    let app = scratch.join(format!("{slug}.app"));
    let contents = app.join("Contents");
    let macos = contents.join("MacOS");
    let frameworks = contents.join("Frameworks");
    let resources = contents.join("Resources");
    for d in [&macos, &frameworks, &resources] {
        std::fs::create_dir_all(d)?;
    }
    let real = macos.join(format!("{slug}.bin"));
    relift_cache::write_if_changed(&real, &std::fs::read(req.exe)?)?;
    crate::appimage::set_mode(&real, 0o755)?;
    let mut done = Vec::new();
    bundle_libs(req.exe, &frameworks, &real, &mut done, &macos)?;
    let rpaths = parse_rpaths(&run(
        relift_proc::command("otool").arg("-l").arg(&real),
        "otool -l",
    )?);
    for r in rpaths.iter().filter(|r| !r.starts_with('@')) {
        run(
            relift_proc::command("install_name_tool")
                .args(["-delete_rpath", r])
                .arg(&real),
            "install_name_tool -delete_rpath",
        )?;
    }
    if !done.is_empty() {
        run(
            relift_proc::command("install_name_tool")
                .args(["-add_rpath", "@executable_path/../Frameworks"])
                .arg(&real),
            "install_name_tool -add_rpath",
        )?;
    }
    let (romfs, dlc) = match req.import {
        Some(i) => crate::gamedata::write_images(i, &resources, true)?,
        None => (false, false),
    };
    crate::shaders::install(req.shaders, &resources.join("shaders"))?;
    if let Some(f) = req.fonts {
        crate::fonts::install(f, &resources.join("fonts"))?;
    }
    let script = macos.join(slug);
    relift_cache::write_if_changed(
        &script,
        launcher(
            slug,
            info.identity.platform.as_str(),
            romfs,
            dlc,
            !req.shaders.is_empty(),
        )
        .as_bytes(),
    )?;
    crate::appimage::set_mode(&script, 0o755)?;
    relift_cache::write_if_changed(
        &contents.join("Info.plist"),
        info_plist(slug, name, info.identity.is_game()).as_bytes(),
    )?;
    relift_cache::write_if_changed(&contents.join("PkgInfo"), b"APPL????")?;
    relift_cache::write_if_changed(
        &resources.join(ICON_FILE),
        &crate::icon::icns(info.identity.icon.as_ref(), name),
    )?;
    for f in &done {
        sign(signing, &frameworks.join(f))?;
    }
    sign(signing, &real)?;
    sign(signing, &app)?;
    run(
        relift_proc::command("codesign")
            .args(["--verify", "--strict", "--deep", "--verbose=2"])
            .arg(&app),
        "codesign --verify",
    )?;
    let mut notarized = false;
    if let Some(n) = notary {
        let zip = scratch.join(format!("{slug}.zip"));
        run(
            relift_proc::command("ditto")
                .args(["-c", "-k", "--keepParent"])
                .arg(&app)
                .arg(&zip),
            "ditto",
        )?;
        let id = notarize(n, &zip)?;
        ui.summary(&Line::new().text(format!("  notarized: submission {id}")));
        run(
            relift_proc::command("xcrun")
                .args(["stapler", "staple"])
                .arg(&app),
            "stapler staple",
        )?;
        run(
            relift_proc::command("xcrun")
                .args(["stapler", "validate"])
                .arg(&app),
            "stapler validate",
        )?;
        notarized = true;
    } else if signing.identity.is_some() {
        ui.summary(&Line::new().paint(
            Paint::Yellow,
            "signed but not notarized: Gatekeeper warns on downloaded copies (package.notarize)",
        ));
    }
    let target = out.join(format!("{slug}.app"));
    let _ = std::fs::remove_dir_all(&target);
    std::fs::rename(&app, &target).map_err(|e| format!("{}: {e}", target.display()))?;
    Ok(Packaged {
        path: target,
        bundled: done,
        signed_with: signing.identity.clone(),
        notarized,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const OTOOL: &str = "/x/prog:\n\t/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib (compatibility version 401.0.0, current version 401.20.0)\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1356.0.0)\n\t/System/Library/Frameworks/Metal.framework/Versions/A/Metal (compatibility version 1.0.0, current version 372.16.0)\n\t@rpath/libMoltenVK.dylib (compatibility version 1.0.0, current version 1.0.0)\n";

    fn plan_for(id: &str, notarize: bool, key: &str) -> Result<(Signing, Option<Notary>), String> {
        let (key, kid, iss) = (PathBuf::from(key), "K".to_string(), "I".to_string());
        plan_of(id, Path::new(""), notarize.then_some((&key, &kid, &iss)))
    }

    #[test]
    fn notarization_needs_an_identity_and_the_key() {
        assert!(plan_for("", true, "k.p8").is_err());
        assert!(plan_for("-", true, "k.p8").is_err());
        assert!(plan_for("ID", true, "").is_err());
        let (sign, notary) = plan_for("ID", true, "k.p8").unwrap();
        assert_eq!(sign.identity.as_deref(), Some("ID"));
        assert_eq!(notary.unwrap().key_id, "K");
        let (sign, notary) = plan_for("", false, "").unwrap();
        assert!(sign.identity.is_none() && notary.is_none());
    }

    #[test]
    fn only_non_system_libraries_are_bundled() {
        assert_eq!(
            parse_deps(OTOOL),
            [
                "/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib",
                "@rpath/libMoltenVK.dylib"
            ]
        );
    }

    #[test]
    fn run_paths_are_read_from_load_commands() {
        let l = "Load command 9\n          cmd LC_RPATH\n      cmdsize 40\n         path /opt/m/lib (offset 12)\nLoad command 10\n          cmd LC_RPATH\n      cmdsize 40\n         path @loader_path/x (offset 12)\n";
        assert_eq!(parse_rpaths(l), ["/opt/m/lib", "@loader_path/x"]);
    }

    #[test]
    fn the_plist_escapes_and_names_the_executable() {
        let p = info_plist("a-b", "A & B", true);
        assert!(p.contains("<string>A &amp; B</string>"));
        assert!(p.contains("<key>CFBundleExecutable</key>\n\t<string>a-b</string>"));
        assert!(p.contains("relift.game.a-b"));
        assert!(p.contains("<key>CFBundleIconFile</key>\n\t<string>AppIcon.icns</string>"));
        assert!(p.contains("<key>CFBundleDisplayName</key>\n\t<string>A &amp; B</string>"));
        assert!(p.contains("public.app-category.games"));
        assert!(p.contains("<key>LSSupportsGameMode</key>\n\t<true/>"));
    }

    #[test]
    fn an_application_plist_has_no_game_category_or_game_mode() {
        let p = info_plist("a-b", "A B", false);
        assert!(!p.contains("app-category"));
        assert!(!p.contains("GameMode"));
        assert!(p.contains("<key>CFBundleExecutable</key>"));
    }

    #[test]
    fn the_launcher_keeps_saves_per_user() {
        let gb = launcher("g", "gbc", false, false, false);
        assert!(gb.contains("Library/Application Support/"));
        assert!(gb.contains("--save \"$DATA/\"'g'\".sav\""));
        assert!(gb.ends_with("\"$@\"\n"));
        let sw = launcher("g", "switch", true, true, true);
        assert!(sw.contains("RELIFT_NVN_SHADERS:=$DATA/shaders"));
        assert!(sw.contains("RELIFT_NVN_TABLES:=$HERE/../Resources/shaders"));
        assert!(sw.contains("RELIFT_ROMFS:=$HERE/../Resources/romfs.img"));
        assert!(sw.contains("RELIFT_DLC"));
    }

    #[test]
    fn a_developer_id_signs_with_the_hardened_runtime_and_ad_hoc_does_not() {
        let id = Signing {
            identity: Some("Developer ID Application: X".into()),
            keychain: Some("/k".into()),
        };
        let a: Vec<String> = sign_args(&id, Path::new("/f"))
            .iter()
            .map(|s| s.to_string_lossy().into_owned())
            .collect();
        assert_eq!(
            a,
            [
                "--force",
                "--sign",
                "Developer ID Application: X",
                "--keychain",
                "/k",
                "--options",
                "runtime",
                "--timestamp",
                "/f"
            ]
        );
        let adhoc = Signing {
            identity: None,
            keychain: None,
        };
        let a = sign_args(&adhoc, Path::new("/f"));
        assert_eq!(a.len(), 4);
        assert_eq!(a[2], "-");
    }
}

//! iOS as a host target: Xcode's clang for the device or the simulator,
//! the static SDL3 and libopus builds, and the app bundle around the
//! program.
//!
//! The program is one statically linked executable: iOS loads no code
//! besides the system's, so SDL3, libopus and the device library are all
//! linked in. Its `main` hands the program to SDL's UIKit glue, which runs
//! it once the app has launched. A device build is signed with an Apple
//! Development identity and a provisioning profile from the keychain and
//! Xcode's profile folders; a simulator build is signed ad hoc.

use std::path::{Path, PathBuf};

use crate::import::{BoxError, cache_root};
use crate::settings::Settings;
use crate::ui::Ui;
use relift_progress::Line;

/// The oldest iOS the program runs on.
pub const MIN_IOS: &str = "15.0";

/// One iOS target: the name `--target` takes and what the toolchains call
/// it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Target {
    pub name: &'static str,
    /// Rust's name for the target.
    pub triple: &'static str,
    /// The Xcode SDK (`xcrun --sdk`).
    pub sdk: &'static str,
    /// clang's `-target`.
    pub clang: &'static str,
    /// The platform name of the bundle's Info.plist.
    pub platform: &'static str,
    /// The directory of the pinned SDKs in the cache.
    pub sdk_dir: &'static str,
}

impl Target {
    /// Whether the target is the simulator, where builds are signed ad hoc.
    #[must_use]
    pub fn simulator(&self) -> bool {
        self.sdk == "iphonesimulator"
    }
}

const TARGETS: [Target; 2] = [
    Target {
        name: "ios-arm64",
        triple: "aarch64-apple-ios",
        sdk: "iphoneos",
        clang: "arm64-apple-ios15.0",
        platform: "iPhoneOS",
        sdk_dir: "arm64-ios",
    },
    Target {
        name: "ios-sim-arm64",
        triple: "aarch64-apple-ios-sim",
        sdk: "iphonesimulator",
        clang: "arm64-apple-ios15.0-simulator",
        platform: "iPhoneSimulator",
        sdk_dir: "arm64-ios-sim",
    },
];

/// The iOS target named `name`, if it is one.
#[must_use]
pub fn target(name: &str) -> Option<Target> {
    TARGETS.iter().copied().find(|t| t.name == name)
}

/// Xcode's compiler and the SDK of one target.
#[derive(Debug, Default)]
pub struct Xcode {
    pub clang: PathBuf,
    pub sysroot: PathBuf,
}

fn xcrun(args: &[&str]) -> Option<String> {
    let out = relift_proc::command("xcrun").args(args).output().ok()?;
    let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
    (out.status.success() && !s.is_empty()).then_some(s)
}

impl Xcode {
    /// The compiler and SDK path from `xcrun`.
    ///
    /// # Errors
    ///
    /// Not on macOS, or Xcode without the target's SDK.
    pub fn find(t: Target) -> Result<Self, BoxError> {
        if !cfg!(target_os = "macos") {
            return Err(format!("target {}: builds on macOS with Xcode", t.name).into());
        }
        let missing = || {
            format!(
                "target {}: Xcode with the {} SDK is needed (install Xcode, then xcode-select -s /Applications/Xcode.app)",
                t.name, t.sdk
            )
        };
        let sysroot = xcrun(&["--sdk", t.sdk, "--show-sdk-path"]).ok_or_else(missing)?;
        let clang = xcrun(&["--sdk", t.sdk, "-f", "clang"]).ok_or_else(missing)?;
        Ok(Self {
            clang: clang.into(),
            sysroot: sysroot.into(),
        })
    }
}

/// SDL3, libopus and MoltenVK for one iOS target, as static libraries.
#[derive(Debug, Default)]
pub struct Deps {
    pub sdl3_include: PathBuf,
    pub sdl3_lib: PathBuf,
    pub opus_include: PathBuf,
    pub opus_lib: PathBuf,
    /// `libMoltenVK.a`, which only Switch games link, when present.
    pub moltenvk: Option<PathBuf>,
    /// What to run when a Switch game needs MoltenVK and it is missing.
    pub moltenvk_missing: String,
    /// The libvpx source tree, when unpacked.
    pub vpx: Option<crate::runtime_link::Vpx>,
}

impl Deps {
    /// SDL3 and libopus for `t` under the cache's `sdk/` directory.
    ///
    /// # Errors
    ///
    /// A part missing; the message names the script that builds them.
    pub fn find(cache_root: &Path, t: Target) -> Result<Self, BoxError> {
        let sdl = cache_root.join("sdk").join("sdl3").join(t.sdk_dir);
        let opus = cache_root.join("sdk").join("opus").join(t.sdk_dir);
        let mvk = cache_root
            .join("sdk")
            .join("moltenvk")
            .join(t.sdk_dir)
            .join("lib")
            .join("libMoltenVK.a");
        let d = Self {
            sdl3_include: sdl.join("include"),
            sdl3_lib: sdl.join("lib").join("libSDL3.a"),
            opus_include: opus.join("include"),
            opus_lib: opus.join("lib").join("libopus.a"),
            moltenvk_missing: format!(
                "{}: not found; get MoltenVK for {} with `python3 runtime/tools/cross/ios_deps.py --cache {} --target {}`",
                mvk.display(),
                t.name,
                cache_root.display(),
                t.name
            ),
            moltenvk: mvk.is_file().then_some(mvk),
            vpx: crate::runtime_link::find_vpx(cache_root),
        };
        for p in [&d.sdl3_lib, &d.opus_lib] {
            if !p.is_file() {
                return Err(format!(
                    "{}: not found; build SDL3 and libopus for {} with `python3 runtime/tools/cross/ios_deps.py --cache {} --target {}`",
                    p.display(),
                    t.name,
                    cache_root.display(),
                    t.name
                )
                .into());
            }
        }
        Ok(d)
    }
}

/// A provisioning profile that covers the bundle.
#[derive(Debug, Clone)]
pub struct Profile {
    pub path: PathBuf,
    pub team: String,
    /// Whether its application identifier ends in `*`.
    pub wildcard: bool,
    /// The profile's entitlements as an XML property list, with the
    /// application identifier made the bundle's own.
    pub entitlements: String,
    /// Its creation date as the profile gives it (ISO 8601, so newer sorts
    /// later).
    pub created: String,
}

/// How the bundle is signed.
#[derive(Debug)]
pub enum Signing {
    /// Ad hoc, for the simulator.
    AdHoc,
    /// An identity from the keychain with a profile for the device.
    Identity { identity: String, profile: Profile },
    /// Not signed; the reason says what is missing.
    Unsigned(String),
}

/// What building for iOS needs beyond the lift.
#[derive(Debug)]
pub struct IosBuild {
    pub target: Target,
    pub xcode: Xcode,
    pub deps: Deps,
    pub bundle_id: String,
    pub signing: Signing,
    /// Every unexpired profile that covers the bundle, the one `signing`
    /// uses first.
    pub profiles: Vec<Profile>,
}

/// Whether `s` is a bundle identifier: dot-separated parts of letters,
/// digits and hyphens.
#[must_use]
pub fn valid_bundle_id(s: &str) -> bool {
    let parts: Vec<&str> = s.split('.').collect();
    parts.len() >= 2
        && parts
            .iter()
            .all(|p| !p.is_empty() && p.chars().all(|c| c.is_ascii_alphanumeric() || c == '-'))
}

/// `dev.relift.game.<name>` with `name` cut down to what a bundle
/// identifier allows.
#[must_use]
pub fn default_bundle_id(name: &str) -> String {
    let slug: String = name
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() {
                c.to_ascii_lowercase()
            } else {
                '-'
            }
        })
        .collect();
    let slug = slug.trim_matches('-');
    if slug.is_empty() {
        "dev.relift.game.game".into()
    } else {
        format!("dev.relift.game.{slug}")
    }
}

impl IosBuild {
    /// Finds Xcode, the libraries and the signing, or says what is missing.
    ///
    /// # Errors
    ///
    /// A missing tool or library, or a bad bundle identifier.
    pub fn prepare(settings: &Settings, target: Target, name: &str) -> Result<Self, BoxError> {
        let cache = crate::paths::canonical(cache_root(settings)?)?;
        let bundle_id = if settings.package.ios_id.is_empty() {
            default_bundle_id(name)
        } else {
            settings.package.ios_id.clone()
        };
        if !matches!(settings.package.ios_data.as_str(), "bundle" | "documents") {
            return Err(format!(
                "package.ios-data {}: bundle or documents",
                settings.package.ios_data
            )
            .into());
        }
        if !valid_bundle_id(&bundle_id) {
            return Err(format!("package.ios-id {bundle_id}: not a bundle identifier").into());
        }
        let xcode = Xcode::find(target)?;
        let deps = Deps::find(&cache, target)?;
        let (signing, profiles) = if target.simulator() {
            (Signing::AdHoc, Vec::new())
        } else {
            find_signing(settings, &bundle_id, &cache.join("ios"))?
        };
        Ok(Self {
            target,
            xcode,
            deps,
            bundle_id,
            signing,
            profiles,
        })
    }

    /// Signs a device build of a Switch game with the newest covering
    /// profile that grants [`EXTENDED_VA`], and refuses the build when none
    /// does: without it iOS lets the app reserve far less than the guest
    /// address space (the compact layout needs about 37 GiB), and the game
    /// stops at start-up.
    ///
    /// # Errors
    ///
    /// No covering profile grants the entitlement.
    pub fn for_switch(&mut self) -> Result<(), BoxError> {
        let Signing::Identity { profile, .. } = &mut self.signing else {
            return Ok(());
        };
        match switch_profile(&self.profiles) {
            Some(p) => {
                *profile = p.clone();
                Ok(())
            }
            None => switch_profile_ok(profile, &self.bundle_id),
        }
    }

    /// The compiler command for the target.
    #[must_use]
    pub fn cc(&self) -> Vec<String> {
        vec![
            self.xcode.clang.display().to_string(),
            "-target".into(),
            self.target.clang.into(),
            "-isysroot".into(),
            self.xcode.sysroot.display().to_string(),
        ]
    }

    /// The system libraries and frameworks a program links.
    #[must_use]
    pub fn libs(&self) -> Vec<String> {
        let mut v: Vec<String> = ["-lm", "-liconv", "-lc++"].map(String::from).into();
        for f in FRAMEWORKS {
            v.push("-framework".into());
            v.push((*f).into());
        }
        v
    }
}

/// What the static SDL3 and MoltenVK need from the system.
const FRAMEWORKS: &[&str] = &[
    "AVFoundation",
    "AudioToolbox",
    "CoreAudio",
    "CoreBluetooth",
    "CoreFoundation",
    "CoreGraphics",
    "CoreHaptics",
    "CoreMotion",
    "CoreVideo",
    "Foundation",
    "GameController",
    "IOKit",
    "IOSurface",
    "Metal",
    "OpenGLES",
    "QuartzCore",
    "UIKit",
    "UniformTypeIdentifiers",
];

fn run(cmd: &mut std::process::Command, what: &str) -> Result<String, BoxError> {
    let out = cmd.output().map_err(|e| format!("{what}: {e}"))?;
    let text = format!(
        "{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    if out.status.success() {
        Ok(text)
    } else {
        Err(format!("{what} failed ({}):\n{}", out.status, text.trim_end()).into())
    }
}

/// The first Apple Development identity in `security find-identity`
/// output: its SHA-1 hash.
#[must_use]
pub fn development_identity(listing: &str) -> Option<String> {
    listing.lines().find_map(|l| {
        let l = l.trim();
        let (_, rest) = l.split_once(") ")?;
        let (hash, name) = rest.split_once(' ')?;
        let dev = name.contains("Apple Development") || name.contains("iPhone Developer");
        (dev && hash.len() == 40 && hash.chars().all(|c| c.is_ascii_hexdigit()))
            .then(|| hash.to_string())
    })
}

/// Whether the profile's application identifier `pattern` (`TEAM.id` or
/// `TEAM.prefix.*`) covers `bundle` of `team`.
#[must_use]
pub fn app_id_covers(pattern: &str, team: &str, bundle: &str) -> bool {
    let full = format!("{team}.{bundle}");
    match pattern.strip_suffix('*') {
        Some(prefix) => full.starts_with(prefix),
        None => pattern == full,
    }
}

/// Days since 1970-01-01 of a civil date.
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let doy = (153 * (m + if m > 2 { -3 } else { 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

/// Whether an ISO 8601 date (`2027-11-15T10:00:00Z`) is after `now_days`
/// (days since 1970).
#[must_use]
pub fn not_expired(iso: &str, now_days: i64) -> bool {
    let date = iso.get(..10).unwrap_or("");
    let mut it = date.split('-').map(|p| p.parse::<i64>().ok());
    match (
        it.next().flatten(),
        it.next().flatten(),
        it.next().flatten(),
    ) {
        (Some(y), Some(m), Some(d)) => days_from_civil(y, m, d) > now_days,
        _ => false,
    }
}

fn now_days() -> i64 {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs());
    i64::try_from(secs / 86_400).unwrap_or(0)
}

fn plist_raw(file: &Path, key: &str) -> Option<String> {
    let out = relift_proc::command("plutil")
        .args(["-extract", key, "raw", "-o", "-"])
        .arg(file)
        .output()
        .ok()?;
    out.status
        .success()
        .then(|| String::from_utf8_lossy(&out.stdout).trim().to_string())
}

fn profile_dirs() -> Vec<PathBuf> {
    let Some(home) = std::env::var_os("HOME").map(PathBuf::from) else {
        return Vec::new();
    };
    vec![
        home.join("Library/Developer/Xcode/UserData/Provisioning Profiles"),
        home.join("Library/MobileDevice/Provisioning Profiles"),
    ]
}

/// Reads one profile and returns it when it covers `bundle` and has not
/// expired.
fn read_profile(path: &Path, bundle: &str, work: &Path) -> Result<Option<Profile>, BoxError> {
    std::fs::create_dir_all(work)?;
    let decoded = work.join("profile.plist");
    run(
        relift_proc::command("security")
            .args(["cms", "-D", "-i"])
            .arg(path)
            .arg("-o")
            .arg(&decoded),
        "security cms",
    )?;
    let (Some(team), Some(pattern), Some(expires)) = (
        plist_raw(&decoded, "TeamIdentifier.0"),
        plist_raw(&decoded, "Entitlements.application-identifier"),
        plist_raw(&decoded, "ExpirationDate"),
    ) else {
        return Ok(None);
    };
    if !app_id_covers(&pattern, &team, bundle) || !not_expired(&expires, now_days()) {
        return Ok(None);
    }
    let ent = work.join("entitlements.plist");
    run(
        relift_proc::command("plutil")
            .args(["-extract", "Entitlements", "xml1", "-o"])
            .arg(&ent)
            .arg(&decoded),
        "plutil",
    )?;
    run(
        relift_proc::command("plutil")
            .args(["-replace", "application-identifier", "-string"])
            .arg(format!("{team}.{bundle}"))
            .arg(&ent),
        "plutil",
    )?;
    Ok(Some(Profile {
        path: path.to_path_buf(),
        team,
        wildcard: pattern.ends_with('*'),
        entitlements: std::fs::read_to_string(&ent)?,
        created: plist_raw(&decoded, "CreationDate").unwrap_or_default(),
    }))
}

/// The entitlement a Switch game needs on an iOS device to reserve its
/// guest address space.
pub const EXTENDED_VA: &str = "com.apple.developer.kernel.extended-virtual-addressing";

/// The entitlement that lets a Switch game use more memory than iOS gives
/// an app by default.
pub const INCREASED_MEMORY: &str = "com.apple.developer.kernel.increased-memory-limit";

/// Whether the entitlements property list `xml` sets `key` to true.
fn entitled(xml: &str, key: &str) -> bool {
    let needle = format!("<key>{key}</key>");
    xml.split_once(&needle)
        .is_some_and(|(_, rest)| rest.trim_start().starts_with("<true/>"))
}

/// The newest of `profiles` that grants [`EXTENDED_VA`].
fn switch_profile(profiles: &[Profile]) -> Option<&Profile> {
    profiles
        .iter()
        .filter(|p| entitled(&p.entitlements, EXTENDED_VA))
        .max_by(|a, b| a.created.cmp(&b.created))
}

/// The profile to sign with among `profiles`: the newest explicit one,
/// else the newest wildcard one.
fn default_profile(profiles: &[Profile]) -> Option<&Profile> {
    profiles
        .iter()
        .max_by(|a, b| (!a.wildcard, &a.created).cmp(&(!b.wildcard, &b.created)))
}

fn switch_profile_ok(profile: &Profile, bundle: &str) -> Result<(), BoxError> {
    if entitled(&profile.entitlements, EXTENDED_VA) {
        return Ok(());
    }
    Err(format!(
        "the provisioning profile {} for {bundle} does not grant {EXTENDED_VA}, which a Switch game needs to reserve its address space on an iOS device{}; on developer.apple.com create an explicit App ID {bundle} with the capabilities Extended Virtual Addressing ({EXTENDED_VA}) and Increased Memory Limit ({INCREASED_MEMORY}), then a development profile for it, and set package.ios-profile to it or install it",
        profile.path.display(),
        if profile.wildcard {
            " (a wildcard App ID cannot carry it)"
        } else {
            ""
        }
    )
    .into())
}

/// How to sign the bundle, with every unexpired profile that covers it.
fn find_signing(
    settings: &Settings,
    bundle: &str,
    work: &Path,
) -> Result<(Signing, Vec<Profile>), BoxError> {
    let identity = if settings.package.ios_identity.is_empty() {
        let listing = relift_proc::command("security")
            .args(["find-identity", "-v", "-p", "codesigning"])
            .output()
            .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
            .unwrap_or_default();
        development_identity(&listing)
    } else {
        Some(settings.package.ios_identity.clone())
    };
    let Some(identity) = identity else {
        return Ok((
            Signing::Unsigned(
                "no Apple Development identity in the keychain (Xcode > Settings > Accounts > Manage Certificates)".into(),
            ),
            Vec::new(),
        ));
    };
    let candidates: Vec<PathBuf> = if settings.package.ios_profile.as_os_str().is_empty() {
        let mut v: Vec<PathBuf> = profile_dirs()
            .iter()
            .filter_map(|d| std::fs::read_dir(d).ok())
            .flatten()
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| p.extension().is_some_and(|x| x == "mobileprovision"))
            .collect();
        v.sort();
        v
    } else {
        vec![settings.package.ios_profile.clone()]
    };
    let mut profiles = Vec::new();
    for c in &candidates {
        if let Some(p) = read_profile(c, bundle, work)? {
            profiles.push(p);
        }
    }
    Ok((
        match default_profile(&profiles) {
            Some(profile) => Signing::Identity {
                identity,
                profile: profile.clone(),
            },
            None => Signing::Unsigned(format!(
                "no provisioning profile for {bundle} (set package.ios-profile, or make one in Xcode or on developer.apple.com)"
            )),
        },
        profiles,
    ))
}

fn xml(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

/// What the home screen shows for a game.
pub struct Home {
    /// The name under the icon.
    pub label: String,
    /// A file name in the bundle and the PNG bytes for each size.
    pub icons: Vec<(&'static str, Vec<u8>)>,
}

/// The home screen entry of the program `exe`, from the identity it embeds:
/// its title (else `label`) and its icon (else the generated one for `seed`).
///
/// # Errors
///
/// The program cannot be read or carries no self-description.
pub fn home(exe: &Path, seed: &str, label: &str) -> Result<Home, BoxError> {
    let image = std::fs::read(exe)?;
    let doc = relift_info::section::read(&image).map_err(|e| format!("{}: {e}", exe.display()))?;
    let info = relift_info::Info::from_bytes(doc)?;
    Ok(Home {
        label: crate::appimage::display_name(&info, label),
        icons: crate::icon::ios_pngs(info.identity.icon.as_ref(), seed),
    })
}

/// `CFBundleName`, which iOS keeps to 15 characters.
fn short_name(label: &str) -> String {
    if label.chars().count() <= 15 {
        return label.to_string();
    }
    let cut: String = label.chars().take(14).collect();
    format!("{}\u{2026}", cut.trim_end())
}

/// The bundle's Info.plist.
#[must_use]
pub fn info_plist(b: &IosBuild, exe: &str, label: &str) -> String {
    plist(&b.bundle_id, b.target, exe, label)
}

fn plist(bundle_id: &str, target: Target, exe: &str, label: &str) -> String {
    let short = xml(&short_name(label));
    let label = xml(label);
    format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleDisplayName</key><string>{label}</string>
  <key>CFBundleExecutable</key><string>{exe}</string>
  <key>CFBundleIdentifier</key><string>{id}</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>{short}</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleSupportedPlatforms</key><array><string>{platform}</string></array>
  <key>CFBundleIcons</key><dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconFiles</key><array><string>AppIcon60x60</string></array></dict></dict>
  <key>CFBundleIcons~ipad</key><dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconFiles</key><array><string>AppIcon60x60</string><string>AppIcon76x76</string><string>AppIcon83.5x83.5</string></array></dict></dict>
  <key>DTPlatformName</key><string>{sdk}</string>
  <key>LSRequiresIPhoneOS</key><true/>
  <key>LSSupportsOpeningDocumentsInPlace</key><true/>
  <key>MinimumOSVersion</key><string>{MIN_IOS}</string>
  <key>UIApplicationSupportsIndirectInputEvents</key><true/>
  <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
  <key>UIFileSharingEnabled</key><true/>
  <key>UILaunchScreen</key><dict/>
  <key>UIRequiredDeviceCapabilities</key><array><string>arm64</string></array>
  <key>UIRequiresFullScreen</key><true/>
  <key>UIStatusBarHidden</key><true/>
  <key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationPortrait</string><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
</dict>
</plist>
"#,
        id = bundle_id,
        platform = target.platform,
        sdk = target.sdk,
    )
}

/// What goes into the app besides its signature.
#[derive(Clone, Copy)]
pub struct Contents<'a> {
    /// The linked program.
    pub exe: &'a Path,
    /// The bundle's file name, without `.app`.
    pub name: &'a str,
    /// The name under the icon and the icons.
    pub home: &'a Home,
    /// The game data manifest (crate::datamanifest), when there is data.
    pub data_manifest: Option<&'a [u8]>,
    /// The import folder whose romfs and dlc go into the bundle, cloned
    /// (APFS), or `None` when the data goes to Documents later.
    pub data: Option<&'a Path>,
}

/// The packaging step of an iOS build: `<name>.app` beside the program
/// `exe`, signed as [`IosBuild::signing`] says.
///
/// # Errors
///
/// Writing the bundle fails, or `codesign` does.
pub fn package(b: &IosBuild, c: &Contents<'_>, ui: &mut Ui) -> Result<PathBuf, BoxError> {
    let Contents {
        exe,
        name,
        home,
        data_manifest,
        data,
    } = *c;
    let dir = exe.parent().ok_or("the program has no folder")?;
    let row = ui.row("");
    ui.begin(row, "package");
    let app = dir.join(format!("{name}.app"));
    if app.exists() {
        std::fs::remove_dir_all(&app)?;
    }
    std::fs::create_dir_all(&app)?;
    let exe_name = "game";
    let dest = app.join(exe_name);
    relift_cache::write_if_changed(&dest, &std::fs::read(exe)?)?;
    executable(&dest)?;
    relift_cache::write_if_changed(
        &app.join("Info.plist"),
        info_plist(b, exe_name, &home.label).as_bytes(),
    )?;
    relift_cache::write_if_changed(&app.join("PkgInfo"), b"APPL????")?;
    if let Some(m) = data_manifest {
        relift_cache::write_if_changed(&app.join(crate::datamanifest::FILE), m)?;
    }
    for root in ["romfs", "dlc"] {
        if let Some(src) = data.map(|d| d.join(root)).filter(|d| d.is_dir()) {
            run(
                relift_proc::command("cp")
                    .arg("-cR")
                    .arg(&src)
                    .arg(app.join(root)),
                "cp -c (APFS clone of the game data)",
            )?;
        }
    }
    for (n, png) in &home.icons {
        relift_cache::write_if_changed(&app.join(n), png)?;
    }
    let how = sign(b, &app, &dir.join("sign"))?;
    ui.done(row, Line::new().text(format!("{} ({how})", b.bundle_id)));
    Ok(app)
}

#[cfg(unix)]
fn executable(p: &Path) -> std::io::Result<()> {
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(p, std::fs::Permissions::from_mode(0o755))
}

#[cfg(not(unix))]
fn executable(_: &Path) -> std::io::Result<()> {
    Ok(())
}

fn sign(b: &IosBuild, app: &Path, work: &Path) -> Result<String, BoxError> {
    match &b.signing {
        Signing::AdHoc => {
            run(
                relift_proc::command("codesign")
                    .args(["--force", "--sign", "-", "--timestamp=none"])
                    .arg(app),
                "codesign",
            )?;
            Ok("signed ad hoc".into())
        }
        Signing::Identity { identity, profile } => {
            std::fs::create_dir_all(work)?;
            relift_cache::write_if_changed(
                &app.join("embedded.mobileprovision"),
                &std::fs::read(&profile.path)?,
            )?;
            let ent = work.join("entitlements.plist");
            relift_cache::write_if_changed(&ent, profile.entitlements.as_bytes())?;
            run(
                relift_proc::command("codesign")
                    .args(["--force", "--sign", identity, "--timestamp=none"])
                    .arg("--entitlements")
                    .arg(&ent)
                    .arg(app),
                "codesign",
            )?;
            Ok(format!("signed for team {}", profile.team))
        }
        Signing::Unsigned(why) => Ok(format!("not signed: {why}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn profile(name: &str, entitlements: &str, wildcard: bool, created: &str) -> Profile {
        Profile {
            path: PathBuf::from(format!("/p/{name}.mobileprovision")),
            team: "T".into(),
            wildcard,
            entitlements: entitlements.to_string(),
            created: created.into(),
        }
    }

    const WITH_VA: &str = "<dict>\n\t<key>application-identifier</key>\n\t<string>T.dev.relift.game.test</string>\n\t<key>com.apple.developer.kernel.extended-virtual-addressing</key>\n\t<true/>\n</dict>";
    const WITHOUT_VA: &str = "<dict>\n\t<key>application-identifier</key>\n\t<string>T.dev.relift.*</string>\n\t<key>get-task-allow</key>\n\t<true/>\n</dict>";

    #[test]
    fn a_switch_build_needs_the_extended_address_space_entitlement() {
        let off = WITH_VA.replace("<true/>", "<false/>");
        assert!(
            switch_profile_ok(&profile("a", WITH_VA, false, ""), "dev.relift.game.test").is_ok()
        );
        let e = switch_profile_ok(
            &profile("test", WITHOUT_VA, true, ""),
            "dev.relift.game.test",
        )
        .unwrap_err()
        .to_string();
        assert!(e.contains(EXTENDED_VA), "{e}");
        assert!(e.contains("/p/test.mobileprovision"), "{e}");
        assert!(e.contains("a wildcard App ID cannot carry it"), "{e}");
        assert!(e.contains("explicit App ID dev.relift.game.test"), "{e}");
        assert!(e.contains(INCREASED_MEMORY), "{e}");
        assert!(switch_profile_ok(&profile("b", &off, false, ""), "x.y").is_err());
    }

    #[test]
    fn a_switch_build_takes_the_newest_profile_that_grants_extended_addressing() {
        let ps = vec![
            profile("00-old", WITHOUT_VA, false, "2026-01-01T00:00:00Z"),
            profile("11-va", WITH_VA, false, "2026-03-01T00:00:00Z"),
            profile("22-va-newer", WITH_VA, false, "2026-06-01T00:00:00Z"),
            profile("33-newest", WITHOUT_VA, false, "2026-09-01T00:00:00Z"),
            profile("44-wild", WITHOUT_VA, true, "2026-10-01T00:00:00Z"),
        ];
        let name = |p: Option<&Profile>| p.map(|p| p.path.display().to_string());
        assert_eq!(
            name(default_profile(&ps)).as_deref(),
            Some("/p/33-newest.mobileprovision")
        );
        assert_eq!(
            name(switch_profile(&ps)).as_deref(),
            Some("/p/22-va-newer.mobileprovision")
        );
        assert_eq!(switch_profile(&ps[3..]).map(|p| p.created.clone()), None);
        let mut b = IosBuild {
            target: target("ios-arm64").expect("target"),
            xcode: Xcode::default(),
            deps: Deps::default(),
            bundle_id: "dev.relift.game.test".into(),
            signing: Signing::Identity {
                identity: "id".into(),
                profile: ps[3].clone(),
            },
            profiles: ps.clone(),
        };
        b.for_switch().expect("a profile grants it");
        let Signing::Identity { profile, .. } = &b.signing else {
            panic!("signed")
        };
        assert_eq!(
            profile.path,
            PathBuf::from("/p/22-va-newer.mobileprovision")
        );
        b.profiles.truncate(1);
        assert!(b.for_switch().is_ok());
        b.signing = Signing::Identity {
            identity: "id".into(),
            profile: ps[0].clone(),
        };
        assert!(b.for_switch().is_err());
    }

    #[test]
    fn target_names_map_to_sdks() {
        assert_eq!(target("ios-arm64").map(|t| t.sdk), Some("iphoneos"));
        let sim = target("ios-sim-arm64").expect("sim");
        assert!(sim.simulator());
        assert_eq!(sim.triple, "aarch64-apple-ios-sim");
        assert!(target("ios-x64").is_none());
    }

    #[test]
    fn a_bundle_id_comes_from_the_project_name() {
        assert_eq!(default_bundle_id("My Game_2"), "dev.relift.game.my-game-2");
        assert_eq!(default_bundle_id("__"), "dev.relift.game.game");
        assert!(valid_bundle_id("dev.relift.test.gb-case"));
        assert!(!valid_bundle_id("dev.relift..x"));
        assert!(!valid_bundle_id("dev.relift.under_score"));
    }

    #[test]
    fn the_development_identity_is_found_in_the_listing() {
        let listing = "  1) 0123456789ABCDEF0123456789ABCDEF01234567 \"Developer ID Application: A (T)\"\n  2) 89ABCDEF0123456789ABCDEF0123456789ABCDEF \"Apple Development: A (M)\"\n     2 valid identities found\n";
        assert_eq!(
            development_identity(listing).as_deref(),
            Some("89ABCDEF0123456789ABCDEF0123456789ABCDEF")
        );
        assert_eq!(development_identity("0 valid identities found"), None);
    }

    #[test]
    fn wildcard_and_explicit_app_ids_cover_bundles() {
        assert!(app_id_covers("T.dev.relift.*", "T", "dev.relift.test.gb"));
        assert!(!app_id_covers("T.dev.relift.*", "U", "dev.relift.test.gb"));
        assert!(app_id_covers("T.*", "T", "x.y"));
        assert!(app_id_covers("T.a.b", "T", "a.b"));
        assert!(!app_id_covers("T.a.b", "T", "a.bc"));
    }

    #[test]
    fn the_bundle_is_named_with_the_title_and_lists_its_icons() {
        assert_eq!(short_name("Short"), "Short");
        assert_eq!(
            short_name("A Very Long Game Title"),
            "A Very Long Ga\u{2026}"
        );
        let t = target("ios-arm64").unwrap();
        let p = plist("dev.relift.x", t, "game", "A & B Long Title Here");
        assert!(
            p.contains("<key>CFBundleDisplayName</key><string>A &amp; B Long Title Here</string>")
        );
        assert!(p.contains("<key>CFBundleName</key><string>A &amp; B Long Tit\u{2026}</string>"));
        assert!(p.contains("<string>AppIcon83.5x83.5</string>"));
    }

    #[test]
    fn expiry_compares_the_date() {
        let day = days_from_civil(2026, 11, 15);
        assert_eq!(days_from_civil(1970, 1, 1), 0);
        assert!(not_expired("2026-11-16T00:00:00Z", day));
        assert!(!not_expired("2026-11-15T23:00:00Z", day));
        assert!(!not_expired("garbage", day));
    }
}

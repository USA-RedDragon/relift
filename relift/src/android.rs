//! Android as a host target: the NDK compiler, the SDL3 and libopus builds
//! for the target, and the APK around the program's shared library.
//!
//! The program is a shared library named after the project (`lib<name>.so`,
//! see [`library`]) that SDL's Java activity loads and enters through
//! `SDL_main`. relift drives the Android
//! SDK's own tools (`aapt2`, `d8`, `zipalign`, `apksigner`) and `javac`; no
//! Gradle. SDL3's Java glue is the `SDL3.jar` that its CMake install makes.

mod zip;

use std::path::{Path, PathBuf};

use crate::import::{BoxError, cache_root};
use crate::settings::Settings;
use crate::ui::Ui;
use relift_progress::Line;

/// The file name of the shared library of the program `stem` (the project's
/// file name, [`crate::progname::slug`]): `lib<stem>.so`.
#[must_use]
pub fn library(stem: &str) -> String {
    format!("lib{stem}.so")
}

/// The Android API level the runtime builds for (`memfd_create` in bionic).
pub const MIN_API: u32 = 30;

/// The activity class of the APK: SDL's activity plus launch arguments and
/// environment variables from the intent, the haptic tick of the on-screen
/// controller, the game's settings pages, the game data import when the
/// start-up check fails, and the manifest's orientation kept (SDL asks for any
/// orientation the user allows when its window is resizable, which put the
/// game in portrait on a device held upright), narrowed to the sensor
/// landscape while another display shows a screen of the game.
/// The license of the Zstandard decoder the Switch runtime compiles in.
const LICENSE_ZSTD: &str = include_str!("../../runtime/third_party/zstd/LICENSE");
/// The licenses of the picture options' FXAA, SMAA, FSR 1, FSR 2 and FSR 3 passes, in every runtime.
const LICENSE_FXAA: &str = include_str!("../../runtime/third_party/fxaa/LICENSE");
const LICENSE_SMAA: &str = include_str!("../../runtime/third_party/smaa/LICENSE");
const LICENSE_FSR1: &str = include_str!("../../runtime/third_party/fsr1/LICENSE");
/// The license of the RenderDoc in-application API header, in every runtime.
const LICENSE_RENDERDOC: &str = include_str!("../../runtime/third_party/renderdoc/LICENSE");
const LICENSE_FSR2: &str = include_str!("../../runtime/third_party/fsr2/LICENSE");
const LICENSE_FSR3: &str = include_str!("../../runtime/third_party/fsr3/LICENSE");
const ACTIVITY: &str = "dev.relift.app.MainActivity";

/// The Java sources of the app, compiled into `classes.dex` with SDL3's
/// own classes.
const JAVA: [(&str, &str); 14] = [
    (
        "MainActivity",
        include_str!("android/java/MainActivity.java"),
    ),
    (
        "SettingsActivity",
        include_str!("android/java/SettingsActivity.java"),
    ),
    ("Ui", include_str!("android/java/Ui.java")),
    ("Drivers", include_str!("android/java/Drivers.java")),
    ("Catalog", include_str!("android/java/Catalog.java")),
    ("GpuProbe", include_str!("android/java/GpuProbe.java")),
    ("Restarter", include_str!("android/java/Restarter.java")),
    ("Displays", include_str!("android/java/Displays.java")),
    ("GameData", include_str!("android/java/GameData.java")),
    ("DataPages", include_str!("android/java/DataPages.java")),
    ("DataImport", include_str!("android/java/DataImport.java")),
    ("DataService", include_str!("android/java/DataService.java")),
    ("Maths", include_str!("android/java/Maths.java")),
    (
        "DataActivity",
        include_str!("android/java/DataActivity.java"),
    ),
];

/// The drivers Get drivers offers, shipped as the asset `gpu_drivers.json`
/// in APKs of games that draw with Vulkan. The app downloads an entry only
/// from its https URL and installs it only when the file's SHA-256 matches.
pub const GPU_DRIVERS_JSON: &str = include_str!("android/gpu_drivers.json");

/// One Android target: the name `--target` takes and what the toolchains
/// call it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Target {
    pub name: &'static str,
    /// The directory under `lib/` in the APK.
    pub abi: &'static str,
    /// The NDK's and Rust's name for the target.
    pub triple: &'static str,
    /// The `<arch>-android` directory of the pinned SDKs in the cache.
    pub sdk_dir: &'static str,
}

const TARGETS: [Target; 2] = [
    Target {
        name: "android-arm64",
        abi: "arm64-v8a",
        triple: "aarch64-linux-android",
        sdk_dir: "aarch64-android",
    },
    Target {
        name: "android-x64",
        abi: "x86_64",
        triple: "x86_64-linux-android",
        sdk_dir: "x86_64-android",
    },
];

/// The Android target named `name`, if it is one.
#[must_use]
pub fn target(name: &str) -> Option<Target> {
    TARGETS.iter().copied().find(|t| t.name == name)
}

/// The tools that compile and link for Android.
#[derive(Debug)]
pub struct Ndk {
    pub clang: PathBuf,
    pub strip: PathBuf,
    pub root: PathBuf,
}

/// The tools that package an APK.
#[derive(Debug)]
pub struct Sdk {
    pub aapt2: PathBuf,
    pub d8: PathBuf,
    pub zipalign: PathBuf,
    pub apksigner: PathBuf,
    pub android_jar: PathBuf,
    pub target_sdk: u32,
    pub javac: PathBuf,
    pub keytool: PathBuf,
}

/// SDL3 and libopus built for one Android target.
#[derive(Debug)]
pub struct Deps {
    pub sdl3_include: PathBuf,
    pub sdl3_lib: PathBuf,
    pub sdl3_jar: PathBuf,
    pub opus_include: PathBuf,
    pub opus_lib: PathBuf,
    /// libadrenotools, which loads a custom Vulkan driver package
    /// (`video.gpu_driver`); arm64 only.
    pub adrenotools: Option<Adrenotools>,
    /// The libvpx source tree, when unpacked.
    pub vpx: Option<crate::runtime_link::Vpx>,
    /// The dav1d source tree, when unpacked.
    pub dav1d: Option<crate::runtime_link::Dav1d>,
}

/// The libadrenotools build of one target.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Adrenotools {
    pub include: PathBuf,
    /// `libadrenotools.so`, which the program links.
    pub lib: PathBuf,
    /// The hook libraries it loads from the program's own folder.
    pub hooks: Vec<PathBuf>,
    /// The license texts of libadrenotools and liblinkernsbypass, by name.
    pub licenses: Vec<(String, PathBuf)>,
}

/// The libadrenotools libraries an APK carries next to the program's library.
const ADRENOTOOLS_LIBS: [&str; 3] = ["libadrenotools.so", "libhook_impl.so", "libmain_hook.so"];

fn version_key(p: &Path) -> Vec<u64> {
    p.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
        .split(|c: char| !c.is_ascii_digit())
        .filter_map(|s| s.parse().ok())
        .collect()
}

fn sorted_children(dir: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir)
        .map(|d| d.filter_map(|e| e.ok().map(|e| e.path())).collect())
        .unwrap_or_default();
    v.sort_by_key(|p| std::cmp::Reverse(version_key(p)));
    v
}

fn env_dir(names: &[&str]) -> Vec<PathBuf> {
    names
        .iter()
        .filter_map(|n| std::env::var_os(n).filter(|v| !v.is_empty()))
        .map(PathBuf::from)
        .collect()
}

fn sdk_roots(settings: &Settings) -> Vec<PathBuf> {
    let mut v = Vec::new();
    if !settings.toolchain.android_sdk.as_os_str().is_empty() {
        v.push(settings.toolchain.android_sdk.clone());
    }
    v.extend(env_dir(&["ANDROID_HOME", "ANDROID_SDK_ROOT"]));
    v.push(PathBuf::from("/opt/android-sdk"));
    if let Some(h) = std::env::var_os("HOME") {
        let home = PathBuf::from(h);
        v.push(home.join("Android").join("Sdk"));
        v.push(home.join("Library/Android/sdk"));
    }
    v.push(PathBuf::from("/usr/local/lib/android/sdk"));
    v.into_iter().filter(|p| p.is_dir()).collect()
}

fn ndk_candidates(settings: &Settings) -> Vec<PathBuf> {
    let mut v = Vec::new();
    if !settings.toolchain.android_ndk.as_os_str().is_empty() {
        v.push(settings.toolchain.android_ndk.clone());
    }
    v.extend(env_dir(&[
        "ANDROID_NDK_HOME",
        "ANDROID_NDK_ROOT",
        "ANDROID_NDK",
        "ANDROID_NDK_LATEST_HOME",
    ]));
    for sdk in sdk_roots(settings) {
        v.push(sdk.join("ndk-bundle"));
        v.extend(sorted_children(&sdk.join("ndk")));
    }
    v.push(PathBuf::from("/opt/android-ndk"));
    v.into_iter()
        .filter(|p| p.join("source.properties").is_file())
        .collect()
}

impl Ndk {
    /// The newest NDK.
    ///
    /// # Errors
    ///
    /// No NDK, or one without the compiler of `target`.
    pub fn find(settings: &Settings, t: Target) -> Result<Self, BoxError> {
        let mut roots = ndk_candidates(settings);
        roots.dedup();
        let root = roots.into_iter().next().ok_or(
            "no Android NDK found: install one (sdkmanager 'ndk;28.2.13676358'), then set toolchain.android-ndk or ANDROID_NDK_HOME",
        )?;
        let bin = sorted_children(&root.join("toolchains/llvm/prebuilt"))
            .into_iter()
            .map(|p| p.join("bin"))
            .find(|p| p.is_dir())
            .ok_or_else(|| format!("{}: no toolchains/llvm/prebuilt/*/bin", root.display()))?;
        let clang = bin.join(format!("{}{MIN_API}-clang", t.triple));
        if !clang.is_file() {
            return Err(
                format!("{}: not in the NDK at {}", clang.display(), root.display()).into(),
            );
        }
        Ok(Self {
            clang,
            strip: bin.join("llvm-strip"),
            root,
        })
    }
}

impl Sdk {
    /// The newest build tools and platform, and the JDK tools.
    ///
    /// # Errors
    ///
    /// Any tool missing; the message says which and how to get it.
    pub fn find(settings: &Settings) -> Result<Self, BoxError> {
        let roots = sdk_roots(settings);
        let tools = roots
            .iter()
            .flat_map(|r| sorted_children(&r.join("build-tools")))
            .find(|d| d.join("aapt2").is_file() && d.join("d8").is_file())
            .ok_or("no Android build-tools with aapt2 and d8: sdkmanager 'build-tools;36.0.0', then set toolchain.android-sdk or ANDROID_HOME")?;
        let tool = |n: &str| -> Result<PathBuf, BoxError> {
            let p = tools.join(n);
            if p.is_file() {
                Ok(p)
            } else {
                Err(format!("{}: missing from the build-tools", p.display()).into())
            }
        };
        let (jar, level) = roots
            .iter()
            .flat_map(|r| sorted_children(&r.join("platforms")))
            .filter_map(|d| {
                let level: u32 = d
                    .file_name()?
                    .to_string_lossy()
                    .strip_prefix("android-")?
                    .split('.')
                    .next()?
                    .parse()
                    .ok()?;
                (level >= 33 && d.join("android.jar").is_file())
                    .then(|| (d.join("android.jar"), level))
            })
            .min_by_key(|(_, level)| level.abs_diff(36))
            .ok_or("no Android platform 33 or newer: sdkmanager 'platforms;android-36'")?;
        let jdk = |n: &str, why: &str| -> Result<PathBuf, BoxError> {
            relift_build::find_tool(n)
                .ok_or_else(|| format!("{n} not found ({why}): install a JDK 17 or newer").into())
        };
        Ok(Self {
            aapt2: tool("aapt2")?,
            d8: tool("d8")?,
            zipalign: tool("zipalign")?,
            apksigner: tool("apksigner")?,
            android_jar: jar,
            target_sdk: level,
            javac: jdk("javac", "compiles the activity")?,
            keytool: jdk("keytool", "makes the debug signing key")?,
        })
    }
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
        let at = cache_root.join("sdk").join("adrenotools").join(t.sdk_dir);
        let at_lib = at.join("lib");
        let adrenotools = (t.abi == "arm64-v8a").then(|| Adrenotools {
            include: at.join("include"),
            lib: at_lib.join(ADRENOTOOLS_LIBS[0]),
            hooks: ADRENOTOOLS_LIBS[1..]
                .iter()
                .map(|n| at_lib.join(n))
                .collect(),
            licenses: ["libadrenotools", "liblinkernsbypass"]
                .map(|n| {
                    (
                        n.to_string(),
                        at.join("share/licenses").join(n).join("LICENSE"),
                    )
                })
                .into(),
        });
        let d = Self {
            sdl3_include: sdl.join("include"),
            sdl3_lib: sdl.join("lib").join("libSDL3.so"),
            sdl3_jar: sdl.join("java").join("SDL3.jar"),
            opus_include: opus.join("include"),
            opus_lib: opus.join("lib").join("libopus.a"),
            adrenotools,
            vpx: crate::runtime_link::find_vpx(cache_root),
            dav1d: crate::runtime_link::find_dav1d(cache_root),
        };
        let at_files = d.adrenotools.iter().flat_map(|a| {
            std::iter::once(&a.lib)
                .chain(&a.hooks)
                .chain(a.licenses.iter().map(|(_, p)| p))
        });
        for p in [&d.sdl3_lib, &d.sdl3_jar, &d.opus_lib]
            .into_iter()
            .chain(at_files)
        {
            if !p.is_file() {
                return Err(format!(
                    "{}: not found; build SDL3, libopus and libadrenotools for {} with `python3 runtime/tools/cross/android_deps.py --cache {}`",
                    p.display(),
                    t.name,
                    cache_root.display()
                )
                .into());
            }
        }
        Ok(d)
    }
}

/// What building for Android needs beyond the lift: the tools, the SDL3 and
/// libopus builds, the signing key and the application id.
pub struct AndroidBuild {
    pub target: Target,
    pub ndk: Ndk,
    pub sdk: Sdk,
    pub deps: Deps,
    pub signing: Signing,
    pub package: String,
}

impl AndroidBuild {
    /// Finds every tool and dependency, or says what is missing.
    ///
    /// # Errors
    ///
    /// A missing tool or dependency, or a bad package name.
    pub fn prepare(settings: &Settings, target: Target, name: &str) -> Result<Self, BoxError> {
        let cache = crate::paths::canonical(cache_root(settings)?)?;
        let sdk = Sdk::find(settings)?;
        let package = if settings.package.android_id.is_empty() {
            default_package(name)
        } else {
            settings.package.android_id.clone()
        };
        if !valid_package(&package) {
            return Err(format!("package.android-id {package}: not a Java package name").into());
        }
        Ok(Self {
            target,
            ndk: Ndk::find(settings, target)?,
            deps: Deps::find(&cache, target)?,
            signing: Signing::find(settings, &sdk, &cache)?,
            sdk,
            package,
        })
    }
}

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

/// What goes into one APK.
pub struct Apk<'a> {
    pub package: &'a str,
    pub label: &'a str,
    /// The launcher icon: a density folder (`mipmap-xxhdpi`) and PNG bytes
    /// for each size. Empty: the system's default icon.
    pub icons: &'a [(&'static str, Vec<u8>)],
    pub target: Target,
    /// The program's shared library, installed under `library`.
    pub program: &'a Path,
    /// The project's file name: the program is installed as
    /// [`library`]`(stem)` and the activity loads it by `stem`.
    pub stem: &'a str,
    pub deps: &'a Deps,
    /// Where intermediate files go.
    pub work: &'a Path,
    /// The signed APK to write.
    pub output: &'a Path,
    /// The game data manifest, shipped as an asset (crate::datamanifest).
    pub data_manifest: Option<&'a [u8]>,
    /// The build's fallback fonts (crate::fonts::packaged), shipped as the
    /// assets `fonts/<type>.ttf`.
    pub fonts: Option<&'a Path>,
    /// What the settings pages say about the game.
    pub about: &'a About,
}

/// What the settings pages say about the game and offer for it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct About {
    /// The platform's name, such as `Switch`.
    pub platform: String,
    /// The game's version, or empty.
    pub version: String,
    /// The game draws with Vulkan, so a GPU driver can be chosen for it.
    pub vulkan: bool,
    /// The game follows the device's rotation (a tall picture); else the
    /// activity keeps the sensor landscape orientation.
    pub portrait: bool,
}

/// The name the settings pages give a guest platform.
#[must_use]
pub fn platform_name(platform: &str) -> &str {
    match platform {
        "switch" => "Switch",
        "gb" => "Game Boy",
        "gbc" => "Game Boy Color",
        "gba" => "Game Boy Advance",
        "nds" => "Nintendo DS",
        "sms" => "Master System",
        "gg" => "Game Gear",
        "sg1000" => "SG-1000",
        other => other,
    }
}

/// How an APK is signed.
pub struct Signing {
    pub keystore: PathBuf,
    pub password: String,
    pub debug: bool,
}

const DEBUG_PASSWORD: &str = "relift-debug";
const KEY_ALIAS: &str = "relift";

impl Signing {
    /// The user's key from `package.android-keystore`, or a debug key kept under
    /// `<cache>/android/` and made on first use.
    ///
    /// # Errors
    ///
    /// `keytool` cannot make the debug key, or a keystore is named without
    /// its password.
    pub fn find(settings: &Settings, sdk: &Sdk, cache_root: &Path) -> Result<Self, BoxError> {
        if !settings.package.android_keystore.as_os_str().is_empty() {
            if settings.package.android_keystore_pass.is_empty() {
                return Err(
                    "package.android-keystore is set without package.android-keystore-pass".into(),
                );
            }
            return Ok(Self {
                keystore: settings.package.android_keystore.clone(),
                password: settings.package.android_keystore_pass.clone(),
                debug: false,
            });
        }
        let ks = cache_root.join("android").join("debug.keystore");
        if !ks.is_file() {
            std::fs::create_dir_all(ks.parent().ok_or("no cache directory")?)?;
            run(
                relift_proc::command(&sdk.keytool)
                    .args(["-genkeypair", "-keystore"])
                    .arg(&ks)
                    .args(["-storepass", DEBUG_PASSWORD, "-keypass", DEBUG_PASSWORD])
                    .args(["-alias", KEY_ALIAS, "-keyalg", "RSA", "-keysize", "2048"])
                    .args(["-validity", "10000", "-dname", "CN=relift debug"]),
                "keytool",
            )?;
        }
        Ok(Self {
            keystore: ks,
            password: DEBUG_PASSWORD.into(),
            debug: true,
        })
    }
}

fn xml(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('"', "&quot;")
}

fn manifest(a: &Apk<'_>, sdk: &Sdk, debuggable: bool) -> String {
    let label = xml(a.label);
    format!(
        r#"<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{package}" android:versionCode="1" android:versionName="1.0">
  <uses-sdk android:minSdkVersion="{MIN_API}" android:targetSdkVersion="{target}"/>
  <uses-feature android:glEsVersion="0x00020000"/>
  <uses-feature android:name="android.hardware.touchscreen" android:required="false"/>
  <uses-feature android:name="android.hardware.gamepad" android:required="false"/>
  {internet}{data_permissions}<application android:label="{label}"{icon} android:extractNativeLibs="false" android:debuggable="{debuggable}" android:allowBackup="false" android:hardwareAccelerated="true" android:theme="@android:style/Theme.NoTitleBar.Fullscreen">
    <meta-data android:name="relift.gpu_driver" android:value="{vulkan}"/>
    <meta-data android:name="relift.library" android:value="{stem}"/>
    <meta-data android:name="relift.platform" android:value="{platform}"/>
    <meta-data android:name="relift.game_version" android:value="{version}"/>
    <activity android:name="{ACTIVITY}" android:exported="true" android:launchMode="singleInstance" android:screenOrientation="{orientation}" android:configChanges="layoutDirection|locale|fontScale|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation">
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
      <meta-data android:name="android.app.shortcuts" android:resource="@xml/shortcuts"/>
    </activity>
    <activity android:name="dev.relift.app.SettingsActivity" android:exported="false" android:taskAffinity="{package}.settings" android:excludeFromRecents="true" android:screenOrientation="behind" android:configChanges="orientation|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation"/>
    <activity android:name="dev.relift.app.Restarter" android:exported="false" android:process=":restart" android:taskAffinity="{package}.restart" android:excludeFromRecents="true" android:theme="@android:style/Theme.Translucent.NoTitleBar"/>
    <service android:name="dev.relift.app.GpuProbe" android:exported="false" android:process=":gpucheck"/>
{data}  </application>
</manifest>
"#,
        package = a.package,
        target = sdk.target_sdk,
        internet = if a.about.vulkan {
            "<uses-permission android:name=\"android.permission.INTERNET\"/>\n  <uses-permission android:name=\"android.permission.ACCESS_NETWORK_STATE\"/>\n  "
        } else {
            ""
        },
        data_permissions = if a.data_manifest.is_some() {
            DATA_PERMISSIONS
        } else {
            ""
        },
        data = if a.data_manifest.is_some() {
            data_components(a.package)
        } else {
            String::new()
        },
        vulkan = a.about.vulkan,
        orientation = if a.about.portrait {
            "fullUser"
        } else {
            "sensorLandscape"
        },
        stem = xml(a.stem),
        platform = xml(&a.about.platform),
        version = xml(&a.about.version),
        icon = if a.icons.is_empty() {
            ""
        } else {
            " android:icon=\"@mipmap/ic_launcher\""
        },
    )
}

/// What the game data import needs: a foreground service that keeps copying
/// when the player leaves the app, its notification and a wake lock.
const DATA_PERMISSIONS: &str =
    "<uses-permission android:name=\"android.permission.FOREGROUND_SERVICE\"/>
  <uses-permission android:name=\"android.permission.FOREGROUND_SERVICE_DATA_SYNC\"/>
  <uses-permission android:name=\"android.permission.POST_NOTIFICATIONS\"/>
  <uses-permission android:name=\"android.permission.WAKE_LOCK\"/>
  ";

/// The game data import's activity and service, in a process of their own
/// so the copy outlives the game's.
fn data_components(package: &str) -> String {
    format!(
        r#"    <activity android:name="dev.relift.app.DataActivity" android:exported="false" android:process=":data" android:launchMode="singleTask" android:taskAffinity="{package}.data" android:screenOrientation="behind" android:configChanges="orientation|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation"/>
    <service android:name="dev.relift.app.DataService" android:exported="false" android:process=":data" android:foregroundServiceType="dataSync"/>
"#
    )
}

/// Whether `s` is a Java package name.
#[must_use]
pub fn valid_package(s: &str) -> bool {
    let parts: Vec<&str> = s.split('.').collect();
    parts.len() >= 2
        && parts.iter().all(|p| {
            p.chars().next().is_some_and(|c| c.is_ascii_alphabetic())
                && p.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')
        })
}

/// `dev.relift.game.<name>` with `name` cut down to what a package part
/// allows.
#[must_use]
pub fn default_package(name: &str) -> String {
    let slug: String = name
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() {
                c.to_ascii_lowercase()
            } else {
                '_'
            }
        })
        .collect();
    let slug = slug.trim_matches('_');
    if slug.is_empty() || !slug.starts_with(|c: char| c.is_ascii_alphabetic()) {
        format!("dev.relift.game.g{slug}")
    } else {
        format!("dev.relift.game.{slug}")
    }
}

/// The packaging step of an Android build: the signed APK of the program
/// `exe`, written beside it, with `label` as the launcher name.
///
/// # Errors
///
/// A tool fails; its output is in the message.
pub fn package(
    a: &AndroidBuild,
    exe: &Path,
    stem: &str,
    launcher: &Launcher,
    data_manifest: Option<&[u8]>,
    fonts: Option<&Path>,
    ui: &mut Ui,
) -> Result<PathBuf, BoxError> {
    let dir = exe.parent().ok_or("the program has no folder")?;
    let row = ui.row("");
    ui.begin(row, "package");
    let apk = dir.join(format!("{stem}.apk"));
    build_apk(
        &Apk {
            package: &a.package,
            label: &launcher.label,
            icons: &launcher.icons,
            target: a.target,
            program: exe,
            stem,
            deps: &a.deps,
            work: &dir.join("apk"),
            output: &apk,
            data_manifest,
            fonts,
            about: &launcher.about,
        },
        &a.sdk,
        &a.ndk,
        &a.signing,
    )?;
    ui.done(
        row,
        Line::new().text(format!(
            "{} ({}, signed with the {} key)",
            a.package,
            a.target.abi,
            if a.signing.debug { "debug" } else { "given" }
        )),
    );
    Ok(apk)
}

/// What the launcher shows for a game.
pub struct Launcher {
    /// The name under the icon.
    pub label: String,
    /// A density folder and the PNG bytes for each size.
    pub icons: Vec<(&'static str, Vec<u8>)>,
    /// What the settings pages say about the game.
    pub about: About,
}

/// The launcher of the program `exe`, from the identity it embeds: its
/// title (else `label`) and its icon at each density (else the generated
/// one for `seed`).
///
/// # Errors
///
/// The program cannot be read or carries no self-description.
pub fn launcher(exe: &Path, seed: &str, label: &str) -> Result<Launcher, BoxError> {
    let image = std::fs::read(exe)?;
    let doc = relift_info::section::read(&image).map_err(|e| format!("{}: {e}", exe.display()))?;
    let info = relift_info::Info::from_bytes(doc)?;
    Ok(Launcher {
        label: crate::appimage::display_name(&info, label),
        icons: crate::icon::launcher_pngs(info.identity.icon.as_ref(), seed),
        about: About {
            platform: platform_name(&info.identity.platform).to_string(),
            version: info.identity.version.clone().unwrap_or_default(),
            vulkan: info.identity.platform == "switch",
            portrait: info.identity.platform == "nds",
        },
    })
}

/// The strings the manifest and the shortcuts name.
const STRINGS_XML: &str = r#"<?xml version="1.0" encoding="utf-8"?>
<resources>
  <string name="game_settings">Game settings</string>
</resources>
"#;

/// The Game settings shortcut on a long press of the game's icon.
fn shortcuts_xml(package: &str) -> String {
    format!(
        r#"<?xml version="1.0" encoding="utf-8"?>
<shortcuts xmlns:android="http://schemas.android.com/apk/res/android">
  <shortcut android:shortcutId="game_settings" android:enabled="true" android:icon="@mipmap/ic_launcher" android:shortcutShortLabel="@string/game_settings" android:shortcutLongLabel="@string/game_settings">
    <intent android:action="android.intent.action.VIEW" android:targetPackage="{package}" android:targetClass="dev.relift.app.SettingsActivity"/>
  </shortcut>
</shortcuts>
"#
    )
}

/// Links the manifest and the resources of the APK into `res/` of its work
/// folder, with the launcher icon compiled in.
fn link_resources(a: &Apk<'_>, sdk: &Sdk, debuggable: bool) -> Result<(), BoxError> {
    let manifest_path = a.work.join("AndroidManifest.xml");
    relift_cache::write_if_changed(&manifest_path, manifest(a, sdk, debuggable).as_bytes())?;
    let flat = a.work.join("flat");
    std::fs::create_dir_all(&flat)?;
    let mut compiled = Vec::new();
    for (dir, png) in a.icons {
        let src = a.work.join("icons").join(dir);
        std::fs::create_dir_all(&src)?;
        let file = src.join("ic_launcher.png");
        relift_cache::write_if_changed(&file, png)?;
        run(
            relift_proc::command(&sdk.aapt2)
                .arg("compile")
                .arg(&file)
                .arg("-o")
                .arg(&flat),
            "aapt2 compile",
        )?;
        compiled.push(flat.join(format!("{dir}_ic_launcher.png.flat")));
    }
    let values = a.work.join("values").join("values");
    std::fs::create_dir_all(&values)?;
    let strings = values.join("strings.xml");
    relift_cache::write_if_changed(&strings, STRINGS_XML.as_bytes())?;
    let xml_dir = a.work.join("values").join("xml");
    std::fs::create_dir_all(&xml_dir)?;
    let shortcuts = xml_dir.join("shortcuts.xml");
    relift_cache::write_if_changed(&shortcuts, shortcuts_xml(a.package).as_bytes())?;
    for f in [&strings, &shortcuts] {
        run(
            relift_proc::command(&sdk.aapt2)
                .arg("compile")
                .arg(f)
                .arg("-o")
                .arg(&flat),
            "aapt2 compile",
        )?;
    }
    compiled.push(flat.join("values_strings.arsc.flat"));
    compiled.push(flat.join("xml_shortcuts.xml.flat"));
    run(
        relift_proc::command(&sdk.aapt2)
            .arg("link")
            .arg("--manifest")
            .arg(&manifest_path)
            .args(&compiled)
            .arg("-I")
            .arg(&sdk.android_jar)
            .args(["--output-to-dir", "-o"])
            .arg(a.work.join("res")),
        "aapt2",
    )?;
    Ok(())
}

/// The page size Android 15 and later may run with; native libraries must
/// be built and stored for it.
const PAGE: usize = 16384;

/// The `p_align` of every loadable segment of the ELF image `elf`, or what
/// is wrong with it.
fn load_alignments(elf: &[u8]) -> Result<Vec<u64>, String> {
    let rd = |at: usize, n: usize| elf.get(at..at + n).ok_or("a truncated ELF header");
    if rd(0, 4)? != b"\x7fELF" || rd(4, 1)?[0] != 2 {
        return Err("not a 64-bit ELF".into());
    }
    let u16_at = |at: usize| rd(at, 2).map(|b| usize::from(u16::from_le_bytes([b[0], b[1]])));
    let u64_at = |at: usize| {
        rd(at, 8).map(|b| u64::from_le_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
    };
    let phoff = usize::try_from(u64_at(0x20)?).map_err(|_| "a bad program header offset")?;
    let entsize = u16_at(0x36)?;
    let count = u16_at(0x38)?;
    let mut out = Vec::new();
    for i in 0..count {
        let at = phoff + i * entsize;
        if rd(at, 4)? == [1, 0, 0, 0] {
            out.push(u64_at(at + 0x30)?);
        }
    }
    Ok(out)
}

/// Fails when a native library in the APK `apk` cannot be mapped on a
/// device with 16 KiB pages: an entry that is compressed or does not start
/// on a 16 KiB boundary, or a loadable segment aligned to less.
///
/// # Errors
///
/// The first offending library, named with the reason.
pub fn check_page_alignment(apk: &[u8]) -> Result<(), BoxError> {
    for e in zip::entries(apk)? {
        if !(e.name.starts_with("lib/") && e.name.ends_with(".so")) {
            continue;
        }
        if e.method != 0 {
            return Err(format!("{}: stored compressed; it must be stored", e.name).into());
        }
        if e.offset % PAGE != 0 {
            return Err(format!(
                "{}: starts at byte {}, not on a 16 KiB boundary",
                e.name, e.offset
            )
            .into());
        }
        for align in load_alignments(e.data).map_err(|m| format!("{}: {m}", e.name))? {
            if align < PAGE as u64 {
                return Err(format!(
                    "{}: a loadable segment is aligned to {align:#x}; link with -Wl,-z,max-page-size=16384",
                    e.name
                )
                .into());
            }
        }
    }
    Ok(())
}

/// Makes the signed APK of `a`.
///
/// # Errors
///
/// A tool fails; its output is in the message.
fn classes(dir: &Path) -> Result<Vec<PathBuf>, BoxError> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir)?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "class"))
        .collect();
    v.sort();
    Ok(v)
}

/// The compiled XML resources aapt2 wrote under `res/res` besides the icons.
fn compiled_resources(w: &Path) -> Result<Vec<(String, Vec<u8>)>, BoxError> {
    let mut out = Vec::new();
    let base = w.join("res/res");
    let Ok(dirs) = std::fs::read_dir(&base) else {
        return Ok(out);
    };
    let mut dirs: Vec<PathBuf> = dirs.filter_map(|e| e.ok().map(|e| e.path())).collect();
    dirs.sort();
    for d in dirs {
        let dn = d
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        if !dn.starts_with("xml") {
            continue;
        }
        let mut files: Vec<PathBuf> = std::fs::read_dir(&d)?
            .filter_map(|e| e.ok().map(|e| e.path()))
            .collect();
        files.sort();
        for f in files {
            let n = f
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .into_owned();
            out.push((format!("res/{dn}/{n}"), std::fs::read(&f)?));
        }
    }
    Ok(out)
}

pub fn build_apk(a: &Apk<'_>, sdk: &Sdk, ndk: &Ndk, signing: &Signing) -> Result<(), BoxError> {
    let w = a.work;
    let _ = std::fs::remove_dir_all(w);
    for d in ["java", "classes", "dex", "res"] {
        std::fs::create_dir_all(w.join(d))?;
    }
    let mut sources = Vec::new();
    for (class, text) in JAVA {
        let f = w.join("java").join(format!("{class}.java"));
        relift_cache::write_if_changed(&f, text.as_bytes())?;
        sources.push(f);
    }
    let cp = format!(
        "{}{}{}",
        sdk.android_jar.display(),
        if cfg!(windows) { ";" } else { ":" },
        a.deps.sdl3_jar.display()
    );
    run(
        relift_proc::command(&sdk.javac)
            .args(["--release", "11", "-Xlint:-options", "-cp", &cp, "-d"])
            .arg(w.join("classes"))
            .args(&sources),
        "javac",
    )?;
    run(
        relift_proc::command(&sdk.d8)
            .args(["--release", "--min-api", &MIN_API.to_string(), "--lib"])
            .arg(&sdk.android_jar)
            .arg("--output")
            .arg(w.join("dex"))
            .args(classes(&w.join("classes/dev/relift/app"))?)
            .arg(&a.deps.sdl3_jar),
        "d8",
    )?;
    link_resources(a, sdk, signing.debug)?;
    let lib = library(a.stem);
    let stripped = w.join(&lib);
    run(
        relift_proc::command(&ndk.strip)
            .arg("--strip-unneeded")
            .arg("-o")
            .arg(&stripped)
            .arg(a.program),
        "llvm-strip",
    )?;
    let mut z = zip::Zip::default();
    z.add(
        "AndroidManifest.xml",
        std::fs::read(w.join("res/AndroidManifest.xml"))?,
    );
    z.add(
        "resources.arsc",
        std::fs::read(w.join("res/resources.arsc"))?,
    );
    for (dir, _) in a.icons {
        let name = format!("{dir}-v4/ic_launcher.png");
        z.add(
            &format!("res/{name}"),
            std::fs::read(w.join("res/res").join(&name))?,
        );
    }
    z.add("classes.dex", std::fs::read(w.join("dex/classes.dex"))?);
    z.add(
        &format!("lib/{}/{lib}", a.target.abi),
        std::fs::read(&stripped)?,
    );
    z.add(
        &format!("lib/{}/libSDL3.so", a.target.abi),
        std::fs::read(&a.deps.sdl3_lib)?,
    );
    for lib in a
        .deps
        .adrenotools
        .iter()
        .flat_map(|t| std::iter::once(&t.lib).chain(&t.hooks))
    {
        let name = lib.file_name().ok_or("a library path without a name")?;
        z.add(
            &format!("lib/{}/{}", a.target.abi, name.to_string_lossy()),
            std::fs::read(lib)?,
        );
    }
    if let Some(m) = a.data_manifest {
        z.add(&format!("assets/{}", crate::datamanifest::FILE), m.to_vec());
    }
    if let Some(f) = a.fonts {
        for (name, bytes) in crate::fonts::files(f)? {
            z.add(&format!("assets/fonts/{name}"), bytes);
        }
    }
    if a.about.vulkan {
        z.add(
            "assets/gpu_drivers.json",
            GPU_DRIVERS_JSON.as_bytes().to_vec(),
        );
    }
    for (name, data) in compiled_resources(w)? {
        z.add(&name, data);
    }
    for (name, text) in a.deps.adrenotools.iter().flat_map(|t| &t.licenses) {
        z.add(&format!("assets/licenses/{name}.txt"), std::fs::read(text)?);
    }
    z.add("assets/licenses/fxaa.txt", LICENSE_FXAA.as_bytes().to_vec());
    z.add("assets/licenses/smaa.txt", LICENSE_SMAA.as_bytes().to_vec());
    z.add("assets/licenses/fsr1.txt", LICENSE_FSR1.as_bytes().to_vec());
    z.add(
        "assets/licenses/renderdoc.txt",
        LICENSE_RENDERDOC.as_bytes().to_vec(),
    );
    z.add("assets/licenses/fsr2.txt", LICENSE_FSR2.as_bytes().to_vec());
    z.add("assets/licenses/fsr3.txt", LICENSE_FSR3.as_bytes().to_vec());
    if a.deps.dav1d.is_some() {
        z.add(
            "assets/licenses/dav1d.txt",
            include_bytes!("../../runtime/host/dav1d/NOTICE").to_vec(),
        );
    }
    if a.about.vulkan {
        z.add("assets/licenses/zstd.txt", LICENSE_ZSTD.as_bytes().to_vec());
    }
    let unaligned = w.join("unaligned.apk");
    relift_cache::write_if_changed(&unaligned, &z.finish()?)?;
    let aligned = w.join("aligned.apk");
    run(
        relift_proc::command(&sdk.zipalign)
            .args(["-f", "-P", "16", "4"])
            .arg(&unaligned)
            .arg(&aligned),
        "zipalign",
    )?;
    if let Some(d) = a.output.parent() {
        std::fs::create_dir_all(d)?;
    }
    run(
        relift_proc::command(&sdk.apksigner)
            .arg("sign")
            .arg("--ks")
            .arg(&signing.keystore)
            .args([
                "--ks-key-alias",
                KEY_ALIAS,
                "--ks-pass",
                "env:RELIFT_KS_PASS",
                "--out",
            ])
            .arg(a.output)
            .arg(&aligned)
            .env("RELIFT_KS_PASS", &signing.password),
        "apksigner",
    )?;
    run(
        relift_proc::command(&sdk.apksigner)
            .arg("verify")
            .arg(a.output),
        "apksigner verify",
    )?;
    check_page_alignment(&std::fs::read(a.output)?)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_library_is_named_after_the_project() {
        assert_eq!(library("example-project-2"), "libexample-project-2.so");
    }

    #[test]
    fn target_names_map_to_triples() {
        assert_eq!(
            target("android-arm64").unwrap().triple,
            "aarch64-linux-android"
        );
        assert_eq!(target("android-x64").unwrap().abi, "x86_64");
        assert!(target("host").is_none());
    }

    #[test]
    fn the_manifest_decides_the_orientation() {
        assert!(JAVA[0].1.contains(
            "public void setOrientationBis(int w, int h, boolean resizable, String hint) {\n    }"
        ));
    }

    #[test]
    fn displays_besides_the_game_keep_the_panel_landscape() {
        let java = JAVA[0].1;
        assert!(java.contains("void applyOrientation()"));
        assert!(java.contains("displays.count() > 0"));
        assert!(java.contains("SCREEN_ORIENTATION_SENSOR_LANDSCAPE"));
        assert!(java.contains("registerDisplayListener(watch, null)"));
        assert!(java.contains("unregisterDisplayListener(watch)"));
    }

    #[test]
    fn a_task_moved_to_another_display_goes_back_to_the_main_one() {
        let java = JAVA[0].1;
        assert!(java.contains("private void keepOnMainDisplay()"));
        assert!(java.contains("setLaunchDisplayId(Display.DEFAULT_DISPLAY)"));
        assert!(java.contains("if (!resumed || own == null"));
        assert!(!java.contains("onConfigurationChanged"));
        let event = java.split("private void displayEvent()").nth(1).unwrap();
        let event = &event[..event.find("}\n").unwrap()];
        assert!(!event.contains("keepOnMainDisplay"));
        assert!(java.contains("protected void onResume()"));
    }

    #[test]
    fn the_settings_pages_keep_the_dark_palette_and_have_a_light_one() {
        let ui = JAVA.iter().find(|j| j.0 == "Ui").unwrap().1;
        for (l, d) in [
            ("0xfff4f5f7", "0xff14161a"),
            ("0xffffffff", "0xff1e2127"),
            ("0xff1f5fcc", "0xff8ab4ff"),
            ("0xff15181d", "0xffffffff"),
        ] {
            assert!(ui.contains(&format!("l ? {l} : {d};")), "{l} {d}");
        }
        assert!(ui.contains("UI_MODE_NIGHT_MASK"));
        for (name, java) in JAVA {
            if name == "Ui" || name == "DataPages" {
                assert!(
                    !java.contains("Color.WHITE"),
                    "{name} draws text in a fixed white"
                );
            }
        }
    }

    #[test]
    fn the_shader_math_choice_reaches_the_runtime_at_start() {
        let main = JAVA[0].1;
        assert!(main.contains("args.add(\"gpu.math=\" + launchedMath);"));
        let maths = JAVA.iter().find(|j| j.0 == "Maths").unwrap().1;
        assert!(maths.contains("static final String EXACT = \"exact\";"));
        assert!(maths.contains("static final String NATIVE = \"native\";"));
        assert!(maths.contains("prefs.getString(\"gpu.math\", EXACT)"));
        let ui = JAVA.iter().find(|j| j.0 == "Ui").unwrap().1;
        assert!(ui.contains("\"Takes effect next start\""));
        assert!(!ui.contains("Same as all games"));
    }

    #[test]
    fn presentations_follow_what_the_runtime_asked_for() {
        let d = JAVA[7].1;
        assert!(JAVA[7].0 == "Displays");
        assert!(d.contains("void sync()"));
        assert!(d.contains("p.displayId() != d.get(i).getDisplayId()"));
        assert!(d.contains("later.postDelayed(this::sync, RESHOW_MS)"));
        assert!(d.contains("c.hide(WindowInsets.Type.systemBars())"));
        assert!(d.contains("SYSTEM_UI_FLAG_IMMERSIVE_STICKY"));
        assert!(d.contains("public void onBackPressed() {\n        }"));
        assert!(
            d.contains("registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT")
        );
        assert!(d.contains("retry(i);"));
        assert!(d.contains("RETRY_MAX_MS"));
    }

    #[test]
    fn a_package_comes_from_the_project_name() {
        assert_eq!(default_package("My Game!"), "dev.relift.game.my_game");
        assert_eq!(default_package("2048"), "dev.relift.game.g2048");
        assert!(valid_package(&default_package("hp cos")));
        assert!(!valid_package("nodots"));
        assert!(!valid_package("a.1b"));
    }

    #[test]
    fn versions_sort_numerically_newest_first() {
        let mut v = [
            PathBuf::from("34.0.0"),
            PathBuf::from("5.1"),
            PathBuf::from("36.0.0"),
        ];
        v.sort_by_key(|p| std::cmp::Reverse(version_key(p)));
        assert_eq!(v[0], PathBuf::from("36.0.0"));
        assert_eq!(v[2], PathBuf::from("5.1"));
    }

    fn elf(align: u64) -> Vec<u8> {
        let mut b = vec![0u8; 0x40 + 56];
        b[..4].copy_from_slice(b"\x7fELF");
        b[4] = 2;
        b[0x20..0x28].copy_from_slice(&0x40u64.to_le_bytes());
        b[0x36..0x38].copy_from_slice(&56u16.to_le_bytes());
        b[0x38..0x3a].copy_from_slice(&1u16.to_le_bytes());
        b[0x40..0x44].copy_from_slice(&1u32.to_le_bytes());
        b[0x40 + 0x30..0x40 + 0x38].copy_from_slice(&align.to_le_bytes());
        b
    }

    fn apk_with(lib: &[u8], pad: usize) -> Vec<u8> {
        let mut z = zip::Zip::default();
        z.add("pad", vec![0; pad]);
        z.add("lib/arm64-v8a/libx.so", lib.to_vec());
        z.finish().unwrap()
    }

    #[test]
    fn a_library_stored_off_the_page_or_aligned_low_fails_the_check() {
        assert!(check_page_alignment(&apk_with(&elf(0x4000), PAGE - 84)).is_ok());
        let m = check_page_alignment(&apk_with(&elf(0x4000), PAGE - 83))
            .unwrap_err()
            .to_string();
        assert!(m.contains("not on a 16 KiB boundary"), "{m}");
        let m = check_page_alignment(&apk_with(&elf(0x1000), PAGE - 84))
            .unwrap_err()
            .to_string();
        assert!(m.contains("max-page-size"), "{m}");
        assert_eq!(load_alignments(&elf(0x4000)).unwrap(), [0x4000]);
        assert!(load_alignments(b"nope").is_err());
    }

    #[test]
    fn the_manifest_names_the_activity_and_the_target() {
        let sdk = Sdk {
            aapt2: PathBuf::new(),
            d8: PathBuf::new(),
            zipalign: PathBuf::new(),
            apksigner: PathBuf::new(),
            android_jar: PathBuf::new(),
            target_sdk: 36,
            javac: PathBuf::new(),
            keytool: PathBuf::new(),
        };
        let deps = Deps {
            sdl3_include: PathBuf::new(),
            sdl3_lib: PathBuf::new(),
            sdl3_jar: PathBuf::new(),
            opus_include: PathBuf::new(),
            opus_lib: PathBuf::new(),
            adrenotools: None,
            vpx: None,
            dav1d: None,
        };
        let a = Apk {
            package: "dev.relift.game.x",
            label: "A & \"B\"",
            icons: &[],
            target: target("android-arm64").unwrap(),
            program: Path::new("p"),
            stem: "example-project-2",
            deps: &deps,
            work: Path::new("w"),
            output: Path::new("o"),
            data_manifest: None,
            fonts: None,
            about: &About {
                platform: "Switch".into(),
                version: "1.0.2".into(),
                vulkan: true,
                portrait: false,
            },
        };
        let m = manifest(&a, &sdk, true);
        assert!(m.contains("android:screenOrientation=\"sensorLandscape\""));
        assert!(m.contains("package=\"dev.relift.game.x\""));
        assert!(m.contains("android:targetSdkVersion=\"36\""));
        assert!(m.contains("android:name=\"dev.relift.app.MainActivity\""));
        assert!(m.contains("A &amp; &quot;B&quot;"));
        assert!(m.contains("android:debuggable=\"true\""));
        assert!(m.contains("android.permission.INTERNET"));
        assert!(m.contains("android:name=\"relift.gpu_driver\" android:value=\"true\""));
        assert!(m.contains("android:process=\":gpucheck\""));
        assert!(m.contains("android:name=\"relift.library\" android:value=\"example-project-2\""));
        assert!(m.contains("android:taskAffinity=\"dev.relift.game.x.settings\" android:excludeFromRecents=\"true\" android:screenOrientation=\"behind\""));
        assert!(!m.contains("DataService"));
        let with_data = Apk {
            data_manifest: Some(b""),
            ..a
        };
        let m = manifest(&with_data, &sdk, true);
        assert!(m.contains("android:name=\"dev.relift.app.DataService\" android:exported=\"false\" android:process=\":data\" android:foregroundServiceType=\"dataSync\""));
        assert!(m.contains(
            "android:taskAffinity=\"dev.relift.game.x.data\" android:screenOrientation=\"behind\""
        ));
        assert!(m.contains("android.permission.FOREGROUND_SERVICE_DATA_SYNC"));
        assert!(m.contains("android.permission.POST_NOTIFICATIONS"));
    }

    #[test]
    fn the_driver_list_is_https_with_a_checksum_per_entry() {
        let v: serde_json::Value = serde_json::from_str(GPU_DRIVERS_JSON).unwrap();
        let list = v["drivers"].as_array().unwrap();
        assert!(!list.is_empty());
        for d in list {
            assert!(d["url"].as_str().unwrap().starts_with("https://"));
            let sha = d["sha256"].as_str().unwrap();
            assert_eq!(sha.len(), 64);
            assert!(sha.bytes().all(|c| c.is_ascii_hexdigit()));
            assert!(d["size"].as_u64().unwrap() > 0);
        }
    }
}

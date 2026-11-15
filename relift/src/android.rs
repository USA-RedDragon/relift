//! Android as a host target: the NDK compiler, the SDL3 and libopus builds
//! for the target, and the APK around the program's shared library.
//!
//! The program is a shared library named `libmain.so` that SDL's Java
//! activity loads and enters through `SDL_main`. relift drives the Android
//! SDK's own tools (`aapt2`, `d8`, `zipalign`, `apksigner`) and `javac`; no
//! Gradle. SDL3's Java glue is the `SDL3.jar` that its CMake install makes.

mod zip;

use std::path::{Path, PathBuf};

use crate::import::{BoxError, cache_root};
use crate::settings::Settings;
use crate::ui::Ui;
use relift_progress::Line;

/// The Android API level the runtime builds for (`memfd_create` in bionic).
pub const MIN_API: u32 = 30;

/// The activity class of the APK: SDL's activity plus launch arguments and
/// environment variables from the intent, and the haptic tick of the
/// on-screen controller.
const ACTIVITY: &str = "dev.relift.app.MainActivity";

const ACTIVITY_JAVA: &str = r#"package dev.relift.app;

import android.os.Build;
import android.os.Bundle;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import org.libsdl.app.SDLActivity;

public class MainActivity extends SDLActivity implements Runnable, OnBackInvokedCallback {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, this);
        }
    }

    @Override
    public void onBackInvoked() {
        onNativeKeyDown(KeyEvent.KEYCODE_BACK);
        onNativeKeyUp(KeyEvent.KEYCODE_BACK);
    }

    @Override
    protected String[] getArguments() {
        String e = getIntent().getStringExtra("relift_env");
        if (e != null) {
            for (String kv : e.trim().split("\\s+")) {
                int i = kv.indexOf('=');
                if (i > 0) {
                    nativeSetenv(kv.substring(0, i), kv.substring(i + 1));
                }
            }
        }
        String a = getIntent().getStringExtra("relift_args");
        if (a == null || a.trim().isEmpty()) {
            return new String[0];
        }
        return a.trim().split("\\s+");
    }

    public void reliftHaptic() {
        runOnUiThread(this);
    }

    @Override
    public void run() {
        getWindow().getDecorView().performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
    }
}
"#;

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
}

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
        let d = Self {
            sdl3_include: sdl.join("include"),
            sdl3_lib: sdl.join("lib").join("libSDL3.so"),
            sdl3_jar: sdl.join("java").join("SDL3.jar"),
            opus_include: opus.join("include"),
            opus_lib: opus.join("lib").join("libopus.a"),
        };
        for p in [&d.sdl3_lib, &d.sdl3_jar, &d.opus_lib] {
            if !p.is_file() {
                return Err(format!(
                    "{}: not found; build SDL3 and libopus for {} with `python3 runtime/tools/cross/android_deps.py --cache {}`",
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
    /// The program's shared library, installed as `libmain.so`.
    pub program: &'a Path,
    pub deps: &'a Deps,
    /// Where intermediate files go.
    pub work: &'a Path,
    /// The signed APK to write.
    pub output: &'a Path,
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

fn manifest(a: &Apk<'_>, sdk: &Sdk, debuggable: bool) -> String {
    let label = a
        .label
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('"', "&quot;");
    format!(
        r#"<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{package}" android:versionCode="1" android:versionName="1.0">
  <uses-sdk android:minSdkVersion="{MIN_API}" android:targetSdkVersion="{target}"/>
  <uses-feature android:glEsVersion="0x00020000"/>
  <uses-feature android:name="android.hardware.touchscreen" android:required="false"/>
  <uses-feature android:name="android.hardware.gamepad" android:required="false"/>
  <application android:label="{label}"{icon} android:extractNativeLibs="false" android:debuggable="{debuggable}" android:allowBackup="false" android:hardwareAccelerated="true" android:theme="@android:style/Theme.NoTitleBar.Fullscreen">
    <activity android:name="{ACTIVITY}" android:exported="true" android:launchMode="singleInstance" android:screenOrientation="sensorLandscape" android:configChanges="layoutDirection|locale|fontScale|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation">
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
    </activity>
  </application>
</manifest>
"#,
        package = a.package,
        target = sdk.target_sdk,
        icon = if a.icons.is_empty() {
            ""
        } else {
            " android:icon=\"@mipmap/ic_launcher\""
        },
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
    name: &str,
    label: &str,
    icons: &[(&'static str, Vec<u8>)],
    ui: &mut Ui,
) -> Result<PathBuf, BoxError> {
    let dir = exe.parent().ok_or("the program has no folder")?;
    let row = ui.row("");
    ui.begin(row, "package");
    let apk = dir.join(format!("{name}.apk"));
    build_apk(
        &Apk {
            package: &a.package,
            label,
            icons,
            target: a.target,
            program: exe,
            deps: &a.deps,
            work: &dir.join("apk"),
            output: &apk,
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
    })
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
pub fn build_apk(a: &Apk<'_>, sdk: &Sdk, ndk: &Ndk, signing: &Signing) -> Result<(), BoxError> {
    let w = a.work;
    let _ = std::fs::remove_dir_all(w);
    for d in ["java", "classes", "dex", "res"] {
        std::fs::create_dir_all(w.join(d))?;
    }
    let java = w.join("java").join("MainActivity.java");
    relift_cache::write_if_changed(&java, ACTIVITY_JAVA.as_bytes())?;
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
            .arg(&java),
        "javac",
    )?;
    run(
        relift_proc::command(&sdk.d8)
            .args(["--release", "--min-api", &MIN_API.to_string(), "--lib"])
            .arg(&sdk.android_jar)
            .arg("--output")
            .arg(w.join("dex"))
            .arg(w.join("classes/dev/relift/app/MainActivity.class"))
            .arg(&a.deps.sdl3_jar),
        "d8",
    )?;
    link_resources(a, sdk, signing.debug)?;
    let stripped = w.join("libmain.so");
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
        &format!("lib/{}/libmain.so", a.target.abi),
        std::fs::read(&stripped)?,
    );
    z.add(
        &format!("lib/{}/libSDL3.so", a.target.abi),
        std::fs::read(&a.deps.sdl3_lib)?,
    );
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
    fn target_names_map_to_triples() {
        assert_eq!(
            target("android-arm64").unwrap().triple,
            "aarch64-linux-android"
        );
        assert_eq!(target("android-x64").unwrap().abi, "x86_64");
        assert!(target("host").is_none());
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
        };
        let a = Apk {
            package: "dev.relift.game.x",
            label: "A & \"B\"",
            icons: &[],
            target: target("android-arm64").unwrap(),
            program: Path::new("p"),
            deps: &deps,
            work: Path::new("w"),
            output: Path::new("o"),
        };
        let m = manifest(&a, &sdk, true);
        assert!(m.contains("package=\"dev.relift.game.x\""));
        assert!(m.contains("android:targetSdkVersion=\"36\""));
        assert!(m.contains("android:name=\"dev.relift.app.MainActivity\""));
        assert!(m.contains("A &amp; &quot;B&quot;"));
        assert!(m.contains("android:debuggable=\"true\""));
    }
}

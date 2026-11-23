//! The tool's own settings, loaded by configulator-rs from defaults,
//! `relift.toml`, `RELIFT_*` environment variables and global flags.
//!
//! Per-game data lives in the project tables of the same file (see
//! [`crate::projectfile`]), never here. No key
//! may share a flag name with a subcommand flag, so `target` is never a key.

use std::path::PathBuf;

use configulator::{Config, Validate};

/// Global settings, shared by every subcommand.
#[derive(Config, Debug)]
pub struct Settings {
    #[configulator(
        name = "cache-dir",
        description = "where lifted, emitted and built artifacts are cached (empty: the user cache dir)"
    )]
    pub cache_dir: PathBuf,

    #[configulator(
        name = "jobs",
        short = 'j',
        default = "0",
        description = "worker threads for every parallel stage of a build: analysis, emit and compile (0: one per CPU)"
    )]
    pub jobs: u32,

    #[configulator(
        name = "runtime-dir",
        description = "the C runtime sources to build against (empty: the ones beside this relift's source)"
    )]
    pub runtime_dir: PathBuf,

    #[configulator(
        name = "color",
        default = "auto",
        description = "color in output: auto (a terminal that shows it, and NO_COLOR unset), always or never"
    )]
    pub color: String,

    #[configulator(
        name = "shader-math",
        default = "exact",
        description = "the GPU float math of the shader tables a build makes for this machine: exact (the console's results), or native or both (also the table of the host GPU's own math, which a run uses with gpu.math=native). Packages and phone targets always carry both"
    )]
    pub shader_math: String,

    #[configulator(name = "toolchain", nested)]
    pub toolchain: Toolchain,

    #[configulator(name = "emit", nested)]
    pub emit: Emit,

    #[configulator(name = "package", nested)]
    pub package: Package,
}

/// What a release build packages.
#[derive(Config, Debug)]
pub struct Package {
    #[configulator(
        name = "appimage",
        default = "auto",
        description = "the desktop release output: an AppImage on Linux, a signed .app on macOS. auto (an AppImage when appimagetool is found), always (a Linux build without appimagetool fails) or never (no desktop package; the Android APK is always built)"
    )]
    pub appimage: String,

    #[configulator(
        name = "android-id",
        description = "the application id of the Android APK (empty: dev.relift.game.<project folder>)"
    )]
    pub android_id: String,

    #[configulator(
        name = "android-keystore",
        description = "the keystore that signs the APK, with the key alias relift (empty: a debug key in the cache)"
    )]
    pub android_keystore: PathBuf,

    #[configulator(
        name = "android-keystore-pass",
        description = "the password of the keystore and its key"
    )]
    pub android_keystore_pass: String,

    #[configulator(
        name = "notarize",
        default = "false",
        description = "notarize a macOS game build with Apple (needs toolchain.codesign-identity and the notarize-* keys)"
    )]
    pub notarize: bool,

    #[configulator(
        name = "notarize-key",
        description = "the App Store Connect API key file (.p8) that notarization signs in with"
    )]
    pub notarize_key: PathBuf,

    #[configulator(
        name = "notarize-key-id",
        description = "the key ID of the notarization API key"
    )]
    pub notarize_key_id: String,

    #[configulator(
        name = "notarize-issuer",
        description = "the issuer ID of the notarization API key"
    )]
    pub notarize_issuer: String,

    #[configulator(
        name = "ios-id",
        description = "the bundle identifier of the iOS app (empty: dev.relift.game.<project folder>)"
    )]
    pub ios_id: String,

    #[configulator(
        name = "ios-identity",
        description = "the keychain identity that signs the iOS app for a device (empty: the first Apple Development identity)"
    )]
    pub ios_identity: String,

    #[configulator(
        name = "ios-data",
        default = "bundle",
        description = "where an iOS app finds the game data: bundle (copied into the app, sealed by its signature) or documents (added later to the app's Documents, for example with relift push)"
    )]
    pub ios_data: String,

    #[configulator(
        name = "ios-profile",
        description = "the provisioning profile of the iOS app (empty: one from Xcode's profile folders that covers the bundle identifier)"
    )]
    pub ios_profile: PathBuf,
}

/// How lifted code is written as C.
#[derive(Config, Debug)]
pub struct Emit {
    #[configulator(
        name = "shard-kib",
        default = "4096",
        description = "the most KiB of C per emitted file; clang needs about 120 bytes of memory per byte of C"
    )]
    pub shard_kib: u32,

    #[configulator(
        name = "piece-kib",
        default = "1024",
        description = "the most KiB of C per C function; a larger guest function is split into several"
    )]
    pub piece_kib: u32,

    #[configulator(
        name = "cold",
        default = "true",
        description = "also emit a cold copy of every function that dispatch enters at any instruction, so no guest code is missed"
    )]
    pub cold: bool,

    #[configulator(
        name = "cold-piece-kib",
        default = "4",
        description = "the most KiB of C per C function of the cold copy"
    )]
    pub cold_piece_kib: u32,

    #[configulator(
        name = "cold-first",
        default = "false",
        description = "for tests: run every function from the cold copy, which then calls only itself"
    )]
    pub cold_first: bool,

    #[configulator(
        name = "fpcr-guards",
        default = "false",
        description = "for debugging: keep the FPCR guards of lifted code even when the build proves the FP mode constant (RELIFT_FP_EXACT=1 needs them)"
    )]
    pub fpcr_guards: bool,

    #[configulator(
        name = "trampoline",
        default = "false",
        description = "make every guest return and indirect branch go through dispatch, so the host stack never mirrors the guest stack (a Linux program that the build proves safe otherwise returns directly)"
    )]
    pub trampoline: bool,

    #[configulator(
        name = "line-tables",
        default = "true",
        description = "give the lifted C of a host build line tables, so a crash names the guest function; false compiles it about 40% faster"
    )]
    pub line_tables: bool,
}

/// Host tools relift drives. An empty path means look it up on `PATH`.
#[derive(Config, Debug)]
pub struct Toolchain {
    #[configulator(name = "clang", description = "the clang driver")]
    pub clang: PathBuf,

    #[configulator(name = "zig", description = "zig, for `zig cc` player builds")]
    pub zig: PathBuf,

    #[configulator(name = "ninja", description = "the ninja build tool")]
    pub ninja: PathBuf,

    #[configulator(
        name = "appimagetool",
        description = "appimagetool, which packs the Linux release output"
    )]
    pub appimagetool: PathBuf,

    #[configulator(
        name = "android-sdk",
        description = "the Android SDK with build-tools and platforms (empty: ANDROID_HOME, then common places)"
    )]
    pub android_sdk: PathBuf,

    #[configulator(
        name = "android-ndk",
        description = "the Android NDK (empty: ANDROID_NDK_HOME, then the SDK's newest)"
    )]
    pub android_ndk: PathBuf,

    #[configulator(
        name = "codesign-identity",
        description = "the identity that signs macOS game builds, such as a Developer ID Application certificate name or its SHA-1 (empty: an ad-hoc signature)"
    )]
    pub codesign_identity: String,

    #[configulator(
        name = "codesign-keychain",
        description = "the keychain that holds the signing identity (empty: the default keychain search list)"
    )]
    pub codesign_keychain: PathBuf,
}

impl Settings {
    /// Makes the relative directory settings absolute, from the working
    /// directory: the build steps run in other directories and must find
    /// the same cache and runtime.
    ///
    /// # Errors
    ///
    /// No working directory.
    pub fn resolve_paths(&mut self) -> std::io::Result<()> {
        for p in [&mut self.cache_dir, &mut self.runtime_dir] {
            if !p.as_os_str().is_empty() {
                *p = std::path::absolute(&*p)?;
            }
        }
        Ok(())
    }
}

impl Validate for Settings {
    fn validate(&self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        relift_progress::ColorChoice::parse(&self.color)?;
        crate::appimage::Policy::parse(&self.package.appimage)?;
        Ok(())
    }
}

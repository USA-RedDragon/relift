//! The tool's own settings, loaded by configulator-rs from defaults,
//! `relift.toml`, `RELIFT_*` environment variables and global flags.
//!
//! Per-game data lives in `game.toml` (`relift-manifest`), never here. No key
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
        description = "parallel compile jobs (0: one per CPU)"
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

impl Validate for Settings {
    fn validate(&self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        relift_progress::ColorChoice::parse(&self.color)?;
        crate::appimage::Policy::parse(&self.package.appimage)?;
        Ok(())
    }
}

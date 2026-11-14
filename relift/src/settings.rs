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
        name = "keys",
        description = "path to the console key file that import reads"
    )]
    pub keys: PathBuf,

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

    #[configulator(name = "toolchain", nested)]
    pub toolchain: Toolchain,
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
}

impl Validate for Settings {
    fn validate(&self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        Ok(())
    }
}

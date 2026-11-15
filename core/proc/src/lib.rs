//! The one place that starts a subprocess.
//!
//! `clippy.toml` disallows `std::process::Command::new` everywhere else, so
//! every compiler, linker and container run goes through [`command`], where
//! logging and a fixed environment for reproducible builds will live.

use std::ffi::OsStr;
use std::process::Command;

/// A [`Command`] that runs `program`.
#[expect(
    clippy::disallowed_methods,
    reason = "the funnel clippy.toml points every other caller at"
)]
pub fn command(program: impl AsRef<OsStr>) -> Command {
    Command::new(program)
}

pub mod workers;

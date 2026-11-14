//! The content-addressed store and its stage keys, and the one place that
//! writes files.
//!
//! `clippy.toml` disallows `std::fs::write`, `File::create`,
//! `OpenOptions::open`, `fs::copy` and `io::copy` everywhere else, so every
//! file relift produces goes through [`write_if_changed`] (or, later, a
//! streaming writer beside it), and an unchanged output keeps its mtime.

use std::io;
use std::path::Path;

/// Write `bytes` to `path` unless the file already holds exactly them.
/// Returns whether the file was written.
///
/// # Errors
///
/// Any error reading the old file (other than it not existing) or writing
/// the new one.
#[expect(
    clippy::disallowed_methods,
    reason = "the funnel clippy.toml points every other writer at"
)]
pub fn write_if_changed(path: &Path, bytes: &[u8]) -> io::Result<bool> {
    match std::fs::read(path) {
        Ok(old) if old == bytes => return Ok(false),
        Ok(_) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => {}
        Err(e) => return Err(e),
    }
    std::fs::write(path, bytes)?;
    Ok(true)
}

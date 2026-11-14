//! Shared plumbing of the single-purpose tools for Game Boy files
//! (`relift-gbrom`): inputs from files or stdin, JSON and the one-line
//! error exit. All format logic lives in `relift-gb-formats`.

use std::io::{Read, Write};
use std::path::Path;
use std::process::ExitCode;

use clap::Parser;

/// The error type tool bodies return.
pub type BoxError = Box<dyn std::error::Error + Send + Sync>;

/// Parse the arguments of tool `name`, run `body`, and turn an error into
/// one line on stderr and exit code 1. A closed stdout is a normal end.
pub fn run<C: Parser>(name: &str, body: impl FnOnce(C) -> Result<(), BoxError>) -> ExitCode {
    let args = C::parse();
    match body(args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) if is_broken_pipe(&*e) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{name}: {e}");
            ExitCode::FAILURE
        }
    }
}

/// Whether `e`, or an error it wraps, is a broken pipe.
pub fn is_broken_pipe(e: &(dyn std::error::Error + 'static)) -> bool {
    let mut at = Some(e);
    while let Some(e) = at {
        if let Some(io) = e.downcast_ref::<std::io::Error>()
            && io.kind() == std::io::ErrorKind::BrokenPipe
        {
            return true;
        }
        if let Some(j) = e.downcast_ref::<serde_json::Error>()
            && j.io_error_kind() == Some(std::io::ErrorKind::BrokenPipe)
        {
            return true;
        }
        at = e.source();
    }
    false
}

/// Read an input: a file, or stdin for `-`.
///
/// # Errors
///
/// The file or stdin cannot be read.
pub fn input(path: &Path) -> Result<Vec<u8>, BoxError> {
    if path.as_os_str() == "-" {
        let mut v = Vec::new();
        std::io::stdin().read_to_end(&mut v)?;
        return Ok(v);
    }
    std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()).into())
}

/// Print a JSON value, pretty, with a trailing newline.
///
/// # Errors
///
/// Writing stdout fails.
pub fn print_json(v: &serde_json::Value) -> Result<(), BoxError> {
    let mut out = std::io::stdout().lock();
    serde_json::to_writer_pretty(&mut out, v)?;
    out.write_all(b"\n")?;
    Ok(())
}

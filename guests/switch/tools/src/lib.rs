//! Shared plumbing of the single-purpose tools for decrypted Switch files
//! (`relift-romfs`, `relift-nso`, `relift-npdm`, `relift-nro`, `relift-nup`):
//! inputs from files or stdin, outputs to files or stdout, JSON and the
//! one-line error exit. All format logic lives in `relift-switch-formats`;
//! every binary here is a thin wrapper.

use std::io::{Read, Write};
use std::path::Path;
use std::process::ExitCode;
use std::sync::Arc;

use clap::Parser;
use relift_switch_formats::storage::{FileStorage, MemStorage, Shared, Storage};

/// The error type tool bodies return.
pub type BoxError = Box<dyn std::error::Error + Send + Sync>;

/// Parse the arguments of tool `name`, run `body`, and turn an error into
/// one line on stderr and exit code 1.
pub fn run<C: Parser>(name: &str, body: impl FnOnce(C) -> Result<(), BoxError>) -> ExitCode {
    let args = C::parse();
    match body(args) {
        Ok(()) => ExitCode::SUCCESS,
        // A reader that closed stdout early (`| head`) wanted no more: done.
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

/// Open an input: a file, or stdin (read whole) for `-`.
///
/// # Errors
///
/// The file cannot be opened or stdin cannot be read.
pub fn input(path: &Path) -> Result<Shared, BoxError> {
    if path.as_os_str() == "-" {
        let mut v = Vec::new();
        std::io::stdin().read_to_end(&mut v)?;
        return Ok(Arc::new(MemStorage(v)));
    }
    Ok(Arc::new(FileStorage::open(path)?))
}

/// Write `bytes` to a file through relift-cache, or to stdout for `-` or
/// no path.
///
/// # Errors
///
/// The write fails.
pub fn output(path: Option<&Path>, bytes: &[u8]) -> Result<(), BoxError> {
    match path {
        Some(p) if p.as_os_str() != "-" => {
            if let Some(dir) = p.parent().filter(|d| !d.as_os_str().is_empty()) {
                std::fs::create_dir_all(dir)?;
            }
            relift_cache::write_if_changed(p, bytes)?;
        }
        _ => {
            let mut out = std::io::stdout().lock();
            out.write_all(bytes)?;
            out.flush()?;
        }
    }
    Ok(())
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

/// Stream `storage` in 1 MiB chunks to a file through relift-cache's
/// write-if-changed stream writer, or to stdout for `-`.
///
/// # Errors
///
/// A read, hash check or write fails.
pub fn write_storage(path: &Path, storage: &dyn Storage) -> Result<(), BoxError> {
    const CHUNK: u64 = 1 << 20;
    let mut buf = vec![0u8; usize::try_from(CHUNK.min(storage.size()))?];
    let mut stdout = std::io::stdout().lock();
    let mut file = None;
    if path.as_os_str() != "-" {
        if let Some(dir) = path.parent().filter(|d| !d.as_os_str().is_empty()) {
            std::fs::create_dir_all(dir)?;
        }
        file = Some(relift_cache::StreamWriter::new(path)?);
    }
    let mut off = 0;
    while off < storage.size() {
        let n = CHUNK.min(storage.size() - off);
        let b = &mut buf[..usize::try_from(n)?];
        storage.read_at(off, b)?;
        match &mut file {
            Some(f) => f.write_all(b)?,
            None => stdout.write_all(b)?,
        }
        off += n;
    }
    match file {
        Some(f) => {
            f.finish()?;
        }
        None => stdout.flush()?,
    }
    Ok(())
}

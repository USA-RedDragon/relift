//! The content-addressed store and its stage keys, and the one place that
//! writes files.
//!
//! `clippy.toml` disallows `std::fs::write`, `File::create`,
//! `OpenOptions::open`, `fs::copy` and `io::copy` everywhere else, so every
//! file relift produces goes through [`write_if_changed`] or the streaming
//! [`StreamWriter`] beside it, and an unchanged output keeps its mtime. Both
//! open files for writing through one private funnel function.

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

/// Open `path` for writing, creating or truncating it. The one function that
/// opts out of the clippy funnel.
#[expect(
    clippy::disallowed_methods,
    reason = "the funnel clippy.toml points every other writer at"
)]
fn create(path: &Path) -> io::Result<File> {
    File::create(path)
}

/// Write `bytes` to `path` unless the file already holds exactly them.
/// Returns whether the file was written.
///
/// # Errors
///
/// Any error reading the old file (other than it not existing) or writing
/// the new one.
pub fn write_if_changed(path: &Path, bytes: &[u8]) -> io::Result<bool> {
    match std::fs::read(path) {
        Ok(old) if old == bytes => return Ok(false),
        Ok(_) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => {}
        Err(e) => return Err(e),
    }
    let mut file = create(path)?;
    file.write_all(bytes)?;
    file.flush()?;
    Ok(true)
}

/// Where a [`StreamWriter`] stands.
enum State {
    /// Every byte so far equals the old file's; nothing is written yet.
    Matching { old: File, matched: u64 },
    /// The output differs (or there was no old file): bytes go to a
    /// temporary file that replaces the target on [`StreamWriter::finish`].
    Writing { tmp: File },
}

/// A write-if-changed writer for outputs too large to hold in memory.
///
/// While the stream equals the existing file it only reads and compares; at
/// the first difference it starts a temporary file beside the target, copies
/// the equal prefix into it and goes on writing. [`StreamWriter::finish`]
/// renames the temporary file over the target, or leaves an unchanged target
/// (and its mtime) alone. Dropping the writer without `finish` leaves the
/// target as it was.
pub struct StreamWriter {
    path: PathBuf,
    tmp_path: PathBuf,
    state: State,
    scratch: Vec<u8>,
}

impl StreamWriter {
    /// Start writing `path`.
    ///
    /// # Errors
    ///
    /// Opening the old file failed for a reason other than it not existing,
    /// or the temporary file could not be created.
    pub fn new(path: &Path) -> io::Result<StreamWriter> {
        let mut name = path
            .file_name()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "path has no file name"))?
            .to_os_string();
        name.push(".relift-tmp");
        let tmp_path = path.with_file_name(name);
        let state = match File::open(path) {
            Ok(old) if old.metadata()?.is_file() => State::Matching { old, matched: 0 },
            Ok(_) => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("{} is not a file", path.display()),
                ));
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => State::Writing {
                tmp: create(&tmp_path)?,
            },
            Err(e) => return Err(e),
        };
        Ok(StreamWriter {
            path: path.to_path_buf(),
            tmp_path,
            state,
            scratch: Vec::new(),
        })
    }

    /// Leave matching mode: copy the matched prefix of the old file into a
    /// new temporary file.
    fn diverge(&mut self) -> io::Result<()> {
        if let State::Matching { old, matched } = &mut self.state {
            let mut tmp = create(&self.tmp_path)?;
            old.seek(SeekFrom::Start(0))?;
            let mut left = *matched;
            let mut buf = vec![0u8; 1 << 20];
            while left > 0 {
                let n = usize::try_from(left.min(buf.len() as u64)).unwrap_or(buf.len());
                old.read_exact(&mut buf[..n])?;
                tmp.write_all(&buf[..n])?;
                left -= n as u64;
            }
            self.state = State::Writing { tmp };
        }
        Ok(())
    }

    /// Finish the stream. Returns whether the target was (re)written.
    ///
    /// # Errors
    ///
    /// Reading the old file, writing or renaming the new one failed.
    pub fn finish(mut self) -> io::Result<bool> {
        if let State::Matching { old, .. } = &mut self.state {
            let mut probe = [0u8; 1];
            if old.read(&mut probe)? == 0 {
                return Ok(false);
            }
            self.diverge()?;
        }
        if let State::Writing { tmp } = &mut self.state {
            tmp.flush()?;
        }
        std::fs::rename(&self.tmp_path, &self.path)?;
        Ok(true)
    }
}

impl Write for StreamWriter {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        if let State::Matching { old, matched } = &mut self.state {
            self.scratch.resize(buf.len(), 0);
            let mut got = 0;
            while got < buf.len() {
                let n = old.read(&mut self.scratch[got..])?;
                if n == 0 {
                    break;
                }
                got += n;
            }
            if got == buf.len() && self.scratch[..] == buf[..] {
                *matched += buf.len() as u64;
                return Ok(buf.len());
            }
            self.diverge()?;
        }
        match &mut self.state {
            State::Writing { tmp } => tmp.write_all(buf).map(|()| buf.len()),
            State::Matching { .. } => Err(io::Error::other("stream writer did not diverge")),
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        match &mut self.state {
            State::Writing { tmp } => tmp.flush(),
            State::Matching { .. } => Ok(()),
        }
    }
}

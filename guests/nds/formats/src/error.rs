//! The one error type of the cartridge reader.

use std::fmt;

use crate::blz;

/// A failure to read a cartridge image.
#[derive(Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    /// The image ends before the 0x200-byte header does; the length is
    /// attached.
    TooShort(usize),
    /// A table or file lies outside the image.
    Range {
        /// What was being read.
        what: &'static str,
        /// Offset of the first byte wanted.
        offset: u64,
        /// Number of bytes wanted.
        len: u64,
        /// Length of the image.
        file_len: usize,
    },
    /// A compressed unit does not decode.
    Blz {
        /// The unit.
        what: &'static str,
        /// Why not.
        source: blz::Error,
    },
    /// A structure is inconsistent.
    Malformed(&'static str),
}

/// The result type of the cartridge reader.
pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::TooShort(n) => write!(f, "malformed: {n} bytes is shorter than the header"),
            Error::Range {
                what,
                offset,
                len,
                file_len,
            } => write!(
                f,
                "malformed: {what} at {offset:#x} for {len:#x} bytes is outside the {file_len}-byte image"
            ),
            Error::Blz { what, source } => write!(f, "malformed: {what}: {source}"),
            Error::Malformed(what) => write!(f, "malformed: {what}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Blz { source, .. } => Some(source),
            _ => None,
        }
    }
}

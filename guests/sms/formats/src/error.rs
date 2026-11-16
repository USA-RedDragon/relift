//! The one error type of the cartridge reader.

use std::fmt;

/// A failure to read a cartridge image.
#[derive(Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    /// The image has no ROM bytes (after a copier header, if any).
    Empty,
    /// The ROM is larger than the 4 MiB an 8-bit bank register reaches; the
    /// size is attached.
    TooLarge(usize),
}

/// The result type of the cartridge reader.
pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Empty => write!(f, "malformed: the image has no ROM bytes"),
            Error::TooLarge(n) => write!(f, "unsupported: {n} bytes is more than 4 MiB"),
        }
    }
}

impl std::error::Error for Error {}

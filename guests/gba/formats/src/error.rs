//! The one error type of the cartridge reader.

use std::fmt;

/// A failure to read a cartridge image.
#[derive(Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    /// The image ends before the header does; the length is attached.
    TooShort(usize),
    /// The image is larger than the 32 MiB cartridge address space; the
    /// length is attached.
    TooLong(usize),
}

/// The result type of the cartridge reader.
pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::TooShort(n) => write!(f, "malformed: {n} bytes is shorter than the header"),
            Error::TooLong(n) => {
                write!(f, "malformed: {n} bytes exceeds the 32 MiB cartridge space")
            }
        }
    }
}

impl std::error::Error for Error {}

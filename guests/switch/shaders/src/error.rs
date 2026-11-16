//! The error type of the discovery library.

use std::fmt;
use std::io;

/// Why a file, container or scan failed.
#[derive(Debug)]
#[non_exhaustive]
pub enum Error {
    /// Reading or writing a file failed.
    Io(io::Error),
    /// A format reader of `relift-switch-formats` failed.
    Formats(relift_switch_formats::Error),
    /// The bytes do not have the expected shape.
    Format(String),
    /// A zstd frame names a dictionary that was not found in the input.
    MissingDictionary(u32),
}

/// The result type of the library.
pub type Result<T> = std::result::Result<T, Error>;

pub(crate) fn malformed<T>(msg: impl Into<String>) -> Result<T> {
    Err(Error::Format(msg.into()))
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "i/o: {e}"),
            Error::Formats(e) => write!(f, "{e}"),
            Error::Format(m) => write!(f, "malformed: {m}"),
            Error::MissingDictionary(id) => write!(f, "missing zstd dictionary {id}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Error {
        Error::Io(e)
    }
}

impl From<relift_switch_formats::Error> for Error {
    fn from(e: relift_switch_formats::Error) -> Error {
        Error::Formats(e)
    }
}

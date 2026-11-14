//! The one error type of the format readers.

use std::fmt;
use std::io;

/// A failure to read or verify a decrypted Switch file or dump.
#[derive(Debug)]
#[non_exhaustive]
pub enum Error {
    /// Reading the underlying file failed.
    Io(io::Error),
    /// The bytes do not have the expected shape.
    Format(String),
    /// A hash in a verified region does not match its data.
    Hash(String),
    /// A valid container uses a feature these readers do not handle.
    Unsupported(String),
    /// The input is an encrypted dump (NSP, NSZ, XCI, XCZ or NCA), which
    /// relift does not read; the message names the input.
    Encrypted(String),
}

/// What an [`Error::Encrypted`] tells the user to do.
pub const ENCRYPTED_HINT: &str = "relift reads decrypted dumps only (a .nup file or an extracted exefs/romfs tree); convert an encrypted dump with a separate tool first";

/// The result type of the format readers.
pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "i/o: {e}"),
            Error::Format(m) => write!(f, "malformed: {m}"),
            Error::Hash(m) => write!(f, "hash mismatch: {m}"),
            Error::Unsupported(m) => write!(f, "unsupported: {m}"),
            Error::Encrypted(m) => write!(f, "{m}: {ENCRYPTED_HINT}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Io(e) => Some(e),
            _ => None,
        }
    }
}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Error {
        Error::Io(e)
    }
}

/// Shorthand for a [`Error::Format`].
pub(crate) fn malformed<T>(msg: impl Into<String>) -> Result<T> {
    Err(Error::Format(msg.into()))
}

//! The one error type of the cartridge reader.

use std::fmt;

/// A failure to read a cartridge image or a compressed stream.
#[derive(Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    /// The input ends before the structure does; the length is attached.
    TooShort(usize),
    /// The length does not fit the word size of the byte order; the length
    /// is attached.
    Misaligned(usize),
    /// The first word names no known byte order; the word is attached.
    UnknownOrder(u32),
    /// A compressed stream ends before its decoded size is reached; the
    /// part that was being read is attached.
    Truncated(&'static str),
    /// A compressed stream is malformed; the reason is attached.
    Invalid(&'static str),
}

/// The result type of the cartridge reader.
pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::TooShort(n) => write!(f, "malformed: {n} bytes is shorter than the structure"),
            Error::Misaligned(n) => {
                write!(f, "malformed: {n} bytes is not a whole number of words")
            }
            Error::UnknownOrder(w) => write!(f, "unknown byte order: first word is {w:08X}"),
            Error::Truncated(what) => write!(f, "truncated stream: {what}"),
            Error::Invalid(why) => write!(f, "invalid stream: {why}"),
        }
    }
}

impl std::error::Error for Error {}

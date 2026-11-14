//! The one error type of the cartridge reader.

use std::fmt;

/// A failure to read a cartridge image.
#[derive(Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    /// The image ends before the header does; the length is attached.
    TooShort(usize),
    /// `$0147` is not a known cartridge type.
    UnknownCartridgeType(u8),
    /// `$0148` is not a known ROM size code.
    UnknownRomSize(u8),
    /// `$0149` is not a known RAM size code.
    UnknownRamSize(u8),
}

/// The result type of the cartridge reader.
pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::TooShort(n) => write!(f, "malformed: {n} bytes is shorter than the header"),
            Error::UnknownCartridgeType(b) => write!(f, "unsupported: cartridge type ${b:02X}"),
            Error::UnknownRomSize(b) => write!(f, "unsupported: ROM size code ${b:02X}"),
            Error::UnknownRamSize(b) => write!(f, "unsupported: RAM size code ${b:02X}"),
        }
    }
}

impl std::error::Error for Error {}

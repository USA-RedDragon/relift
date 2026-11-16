//! An image held as big-endian bytes with its header and variant.

use crate::checksum;
use crate::cic::Cic;
use crate::error::{Error, Result};
use crate::header::{Header, SIZE};
use crate::order::ByteOrder;
use crate::scan::{self, Stream};

/// The outcome of the CRC1/CRC2 check.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CrcCheck {
    /// Both stored values equal the computed ones.
    Good,
    /// At least one differs; the computed pair is attached.
    Bad((u32, u32)),
    /// The checksum cannot be computed, with the reason.
    Unavailable(&'static str),
}

/// An image, normalized to big-endian.
#[derive(Debug, Clone)]
pub struct Rom {
    /// The byte order the input had.
    pub order: ByteOrder,
    /// The big-endian bytes.
    pub bytes: Vec<u8>,
}

impl Rom {
    /// Detect the byte order of `input` and normalize it.
    ///
    /// # Errors
    ///
    /// The input is shorter than the header, its first word names no byte
    /// order or its length is not a whole number of words.
    pub fn new(input: &[u8]) -> Result<Rom> {
        if input.len() < SIZE {
            return Err(Error::TooShort(input.len()));
        }
        let order = ByteOrder::detect(input)?;
        Ok(Rom {
            order,
            bytes: order.normalize(input)?,
        })
    }

    /// The header.
    ///
    /// # Panics
    ///
    /// Never: [`Rom::new`] guarantees the header is there.
    #[must_use]
    pub fn header(&self) -> Header {
        Header::parse(&self.bytes).expect("a Rom holds a whole header")
    }

    /// The boot variant, when the boot code area is present and known.
    #[must_use]
    pub fn cic(&self) -> Option<Cic> {
        Cic::detect(&self.bytes)
    }

    /// The checksum check under the detected variant, or under `cic` when
    /// given.
    #[must_use]
    pub fn check_crc(&self, cic: Option<Cic>) -> CrcCheck {
        let Some(cic) = cic.or_else(|| self.cic()) else {
            return CrcCheck::Unavailable("boot variant unknown");
        };
        if cic.seed().is_none() {
            return CrcCheck::Unavailable("no checksum for this boot variant");
        }
        let Some(computed) = checksum::compute(&self.bytes, cic) else {
            return CrcCheck::Unavailable("image shorter than 0x101000 bytes");
        };
        let h = self.header();
        if computed == (h.crc1, h.crc2) {
            CrcCheck::Good
        } else {
            CrcCheck::Bad(computed)
        }
    }

    /// The compressed streams in the image.
    #[must_use]
    pub fn streams(&self) -> Vec<Stream> {
        scan::scan(&self.bytes)
    }
}

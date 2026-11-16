//! The three byte orders of a Nintendo 64 dump, told apart by the first
//! word, which is the PI bus configuration `0x80371240` in big-endian.

use crate::error::{Error, Result};

/// The byte order of a dump.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ByteOrder {
    /// Big-endian, the console's own order (`.z64`).
    Z64,
    /// Every pair of bytes swapped (`.v64`).
    V64,
    /// Little-endian words (`.n64`).
    N64,
}

impl ByteOrder {
    /// The first word of a dump in this order, read as stored big-endian.
    #[must_use]
    pub const fn magic(self) -> u32 {
        match self {
            ByteOrder::Z64 => 0x8037_1240,
            ByteOrder::V64 => 0x3780_4012,
            ByteOrder::N64 => 0x4012_3780,
        }
    }

    /// The name used in reports.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            ByteOrder::Z64 => "z64",
            ByteOrder::V64 => "v64",
            ByteOrder::N64 => "n64",
        }
    }

    /// The order of a dump, from its first four bytes.
    ///
    /// # Errors
    ///
    /// The input has fewer than four bytes, or its first word is none of the
    /// three known ones.
    pub fn detect(bytes: &[u8]) -> Result<ByteOrder> {
        let Some(&[a, b, c, d]) = bytes.first_chunk::<4>() else {
            return Err(Error::TooShort(bytes.len()));
        };
        let word = u32::from_be_bytes([a, b, c, d]);
        [ByteOrder::Z64, ByteOrder::V64, ByteOrder::N64]
            .into_iter()
            .find(|o| o.magic() == word)
            .ok_or(Error::UnknownOrder(word))
    }

    /// `bytes`, a dump in this order, as big-endian.
    ///
    /// # Errors
    ///
    /// The length is not a multiple of the order's word size.
    pub fn normalize(self, bytes: &[u8]) -> Result<Vec<u8>> {
        self.swapped(bytes)
    }

    /// `bytes`, big-endian, as a dump in this order. The swaps undo
    /// themselves, so this is the same operation as [`normalize`](Self::normalize).
    ///
    /// # Errors
    ///
    /// The length is not a multiple of the order's word size.
    pub fn encode(self, bytes: &[u8]) -> Result<Vec<u8>> {
        self.swapped(bytes)
    }

    fn swapped(self, bytes: &[u8]) -> Result<Vec<u8>> {
        let width = match self {
            ByteOrder::Z64 => return Ok(bytes.to_vec()),
            ByteOrder::V64 => 2,
            ByteOrder::N64 => 4,
        };
        if !bytes.len().is_multiple_of(width) {
            return Err(Error::Misaligned(bytes.len()));
        }
        let mut out = bytes.to_vec();
        for chunk in out.chunks_exact_mut(width) {
            chunk.reverse();
        }
        Ok(out)
    }
}

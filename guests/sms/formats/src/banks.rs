//! Image size validation and the bank layout.

use crate::error::{Error, Result};

/// Size of one ROM bank, the unit every mapper switches.
pub const BANK_SIZE: usize = 0x4000;
/// Size of the header some copiers put before the ROM.
pub const COPIER_HEADER: usize = 512;
/// The largest ROM an 8-bit bank register addresses.
pub const MAX_ROM: usize = 256 * BANK_SIZE;

/// How an image splits into an optional copier header and 16 KiB banks.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Layout {
    /// The length of the whole image.
    pub image_bytes: usize,
    /// Whether the image is 512 bytes longer than a whole number of banks,
    /// the mark of a copier header.
    pub copier_header: bool,
    /// The ROM length without the copier header.
    pub rom_bytes: usize,
    /// The number of banks, counting a partial last bank.
    pub banks: usize,
    /// Whether the ROM does not end on a bank boundary.
    pub partial_bank: bool,
    /// Whether the ROM length is a power of two.
    pub power_of_two: bool,
}

impl Layout {
    /// The layout of an image of `len` bytes.
    ///
    /// # Errors
    ///
    /// [`Error::Empty`] when no ROM bytes remain, [`Error::TooLarge`] when
    /// the ROM is over 4 MiB.
    pub fn of(len: usize) -> Result<Layout> {
        let copier_header = len % BANK_SIZE == COPIER_HEADER;
        let rom_bytes = if copier_header {
            len - COPIER_HEADER
        } else {
            len
        };
        if rom_bytes == 0 {
            return Err(Error::Empty);
        }
        if rom_bytes > MAX_ROM {
            return Err(Error::TooLarge(rom_bytes));
        }
        Ok(Layout {
            image_bytes: len,
            copier_header,
            rom_bytes,
            banks: rom_bytes.div_ceil(BANK_SIZE),
            partial_bank: rom_bytes % BANK_SIZE != 0,
            power_of_two: rom_bytes.is_power_of_two(),
        })
    }

    /// The offset of the ROM in the image.
    #[must_use]
    pub fn rom_start(&self) -> usize {
        if self.copier_header { COPIER_HEADER } else { 0 }
    }
}

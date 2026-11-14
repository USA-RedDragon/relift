//! The two cartridge checksums.

use crate::header::{HEADER_CHECKSUM, HEADER_END, TITLE_START};

/// The header checksum over `$0134-$014C`: start at 0 and subtract each byte
/// and one. The boot ROM refuses a cartridge whose stored byte differs.
/// `rom` must reach `$014D`.
#[must_use]
pub fn header_checksum(rom: &[u8]) -> u8 {
    rom[TITLE_START..HEADER_CHECKSUM]
        .iter()
        .fold(0u8, |x, b| x.wrapping_sub(*b).wrapping_sub(1))
}

/// The global checksum: the 16-bit wrapping sum of every byte of the image
/// except the two checksum bytes at `$014E-$014F`. Hardware does not check
/// it.
#[must_use]
pub fn global_checksum(rom: &[u8]) -> u16 {
    rom.iter()
        .enumerate()
        .filter(|(i, _)| *i != HEADER_END - 2 && *i != HEADER_END - 1)
        .fold(0u16, |s, (_, b)| s.wrapping_add(u16::from(*b)))
}

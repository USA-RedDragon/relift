//! Bank sizes and address arithmetic.

/// The size of a ROM bank, 16 KiB.
pub const ROM_BANK_SIZE: usize = 0x4000;
/// The size of an external RAM bank, 8 KiB.
pub const RAM_BANK_SIZE: usize = 0x2000;

/// ROM and RAM geometry named by the header size codes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Layout {
    /// Number of 16 KiB ROM banks.
    pub rom_banks: usize,
    /// Number of 8 KiB RAM banks (0 for none; 1 for the 2 KiB chip).
    pub ram_banks: usize,
    /// External RAM size in bytes.
    pub ram_bytes: usize,
}

impl Layout {
    /// ROM size in bytes.
    #[must_use]
    pub fn rom_bytes(&self) -> usize {
        self.rom_banks * ROM_BANK_SIZE
    }
}

/// The ROM banks and byte count for a `$0148` code.
#[must_use]
pub fn rom_banks(code: u8) -> Option<usize> {
    match code {
        0x00..=0x08 => Some(2usize << code),
        0x52 => Some(72),
        0x53 => Some(80),
        0x54 => Some(96),
        _ => None,
    }
}

/// The RAM size in bytes and bank count for a `$0149` code. Code 1 is the
/// 2 KiB chip some documents list; it occupies one bank window.
#[must_use]
pub fn ram_size(code: u8) -> Option<(usize, usize)> {
    match code {
        0x00 => Some((0, 0)),
        0x01 => Some((0x800, 1)),
        0x02 => Some((0x2000, 1)),
        0x03 => Some((0x8000, 4)),
        0x04 => Some((0x20000, 16)),
        0x05 => Some((0x10000, 8)),
        _ => None,
    }
}

/// The file offset of CPU address `addr` (`$0000-$7FFF`) while ROM bank
/// `bank` is mapped at `$4000-$7FFF`, or `None` outside ROM.
#[must_use]
pub fn rom_offset(bank: usize, addr: u16) -> Option<usize> {
    match addr {
        0x0000..=0x3FFF => Some(usize::from(addr)),
        0x4000..=0x7FFF => Some(bank * ROM_BANK_SIZE + usize::from(addr - 0x4000)),
        _ => None,
    }
}

/// The bank and the CPU address a file offset is seen at. Bank 0 maps at
/// `$0000`; every other bank at `$4000`.
#[must_use]
pub fn bank_address(offset: usize) -> (usize, u16) {
    let bank = offset / ROM_BANK_SIZE;
    let within = u16::try_from(offset % ROM_BANK_SIZE).unwrap_or(0);
    if bank == 0 {
        (0, within)
    } else {
        (bank, 0x4000 + within)
    }
}

/// The bytes of ROM bank `bank` in `rom`, or `None` when the image is too
/// short to hold it whole.
#[must_use]
pub fn rom_bank(rom: &[u8], bank: usize) -> Option<&[u8]> {
    let start = bank.checked_mul(ROM_BANK_SIZE)?;
    rom.get(start..start.checked_add(ROM_BANK_SIZE)?)
}

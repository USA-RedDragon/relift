//! The image as the three cartridge wait regions see it.

use crate::header::MAX_ROM;

/// The first address of the cartridge space (wait state 0).
pub const ROM_BASE: u32 = 0x0800_0000;
/// Mask of the offset inside one 32 MiB wait region.
pub const OFFSET_MASK: u32 = 0x01FF_FFFF;
/// Size of the part of the 0x0D region EEPROM answers in, on a ROM of more
/// than 16 MiB.
pub const EEPROM_WINDOW_LARGE: u32 = 0x100;

/// A cartridge image mapped at `0x08`, `0x0A` and `0x0C`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Rom<'a> {
    bytes: &'a [u8],
}

impl<'a> Rom<'a> {
    /// Wrap an image.
    ///
    /// # Panics
    ///
    /// The image is larger than 32 MiB.
    #[must_use]
    pub fn new(bytes: &'a [u8]) -> Rom<'a> {
        assert!(bytes.len() <= MAX_ROM, "image exceeds the cartridge space");
        Rom { bytes }
    }

    /// The image bytes.
    #[must_use]
    pub fn bytes(&self) -> &'a [u8] {
        self.bytes
    }

    /// Whether `addr` is in the three ROM wait regions, `0x08` to `0x0D`
    /// (the `0x0D` region is the EEPROM mirror of wait state 2).
    #[must_use]
    pub fn contains(addr: u32) -> bool {
        (0x08..=0x0D).contains(&(addr >> 24))
    }

    /// The open-bus halfword for `addr`: the address lines read back,
    /// `(addr / 2) & 0xFFFF`.
    #[must_use]
    pub fn open_bus(addr: u32) -> u16 {
        u16::try_from((addr >> 1) & 0xFFFF).unwrap_or(0)
    }

    /// Read a byte at bus address `addr`. Every region mirrors the same 32
    /// MiB; past the image the open-bus pattern answers.
    #[must_use]
    pub fn read8(&self, addr: u32) -> u8 {
        let off = (addr & OFFSET_MASK) as usize;
        match self.bytes.get(off) {
            Some(b) => *b,
            None => Rom::open_bus(addr).to_le_bytes()[(addr & 1) as usize],
        }
    }

    /// Read the halfword at `addr` with bit 0 ignored.
    #[must_use]
    pub fn read16(&self, addr: u32) -> u16 {
        let a = addr & !1;
        u16::from_le_bytes([self.read8(a), self.read8(a | 1)])
    }

    /// Read the word at `addr` with bits 0-1 ignored.
    #[must_use]
    pub fn read32(&self, addr: u32) -> u32 {
        let a = addr & !3;
        u32::from(self.read16(a)) | u32::from(self.read16(a | 2)) << 16
    }

    /// Whether EEPROM answers across the whole `0x0D` region: true up to
    /// 16 MiB. Larger images leave only the last [`EEPROM_WINDOW_LARGE`]
    /// bytes of the region to it.
    #[must_use]
    pub fn eeprom_spans_region(&self) -> bool {
        self.bytes.len() <= 16 * 1024 * 1024
    }
}

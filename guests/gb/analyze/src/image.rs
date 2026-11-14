//! A ROM image read through bank-qualified locations.

use relift_gb_formats::banks::{ROM_BANK_SIZE, rom_offset};

use crate::loc::Loc;

/// Read access to the code bytes of an image.
pub struct Image<'a> {
    rom: &'a [u8],
}

impl<'a> Image<'a> {
    /// Wrap `rom`.
    #[must_use]
    pub fn new(rom: &'a [u8]) -> Image<'a> {
        Image { rom }
    }

    /// The bytes from `loc` to the end of the bank window it is in, or
    /// `None` when `loc` is not in ROM or the image is too short.
    #[must_use]
    pub fn fetch(&self, loc: Loc) -> Option<&'a [u8]> {
        let start = rom_offset(usize::from(loc.bank), loc.addr)?;
        let window_end = if loc.addr < 0x4000 { 0x4000 } else { 0x8000 };
        let len = usize::from(window_end - loc.addr).min(ROM_BANK_SIZE);
        let end = start.checked_add(len)?.min(self.rom.len());
        self.rom.get(start..end).filter(|s| !s.is_empty())
    }
}

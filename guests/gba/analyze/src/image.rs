//! The bytes code and constants come from: the ROM image and the proven
//! ROM-to-RAM copies.

use relift_gba_formats::rom::OFFSET_MASK;

use crate::loc::{Loc, Region};
use crate::report::{CopyKind, RamCopy};

#[derive(Clone)]
pub(crate) struct Image<'a> {
    rom: &'a [u8],
    pub copies: Vec<RamCopy>,
}

fn in_rom(rom: &[u8], addr: u32, n: u32) -> Option<usize> {
    if !(0x08..=0x0C).contains(&(addr >> 24)) {
        return None;
    }
    let off = (addr & OFFSET_MASK) as usize;
    (off + n as usize <= rom.len()).then_some(off)
}

fn le(b: &[u8]) -> u32 {
    b.iter()
        .rev()
        .fold(0u32, |acc, x| (acc << 8) | u32::from(*x))
}

impl<'a> Image<'a> {
    pub(crate) fn new(rom: &'a [u8]) -> Image<'a> {
        Image {
            rom,
            copies: Vec::new(),
        }
    }

    /// The ROM bytes at `addr`.
    pub(crate) fn rom_bytes(&self, addr: u32, n: u32) -> Option<&'a [u8]> {
        let off = in_rom(self.rom, addr, n)?;
        Some(&self.rom[off..off + n as usize])
    }

    /// Reads `n` bytes (1, 2 or 4) at `addr` as the code of `copy` sees
    /// them, when they are in the ROM image or in that copy.
    pub(crate) fn read(&self, copy: Option<u16>, addr: u32, n: u32) -> Option<u32> {
        if let Some(i) = copy
            && Region::of(addr).is_ram()
        {
            let c = self.copies.get(usize::from(i))?;
            let off = addr.checked_sub(c.dst)? as usize;
            return c.bytes.get(off..off + n as usize).map(le);
        }
        self.rom_bytes(addr, n).map(le)
    }

    /// The instruction word or halfword at `loc`.
    pub(crate) fn fetch(&self, loc: Loc) -> Option<u32> {
        if !loc.addr.is_multiple_of(loc.size()) {
            return None;
        }
        match (loc.region(), loc.copy) {
            (Region::Rom, None) => self.read(None, loc.addr, loc.size()),
            (Region::Ewram | Region::Iwram, Some(_)) => self.read(loc.copy, loc.addr, loc.size()),
            _ => None,
        }
    }

    /// The copies whose destination holds `addr`, but for rewrites, which
    /// code reaches only from inside them.
    pub(crate) fn copies_at(&self, addr: u32) -> Vec<u16> {
        self.copies
            .iter()
            .enumerate()
            .filter(|(_, c)| {
                addr.wrapping_sub(c.dst) < c.len() && !matches!(c.kind, CopyKind::Rewrite(_))
            })
            .filter_map(|(i, _)| u16::try_from(i).ok())
            .collect()
    }

    /// Whether copy `c` holds `addr`.
    pub(crate) fn holds(&self, c: u16, addr: u32) -> bool {
        self.copies
            .get(usize::from(c))
            .is_some_and(|c| addr.wrapping_sub(c.dst) < c.len())
    }

    /// The copy a rewrite `c` was made from, or `c`.
    pub(crate) fn base(&self, c: u16) -> u16 {
        match self.copies.get(usize::from(c)).map(|c| c.kind) {
            Some(CopyKind::Rewrite(b)) => b,
            _ => c,
        }
    }

    /// The bytes a copy can read at `src`: the ROM image, or the multiboot
    /// image where the BIOS loaded it.
    pub(crate) fn source(&self, src: u32, n: u32) -> Option<&[u8]> {
        if let Some(b) = self.rom_bytes(src, n) {
            return Some(b);
        }
        let c = self.copies.iter().find(|c| c.kind == CopyKind::Load)?;
        let off = src.checked_sub(c.dst)? as usize;
        c.bytes.get(off..off + n as usize)
    }

    /// Whether `addr` is in the ROM image or in a loaded multiboot image.
    pub(crate) fn is_source(&self, addr: u32) -> bool {
        self.source(addr, 1).is_some()
    }

    /// Whether `addr` is a ROM address inside the image.
    pub(crate) fn in_image(&self, addr: u32) -> bool {
        in_rom(self.rom, addr, 1).is_some()
    }
}

//! Code locations: an address, the CPU state the code runs in and, for code
//! in work RAM, the ROM-to-RAM copy that put it there.

use std::fmt;

/// Where an instruction is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Loc {
    /// The address the instruction runs at.
    pub addr: u32,
    /// Whether it runs in Thumb state.
    pub thumb: bool,
    /// For code in work RAM, the index of the copy (in
    /// [`crate::Report::copies`]) whose bytes are there.
    pub copy: Option<u16>,
}

impl Loc {
    /// A location in ROM.
    #[must_use]
    pub const fn rom(addr: u32, thumb: bool) -> Loc {
        Loc {
            addr,
            thumb,
            copy: None,
        }
    }

    /// The instruction size in bytes.
    #[must_use]
    pub const fn size(self) -> u32 {
        if self.thumb { 2 } else { 4 }
    }

    /// The region the address is in.
    #[must_use]
    pub fn region(self) -> Region {
        Region::of(self.addr)
    }
}

impl fmt::Display for Loc {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = if self.thumb { 'T' } else { 'A' };
        write!(f, "{s}:{:08X}", self.addr)?;
        if let Some(c) = self.copy {
            write!(f, "@{c}")?;
        }
        Ok(())
    }
}

/// The memory regions code can run from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Region {
    /// The BIOS, `0x00000000-0x00003FFF`.
    Bios,
    /// External work RAM, `0x02xxxxxx`.
    Ewram,
    /// Internal work RAM, `0x03xxxxxx`.
    Iwram,
    /// The cartridge ROM in its three wait regions, `0x08-0x0D`.
    Rom,
    /// Anything else (I/O, palette, VRAM, OAM, SRAM, unmapped).
    Other,
}

impl Region {
    /// The region of `addr`.
    #[must_use]
    pub fn of(addr: u32) -> Region {
        match addr >> 24 {
            0x00 if addr < 0x4000 => Region::Bios,
            0x02 => Region::Ewram,
            0x03 => Region::Iwram,
            0x08..=0x0D => Region::Rom,
            _ => Region::Other,
        }
    }

    /// Whether the region is work RAM.
    #[must_use]
    pub fn is_ram(self) -> bool {
        matches!(self, Region::Ewram | Region::Iwram)
    }

    /// The lowercase name.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Region::Bios => "bios",
            Region::Ewram => "ewram",
            Region::Iwram => "iwram",
            Region::Rom => "rom",
            Region::Other => "other",
        }
    }
}

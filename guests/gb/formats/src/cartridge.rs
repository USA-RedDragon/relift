//! A parsed cartridge image: header, checks and the mapper in effect.

use crate::banks::{ROM_BANK_SIZE, rom_bank};
use crate::checksum;
use crate::error::Result;
use crate::header::{Header, LOGO, LOGO_START};
use crate::mapper::MapperKind;

/// The outcome of every check on an image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Checks {
    /// The logo bytes match.
    pub logo: bool,
    /// The stored header checksum matches the bytes.
    pub header_checksum: bool,
    /// The stored global checksum matches the bytes.
    pub global_checksum: bool,
    /// The image length equals the size the ROM size code names.
    pub size: bool,
}

impl Checks {
    /// Whether every check passed.
    #[must_use]
    pub fn all(&self) -> bool {
        self.logo && self.header_checksum && self.global_checksum && self.size
    }
}

/// A cartridge image with its decoded header.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Cartridge {
    /// The decoded header.
    pub header: Header,
    /// The global checksum computed from the image.
    pub global_checksum_computed: u16,
    /// Length of the image in bytes.
    pub len: usize,
    multicart: bool,
}

impl Cartridge {
    /// Decode `rom`.
    ///
    /// # Errors
    ///
    /// See [`Header::parse`].
    pub fn parse(rom: &[u8]) -> Result<Cartridge> {
        let header = Header::parse(rom)?;
        let multicart = header.mapper.kind == MapperKind::Mbc1 && is_mbc1_multicart(rom);
        Ok(Cartridge {
            global_checksum_computed: checksum::global_checksum(rom),
            header,
            len: rom.len(),
            multicart,
        })
    }

    /// The controller in effect: the header's, except that an MBC1
    /// cartridge laid out as a multicart reports [`MapperKind::Mbc1M`].
    #[must_use]
    pub fn mapper(&self) -> MapperKind {
        if self.multicart {
            MapperKind::Mbc1M
        } else {
            self.header.mapper.kind
        }
    }

    /// Run every check.
    #[must_use]
    pub fn checks(&self) -> Checks {
        Checks {
            logo: self.header.logo_ok,
            header_checksum: self.header.header_checksum == self.header.header_checksum_computed,
            global_checksum: self.header.global_checksum == self.global_checksum_computed,
            size: self.len == self.header.layout.rom_bytes(),
        }
    }
}

/// Whether a 1 MiB MBC1 image holds several games: a multicart has the
/// logo at the start of at least two of the banks `$10`, `$20` and `$30`
/// (each game's own header), which a single game never does.
#[must_use]
pub fn is_mbc1_multicart(rom: &[u8]) -> bool {
    if rom.len() != 64 * ROM_BANK_SIZE {
        return false;
    }
    let hits = [0x10, 0x20, 0x30]
        .into_iter()
        .filter(|b| rom_bank(rom, *b).is_some_and(|bank| bank[LOGO_START..][..LOGO.len()] == LOGO))
        .count();
    hits >= 2
}

//! The cartridge header at `0x000-0x1FF`.

use crate::bytes::{u16_at, u32_at};
use crate::checksum::{self, crc16};
use crate::error::{Error, Result};

/// Length of the header block the loader reads.
pub const HEADER_LEN: usize = 0x200;
/// Start of the logo.
pub const LOGO_START: usize = 0xC0;
/// Length of the logo.
pub const LOGO_LEN: usize = 156;
/// Offset of the logo CRC.
pub const LOGO_CRC: usize = 0x15C;
/// Offset of the header CRC, which covers `0x000-0x15D`.
pub const HEADER_CRC: usize = 0x15E;
/// Offset of the secure area CRC.
pub const SECURE_CRC: usize = 0x6C;
/// End of the secure area: its CRC covers the ARM9 binary's start up to
/// here.
pub const SECURE_END: usize = 0x8000;

/// Where a processor's binary sits in the image and in memory.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Binary {
    /// Offset of the binary in the image.
    pub rom_offset: u32,
    /// The entry address.
    pub entry: u32,
    /// The address the binary loads to.
    pub ram: u32,
    /// Size of the binary in the image.
    pub size: u32,
}

/// An offset and a size in the image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Table {
    /// Offset in the image.
    pub offset: u32,
    /// Size in bytes.
    pub size: u32,
}

/// The outcome of the checks on a header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Checks {
    /// The logo hash matches.
    pub logo: bool,
    /// The stored logo CRC matches the logo bytes.
    pub logo_crc: bool,
    /// The stored header CRC matches the bytes.
    pub header_crc: bool,
    /// The stored secure area CRC matches, or `None` when the image does
    /// not reach the secure area's end.
    pub secure_crc: Option<bool>,
}

impl Checks {
    /// Whether the logo hash, logo CRC and header CRC all passed. The secure
    /// area CRC is reported apart: dumps that decrypted the area differ.
    #[must_use]
    pub fn all(&self) -> bool {
        self.logo && self.logo_crc && self.header_crc
    }
}

/// A decoded header.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Header {
    /// The title with trailing NULs removed and bytes outside printable
    /// ASCII shown as `?`.
    pub title: String,
    /// The four-character game code.
    pub game_code: String,
    /// The two-character maker code.
    pub maker: String,
    /// The unit code at `0x12`.
    pub unit_code: u8,
    /// The encryption seed select at `0x13`.
    pub seed_select: u8,
    /// The device capacity at `0x14`.
    pub capacity: u8,
    /// The software version at `0x1E`.
    pub version: u8,
    /// The auto-start flags at `0x1F`.
    pub autostart: u8,
    /// The ARM9 binary.
    pub arm9: Binary,
    /// The ARM7 binary.
    pub arm7: Binary,
    /// The file name table.
    pub fnt: Table,
    /// The file allocation table.
    pub fat: Table,
    /// The ARM9 overlay table.
    pub ovt9: Table,
    /// The ARM7 overlay table.
    pub ovt7: Table,
    /// The secure area CRC as stored.
    pub secure_crc: u16,
    /// The secure area CRC computed from the image, when it reaches the
    /// end of the secure area.
    pub secure_crc_computed: Option<u16>,
    /// The total used ROM size at `0x80`.
    pub total_size: u32,
    /// The header size at `0x84`.
    pub header_size: u32,
    /// The SHA-256 of the logo field.
    pub logo_sha256: [u8; 32],
    /// The logo CRC as stored.
    pub logo_crc: u16,
    /// The logo CRC computed from the bytes.
    pub logo_crc_computed: u16,
    /// The header CRC as stored.
    pub header_crc: u16,
    /// The header CRC computed from the bytes.
    pub header_crc_computed: u16,
    /// Length of the image in bytes.
    pub len: usize,
}

fn text(bytes: &[u8]) -> String {
    let used = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
    bytes[..used]
        .iter()
        .map(|b| {
            if (0x20..0x7F).contains(b) {
                char::from(*b)
            } else {
                '?'
            }
        })
        .collect::<String>()
        .trim_end()
        .to_owned()
}

fn word(rom: &[u8], at: usize) -> u32 {
    u32_at(rom, at).unwrap_or(0)
}

fn binary(rom: &[u8], at: usize) -> Binary {
    Binary {
        rom_offset: word(rom, at),
        entry: word(rom, at + 4),
        ram: word(rom, at + 8),
        size: word(rom, at + 12),
    }
}

fn table(rom: &[u8], at: usize) -> Table {
    Table {
        offset: word(rom, at),
        size: word(rom, at + 4),
    }
}

impl Header {
    /// Decode the header of `rom`.
    ///
    /// # Errors
    ///
    /// The image ends before `0x200`. Failing checks are not errors; see
    /// [`Header::checks`].
    pub fn parse(rom: &[u8]) -> Result<Header> {
        if rom.len() < HEADER_LEN {
            return Err(Error::TooShort(rom.len()));
        }
        let arm9 = binary(rom, 0x20);
        let secure_crc_computed = Some(arm9.rom_offset as usize)
            .filter(|start| *start < SECURE_END)
            .and_then(|start| rom.get(start..SECURE_END))
            .map(crc16);
        Ok(Header {
            title: text(&rom[0..12]),
            game_code: text(&rom[0x0C..0x10]),
            maker: text(&rom[0x10..0x12]),
            unit_code: rom[0x12],
            seed_select: rom[0x13],
            capacity: rom[0x14],
            version: rom[0x1E],
            autostart: rom[0x1F],
            arm9,
            arm7: binary(rom, 0x30),
            fnt: table(rom, 0x40),
            fat: table(rom, 0x48),
            ovt9: table(rom, 0x50),
            ovt7: table(rom, 0x58),
            secure_crc: u16_at(rom, SECURE_CRC).unwrap_or(0),
            secure_crc_computed,
            total_size: word(rom, 0x80),
            header_size: word(rom, 0x84),
            logo_sha256: checksum::sha256(&rom[LOGO_START..LOGO_START + LOGO_LEN]),
            logo_crc: u16_at(rom, LOGO_CRC).unwrap_or(0),
            logo_crc_computed: crc16(&rom[LOGO_START..LOGO_START + LOGO_LEN]),
            header_crc: u16_at(rom, HEADER_CRC).unwrap_or(0),
            header_crc_computed: crc16(&rom[..HEADER_CRC]),
            len: rom.len(),
        })
    }

    /// Run every check against the expected logo hash.
    #[must_use]
    pub fn checks_against(&self, logo: &[u8; 32]) -> Checks {
        Checks {
            logo: self.logo_sha256 == *logo,
            logo_crc: self.logo_crc == self.logo_crc_computed,
            header_crc: self.header_crc == self.header_crc_computed,
            secure_crc: self.secure_crc_computed.map(|c| c == self.secure_crc),
        }
    }

    /// Run every check against the hash of the real logo.
    #[must_use]
    pub fn checks(&self) -> Checks {
        self.checks_against(&checksum::LOGO_SHA256)
    }
}

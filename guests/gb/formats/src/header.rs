//! The cartridge header at `$0100-$014F`.

use crate::banks::{self, Layout};
use crate::checksum;
use crate::error::{Error, Result};
use crate::mapper::Mapper;

/// Start of the entry point.
pub const ENTRY_START: usize = 0x100;
/// Start of the Nintendo logo.
pub const LOGO_START: usize = 0x104;
/// Length of the logo.
pub const LOGO_LEN: usize = 48;
/// Start of the title.
pub const TITLE_START: usize = 0x134;
/// Offset of the CGB flag.
pub const CGB_FLAG: usize = 0x143;
/// Offset of the SGB flag.
pub const SGB_FLAG: usize = 0x146;
/// Offset of the cartridge type.
pub const CART_TYPE: usize = 0x147;
/// Offset of the header checksum.
pub const HEADER_CHECKSUM: usize = 0x14D;
/// One past the last header byte.
pub const HEADER_END: usize = 0x150;

/// The logo bitmap every cartridge carries at `$0104`.
pub const LOGO: [u8; LOGO_LEN] = [
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
];

/// What `$0143` says about Game Boy Color support.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CgbFlag {
    /// Neither `$80` nor `$C0`: a DMG cartridge.
    DmgOnly,
    /// `$80`: runs on both.
    Compatible,
    /// `$C0`: runs on a Game Boy Color only.
    Only,
}

impl CgbFlag {
    /// A short lowercase name, stable for tool output.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            CgbFlag::DmgOnly => "dmg-only",
            CgbFlag::Compatible => "cgb-compatible",
            CgbFlag::Only => "cgb-only",
        }
    }
}

/// A decoded header.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Header {
    /// The four bytes at `$0100`, run after the boot ROM.
    pub entry: [u8; 4],
    /// Whether the 48 logo bytes match the Nintendo logo.
    pub logo_ok: bool,
    /// The title with trailing NULs removed and bytes outside printable ASCII
    /// shown as `?`.
    pub title: String,
    /// The four-character manufacturer code of late cartridges, when the
    /// bytes at `$013F-$0142` are uppercase letters or digits and the CGB
    /// flag is set.
    pub manufacturer: Option<String>,
    /// The CGB flag as stored.
    pub cgb_byte: u8,
    /// The decoded CGB flag.
    pub cgb: CgbFlag,
    /// The two ASCII bytes at `$0144-$0145`; meaningful when
    /// [`Header::old_licensee`] is `$33`.
    pub new_licensee: [u8; 2],
    /// Whether `$0146` is `$03`, the Super Game Boy function flag.
    pub sgb: bool,
    /// The decoded cartridge type.
    pub mapper: Mapper,
    /// The ROM size code at `$0148`.
    pub rom_size_code: u8,
    /// The RAM size code at `$0149`.
    pub ram_size_code: u8,
    /// ROM and RAM geometry the codes name.
    pub layout: Layout,
    /// `$014A`: 0 for Japan, anything else overseas.
    pub destination: u8,
    /// `$014B`; `$33` defers to the new licensee code.
    pub old_licensee: u8,
    /// `$014C`.
    pub version: u8,
    /// The header checksum as stored at `$014D`.
    pub header_checksum: u8,
    /// The header checksum computed from the bytes.
    pub header_checksum_computed: u8,
    /// The global checksum as stored, big-endian, at `$014E`.
    pub global_checksum: u16,
}

impl Header {
    /// Decode the header of `rom`.
    ///
    /// # Errors
    ///
    /// The image ends before `$0150`, or the cartridge type, ROM size or RAM
    /// size code is not assigned. Failing checksums or logo are not errors;
    /// see [`crate::Cartridge::checks`].
    pub fn parse(rom: &[u8]) -> Result<Header> {
        if rom.len() < HEADER_END {
            return Err(Error::TooShort(rom.len()));
        }
        let code = rom[CART_TYPE];
        let mapper = Mapper::from_code(code).ok_or(Error::UnknownCartridgeType(code))?;
        let rom_size_code = rom[0x148];
        let ram_size_code = rom[0x149];
        let rom_banks =
            banks::rom_banks(rom_size_code).ok_or(Error::UnknownRomSize(rom_size_code))?;
        let (ram_bytes, ram_banks) =
            banks::ram_size(ram_size_code).ok_or(Error::UnknownRamSize(ram_size_code))?;
        let cgb_byte = rom[CGB_FLAG];
        let cgb = match cgb_byte {
            0x80 => CgbFlag::Compatible,
            0xC0 => CgbFlag::Only,
            _ => CgbFlag::DmgOnly,
        };
        let title_end = if cgb == CgbFlag::DmgOnly {
            CGB_FLAG + 1
        } else {
            CGB_FLAG
        };
        let title_bytes = &rom[TITLE_START..title_end];
        let used = title_bytes
            .iter()
            .position(|b| *b == 0)
            .unwrap_or(title_bytes.len());
        let title = title_bytes[..used]
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
            .to_owned();
        let manufacturer = rom[0x13F..0x143]
            .iter()
            .all(|b| b.is_ascii_uppercase() || b.is_ascii_digit())
            .then(|| rom[0x13F..0x143].iter().map(|b| char::from(*b)).collect())
            .filter(|_| cgb != CgbFlag::DmgOnly);
        Ok(Header {
            entry: [rom[0x100], rom[0x101], rom[0x102], rom[0x103]],
            logo_ok: rom[LOGO_START..LOGO_START + LOGO_LEN] == LOGO,
            title,
            manufacturer,
            cgb_byte,
            cgb,
            new_licensee: [rom[0x144], rom[0x145]],
            sgb: rom[SGB_FLAG] == 0x03,
            mapper,
            rom_size_code,
            ram_size_code,
            layout: Layout {
                rom_banks,
                ram_banks,
                ram_bytes,
            },
            destination: rom[0x14A],
            old_licensee: rom[0x14B],
            version: rom[0x14C],
            header_checksum: rom[HEADER_CHECKSUM],
            header_checksum_computed: checksum::header_checksum(rom),
            global_checksum: u16::from_be_bytes([rom[0x14E], rom[0x14F]]),
        })
    }

    /// The target of the usual `nop; jp a16` entry stub, or `None` when the
    /// entry bytes are anything else.
    #[must_use]
    pub fn entry_jump(&self) -> Option<u16> {
        (self.entry[0] == 0x00 && self.entry[1] == 0xC3)
            .then(|| u16::from_le_bytes([self.entry[2], self.entry[3]]))
    }
}

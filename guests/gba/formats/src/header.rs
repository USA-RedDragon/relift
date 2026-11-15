//! The cartridge header at `0x00-0xBF`.

use crate::checksum;
use crate::error::{Error, Result};

/// Start of the entry branch.
pub const ENTRY_START: usize = 0x00;
/// Start of the logo.
pub const LOGO_START: usize = 0x04;
/// Length of the logo.
pub const LOGO_LEN: usize = 156;
/// Start of the title.
pub const TITLE_START: usize = 0xA0;
/// Offset of the fixed byte.
pub const FIXED: usize = 0xB2;
/// Offset of the complement check.
pub const COMPLEMENT: usize = 0xBD;
/// Offset of the multiboot entry word.
pub const MULTIBOOT_ENTRY: usize = 0xC0;
/// Where the BIOS loads a multiboot image: the start of external work RAM.
pub const MULTIBOOT_BASE: u32 = 0x0200_0000;
/// One past the last byte the BIOS reads from the header.
pub const HEADER_END: usize = 0xC0;
/// The value of the fixed byte.
pub const FIXED_VALUE: u8 = 0x96;
/// The cartridge address space, and so the largest image.
pub const MAX_ROM: usize = 32 * 1024 * 1024;
/// The largest multiboot program: work RAM.
pub const MULTIBOOT_MAX: usize = 256 * 1024;

/// The outcome of every check on an image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Checks {
    /// The logo hash matches.
    pub logo: bool,
    /// The fixed byte at `0xB2` is `0x96`.
    pub fixed: bool,
    /// The stored complement matches the bytes.
    pub complement: bool,
    /// The image is at most 32 MiB and a whole number of words.
    pub size: bool,
}

impl Checks {
    /// Whether every check passed.
    #[must_use]
    pub fn all(&self) -> bool {
        self.logo && self.fixed && self.complement && self.size
    }
}

/// A decoded header and the facts about the image it came from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Header {
    /// The word at `0x00`, run first.
    pub entry: u32,
    /// The SHA-256 of the logo field.
    pub logo_sha256: [u8; 32],
    /// The title with trailing NULs removed and bytes outside printable
    /// ASCII shown as `?`.
    pub title: String,
    /// The four-character game code.
    pub game_code: String,
    /// The two-character maker code.
    pub maker: String,
    /// The byte at `0xB2`.
    pub fixed: u8,
    /// The main unit code at `0xB3`.
    pub unit_code: u8,
    /// The device type at `0xB4`.
    pub device_type: u8,
    /// The software version at `0xBC`.
    pub version: u8,
    /// The complement as stored at `0xBD`.
    pub complement: u8,
    /// The complement computed from the bytes.
    pub complement_computed: u8,
    /// Length of the image in bytes.
    pub len: usize,
    /// The word at `0xC0`, when the image is a [`is_multiboot`] program:
    /// the branch the BIOS runs after loading it to [`MULTIBOOT_BASE`].
    pub multiboot_entry: Option<u32>,
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
    u32::from_le_bytes([rom[at], rom[at + 1], rom[at + 2], rom[at + 3]])
}

/// Whether `w` is an ARM `B` with the always condition.
#[must_use]
pub fn is_branch(w: u32) -> bool {
    w >> 24 == 0xEA
}

/// Whether `rom` can be a multiboot program: it fits work RAM and carries
/// a branch in the multiboot entry word at `0xC0`. The same file can run
/// as a cartridge too (the toolchains put that branch in every image), so
/// this says how it may be loaded, not how it is.
#[must_use]
pub fn is_multiboot(rom: &[u8]) -> bool {
    rom.len() >= MULTIBOOT_ENTRY + 4
        && rom.len() <= MULTIBOOT_MAX
        && is_branch(word(rom, MULTIBOOT_ENTRY))
}

/// Where the ARM `B` `w` at `at` jumps: `at` plus 8 plus the offset.
fn branch_target(at: u32, w: u32) -> u32 {
    let imm = (w & 0x00FF_FFFF) << 8;
    let off = (imm.cast_signed() >> 6).cast_unsigned();
    at.wrapping_add(8).wrapping_add(off)
}

impl Header {
    /// Decode the header of `rom`.
    ///
    /// # Errors
    ///
    /// The image ends before `0xC0` or is larger than 32 MiB. Failing
    /// checks are not errors; see [`Header::checks`].
    pub fn parse(rom: &[u8]) -> Result<Header> {
        if rom.len() < HEADER_END {
            return Err(Error::TooShort(rom.len()));
        }
        if rom.len() > MAX_ROM {
            return Err(Error::TooLong(rom.len()));
        }
        Ok(Header {
            entry: word(rom, ENTRY_START),
            logo_sha256: checksum::logo_sha256(rom),
            title: text(&rom[0xA0..0xAC]),
            game_code: text(&rom[0xAC..0xB0]),
            maker: text(&rom[0xB0..0xB2]),
            fixed: rom[FIXED],
            unit_code: rom[0xB3],
            device_type: rom[0xB4],
            version: rom[0xBC],
            complement: rom[COMPLEMENT],
            complement_computed: checksum::complement(rom),
            len: rom.len(),
            multiboot_entry: is_multiboot(rom).then(|| word(rom, MULTIBOOT_ENTRY)),
        })
    }

    /// The address the entry branch jumps to, when the entry word is an
    /// always-condition `B`: the branch address plus 8 plus the offset.
    #[must_use]
    pub fn entry_target(&self) -> Option<u32> {
        is_branch(self.entry).then(|| branch_target(0x0800_0000, self.entry))
    }

    /// The address the multiboot entry branch jumps to once the image is
    /// at [`MULTIBOOT_BASE`].
    #[must_use]
    pub fn multiboot_target(&self) -> Option<u32> {
        let at = MULTIBOOT_BASE + MULTIBOOT_ENTRY as u32;
        self.multiboot_entry.map(|w| branch_target(at, w))
    }

    /// Run every check against the expected logo hash.
    #[must_use]
    pub fn checks_against(&self, logo: &[u8; 32]) -> Checks {
        Checks {
            logo: self.logo_sha256 == *logo,
            fixed: self.fixed == FIXED_VALUE,
            complement: self.complement == self.complement_computed,
            size: self.len <= MAX_ROM && self.len.is_multiple_of(4),
        }
    }

    /// Run every check against the hash of the real logo.
    #[must_use]
    pub fn checks(&self) -> Checks {
        self.checks_against(&checksum::LOGO_SHA256)
    }
}

//! The header complement check and the logo hash.

use sha2::{Digest, Sha256};

use crate::header::{COMPLEMENT, LOGO_LEN, LOGO_START, TITLE_START};

/// The SHA-256 of the 156 logo bytes every licensed cartridge carries at
/// `0x04`. The bytes themselves are not kept in this repository.
pub const LOGO_SHA256: [u8; 32] = [
    0x08, 0xA0, 0x15, 0x3C, 0xFD, 0x6B, 0x0E, 0xA5, 0x4B, 0x93, 0x8F, 0x7D, 0x20, 0x99, 0x33, 0xFA,
    0x84, 0x9D, 0xA0, 0xD5, 0x6F, 0x5A, 0x34, 0xC4, 0x81, 0x06, 0x0C, 0x9F, 0xF2, 0xFA, 0xD8, 0x18,
];

/// The complement check over `0xA0-0xBC`: start at 0, subtract each byte,
/// then subtract `0x19`. The BIOS refuses a cartridge whose stored byte at
/// `0xBD` differs. `rom` must reach `0xBD`.
#[must_use]
pub fn complement(rom: &[u8]) -> u8 {
    rom[TITLE_START..COMPLEMENT]
        .iter()
        .fold(0u8, |x, b| x.wrapping_sub(*b))
        .wrapping_sub(0x19)
}

/// The SHA-256 of the logo field of `rom`, which must reach `0xA0`.
#[must_use]
pub fn logo_sha256(rom: &[u8]) -> [u8; 32] {
    Sha256::digest(&rom[LOGO_START..LOGO_START + LOGO_LEN]).into()
}

/// Lowercase hex of `bytes`.
#[must_use]
pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// Parse 64 hex digits into a SHA-256 value.
#[must_use]
pub fn parse_sha256(s: &str) -> Option<[u8; 32]> {
    if s.len() != 64 || !s.is_ascii() {
        return None;
    }
    let mut out = [0u8; 32];
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok()?;
    }
    Some(out)
}

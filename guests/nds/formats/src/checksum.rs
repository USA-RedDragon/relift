//! The CRC16 of the header and the logo hash.

use sha2::{Digest, Sha256};

/// The SHA-256 of the 156 logo bytes every licensed cartridge carries at
/// `0xC0`. The bytes themselves are not kept in this repository.
pub const LOGO_SHA256: [u8; 32] = [
    0x08, 0xA0, 0x15, 0x3C, 0xFD, 0x6B, 0x0E, 0xA5, 0x4B, 0x93, 0x8F, 0x7D, 0x20, 0x99, 0x33, 0xFA,
    0x84, 0x9D, 0xA0, 0xD5, 0x6F, 0x5A, 0x34, 0xC4, 0x81, 0x06, 0x0C, 0x9F, 0xF2, 0xFA, 0xD8, 0x18,
];

/// The CRC16 the BIOS and the header use: reflected polynomial `0xA001`,
/// initial value `0xFFFF`.
#[must_use]
pub fn crc16(bytes: &[u8]) -> u16 {
    let mut crc = 0xFFFFu16;
    for b in bytes {
        crc ^= u16::from(*b);
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xA001
            } else {
                crc >> 1
            };
        }
    }
    crc
}

/// The SHA-256 of `bytes`.
#[must_use]
pub fn sha256(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
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

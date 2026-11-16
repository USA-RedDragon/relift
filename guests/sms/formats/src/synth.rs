//! Synthetic cartridge images for tests and tools; never game data.

use crate::banks::{BANK_SIZE, COPIER_HEADER};
use crate::codemasters;
use crate::header::{self, SIGNATURE};

/// What a synthetic image looks like.
#[derive(Clone, Debug)]
pub struct Spec {
    /// The ROM length.
    pub len: usize,
    /// Where to write the `TMR SEGA` header, if at all.
    pub header_at: Option<usize>,
    /// The region code (high nibble of the last header byte).
    pub region: u8,
    /// The size code (low nibble of the last header byte).
    pub size_code: u8,
    /// The version nibble.
    pub version: u8,
    /// The product code bytes at `+12..+15` (the third's high nibble is the
    /// product code's high digit, its low nibble the version).
    pub product: [u8; 3],
    /// Write the correct BIOS checksum.
    pub bios_checksum: bool,
    /// Write a Codemasters header with a correct checksum.
    pub codemasters: bool,
    /// Prepend a 512-byte copier header.
    pub copier_header: bool,
    /// `ld (nn),a` stores to write at offsets from `$0100`, one per address.
    pub stores: Vec<u16>,
}

impl Default for Spec {
    fn default() -> Spec {
        Spec {
            len: 2 * BANK_SIZE,
            header_at: Some(header::OFFSETS[0]),
            region: 4,
            size_code: 0xC,
            version: 0,
            product: [0x26, 0x70, 0x00],
            bios_checksum: true,
            codemasters: false,
            copier_header: false,
            stores: Vec::new(),
        }
    }
}

/// Build an image from `spec`. Filler bytes are odd, so they never form the
/// even store opcode `$32`.
#[must_use]
pub fn rom(spec: &Spec) -> Vec<u8> {
    let mut r: Vec<u8> = (0..spec.len).map(|i| (i % 251) as u8 | 1).collect();
    for (n, addr) in spec.stores.iter().enumerate() {
        let at = 0x100 + n * 4;
        r[at] = 0x32;
        r[at + 1..at + 3].copy_from_slice(&addr.to_le_bytes());
        r[at + 3] = 0;
    }
    if spec.codemasters && spec.len >= codemasters::OFFSET + codemasters::LEN {
        let o = codemasters::OFFSET;
        let banks = (spec.len / BANK_SIZE).min(255) as u8;
        r[o] = banks;
        r[o + 1..o + 6].copy_from_slice(&[0x31, 0x08, 0x93, 0x10, 0x59]);
        r[o + 6..o + 10].fill(0);
        r[o + 10..o + 16].fill(0);
        let sum = codemasters::checksum_over(&r, usize::from(banks));
        r[o + 6..o + 8].copy_from_slice(&sum.to_le_bytes());
        r[o + 8..o + 10].copy_from_slice(&0u16.wrapping_sub(sum).to_le_bytes());
    }
    if let Some(o) = spec.header_at
        && r.len() >= o + header::LEN
    {
        r[o..o + 8].copy_from_slice(&SIGNATURE);
        r[o + 8..o + 10].fill(0);
        r[o + 10..o + 12].fill(0);
        r[o + 12] = spec.product[0];
        r[o + 13] = spec.product[1];
        r[o + 14] = (spec.product[2] & 0xF0) | (spec.version & 15);
        r[o + 15] = (spec.region << 4) | (spec.size_code & 15);
        if spec.bios_checksum
            && let Some(n) = header::size_from_code(spec.size_code)
            && let Some(sum) = header::bios_checksum(&r, o, n)
        {
            r[o + 10..o + 12].copy_from_slice(&sum.to_le_bytes());
        }
    }
    if spec.copier_header {
        let mut v = vec![0u8; COPIER_HEADER];
        v.extend_from_slice(&r);
        return v;
    }
    r
}

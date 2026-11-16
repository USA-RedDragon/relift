//! The Codemasters header at `$7FE0` and its checksum.

use crate::header;

/// Where the header sits.
pub const OFFSET: usize = 0x7FE0;
/// Length of the header.
pub const LEN: usize = 16;

/// A decoded Codemasters header.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CodemastersHeader {
    /// The number of 16 KiB banks the checksum covers, as stored.
    pub banks: u8,
    /// Day, month, year, hour and minute, each a BCD byte as stored.
    pub date: [u8; 5],
    /// The stored checksum word at `$7FE6`.
    pub checksum: u16,
    /// The stored word at `$7FE8`, which is `$10000` minus the checksum.
    pub checksum_negated: u16,
    /// Whether the two words sum to zero and the checksum is not zero.
    pub pair_ok: bool,
    /// The checksum computed over the covered banks.
    pub computed: u16,
    /// Whether the pair is consistent and the computed sum equals it.
    pub checksum_ok: bool,
}

impl CodemastersHeader {
    /// Decode the header of `rom`; `None` when the ROM ends before `$7FF0`.
    #[must_use]
    pub fn read(rom: &[u8]) -> Option<CodemastersHeader> {
        let h = rom.get(OFFSET..OFFSET + LEN)?;
        let checksum = u16::from_le_bytes([h[6], h[7]]);
        let checksum_negated = u16::from_le_bytes([h[8], h[9]]);
        let pair_ok = checksum != 0 && checksum.wrapping_add(checksum_negated) == 0;
        let computed = checksum_over(rom, usize::from(h[0]));
        Some(CodemastersHeader {
            banks: h[0],
            date: [h[1], h[2], h[3], h[4], h[5]],
            checksum,
            checksum_negated,
            pair_ok,
            computed,
            checksum_ok: pair_ok && computed == checksum,
        })
    }
}

/// The Codemasters checksum: the 16-bit wrapping sum of the little-endian
/// words of the first `banks` banks (the whole ROM when `banks` is zero or
/// reaches past its end), leaving out the `TMR SEGA` header at `$7FF0`.
#[must_use]
pub fn checksum_over(rom: &[u8], banks: usize) -> u16 {
    let want = banks * crate::banks::BANK_SIZE;
    let end = if want == 0 || want > rom.len() {
        rom.len()
    } else {
        want
    };
    let skip = header::OFFSETS[0]..header::OFFSETS[0] + header::LEN;
    rom[..end]
        .as_chunks::<2>()
        .0
        .iter()
        .enumerate()
        .filter(|(i, _)| !skip.contains(&(i * 2)))
        .fold(0u16, |s, (_, w)| s.wrapping_add(u16::from_le_bytes(*w)))
}

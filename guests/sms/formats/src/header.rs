//! The `TMR SEGA` header at `$7FF0`, `$3FF0` or `$1FF0`.

/// The header locations, in the order they are searched.
pub const OFFSETS: [usize; 3] = [0x7FF0, 0x3FF0, 0x1FF0];
/// Length of the header.
pub const LEN: usize = 16;
/// The signature at the start of the header.
pub const SIGNATURE: [u8; 8] = *b"TMR SEGA";

/// The system and region a header region code names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Region {
    /// `$3`.
    SmsJapan,
    /// `$4`.
    SmsExport,
    /// `$5`.
    GgJapan,
    /// `$6`.
    GgExport,
    /// `$7`.
    GgInternational,
}

impl Region {
    /// The region of a code, or `None` outside `3..=7`.
    #[must_use]
    pub fn from_code(code: u8) -> Option<Region> {
        match code {
            3 => Some(Region::SmsJapan),
            4 => Some(Region::SmsExport),
            5 => Some(Region::GgJapan),
            6 => Some(Region::GgExport),
            7 => Some(Region::GgInternational),
            _ => None,
        }
    }

    /// The name SMS Power uses.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Region::SmsJapan => "SMS Japan",
            Region::SmsExport => "SMS Export",
            Region::GgJapan => "GG Japan",
            Region::GgExport => "GG Export",
            Region::GgInternational => "GG International",
        }
    }
}

/// The ROM size a size code names, in bytes, or `None` for an unused code.
#[must_use]
pub fn size_from_code(code: u8) -> Option<usize> {
    const K: usize = 1024;
    match code {
        0xA => Some(8 * K),
        0xB => Some(16 * K),
        0xC => Some(32 * K),
        0xD => Some(48 * K),
        0xE => Some(64 * K),
        0xF => Some(128 * K),
        0x0 => Some(256 * K),
        0x1 => Some(512 * K),
        0x2 => Some(1024 * K),
        _ => None,
    }
}

/// A decoded header.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Header {
    /// Where the header sits in the ROM.
    pub offset: usize,
    /// The two reserved bytes at `+8`.
    pub reserved: [u8; 2],
    /// The stored checksum word at `+10`.
    pub checksum: u16,
    /// The product code: the BCD digits of `+12..+14`, preceded by the high
    /// nibble of `+14` when it is not zero.
    pub product_code: String,
    /// The high nibble of byte `+14`.
    pub product_high: u8,
    /// The low nibble of byte `+14`.
    pub version: u8,
    /// The high nibble of byte `+15`.
    pub region_code: u8,
    /// The region, when the code is `3..=7`.
    pub region: Option<Region>,
    /// The low nibble of byte `+15`.
    pub size_code: u8,
    /// The size the code names, when it is a used code.
    pub size_bytes: Option<usize>,
    /// The export BIOS checksum over the range the size code names; `None`
    /// when the code is unused or the ROM is shorter than the range.
    pub computed_checksum: Option<u16>,
    /// Whether the stored and computed checksums are equal.
    pub checksum_ok: bool,
}

impl Header {
    /// Find and decode the header of `rom`, trying `$7FF0`, `$3FF0` and
    /// `$1FF0` in turn.
    #[must_use]
    pub fn find(rom: &[u8]) -> Option<Header> {
        OFFSETS
            .iter()
            .copied()
            .find(|&o| rom.len() >= o + LEN && rom[o..o + 8] == SIGNATURE)
            .map(|o| Header::decode(rom, o))
    }

    fn decode(rom: &[u8], offset: usize) -> Header {
        let h = &rom[offset..offset + LEN];
        let product_high = h[14] >> 4;
        let region_code = h[15] >> 4;
        let size_code = h[15] & 15;
        let size_bytes = size_from_code(size_code);
        let computed_checksum = size_bytes.and_then(|n| bios_checksum(rom, offset, n));
        let checksum = u16::from_le_bytes([h[10], h[11]]);
        let product_code = if product_high == 0 {
            format!("{:02X}{:02X}", h[13], h[12])
        } else {
            format!("{product_high:X}{:02X}{:02X}", h[13], h[12])
        };
        Header {
            offset,
            reserved: [h[8], h[9]],
            checksum,
            product_code,
            product_high,
            version: h[14] & 15,
            region_code,
            region: Region::from_code(region_code),
            size_code,
            size_bytes,
            computed_checksum,
            checksum_ok: computed_checksum == Some(checksum),
        }
    }
}

/// The export BIOS checksum: the 16-bit wrapping sum of the bytes of the
/// first `size` bytes of `rom`, leaving out the 16 header bytes at
/// `header_offset`. `None` when `rom` is shorter than `size`.
#[must_use]
pub fn bios_checksum(rom: &[u8], header_offset: usize, size: usize) -> Option<u16> {
    let range = rom.get(..size)?;
    let sum = range
        .iter()
        .enumerate()
        .filter(|(i, _)| !(header_offset..header_offset + LEN).contains(i))
        .fold(0u16, |s, (_, b)| s.wrapping_add(u16::from(*b)));
    Some(sum)
}

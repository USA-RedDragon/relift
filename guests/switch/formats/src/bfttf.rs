//! The console's shared system fonts as stored in its font system data
//! titles (0100000000000810 to 0100000000000814): `.bfttf` files, a TrueType
//! font obfuscated with a 32-bit key.
//!
//! The layout follows switchbrew.org (Shared Database services) and the
//! public decoders built on it: the first word is a magic XORed with the key,
//! the second the font's size, big-endian, XORed with the key, and the font follows with
//! every 32-bit little-endian word XORed with the key.

use crate::error::{Error, Result};

/// The magic every `.bfttf` file's first word decodes to.
pub const MAGIC: u32 = 0x1802_9a7f;

/// A shared font type, numbered as `nn::pl::SharedFontType` is (switchbrew).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum FontType {
    /// Japan, US and Europe.
    Standard,
    /// Chinese Simplified.
    ChineseSimplified,
    /// Extended Chinese Simplified.
    ChineseSimplifiedExt,
    /// Chinese Traditional.
    ChineseTraditional,
    /// Korean.
    Korean,
    /// The Nintendo extension font (controller buttons and symbols).
    NintendoExt,
}

impl FontType {
    /// Every type, in `SharedFontType` order.
    pub const ALL: [FontType; 6] = [
        FontType::Standard,
        FontType::ChineseSimplified,
        FontType::ChineseSimplifiedExt,
        FontType::ChineseTraditional,
        FontType::Korean,
        FontType::NintendoExt,
    ];

    /// The `SharedFontType` value.
    #[must_use]
    pub fn index(self) -> u32 {
        self as u32
    }

    /// The system data title holding the font and the file's name in its
    /// romfs, as the console's firmware ships them.
    #[must_use]
    pub fn source(self) -> (u64, &'static str) {
        match self {
            FontType::Standard => (0x0100_0000_0000_0811, "nintendo_udsg-r_std_003.bfttf"),
            FontType::ChineseSimplified => {
                (0x0100_0000_0000_0814, "nintendo_udsg-r_org_zh-cn_003.bfttf")
            }
            FontType::ChineseSimplifiedExt => {
                (0x0100_0000_0000_0814, "nintendo_udsg-r_ext_zh-cn_003.bfttf")
            }
            FontType::ChineseTraditional => {
                (0x0100_0000_0000_0813, "nintendo_udjxh-db_zh-tw_003.bfttf")
            }
            FontType::Korean => (0x0100_0000_0000_0812, "nintendo_udsg-r_ko_003.bfttf"),
            FontType::NintendoExt => (0x0100_0000_0000_0810, "nintendo_ext_003.bfttf"),
        }
    }

    /// A short name for messages.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            FontType::Standard => "standard",
            FontType::ChineseSimplified => "Chinese Simplified",
            FontType::ChineseSimplifiedExt => "Chinese Simplified extension",
            FontType::ChineseTraditional => "Chinese Traditional",
            FontType::Korean => "Korean",
            FontType::NintendoExt => "Nintendo extension",
        }
    }
}

/// The TrueType font in a `.bfttf` file.
///
/// # Errors
///
/// The file is shorter than its header, its magic is wrong, or its size runs
/// past the end.
pub fn decode(b: &[u8]) -> Result<Vec<u8>> {
    if b.len() < 8 {
        return Err(Error::Format("bfttf: shorter than its header".into()));
    }
    let word = |i: usize| u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]]);
    let key = word(0) ^ MAGIC;
    let size = (word(4) ^ key).swap_bytes() as usize;
    if size > b.len() - 8 {
        return Err(Error::Format(format!(
            "bfttf: a {size}-byte font in {} bytes of data",
            b.len() - 8
        )));
    }
    let k = key.to_le_bytes();
    Ok(b[8..8 + size]
        .iter()
        .enumerate()
        .map(|(i, x)| x ^ k[i % 4])
        .collect())
}

/// `font` as a `.bfttf` file with `key`, the inverse of [`decode`].
#[must_use]
pub fn encode(font: &[u8], key: u32) -> Vec<u8> {
    let k = key.to_le_bytes();
    let mut out = Vec::with_capacity(font.len() + 8);
    out.extend_from_slice(&(MAGIC ^ key).to_le_bytes());
    out.extend_from_slice(&((font.len() as u32).swap_bytes() ^ key).to_le_bytes());
    out.extend(font.iter().enumerate().map(|(i, x)| x ^ k[i % 4]));
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_what_it_encodes() {
        let font = b"\x00\x01\x00\x00\x00\x0d\x00\x80 a font of odd length".to_vec();
        let file = encode(&font, 0x0618_6249);
        assert_ne!(&file[8..], &font[..]);
        assert_eq!(decode(&file).unwrap(), font);
    }

    #[test]
    fn the_size_is_stored_big_endian() {
        let file = encode(&vec![0; 2_053_628], 0x0618_6249);
        assert_eq!(
            &file[..8],
            &[0x36, 0xf8, 0x1a, 0x1e, 0x49, 0x7d, 0x4d, 0xfa]
        );
    }

    #[test]
    fn refuses_short_and_overlong_files() {
        assert!(decode(&[0; 7]).is_err());
        let mut file = encode(b"abcd", 7);
        file.truncate(10);
        assert!(decode(&file).is_err());
    }

    #[test]
    fn every_type_has_a_source() {
        let mut seen = std::collections::HashSet::new();
        for t in FontType::ALL {
            assert!(seen.insert(t.source()));
            assert_eq!(FontType::ALL[t.index() as usize], t);
        }
    }
}

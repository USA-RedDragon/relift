//! The 0x40-byte header at the start of a big-endian image.

use crate::error::{Error, Result};

/// The size of the header.
pub const SIZE: usize = 0x40;

/// The fields of the header; none is interpreted against a table of games.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Header {
    /// The PI bus domain 1 configuration word at `0x00`.
    pub pi_config: u32,
    /// The clock rate word at `0x04`; zero means the default.
    pub clock_rate: u32,
    /// The entry point word at `0x08`, before the boot variant's adjustment.
    pub entry: u32,
    /// The libultra release word at `0x0C`.
    pub release: u32,
    /// The first checksum at `0x10`.
    pub crc1: u32,
    /// The second checksum at `0x14`.
    pub crc2: u32,
    /// The 20 bytes of the image name at `0x20`.
    pub name: [u8; 20],
    /// The media byte at `0x3B`.
    pub media: u8,
    /// The two game id bytes at `0x3C`.
    pub game_id: [u8; 2],
    /// The region byte at `0x3E`.
    pub region: u8,
    /// The version byte at `0x3F`.
    pub version: u8,
}

fn word(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

impl Header {
    /// Parse the header of a big-endian image.
    ///
    /// # Errors
    ///
    /// The image is shorter than the header.
    pub fn parse(be: &[u8]) -> Result<Header> {
        if be.len() < SIZE {
            return Err(Error::TooShort(be.len()));
        }
        let mut name = [0; 20];
        name.copy_from_slice(&be[0x20..0x34]);
        Ok(Header {
            pi_config: word(be, 0),
            clock_rate: word(be, 4),
            entry: word(be, 8),
            release: word(be, 0xC),
            crc1: word(be, 0x10),
            crc2: word(be, 0x14),
            name,
            media: be[0x3B],
            game_id: [be[0x3C], be[0x3D]],
            region: be[0x3E],
            version: be[0x3F],
        })
    }

    /// The image name as text: printable ASCII kept, every other byte shown
    /// as `?`, trailing spaces and zero bytes dropped.
    #[must_use]
    pub fn name_text(&self) -> String {
        let text: String = self
            .name
            .iter()
            .map(|&b| {
                if (0x20..0x7F).contains(&b) {
                    char::from(b)
                } else if b == 0 {
                    ' '
                } else {
                    '?'
                }
            })
            .collect();
        text.trim_end().to_owned()
    }

    /// A header byte as a character when printable, else `?`.
    #[must_use]
    pub fn printable(b: u8) -> char {
        if (0x21..0x7F).contains(&b) {
            char::from(b)
        } else {
            '?'
        }
    }
}

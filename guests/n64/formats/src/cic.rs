//! The boot variant (CIC), named by a CRC-32 of the boot code area.
//!
//! Here the bytes at `0x40..0x1000` are only hashed; the analysis
//! (`relift-n64-analyze`) lifts the boot code of a known variant. The hash values are public facts from the
//! n64brew documentation and from emulator sources.

use crate::checksum::crc32;

/// The first byte of the boot code area.
pub const START: usize = 0x40;
/// The end of the boot code area.
pub const END: usize = 0x1000;

/// A boot variant with a published boot code area hash.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum Cic {
    /// CIC-NUS-6101.
    C6101,
    /// CIC-NUS-6102 and 7101, which share a boot code area.
    C6102,
    /// CIC-NUS-6103 and 7103.
    C6103,
    /// CIC-NUS-6105 and 7105.
    C6105,
    /// CIC-NUS-6106 and 7106.
    C6106,
    /// CIC-NUS-7102.
    C7102,
    /// CIC-NUS-8303, the 64DD.
    C8303,
}

impl Cic {
    /// Every variant.
    pub const ALL: [Cic; 7] = [
        Cic::C6101,
        Cic::C6102,
        Cic::C6103,
        Cic::C6105,
        Cic::C6106,
        Cic::C7102,
        Cic::C8303,
    ];

    /// The variant whose boot code area has CRC-32 `hash`.
    #[must_use]
    pub fn from_hash(hash: u32) -> Option<Cic> {
        Cic::ALL.into_iter().find(|c| c.hash() == hash)
    }

    /// The variant of a big-endian image, if it is at least `0x1000` bytes
    /// and its boot code area hashes to a known one.
    #[must_use]
    pub fn detect(be: &[u8]) -> Option<Cic> {
        Cic::from_hash(crc32(be.get(START..END)?))
    }

    /// The variant named `name`: any of the chip numbers, such as `6102` or
    /// `7101`.
    #[must_use]
    pub fn parse(name: &str) -> Option<Cic> {
        Cic::ALL
            .into_iter()
            .find(|c| c.name().split('/').any(|n| n == name))
    }

    /// The CRC-32 of the boot code area.
    #[must_use]
    pub const fn hash(self) -> u32 {
        match self {
            Cic::C6101 => 0x6170_A4A1,
            Cic::C6102 => 0x90BB_6CB5,
            Cic::C6103 => 0x0B05_0EE0,
            Cic::C6105 => 0x98BC_2C86,
            Cic::C6106 => 0xACC8_580A,
            Cic::C7102 => 0x009E_9EA3,
            Cic::C8303 => 0x0E01_8159,
        }
    }

    /// The chip numbers that load this boot code area.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Cic::C6101 => "6101",
            Cic::C6102 => "6102/7101",
            Cic::C6103 => "6103/7103",
            Cic::C6105 => "6105/7105",
            Cic::C6106 => "6106/7106",
            Cic::C7102 => "7102",
            Cic::C8303 => "8303",
        }
    }

    /// The seed of the checksum, where it is known and the checksum can be
    /// computed from the game's own data.
    #[must_use]
    pub const fn seed(self) -> Option<u32> {
        match self {
            Cic::C6101 | Cic::C6102 => Some(0xF8CA_4DDC),
            Cic::C6103 => Some(0xA388_6759),
            Cic::C6106 => Some(0x1FEA_617A),
            Cic::C6105 | Cic::C7102 | Cic::C8303 => None,
        }
    }

    /// What the boot code subtracts from the header's entry point word.
    #[must_use]
    pub const fn entry_adjust(self) -> u32 {
        match self {
            Cic::C6103 => 0x10_0000,
            Cic::C6106 => 0x20_0000,
            _ => 0,
        }
    }

    /// The entry point the boot code jumps to, from the header word.
    #[must_use]
    pub const fn entry(self, header_entry: u32) -> u32 {
        header_entry.wrapping_sub(self.entry_adjust())
    }
}

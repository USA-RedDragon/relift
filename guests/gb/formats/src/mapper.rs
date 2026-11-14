//! The cartridge-type byte at `$0147`: which memory bank controller the
//! cartridge carries and which extras sit on the board.

/// A memory bank controller family.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum MapperKind {
    /// No controller: 32 KiB of ROM, optionally RAM, no banking.
    None,
    /// MBC1.
    Mbc1,
    /// MBC1 wired as a multicart: the upper bank register has 4 bits, not 5,
    /// and the second register selects a game. Not a type byte; see
    /// [`crate::Cartridge::mapper`].
    Mbc1M,
    /// MBC2, with 512 half-bytes of built-in RAM.
    Mbc2,
    /// MBC3, with a real-time clock when [`Features::rtc`] is set.
    Mbc3,
    /// MBC5, with a rumble motor when [`Features::rumble`] is set.
    Mbc5,
    /// MBC6 (Net de Get).
    Mbc6,
    /// MBC7 (tilt sensor and serial EEPROM).
    Mbc7,
    /// MMM01 multicart controller.
    Mmm01,
    /// Hudson HuC1, with an infrared port.
    Huc1,
    /// Hudson HuC3, with a clock, speaker and infrared port.
    Huc3,
    /// Game Boy Camera.
    PocketCamera,
    /// Bandai TAMA5.
    Tama5,
}

impl MapperKind {
    /// A short lowercase name, stable for tool output.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            MapperKind::None => "none",
            MapperKind::Mbc1 => "mbc1",
            MapperKind::Mbc1M => "mbc1m",
            MapperKind::Mbc2 => "mbc2",
            MapperKind::Mbc3 => "mbc3",
            MapperKind::Mbc5 => "mbc5",
            MapperKind::Mbc6 => "mbc6",
            MapperKind::Mbc7 => "mbc7",
            MapperKind::Mmm01 => "mmm01",
            MapperKind::Huc1 => "huc1",
            MapperKind::Huc3 => "huc3",
            MapperKind::PocketCamera => "pocket-camera",
            MapperKind::Tama5 => "tama5",
        }
    }
}

/// What else the cartridge type byte says is on the board.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Features {
    /// External RAM chip.
    pub ram: bool,
    /// Battery.
    pub battery: bool,
    /// Real-time clock (MBC3).
    pub rtc: bool,
    /// Rumble motor (MBC5).
    pub rumble: bool,
    /// Accelerometer (MBC7).
    pub sensor: bool,
}

/// A decoded cartridge-type byte.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Mapper {
    /// The type byte as stored.
    pub code: u8,
    /// The controller family.
    pub kind: MapperKind,
    /// The extras.
    pub features: Features,
}

const fn f(ram: bool, battery: bool, rtc: bool, rumble: bool, sensor: bool) -> Features {
    Features {
        ram,
        battery,
        rtc,
        rumble,
        sensor,
    }
}

impl Mapper {
    /// Decode a cartridge-type byte, or `None` when it is not assigned.
    #[must_use]
    pub fn from_code(code: u8) -> Option<Mapper> {
        use MapperKind as K;
        let (kind, features) = match code {
            0x00 => (K::None, f(false, false, false, false, false)),
            0x01 => (K::Mbc1, f(false, false, false, false, false)),
            0x02 => (K::Mbc1, f(true, false, false, false, false)),
            0x03 => (K::Mbc1, f(true, true, false, false, false)),
            0x05 => (K::Mbc2, f(false, false, false, false, false)),
            0x06 => (K::Mbc2, f(false, true, false, false, false)),
            0x08 => (K::None, f(true, false, false, false, false)),
            0x09 => (K::None, f(true, true, false, false, false)),
            0x0B => (K::Mmm01, f(false, false, false, false, false)),
            0x0C => (K::Mmm01, f(true, false, false, false, false)),
            0x0D => (K::Mmm01, f(true, true, false, false, false)),
            0x0F => (K::Mbc3, f(false, true, true, false, false)),
            0x10 => (K::Mbc3, f(true, true, true, false, false)),
            0x11 => (K::Mbc3, f(false, false, false, false, false)),
            0x12 => (K::Mbc3, f(true, false, false, false, false)),
            0x13 => (K::Mbc3, f(true, true, false, false, false)),
            0x19 => (K::Mbc5, f(false, false, false, false, false)),
            0x1A => (K::Mbc5, f(true, false, false, false, false)),
            0x1B => (K::Mbc5, f(true, true, false, false, false)),
            0x1C => (K::Mbc5, f(false, false, false, true, false)),
            0x1D => (K::Mbc5, f(true, false, false, true, false)),
            0x1E => (K::Mbc5, f(true, true, false, true, false)),
            0x20 => (K::Mbc6, f(true, true, false, false, false)),
            0x22 => (K::Mbc7, f(true, true, false, true, true)),
            0xFC => (K::PocketCamera, f(true, true, false, false, false)),
            0xFD => (K::Tama5, f(true, true, true, false, false)),
            0xFE => (K::Huc3, f(true, true, true, false, false)),
            0xFF => (K::Huc1, f(true, true, false, false, false)),
            _ => return None,
        };
        Some(Mapper {
            code,
            kind,
            features,
        })
    }
}

//! The save library strings a ROM carries.

/// The save type a library string names.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SaveKind {
    /// `EEPROM_V`: serial EEPROM, 512 bytes or 8 KiB; the string does not
    /// say which.
    Eeprom,
    /// `SRAM_V` and `SRAM_F_V`: 32 KiB battery SRAM.
    Sram,
    /// `FLASH_V` and `FLASH512_V`: 64 KiB Flash.
    Flash64,
    /// `FLASH1M_V`: 128 KiB Flash in two banks.
    Flash128,
}

impl SaveKind {
    /// A short lowercase name, stable for tool output.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            SaveKind::Eeprom => "eeprom",
            SaveKind::Sram => "sram",
            SaveKind::Flash64 => "flash64",
            SaveKind::Flash128 => "flash128",
        }
    }

    /// The raw save sizes in bytes this type allows.
    #[must_use]
    pub fn sizes(self) -> &'static [usize] {
        match self {
            SaveKind::Eeprom => &[512, 8192],
            SaveKind::Sram => &[32768],
            SaveKind::Flash64 => &[65536],
            SaveKind::Flash128 => &[131_072],
        }
    }
}

/// Digits of the version the libraries append, such as `103`.
const VERSION_DIGITS: usize = 3;

/// The library strings, each with the type it names.
pub const LIBRARY_STRINGS: [(&str, SaveKind); 6] = [
    ("EEPROM_V", SaveKind::Eeprom),
    ("SRAM_V", SaveKind::Sram),
    ("SRAM_F_V", SaveKind::Sram),
    ("FLASH_V", SaveKind::Flash64),
    ("FLASH512_V", SaveKind::Flash64),
    ("FLASH1M_V", SaveKind::Flash128),
];

/// One library string found in an image.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SaveString {
    /// The prefix that matched, such as `FLASH1M_V`.
    pub prefix: &'static str,
    /// The type it names.
    pub kind: SaveKind,
    /// The up to three ASCII digits that follow, such as `103`; empty when
    /// none do.
    pub version: String,
    /// Offset of the string in the image.
    pub offset: usize,
}

/// Find every library string in `rom`, in image order.
#[must_use]
pub fn scan(rom: &[u8]) -> Vec<SaveString> {
    let mut found = Vec::new();
    for (i, b) in rom.iter().enumerate() {
        if !matches!(*b, b'E' | b'S' | b'F') {
            continue;
        }
        let tail = &rom[i..];
        for (prefix, kind) in LIBRARY_STRINGS {
            if tail.starts_with(prefix.as_bytes()) {
                let version = tail[prefix.len()..]
                    .iter()
                    .take(VERSION_DIGITS)
                    .take_while(|d| d.is_ascii_digit())
                    .map(|d| char::from(*d))
                    .collect();
                found.push(SaveString {
                    prefix,
                    kind,
                    version,
                    offset: i,
                });
            }
        }
    }
    found
}

/// The one save type all `strings` agree on; `None` when there are none or
/// they name different types.
#[must_use]
pub fn kind_of(strings: &[SaveString]) -> Option<SaveKind> {
    let first = strings.first()?.kind;
    strings.iter().all(|s| s.kind == first).then_some(first)
}

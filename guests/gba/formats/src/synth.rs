//! Synthetic cartridge images for tests: a valid header over a patterned
//! body, built from a few parameters, with the complement filled in.

use crate::checksum::complement;

/// What a synthetic cartridge looks like.
#[derive(Clone, Debug)]
pub struct Spec {
    /// The title, at most 12 bytes.
    pub title: String,
    /// The game code, 4 bytes.
    pub game_code: String,
    /// The maker code, 2 bytes.
    pub maker: String,
    /// The software version.
    pub version: u8,
    /// The 156 logo bytes; synthetic and unrelated to the real logo.
    pub logo: [u8; 156],
    /// The image length in bytes, at least `0xC0`.
    pub len: usize,
}

impl Default for Spec {
    fn default() -> Spec {
        Spec {
            title: "SYNTH".to_owned(),
            game_code: "ASYN".to_owned(),
            maker: "01".to_owned(),
            version: 0,
            logo: synthetic_logo(),
            len: 0x400,
        }
    }
}

/// A logo-sized pattern that is not the real logo.
#[must_use]
pub fn synthetic_logo() -> [u8; 156] {
    let mut logo = [0u8; 156];
    for (i, b) in logo.iter_mut().enumerate() {
        *b = u8::try_from((i * 7 + 3) % 256).unwrap_or(0);
    }
    logo
}

/// Build the image of `spec`: the entry branch `B 0xC0`, the header,
/// zero padding at `0xC0` and a counting pattern after it, with the
/// complement correct.
///
/// # Panics
///
/// `spec.len` is below `0xC0`, or a field is longer than its slot.
#[must_use]
pub fn rom(spec: &Spec) -> Vec<u8> {
    assert!(spec.len >= 0xC0, "an image holds at least the header");
    let mut rom = vec![0u8; spec.len];
    for (i, b) in rom.iter_mut().enumerate().skip(0xC0) {
        *b = u8::try_from(i % 251).unwrap_or(0);
    }
    rom[0..4].copy_from_slice(&0xEA00_002Eu32.to_le_bytes());
    rom[4..160].copy_from_slice(&spec.logo);
    put(&mut rom, 0xA0, 12, spec.title.as_bytes());
    put(&mut rom, 0xAC, 4, spec.game_code.as_bytes());
    put(&mut rom, 0xB0, 2, spec.maker.as_bytes());
    rom[0xB2] = 0x96;
    rom[0xBC] = spec.version;
    fix_complement(&mut rom);
    rom
}

fn put(rom: &mut [u8], at: usize, max: usize, bytes: &[u8]) {
    assert!(bytes.len() <= max, "field longer than its slot");
    rom[at..at + bytes.len()].copy_from_slice(bytes);
}

/// Recompute and store the complement of `rom`.
pub fn fix_complement(rom: &mut [u8]) {
    rom[0xBD] = complement(rom);
}

/// Write `s` at `at` in `rom`, for planting library strings.
///
/// # Panics
///
/// The string does not fit.
pub fn plant(rom: &mut [u8], at: usize, s: &str) {
    rom[at..at + s.len()].copy_from_slice(s.as_bytes());
}

/// A multiboot program of `len` bytes: a header with a branch at `0xC0`.
///
/// # Panics
///
/// `len` is below `0xC8`.
#[must_use]
pub fn multiboot(len: usize) -> Vec<u8> {
    let mut rom = rom(&Spec {
        len,
        ..Spec::default()
    });
    rom[0xC0..0xC4].copy_from_slice(&0xEA00_000Eu32.to_le_bytes());
    rom
}

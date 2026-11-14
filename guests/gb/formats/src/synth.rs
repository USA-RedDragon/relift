//! Synthetic cartridge images for tests: a valid header over zeroed banks,
//! built from a few parameters, with checksums filled in.

use crate::banks::{ROM_BANK_SIZE, rom_banks};
use crate::checksum::{global_checksum, header_checksum};
use crate::header::LOGO;

/// What a synthetic cartridge looks like.
#[derive(Clone, Debug)]
pub struct Spec {
    /// The title, at most 15 bytes.
    pub title: String,
    /// The CGB flag byte.
    pub cgb: u8,
    /// The SGB flag byte.
    pub sgb: u8,
    /// The cartridge type byte.
    pub cart_type: u8,
    /// The ROM size code; the image gets the size it names.
    pub rom_size: u8,
    /// The RAM size code.
    pub ram_size: u8,
}

impl Default for Spec {
    fn default() -> Spec {
        Spec {
            title: "SYNTH".to_owned(),
            cgb: 0,
            sgb: 0,
            cart_type: 0,
            rom_size: 0,
            ram_size: 0,
        }
    }
}

/// Build the image of `spec`: bank `n` is filled with the byte `n + 1`
/// except for the header, the entry stub `nop; jp $0150` is in place and
/// both checksums are correct.
///
/// # Panics
///
/// `spec.rom_size` is not a ROM size code.
#[must_use]
pub fn rom(spec: &Spec) -> Vec<u8> {
    let banks = rom_banks(spec.rom_size).expect("the spec names a ROM size code");
    let mut rom = vec![0u8; banks * ROM_BANK_SIZE];
    for (n, bank) in rom.chunks_mut(ROM_BANK_SIZE).enumerate() {
        bank.fill(u8::try_from((n + 1) % 256).unwrap_or(0));
    }
    rom[0x100..0x150].fill(0);
    rom[0x100..0x104].copy_from_slice(&[0x00, 0xC3, 0x50, 0x01]);
    rom[0x104..0x134].copy_from_slice(&LOGO);
    let title = spec.title.as_bytes();
    let n = title.len().min(15);
    rom[0x134..0x134 + n].copy_from_slice(&title[..n]);
    rom[0x143] = spec.cgb;
    rom[0x146] = spec.sgb;
    rom[0x147] = spec.cart_type;
    rom[0x148] = spec.rom_size;
    rom[0x149] = spec.ram_size;
    fix_checksums(&mut rom);
    rom
}

/// Recompute and store both checksums of `rom`.
pub fn fix_checksums(rom: &mut [u8]) {
    rom[0x14D] = header_checksum(rom);
    let g = global_checksum(rom).to_be_bytes();
    rom[0x14E] = g[0];
    rom[0x14F] = g[1];
}

/// A 1 MiB MBC1 image laid out as a four-game multicart: each of banks
/// `$00`, `$10`, `$20` and `$30` starts with a header and logo.
///
/// # Panics
///
/// Never, in practice: the spec is fixed.
#[must_use]
pub fn mbc1_multicart() -> Vec<u8> {
    let mut rom = rom(&Spec {
        cart_type: 0x01,
        rom_size: 0x05,
        ..Spec::default()
    });
    for bank in [0x10usize, 0x20, 0x30] {
        let at = bank * ROM_BANK_SIZE + 0x104;
        rom[at..at + LOGO.len()].copy_from_slice(&LOGO);
    }
    fix_checksums(&mut rom);
    rom
}

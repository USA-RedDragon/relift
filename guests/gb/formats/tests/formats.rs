//! The cartridge reader against synthetic images.

use relift_gb_formats::banks::{self, ROM_BANK_SIZE};
use relift_gb_formats::cartridge::is_mbc1_multicart;
use relift_gb_formats::synth::{self, Spec};
use relift_gb_formats::{Cartridge, CgbFlag, Error, Header, MapperKind};

fn spec(cart_type: u8) -> Spec {
    Spec {
        cart_type,
        ..Spec::default()
    }
}

#[test]
fn plain_rom_decodes_and_passes_every_check() {
    let rom = synth::rom(&Spec::default());
    let c = Cartridge::parse(&rom).expect("a valid image");
    assert_eq!(c.header.title, "SYNTH");
    assert_eq!(c.header.cgb, CgbFlag::DmgOnly);
    assert!(!c.header.sgb);
    assert_eq!(c.header.entry_jump(), Some(0x150));
    assert_eq!(c.header.layout.rom_banks, 2);
    assert_eq!(c.header.layout.ram_bytes, 0);
    assert_eq!(c.mapper(), MapperKind::None);
    assert!(c.checks().all());
}

#[test]
fn cgb_and_sgb_flags() {
    let mk = |cgb, sgb| {
        let rom = synth::rom(&Spec {
            cgb,
            sgb,
            ..Spec::default()
        });
        Header::parse(&rom).expect("a valid image")
    };
    assert_eq!(mk(0x80, 0).cgb, CgbFlag::Compatible);
    assert_eq!(mk(0xC0, 0).cgb, CgbFlag::Only);
    assert_eq!(mk(0x00, 0).cgb, CgbFlag::DmgOnly);
    assert_eq!(mk(0x42, 0).cgb, CgbFlag::DmgOnly);
    assert!(mk(0, 3).sgb);
    assert!(!mk(0, 1).sgb);
}

#[test]
fn cgb_title_is_fifteen_bytes_and_manufacturer_is_read() {
    let mut rom = synth::rom(&Spec {
        title: "LONGTITLE".to_owned(),
        cgb: 0x80,
        ..Spec::default()
    });
    rom[0x13F..0x143].copy_from_slice(b"AXYE");
    rom[0x143] = 0x80;
    synth::fix_checksums(&mut rom);
    let h = Header::parse(&rom).expect("a valid image");
    assert_eq!(h.title, "LONGTITLE");
    assert_eq!(h.manufacturer.as_deref(), Some("AXYE"));
    let dmg = synth::rom(&Spec::default());
    assert_eq!(Header::parse(&dmg).expect("valid").manufacturer, None);
}

#[test]
fn every_type_byte_maps_to_its_controller() {
    use MapperKind::*;
    let table: &[(u8, MapperKind, bool, bool, bool, bool)] = &[
        (0x00, None, false, false, false, false),
        (0x01, Mbc1, false, false, false, false),
        (0x03, Mbc1, true, true, false, false),
        (0x05, Mbc2, false, false, false, false),
        (0x06, Mbc2, false, true, false, false),
        (0x09, None, true, true, false, false),
        (0x0B, Mmm01, false, false, false, false),
        (0x0D, Mmm01, true, true, false, false),
        (0x0F, Mbc3, false, true, true, false),
        (0x10, Mbc3, true, true, true, false),
        (0x11, Mbc3, false, false, false, false),
        (0x13, Mbc3, true, true, false, false),
        (0x19, Mbc5, false, false, false, false),
        (0x1B, Mbc5, true, true, false, false),
        (0x1C, Mbc5, false, false, false, true),
        (0x1E, Mbc5, true, true, false, true),
        (0x20, Mbc6, true, true, false, false),
        (0x22, Mbc7, true, true, false, true),
        (0xFC, PocketCamera, true, true, false, false),
        (0xFD, Tama5, true, true, true, false),
        (0xFE, Huc3, true, true, true, false),
        (0xFF, Huc1, true, true, false, false),
    ];
    for (code, kind, ram, battery, rtc, rumble) in table {
        let rom = synth::rom(&spec(*code));
        let c = Cartridge::parse(&rom).expect("a valid image");
        let m = c.header.mapper;
        assert_eq!(m.kind, *kind, "type ${code:02X}");
        assert_eq!(
            (
                m.features.ram,
                m.features.battery,
                m.features.rtc,
                m.features.rumble
            ),
            (*ram, *battery, *rtc, *rumble),
            "type ${code:02X}"
        );
    }
    assert!(
        Cartridge::parse(&synth::rom(&spec(0x22)))
            .expect("valid")
            .header
            .mapper
            .features
            .sensor
    );
}

#[test]
fn unassigned_codes_are_errors() {
    for bad in [0x04, 0x07, 0x0A, 0x0E, 0x14, 0x21, 0xFB] {
        let rom = synth::rom(&spec(bad));
        assert_eq!(Header::parse(&rom), Err(Error::UnknownCartridgeType(bad)));
    }
    let mut rom = synth::rom(&Spec::default());
    rom[0x148] = 0x09;
    assert_eq!(Header::parse(&rom), Err(Error::UnknownRomSize(0x09)));
    let mut rom = synth::rom(&Spec::default());
    rom[0x149] = 0x06;
    assert_eq!(Header::parse(&rom), Err(Error::UnknownRamSize(0x06)));
    assert_eq!(Header::parse(&[0; 0x14F]), Err(Error::TooShort(0x14F)));
}

#[test]
fn rom_and_ram_size_codes() {
    let banks: Vec<usize> = (0..=8)
        .map(|c| banks::rom_banks(c).expect("code"))
        .collect();
    assert_eq!(banks, [2, 4, 8, 16, 32, 64, 128, 256, 512]);
    assert_eq!(banks::rom_banks(0x52), Some(72));
    assert_eq!(banks::rom_banks(0x53), Some(80));
    assert_eq!(banks::rom_banks(0x54), Some(96));
    assert_eq!(banks::rom_banks(0x09), None);
    assert_eq!(banks::ram_size(0x02), Some((0x2000, 1)));
    assert_eq!(banks::ram_size(0x03), Some((0x8000, 4)));
    assert_eq!(banks::ram_size(0x04), Some((0x20000, 16)));
    assert_eq!(banks::ram_size(0x05), Some((0x10000, 8)));
    let rom = synth::rom(&Spec {
        rom_size: 0x06,
        ram_size: 0x04,
        cart_type: 0x03,
        ..Spec::default()
    });
    let c = Cartridge::parse(&rom).expect("valid");
    assert_eq!(c.header.layout.rom_banks, 128);
    assert_eq!(c.header.layout.rom_bytes(), 2 * 1024 * 1024);
    assert_eq!(c.header.layout.ram_banks, 16);
    assert!(c.checks().size);
}

#[test]
fn checksums_detect_damage() {
    let mut rom = synth::rom(&Spec::default());
    rom[0x200] ^= 1;
    let c = Cartridge::parse(&rom).expect("valid");
    let k = c.checks();
    assert!(k.header_checksum && k.logo && !k.global_checksum);
    rom[0x134] ^= 1;
    let k = Cartridge::parse(&rom).expect("valid").checks();
    assert!(!k.header_checksum);
    rom[0x105] ^= 1;
    assert!(!Cartridge::parse(&rom).expect("valid").checks().logo);
    let mut short = synth::rom(&Spec::default());
    short.truncate(ROM_BANK_SIZE + 0x10);
    assert!(!Cartridge::parse(&short).expect("valid").checks().size);
}

#[test]
fn header_checksum_matches_the_documented_formula() {
    let mut rom = vec![0u8; 0x150];
    rom[0x134..0x14D].fill(0);
    assert_eq!(
        relift_gb_formats::checksum::header_checksum(&rom),
        0u8.wrapping_sub(25)
    );
}

#[test]
fn multicart_is_found_only_when_the_layout_says_so() {
    let multi = synth::mbc1_multicart();
    assert!(is_mbc1_multicart(&multi));
    assert_eq!(
        Cartridge::parse(&multi).expect("valid").mapper(),
        MapperKind::Mbc1M
    );
    let single = synth::rom(&Spec {
        cart_type: 0x01,
        rom_size: 0x05,
        ..Spec::default()
    });
    assert!(!is_mbc1_multicart(&single));
    assert_eq!(
        Cartridge::parse(&single).expect("valid").mapper(),
        MapperKind::Mbc1
    );
    let mut mbc5 = multi.clone();
    mbc5[0x147] = 0x19;
    assert_eq!(
        Cartridge::parse(&mbc5).expect("valid").mapper(),
        MapperKind::Mbc5
    );
}

#[test]
fn bank_arithmetic() {
    assert_eq!(banks::rom_offset(5, 0x0100), Some(0x100));
    assert_eq!(banks::rom_offset(5, 0x4000), Some(5 * ROM_BANK_SIZE));
    assert_eq!(banks::rom_offset(5, 0x7FFF), Some(6 * ROM_BANK_SIZE - 1));
    assert_eq!(banks::rom_offset(5, 0x8000), None);
    assert_eq!(banks::bank_address(0x3FFF), (0, 0x3FFF));
    assert_eq!(banks::bank_address(0x4000), (1, 0x4000));
    assert_eq!(banks::bank_address(3 * ROM_BANK_SIZE + 2), (3, 0x4002));
    let rom = synth::rom(&Spec {
        rom_size: 0x01,
        ..Spec::default()
    });
    assert_eq!(banks::rom_bank(&rom, 3).expect("bank")[0], 4);
    assert!(banks::rom_bank(&rom, 4).is_none());
}

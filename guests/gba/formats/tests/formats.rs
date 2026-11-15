use relift_gba_formats::checksum::{complement, logo_sha256};
use relift_gba_formats::header::{MAX_ROM, is_multiboot};
use relift_gba_formats::save::{self, SaveKind};
use relift_gba_formats::synth::{self, Spec};
use relift_gba_formats::{Error, Header, Rom};

fn spec_hash(spec: &Spec) -> [u8; 32] {
    logo_sha256(&synth::rom(spec))
}

#[test]
fn header_fields_are_decoded() {
    let rom = synth::rom(&Spec {
        title: "HELLO".to_owned(),
        game_code: "AHEL".to_owned(),
        maker: "7Z".to_owned(),
        version: 3,
        ..Spec::default()
    });
    let h = Header::parse(&rom).expect("a valid header");
    assert_eq!(h.title, "HELLO");
    assert_eq!(h.game_code, "AHEL");
    assert_eq!(h.maker, "7Z");
    assert_eq!(h.version, 3);
    assert_eq!(h.fixed, 0x96);
    assert_eq!(h.len, 0x400);
    assert_eq!(h.entry_target(), Some(0x0800_00C0));
}

#[test]
fn entry_branch_sign_extends() {
    let mut rom = synth::rom(&Spec::default());
    rom[0..4].copy_from_slice(&0xEAFF_FFFEu32.to_le_bytes());
    assert_eq!(
        Header::parse(&rom).unwrap().entry_target(),
        Some(0x0800_0000)
    );
    rom[0..4].copy_from_slice(&0xE3A0_0000u32.to_le_bytes());
    assert_eq!(Header::parse(&rom).unwrap().entry_target(), None);
}

#[test]
fn complement_covers_a0_to_bc_and_detects_changes() {
    let spec = Spec::default();
    let mut rom = synth::rom(&spec);
    let h = Header::parse(&rom).unwrap();
    let hash = spec_hash(&spec);
    assert!(h.checks_against(&hash).all());
    assert_eq!(h.complement, h.complement_computed);
    for at in [0xA0, 0xAC, 0xB0, 0xB2, 0xBC] {
        let mut bad = rom.clone();
        bad[at] ^= 0x01;
        let h = Header::parse(&bad).unwrap();
        assert!(!h.checks_against(&hash).complement, "byte {at:#x}");
    }
    rom[0xC4] ^= 0xFF;
    assert!(
        Header::parse(&rom)
            .unwrap()
            .checks_against(&hash)
            .complement
    );
    let mut sum = 0u8;
    for b in &rom[0xA0..0xBD] {
        sum = sum.wrapping_add(*b);
    }
    assert_eq!(complement(&rom).wrapping_add(sum).wrapping_add(0x19), 0);
}

#[test]
fn logo_is_checked_by_hash_only() {
    let spec = Spec::default();
    let rom = synth::rom(&spec);
    let h = Header::parse(&rom).unwrap();
    assert!(!h.checks().logo, "the synthetic logo is not the real one");
    assert!(h.checks_against(&spec_hash(&spec)).logo);
    let mut other = rom.clone();
    other[0x20] ^= 1;
    let h = Header::parse(&other).unwrap();
    assert!(!h.checks_against(&spec_hash(&spec)).logo);
}

#[test]
fn fixed_byte_and_size_checks() {
    let spec = Spec::default();
    let hash = spec_hash(&spec);
    let mut rom = synth::rom(&spec);
    rom[0xB2] = 0x97;
    synth::fix_complement(&mut rom);
    assert!(!Header::parse(&rom).unwrap().checks_against(&hash).fixed);
    let odd = synth::rom(&Spec {
        len: 0x403,
        ..Spec::default()
    });
    assert!(!Header::parse(&odd).unwrap().checks_against(&hash).size);
}

#[test]
fn size_limits_and_short_images() {
    assert_eq!(Header::parse(&[0; 0x80]), Err(Error::TooShort(0x80)));
    let big = vec![0u8; MAX_ROM + 4];
    assert_eq!(Header::parse(&big), Err(Error::TooLong(MAX_ROM + 4)));
    let max = synth::rom(&Spec {
        len: MAX_ROM,
        ..Spec::default()
    });
    assert!(Header::parse(&max).is_ok());
}

#[test]
fn multiboot_is_refused() {
    let mb = synth::multiboot(0x2000);
    assert!(is_multiboot(&mb));
    assert_eq!(Header::parse(&mb), Err(Error::Multiboot));
    let msg = Error::Multiboot.to_string();
    assert!(msg.contains("multiboot"), "{msg}");
    let cart = synth::rom(&Spec::default());
    assert!(!is_multiboot(&cart));
    let mut large = synth::multiboot(0x2000);
    large.resize(0x80000, 0);
    assert!(!is_multiboot(&large), "larger than work RAM is a cartridge");
}

#[test]
fn rom_mirrors_the_three_wait_regions() {
    let img = synth::rom(&Spec::default());
    let rom = Rom::new(&img);
    for base in [0x0800_0000u32, 0x0A00_0000, 0x0C00_0000] {
        assert_eq!(rom.read8(base + 0xA0), b'S');
        assert_eq!(rom.read16(base + 0xA0), u16::from_le_bytes(*b"SY"));
        assert_eq!(rom.read32(base), 0xEA00_002E);
    }
    assert_eq!(rom.read32(0x0800_0002), 0xEA00_002E, "low bits ignored");
    assert!(Rom::contains(0x0D00_0000));
    assert!(!Rom::contains(0x0E00_0000));
}

#[test]
fn reads_past_the_image_return_open_bus() {
    let img = synth::rom(&Spec::default());
    let rom = Rom::new(&img);
    let addr = 0x0800_1000u32;
    assert_eq!(rom.read16(addr), 0x0800);
    assert_eq!(rom.read8(addr), 0x00);
    assert_eq!(rom.read8(addr + 1), 0x08);
    assert_eq!(rom.read32(addr + 4), 0x0803_0802);
    assert_eq!(Rom::open_bus(0x0A12_3456), 0x1A2B);
}

#[test]
fn eeprom_window_follows_the_size() {
    let small = synth::rom(&Spec {
        len: 16 * 1024 * 1024,
        ..Spec::default()
    });
    assert!(Rom::new(&small).eeprom_spans_region());
    let large = synth::rom(&Spec {
        len: 16 * 1024 * 1024 + 4,
        ..Spec::default()
    });
    assert!(!Rom::new(&large).eeprom_spans_region());
}

#[test]
fn save_strings_are_found_with_versions() {
    let mut img = synth::rom(&Spec::default());
    synth::plant(&mut img, 0x100, "EEPROM_V124");
    synth::plant(&mut img, 0x120, "SRAM_F_V102");
    synth::plant(&mut img, 0x140, "SRAM_V113");
    synth::plant(&mut img, 0x160, "FLASH_V120");
    synth::plant(&mut img, 0x180, "FLASH512_V131");
    synth::plant(&mut img, 0x1A0, "FLASH1M_V103");
    let found = save::scan(&img);
    let got: Vec<_> = found
        .iter()
        .map(|s| (s.prefix, s.version.as_str(), s.offset))
        .collect();
    assert_eq!(
        got,
        vec![
            ("EEPROM_V", "124", 0x100),
            ("SRAM_F_V", "102", 0x120),
            ("SRAM_V", "113", 0x140),
            ("FLASH_V", "120", 0x160),
            ("FLASH512_V", "131", 0x180),
            ("FLASH1M_V", "103", 0x1A0),
        ]
    );
    assert_eq!(found[5].kind, SaveKind::Flash128);
    assert_eq!(save::kind_of(&found), None);
    assert_eq!(save::kind_of(&found[..1]), Some(SaveKind::Eeprom));
    assert_eq!(save::kind_of(&found[1..3]), Some(SaveKind::Sram));
}

#[test]
fn a_clean_image_has_no_save_strings() {
    let img = synth::rom(&Spec::default());
    assert!(save::scan(&img).is_empty());
    assert_eq!(save::kind_of(&[]), None);
    assert_eq!(SaveKind::Eeprom.sizes(), &[512, 8192]);
}

//! The cartridge reader against synthetic images.

use relift_sms_formats::banks::{BANK_SIZE, COPIER_HEADER};
use relift_sms_formats::codemasters::{self, CodemastersHeader};
use relift_sms_formats::header::{Header, Region, bios_checksum, size_from_code};
use relift_sms_formats::synth::{self, Spec};
use relift_sms_formats::{Cartridge, Error, Layout, MapperKind, System, SystemSource};

#[test]
fn plain_export_rom_decodes() {
    let rom = synth::rom(&Spec::default());
    let c = Cartridge::parse(&rom, None).expect("a valid image");
    let h = c.header.expect("a header");
    assert_eq!(h.offset, 0x7FF0);
    assert_eq!(h.product_code, "7026");
    assert_eq!(h.region, Some(Region::SmsExport));
    assert_eq!(h.size_bytes, Some(32 * 1024));
    assert!(h.checksum_ok);
    assert_eq!(c.layout.banks, 2);
    assert_eq!(c.system, Some((System::Sms, SystemSource::Header)));
    assert_eq!(c.mapper.kind, MapperKind::None);
}

#[test]
fn header_is_found_at_each_offset() {
    for (off, len, code) in [
        (0x7FF0, 0x8000, 0xC),
        (0x3FF0, 0x4000, 0xB),
        (0x1FF0, 0x2000, 0xA),
    ] {
        let rom = synth::rom(&Spec {
            len,
            header_at: Some(off),
            size_code: code,
            ..Spec::default()
        });
        let h = Header::find(&rom).expect("a header");
        assert_eq!(h.offset, off);
        assert!(h.checksum_ok, "offset {off:#X}");
    }
}

#[test]
fn product_code_keeps_the_high_digit_and_the_version() {
    let rom = synth::rom(&Spec {
        product: [0x26, 0x70, 0xA0],
        version: 3,
        ..Spec::default()
    });
    let h = Header::find(&rom).expect("a header");
    assert_eq!(h.product_code, "A7026");
    assert_eq!(h.product_high, 0xA);
    assert_eq!(h.version, 3);
}

#[test]
fn region_codes_name_and_pick_the_system() {
    let names = [
        (3, "SMS Japan", System::Sms),
        (4, "SMS Export", System::Sms),
        (5, "GG Japan", System::Gg),
        (6, "GG Export", System::Gg),
        (7, "GG International", System::Gg),
    ];
    for (code, name, system) in names {
        let rom = synth::rom(&Spec {
            region: code,
            ..Spec::default()
        });
        let c = Cartridge::parse(&rom, Some("sg")).expect("a valid image");
        assert_eq!(
            c.header.expect("a header").region.map(Region::name),
            Some(name)
        );
        assert_eq!(c.system, Some((system, SystemSource::Header)));
    }
}

#[test]
fn extension_is_only_a_fallback() {
    let rom = synth::rom(&Spec {
        header_at: None,
        ..Spec::default()
    });
    for (ext, want) in [
        ("sms", System::Sms),
        (".GG", System::Gg),
        ("Sg", System::Sg1000),
        ("SC", System::Sg1000),
    ] {
        let c = Cartridge::parse(&rom, Some(ext)).expect("a valid image");
        assert_eq!(c.system, Some((want, SystemSource::Extension)), "{ext}");
    }
    assert_eq!(
        Cartridge::parse(&rom, Some("bin")).expect("valid").system,
        None
    );
    assert_eq!(Cartridge::parse(&rom, None).expect("valid").system, None);
    let bad = synth::rom(&Spec {
        region: 9,
        ..Spec::default()
    });
    let c = Cartridge::parse(&bad, Some("gg")).expect("a valid image");
    assert_eq!(c.system, Some((System::Gg, SystemSource::Extension)));
}

#[test]
fn bios_checksum_leaves_out_the_header_and_follows_the_size_code() {
    let mut rom = synth::rom(&Spec {
        len: 64 * 1024,
        size_code: 0xE,
        ..Spec::default()
    });
    let h = Header::find(&rom).expect("a header");
    assert!(h.checksum_ok);
    rom[0x7FF8] ^= 0xFF;
    assert!(Header::find(&rom).expect("a header").checksum_ok);
    rom[0x9000] ^= 0x01;
    assert!(!Header::find(&rom).expect("a header").checksum_ok);
    let small = synth::rom(&Spec {
        size_code: 0xF,
        ..Spec::default()
    });
    let h = Header::find(&small).expect("a header");
    assert_eq!(h.computed_checksum, None);
    assert!(!h.checksum_ok);
    assert_eq!(bios_checksum(&[1, 2, 3, 4], 100, 4), Some(10));
    assert_eq!(size_from_code(0x3), None);
}

#[test]
fn odd_sizes_and_copier_headers_are_reported() {
    let l = Layout::of(48 * 1024).expect("valid");
    assert_eq!(
        (l.banks, l.power_of_two, l.copier_header),
        (3, false, false)
    );
    let l = Layout::of(48 * 1024 + COPIER_HEADER).expect("valid");
    assert_eq!(
        (l.banks, l.rom_bytes, l.copier_header),
        (3, 48 * 1024, true)
    );
    let l = Layout::of(10_000).expect("valid");
    assert_eq!((l.banks, l.partial_bank, l.copier_header), (1, true, false));
    let l = Layout::of(256 * 1024).expect("valid");
    assert!(l.power_of_two);
    assert_eq!(Layout::of(0), Err(Error::Empty));
    assert_eq!(Layout::of(COPIER_HEADER), Err(Error::Empty));
    assert_eq!(
        Layout::of(5 * 1024 * 1024),
        Err(Error::TooLarge(5 * 1024 * 1024))
    );
}

#[test]
fn copier_header_is_skipped_by_the_header_search() {
    let rom = synth::rom(&Spec {
        copier_header: true,
        ..Spec::default()
    });
    let c = Cartridge::parse(&rom, None).expect("a valid image");
    assert!(c.layout.copier_header);
    assert_eq!(c.layout.rom_bytes, 2 * BANK_SIZE);
    assert!(c.header.expect("a header").checksum_ok);
}

#[test]
fn codemasters_header_and_checksum() {
    let rom = synth::rom(&Spec {
        len: 4 * BANK_SIZE,
        codemasters: true,
        ..Spec::default()
    });
    let cm = CodemastersHeader::read(&rom).expect("a header");
    assert_eq!(cm.banks, 4);
    assert_eq!(cm.date, [0x31, 0x08, 0x93, 0x10, 0x59]);
    assert!(cm.pair_ok);
    assert!(cm.checksum_ok);
    assert_eq!(cm.checksum.wrapping_add(cm.checksum_negated), 0);
    let c = Cartridge::parse(&rom, None).expect("a valid image");
    assert_eq!(c.mapper.kind, MapperKind::Codemasters);
    let mut bad = rom.clone();
    bad[0x9000] ^= 0x10;
    assert!(!CodemastersHeader::read(&bad).expect("a header").checksum_ok);
    assert_eq!(codemasters::checksum_over(&rom, 4), cm.checksum);
}

#[test]
fn mapper_follows_the_stores() {
    let mk = |len, stores: &[u16]| {
        let rom = synth::rom(&Spec {
            len,
            size_code: 0xF,
            stores: stores.to_vec(),
            ..Spec::default()
        });
        Cartridge::parse(&rom, None).expect("valid").mapper
    };
    let m = mk(3 * BANK_SIZE, &[]);
    assert_eq!(m.kind, MapperKind::None);
    let m = mk(8 * BANK_SIZE, &[0xFFFF, 0xFFFD, 0x8000]);
    assert_eq!(
        (m.kind, m.sega_stores, m.codemasters_stores),
        (MapperKind::Sega, 2, 1)
    );
    let m = mk(8 * BANK_SIZE, &[0x8000, 0x4000, 0x1234]);
    assert_eq!((m.kind, m.codemasters_stores), (MapperKind::Codemasters, 2));
    let m = mk(8 * BANK_SIZE, &[0xA000]);
    assert_eq!((m.kind, m.korea_stores), (MapperKind::Korea, 1));
    let m = mk(8 * BANK_SIZE, &[]);
    assert_eq!(m.kind, MapperKind::Sega);
    assert!(m.reason.contains("assumed"));
    let m = mk(2 * BANK_SIZE, &[0xA000]);
    assert_eq!(m.kind, MapperKind::None);
    let m = mk(3 * BANK_SIZE, &[0xFFFC]);
    assert_eq!(m.kind, MapperKind::Sega);
}

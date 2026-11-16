use relift_nds_formats::checksum::{crc16, sha256};
use relift_nds_formats::synth::{self, Layout, OverlaySpec, Spec, StaticSpec};
use relift_nds_formats::{Cpu, Error, Header, Rom, blz};

fn overlay(id: u32, compress: bool) -> OverlaySpec {
    OverlaySpec {
        id,
        ram: 0x0210_0000 + id * 0x1000,
        data: synth::pattern(0x300 + 16 * id as usize, id as u8),
        bss: 0x40,
        compress,
        authenticated: id == 1,
        static_init: (0x0210_0000 + id * 0x1000, 0x0210_0008 + id * 0x1000),
    }
}

fn full() -> Spec {
    Spec {
        arm9: StaticSpec {
            compress: true,
            ..StaticSpec::default()
        },
        overlays9: vec![overlay(0, false), overlay(1, true), overlay(2, true)],
        overlays7: vec![overlay(0, true)],
        files: vec![
            ("readme.txt".to_owned(), synth::pattern(33, 9)),
            ("data/a.bin".to_owned(), synth::pattern(100, 4)),
            ("data/b.bin".to_owned(), synth::pattern(7, 5)),
            ("data/sub/c.bin".to_owned(), synth::pattern(300, 6)),
            ("zeta/d.bin".to_owned(), synth::pattern(12, 7)),
        ],
        ..Spec::default()
    }
}

#[test]
fn crc16_matches_the_known_vector() {
    assert_eq!(crc16(b"123456789"), 0x4B37);
    assert_eq!(crc16(&[]), 0xFFFF);
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
    assert_eq!(h.arm9.rom_offset, 0x4000);
    assert_eq!(h.arm9.ram, 0x0200_0000);
    assert_eq!(h.arm9.entry, 0x0200_0800);
    assert_eq!(h.arm7.ram, 0x0238_0000);
    assert_eq!(h.header_size, 0x4000);
    assert_eq!(h.total_size as usize, rom.len());
    assert_eq!(h.len, rom.len());
}

#[test]
fn checks_pass_on_a_built_image_and_fail_when_bytes_change() {
    let mut rom = synth::rom(&Spec::default());
    let h = Header::parse(&rom).unwrap();
    let logo = h.logo_sha256;
    let c = h.checks_against(&logo);
    assert!(c.all());
    assert_eq!(c.secure_crc, Some(true));
    assert!(!h.checks().logo, "the synthetic logo is not the real one");

    rom[0x20] ^= 0x10;
    let c = Header::parse(&rom).unwrap().checks_against(&logo);
    assert!(!c.header_crc);
    assert!(c.logo_crc);

    synth::fix_crcs(&mut rom);
    rom[0xC5] ^= 1;
    let h = Header::parse(&rom).unwrap();
    assert!(!h.checks_against(&logo).logo);
    assert!(!h.checks_against(&logo).logo_crc);

    let mut rom = synth::rom(&Spec::default());
    rom[0x4100] ^= 1;
    let h = Header::parse(&rom).unwrap();
    assert_eq!(h.checks_against(&h.logo_sha256).secure_crc, Some(false));
}

#[test]
fn a_short_image_has_no_secure_crc() {
    let rom = synth::rom(&Spec::default());
    let h = Header::parse(&rom[..0x300]).unwrap();
    assert_eq!(h.secure_crc_computed, None);
    assert_eq!(h.checks_against(&h.logo_sha256).secure_crc, None);
}

#[test]
fn the_arm9_static_with_a_footer_and_its_autoloads() {
    let spec = Spec::default();
    let bytes = synth::rom(&spec);
    let rom = Rom::parse(&bytes).unwrap();
    let u = rom.arm9().unwrap();
    let p = u.params.expect("module parameters");
    assert_eq!(p.source, relift_nds_formats::params::Source::Footer);
    assert_eq!(p.offset, synth::PARAMS_AT);
    assert_eq!(p.sdk_version, Some(0x0003_0000));
    assert_eq!((p.bss_start, p.bss_end), (0x0210_0000, 0x0210_4000));
    assert!(!u.compressed);
    assert_eq!(u.image, synth::static_image(&spec.arm9).0);
    assert_eq!(u.autoloads.len(), 2);
    for (a, want) in u.autoloads.iter().zip(&spec.arm9.autoloads) {
        assert_eq!(
            (a.addr, a.size, a.bss),
            (want.addr, want.data.len() as u32, want.bss)
        );
        assert_eq!(u.autoload_data(a), want.data);
    }
}

#[test]
fn a_compressed_static_is_decompressed_and_its_end_cleared() {
    let spec = full();
    let bytes = synth::rom(&spec);
    let rom = Rom::parse(&bytes).unwrap();
    let u = rom.arm9().unwrap();
    assert!(u.compressed);
    assert!(u.raw.len() < u.image.len());
    assert_eq!(u.image, synth::static_image(&spec.arm9).0);
    let p = u.params.unwrap();
    assert_ne!(p.compressed_end, 0);
    assert_eq!(p.compressed_end - u.base, u.raw.len() as u32);
    assert_eq!(
        u.autoload_data(&u.autoloads[1]),
        spec.arm9.autoloads[1].data
    );
}

#[test]
fn both_marker_orders_are_found() {
    for layout in [Layout::Markers, Layout::MarkersSwapped] {
        let spec = Spec {
            arm9: StaticSpec {
                layout,
                ..StaticSpec::default()
            },
            ..Spec::default()
        };
        let bytes = synth::rom(&spec);
        let u = Rom::parse(&bytes).unwrap().arm9().unwrap();
        let p = u.params.expect("parameters through the markers");
        assert_eq!(p.offset, synth::PARAMS_AT);
        assert_eq!(u.autoloads.len(), 2, "{layout:?}");
    }
}

#[test]
fn the_arm7_static_is_found_through_the_literal_pool() {
    let spec = Spec::default();
    let bytes = synth::rom(&spec);
    let rom = Rom::parse(&bytes).unwrap();
    let u = rom.arm7().unwrap();
    let p = u.params.expect("parameters through the literal pool");
    assert_eq!(p.source, relift_nds_formats::params::Source::LiteralPool);
    assert_eq!(p.offset, synth::PARAMS_AT);
    assert_eq!(u.autoloads.len(), 1);
    assert_eq!(
        u.autoload_data(&u.autoloads[0]),
        spec.arm7.autoloads[0].data
    );
    assert_eq!(u.cpu, Cpu::Arm7);
}

#[test]
fn the_arm7_static_with_markers_and_compression() {
    let spec = Spec {
        arm7: StaticSpec {
            ram: 0x0238_0000,
            entry: 0x0238_0400,
            layout: Layout::Markers,
            compress: true,
            ..StaticSpec::default()
        },
        ..Spec::default()
    };
    let bytes = synth::rom(&spec);
    let u = Rom::parse(&bytes).unwrap().arm7().unwrap();
    assert!(u.compressed);
    assert_eq!(u.image, synth::static_image(&spec.arm7).0);
}

#[test]
fn a_binary_without_parameters_loads_whole() {
    let mut spec = Spec::default();
    spec.arm9.layout = Layout::LiteralPool;
    let mut bytes = synth::rom(&spec);
    let at = 0x4000 + synth::POOL_AT;
    bytes[at..at + 4].fill(0);
    let u = Rom::parse(&bytes).unwrap().arm9().unwrap();
    assert!(u.params.is_none());
    assert!(u.autoloads.is_empty());
    assert_eq!(u.image, u.raw);
}

#[test]
fn overlays_are_listed_and_decompressed() {
    let spec = full();
    let bytes = synth::rom(&spec);
    let rom = Rom::parse(&bytes).unwrap();
    assert_eq!(rom.overlays9.len(), 3);
    assert_eq!(rom.overlays7.len(), 1);
    for (o, want) in rom.overlays9.iter().zip(&spec.overlays9) {
        assert_eq!((o.id, o.ram, o.bss), (want.id, want.ram, want.bss));
        assert_eq!(o.size as usize, want.data.len());
        assert_eq!(o.compressed(), want.compress);
        assert_eq!(o.authenticated(), want.authenticated);
        assert_eq!(rom.overlay_data(o).unwrap(), want.data);
    }
    assert_eq!(rom.overlays9[1].file_id, 1);
    assert_eq!(rom.overlays7[0].file_id, 3);
    let o7 = &rom.overlays7[0];
    assert_eq!(rom.overlay_data(o7).unwrap(), spec.overlays7[0].data);
    let ptrs = rom.overlays9[0].static_init_pointers(&spec.overlays9[0].data);
    assert_eq!(ptrs.len(), 2);
    assert_eq!(rom.overlays(Cpu::Arm7), rom.overlays7.as_slice());
}

#[test]
fn files_are_walked_through_the_name_table() {
    let spec = full();
    let bytes = synth::rom(&spec);
    let rom = Rom::parse(&bytes).unwrap();
    let files = rom.files().unwrap();
    assert_eq!(files.len(), spec.files.len());
    let mut got: Vec<(String, Vec<u8>)> = files
        .iter()
        .map(|f| {
            (
                f.path.clone(),
                rom.file_bytes(u32::from(f.id)).unwrap().to_vec(),
            )
        })
        .collect();
    let mut want = spec.files.clone();
    got.sort();
    want.sort();
    assert_eq!(got, want);
    assert!(files.iter().all(|f| u32::from(f.id) >= 4));
}

#[test]
fn an_empty_file_system_lists_nothing() {
    let bytes = synth::rom(&Spec::default());
    let rom = Rom::parse(&bytes).unwrap();
    assert!(rom.files().unwrap().is_empty());
}

fn sample(kind: usize, n: usize) -> Vec<u8> {
    let patterned = synth::pattern(n, 1);
    let mut x = 0x1234_5678u32;
    (0..n)
        .map(|i| {
            x = x.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            match kind {
                0 => 0,
                1 => (i % 3) as u8,
                2 => b"abcab\0\0\x01"[(x >> 24) as usize % 8],
                3 => patterned[i],
                _ => {
                    if i % 97 < 60 {
                        (i / 97) as u8
                    } else {
                        (x >> 24) as u8
                    }
                }
            }
        })
        .collect()
}

#[test]
fn blz_round_trips_varied_data() {
    for kind in 0..5 {
        for n in [100, 300, 1000, 4097, 5000, 20_000] {
            let d = sample(kind, n);
            let blob = blz::compress(&d, 0).unwrap_or_else(|| panic!("kind {kind} len {n}"));
            assert_eq!(blz::decompress(&blob).unwrap(), d, "kind {kind} len {n}");
        }
    }
}

#[test]
fn blz_keeps_a_raw_prefix() {
    let d = sample(2, 3000);
    let blob = blz::compress(&d, 500).unwrap();
    assert_eq!(&blob[..500], &d[..500]);
    assert_eq!(blz::decompress(&blob).unwrap(), d);
}

#[test]
fn blz_matches_cross_the_longest_length_and_distance() {
    let mut d = vec![7u8; 100];
    d.extend((0..0x1100).map(|i| (i * 31 % 251) as u8));
    d.extend_from_slice(&d.clone()[..0x1000]);
    let blob = blz::compress(&d, 0).unwrap();
    assert_eq!(blz::decompress(&blob).unwrap(), d);
}

#[test]
fn blz_declines_data_it_cannot_shrink() {
    let mut x = 99u32;
    let d: Vec<u8> = (0..400)
        .map(|_| {
            x = x.wrapping_mul(1_103_515_245).wrapping_add(12_345);
            (x >> 16) as u8
        })
        .collect();
    assert_eq!(blz::compress(&d, 0), None);
    assert_eq!(blz::compress(&[], 0), None);
    assert_eq!(blz::compress(&[1, 2, 3], 3), None);
}

#[test]
fn blz_rejects_bad_blobs() {
    assert_eq!(blz::decompress(&[1, 2, 3]), Err(blz::Error::Truncated));
    let good = blz::compress(&sample(2, 2000), 0).unwrap();
    let n = good.len();

    let mut b = good.clone();
    b[n - 5] = 0;
    b[n - 6] = 0xFF;
    assert_eq!(blz::decompress(&b), Err(blz::Error::BadFooter));

    let mut b = good.clone();
    b[n - 1] = 0x80;
    assert_eq!(blz::decompress(&b), Err(blz::Error::BadFooter));

    let mut b = good.clone();
    b[n - 8] ^= 1;
    assert!(blz::decompress(&b).is_err());

    let mut b = good.clone();
    b[n - 4..].copy_from_slice(&i32::MAX.to_le_bytes());
    assert_eq!(blz::decompress(&b), Err(blz::Error::BadFooter));

    for cut in [1, 2, 9, n / 2] {
        let mut b = good[..n - 8 - cut].to_vec();
        b.extend_from_slice(&good[n - 8..]);
        assert!(blz::decompress(&b).is_err() || cut == 0, "cut {cut}");
    }
}

#[test]
fn blz_never_panics_on_damaged_streams() {
    let good = blz::compress(&sample(2, 1500), 0).unwrap();
    let n = good.len();
    for at in 0..n {
        for flip in [0x01u8, 0x80, 0xFF] {
            let mut b = good.clone();
            b[at] ^= flip;
            let _ = blz::decompress(&b);
        }
    }
}

#[test]
fn short_and_damaged_images_are_errors() {
    assert_eq!(Header::parse(&[0; 0x1FF]), Err(Error::TooShort(0x1FF)));
    assert!(Rom::parse(&[]).is_err());
    let good = synth::rom(&full());

    let mut b = good.clone();
    b[0x48..0x4C].copy_from_slice(&0xFFFF_FF00u32.to_le_bytes());
    assert!(matches!(Rom::parse(&b), Err(Error::Range { .. })));

    let mut b = good.clone();
    b[0x50..0x54].copy_from_slice(&(good.len() as u32 - 8).to_le_bytes());
    assert!(Rom::parse(&b).is_err());

    let mut b = good.clone();
    b[0x2C..0x30].copy_from_slice(&0x7FFF_FFFFu32.to_le_bytes());
    let rom = Rom::parse(&b).unwrap();
    assert!(matches!(rom.arm9(), Err(Error::Range { .. })));

    let mut b = good.clone();
    b[0x3C..0x40].copy_from_slice(&0x7FFF_FFFFu32.to_le_bytes());
    assert!(Rom::parse(&b).unwrap().arm7().is_err());

    assert!(Rom::parse(&good).unwrap().file_bytes(9999).is_err());
}

#[test]
fn damaged_tables_never_panic() {
    let good = synth::rom(&full());
    let rom = Rom::parse(&good).unwrap();
    let fnt = rom.header.fnt;
    let fat = rom.header.fat;
    let ovt = rom.header.ovt9;
    for (start, len) in [
        (fnt.offset, fnt.size),
        (fat.offset, fat.size),
        (ovt.offset, ovt.size),
    ] {
        for at in start..start + len {
            for flip in [0x01u8, 0x80, 0xFF] {
                let mut b = good.clone();
                b[at as usize] ^= flip;
                if let Ok(r) = Rom::parse(&b) {
                    let _ = r.files();
                    for o in r.overlays9.iter().chain(&r.overlays7) {
                        let _ = r.overlay_data(o);
                    }
                }
            }
        }
    }
}

#[test]
fn damaged_statics_never_panic() {
    let good = synth::rom(&full());
    for at in (0x4000..0x4000 + 0x260).chain(0x4400..0x4400 + 0x1A0) {
        for flip in [0x01u8, 0x80, 0xFF] {
            let mut b = good.clone();
            b[at] ^= flip;
            if let Ok(r) = Rom::parse(&b) {
                let _ = r.arm9();
                let _ = r.arm7();
            }
        }
    }
}

#[test]
fn a_looping_directory_tree_is_an_error() {
    let good = synth::rom(&full());
    let rom = Rom::parse(&good).unwrap();
    let fnt = rom.header.fnt.offset as usize;
    let mut b = good.clone();
    let dirs = u16::from_le_bytes([b[fnt + 6], b[fnt + 7]]) as usize;
    assert!(dirs > 1);
    let sub0 = u32::from_le_bytes([b[fnt], b[fnt + 1], b[fnt + 2], b[fnt + 3]]) as usize;
    let mut at = fnt + sub0;
    while b[at] != 0 && b[at] & 0x80 == 0 {
        at += 1 + usize::from(b[at] & 0x7F);
    }
    assert_ne!(b[at], 0);
    let id_at = at + 1 + usize::from(b[at] & 0x7F);
    b[id_at..id_at + 2].copy_from_slice(&0xF000u16.to_le_bytes());
    assert!(Rom::parse(&b).unwrap().files().is_err());
}

#[test]
fn unit_hashes_are_stable_across_builds() {
    let a = synth::rom(&full());
    let b = synth::rom(&full());
    assert_eq!(sha256(&a), sha256(&b));
}

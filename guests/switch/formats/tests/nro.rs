//! NRO: header, segments, MOD0, symbols, assets, and negative controls.

use relift_switch_formats::Error;
use relift_switch_formats::nro::{self, NroSpec};
use relift_switch_formats::romfs;

fn spec() -> NroSpec {
    NroSpec {
        text: vec![0xAA; 0x300],
        rodata: b"hello rodata".to_vec(),
        data: vec![0xDD; 0x80],
        bss: 0x2000,
        build_id: [0x5C; 32],
        symbols: vec![("alpha".into(), 0x1000, 8), ("beta".into(), 0x1010, 16)],
        icon: Some(vec![0xFF, 0xD8, 1, 2, 3]),
        nacp: Some(vec![7u8; 0x4000]),
        romfs: Some(romfs::build(&[("a.txt", b"one"), ("d/b.bin", &[2u8; 40])])),
    }
}

#[test]
fn reads_header_segments_symbols_and_mod0() {
    let n = nro::parse(nro::build(&spec())).expect("opens");
    assert_eq!(n.build_id, [0x5C; 32]);
    assert_eq!(n.bss_size, 0x2000);
    assert_eq!(n.segments[0].memory_offset, 0);
    assert_eq!(n.segment_bytes(2).expect("data")[..0x80], [0xDD; 0x80]);
    assert!(n.segment_bytes(1).expect("ro").ends_with(&[0]));
    assert!(n.segment_bytes(3).is_err());
    assert_eq!(n.flat_image().len(), n.image.len() + 0x2000);
    let names: Vec<_> = n
        .symbols()
        .expect("syms")
        .into_iter()
        .map(|s| s.name)
        .collect();
    assert_eq!(names, ["alpha", "beta"]);
    let m = n.mod0().expect("mod0").expect("present");
    assert_eq!(m.offset, 0x80);
    assert!(m.bss_end - m.bss_start == 0x2000);
    assert_eq!(n.build_id_hex(), "5c".repeat(32));
}

#[test]
fn reads_the_assets() {
    let n = nro::parse(nro::build(&spec())).expect("opens");
    assert_eq!(n.icon().expect("icon"), Some(vec![0xFF, 0xD8, 1, 2, 3]));
    assert_eq!(n.nacp().expect("nacp"), Some(vec![7u8; 0x4000]));
    let r = n.romfs().expect("romfs").expect("present");
    let mut v = Vec::new();
    let f = r
        .files()
        .iter()
        .find(|f| f.path == "d/b.bin")
        .expect("file");
    r.copy_file(f, &mut v).expect("copy");
    assert_eq!(v, vec![2u8; 40]);
}

#[test]
fn an_nro_without_assets_has_none() {
    let mut s = spec();
    (s.icon, s.nacp, s.romfs) = (None, None, None);
    let n = nro::parse(nro::build(&s)).expect("opens");
    assert!(n.assets.is_none());
    assert!(n.icon().expect("icon").is_none());
    assert!(n.romfs().expect("romfs").is_none());
}

#[test]
fn negative_controls() {
    let good = nro::build(&spec());
    let bad = |f: &dyn Fn(&mut Vec<u8>)| {
        let mut b = good.clone();
        f(&mut b);
        nro::parse(b)
    };
    assert!(matches!(bad(&|b| b[0x10] = b'X'), Err(Error::Format(_))));
    assert!(matches!(bad(&|b| b.truncate(0x40)), Err(Error::Format(_))));
    assert!(matches!(bad(&|b| b[0x1C] |= 2), Err(Error::Unsupported(_))));
    // Size past the file.
    assert!(matches!(
        bad(&|b| b[0x18..0x1C].copy_from_slice(&u32::MAX.to_le_bytes())),
        Err(Error::Format(_))
    ));
    // Data segment past the module size.
    assert!(matches!(
        bad(&|b| b[0x34..0x38].copy_from_slice(&0x100_0000u32.to_le_bytes())),
        Err(Error::Format(_))
    ));
    // An asset section running past the file.
    let at = u32::from_le_bytes(good[0x18..0x1C].try_into().expect("4")) as usize;
    assert!(matches!(
        bad(&|b| b[at + 0x28 + 8..at + 0x28 + 16].copy_from_slice(&u64::MAX.to_le_bytes())),
        Err(Error::Format(_))
    ));
    // A dynsym extent past the image fails when the symbols are read.
    let n = bad(&|b| b[0x78..0x7C].copy_from_slice(&0x7FFF_0000u32.to_le_bytes()))
        .expect("header opens");
    assert!(n.symbols().is_err());
}

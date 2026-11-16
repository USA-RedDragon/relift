//! The cartridge reader over synthetic input.

use relift_n64_formats::checksum::{self, crc32};
use relift_n64_formats::compress::{self, Format};
use relift_n64_formats::header::Header;
use relift_n64_formats::synth::{self, Spec, encode, filler};
use relift_n64_formats::{ByteOrder, Cic, CrcCheck, Error, Rom};

#[test]
fn crc32_matches_the_published_check_value() {
    assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    assert_eq!(crc32(b""), 0);
}

#[test]
fn byte_orders_are_told_apart_and_round_trip() {
    let be = synth::rom(&Spec::default());
    for order in [ByteOrder::Z64, ByteOrder::V64, ByteOrder::N64] {
        let stored = order.encode(&be).unwrap();
        assert_eq!(ByteOrder::detect(&stored).unwrap(), order);
        assert_eq!(order.normalize(&stored).unwrap(), be);
        assert_eq!(Rom::new(&stored).unwrap().bytes, be);
    }
    let v64 = ByteOrder::V64.encode(&be).unwrap();
    assert_eq!(&v64[..4], &[0x37, 0x80, 0x40, 0x12]);
    let n64 = ByteOrder::N64.encode(&be).unwrap();
    assert_eq!(&n64[..4], &[0x40, 0x12, 0x37, 0x80]);
}

#[test]
fn bad_orders_and_lengths_are_errors() {
    assert_eq!(ByteOrder::detect(&[1, 2, 3]), Err(Error::TooShort(3)));
    assert_eq!(
        ByteOrder::detect(&[1, 2, 3, 4]),
        Err(Error::UnknownOrder(0x0102_0304))
    );
    assert_eq!(ByteOrder::N64.normalize(&[0; 6]), Err(Error::Misaligned(6)));
    assert_eq!(ByteOrder::V64.normalize(&[0; 3]), Err(Error::Misaligned(3)));
    assert!(matches!(
        Rom::new(&[0x80, 0x37, 0x12, 0x40]),
        Err(Error::TooShort(4))
    ));
}

#[test]
fn header_fields_are_read_from_their_offsets() {
    let be = synth::rom(&Spec {
        name: "ABC".to_owned(),
        game_id: *b"QR",
        region: b'P',
        version: 3,
        entry: 0x8010_0400,
        ..Spec::default()
    });
    let h = Header::parse(&be).unwrap();
    assert_eq!(h.pi_config, 0x8037_1240);
    assert_eq!(h.entry, 0x8010_0400);
    assert_eq!(h.name_text(), "ABC");
    assert_eq!(h.media, b'N');
    assert_eq!(h.game_id, *b"QR");
    assert_eq!((h.region, h.version), (b'P', 3));
    assert_eq!(Header::parse(&be[..0x3F]), Err(Error::TooShort(0x3F)));
}

#[test]
fn variants_have_distinct_hashes_and_round_trip() {
    for c in Cic::ALL {
        assert_eq!(Cic::from_hash(c.hash()), Some(c));
        for n in c.name().split('/') {
            assert_eq!(Cic::parse(n), Some(c));
        }
    }
    assert_eq!(Cic::from_hash(0), None);
    assert_eq!(Cic::parse("1234"), None);
    assert_eq!(Cic::detect(&synth::rom(&Spec::default())), None);
    assert_eq!(Cic::detect(&[0; 0x100]), None);
}

#[test]
fn entry_points_are_adjusted_per_variant() {
    assert_eq!(Cic::C6102.entry(0x8000_1000), 0x8000_1000);
    assert_eq!(Cic::C6103.entry(0x8010_1000), 0x8000_1000);
    assert_eq!(Cic::C6106.entry(0x8020_1000), 0x8000_1000);
}

fn reference_crc(rom: &[u8], seed: u32, kind: &str) -> (u32, u32) {
    let w = |i: usize| u32::from_be_bytes(rom[i..i + 4].try_into().unwrap());
    let mut t = [seed; 6];
    let mut carries = 0u32;
    let mut sum = u64::from(seed);
    for i in (0x1000..0x10_1000).step_by(4) {
        let d = w(i);
        sum += u64::from(d);
        if sum > u64::from(u32::MAX) {
            carries += 1;
            sum &= 0xFFFF_FFFF;
        }
        t[2] ^= d;
        let r = d.rotate_left(d % 32);
        t[4] = t[4].wrapping_add(r);
        let t6 = sum as u32;
        t[1] = if t[1] > d { t[1] ^ r } else { t[1] ^ t6 ^ d };
        t[0] = t[0].wrapping_add(t[4] ^ d);
    }
    let (t1, t2, t3, t5, t6) = (t[0], t[1], t[2], t[4], sum as u32);
    let t4 = seed.wrapping_add(carries);
    match kind {
        "add" => ((t6 ^ t4).wrapping_add(t3), (t5 ^ t2).wrapping_add(t1)),
        "mul" => (
            t6.wrapping_mul(t4).wrapping_add(t3),
            t5.wrapping_mul(t2).wrapping_add(t1),
        ),
        _ => (t6 ^ t4 ^ t3, t5 ^ t2 ^ t1),
    }
}

#[test]
fn checksum_matches_an_independent_implementation() {
    let be = synth::rom(&Spec::default());
    for (cic, kind) in [
        (Cic::C6101, "xor"),
        (Cic::C6102, "xor"),
        (Cic::C6103, "add"),
        (Cic::C6106, "mul"),
    ] {
        let want = reference_crc(&be, cic.seed().unwrap(), kind);
        assert_eq!(checksum::compute(&be, cic), Some(want), "{cic:?}");
    }
    assert_eq!(checksum::compute(&be, Cic::C6105), None);
    assert_eq!(checksum::compute(&be[..0x1000], Cic::C6102), None);
}

#[test]
fn check_crc_reports_good_bad_and_unavailable() {
    let be = synth::rom(&Spec {
        crc_as: Some(Cic::C6103),
        ..Spec::default()
    });
    let rom = Rom::new(&be).unwrap();
    assert!(matches!(rom.check_crc(None), CrcCheck::Unavailable(_)));
    assert_eq!(rom.check_crc(Some(Cic::C6103)), CrcCheck::Good);
    assert!(matches!(rom.check_crc(Some(Cic::C6102)), CrcCheck::Bad(_)));
    assert!(matches!(
        rom.check_crc(Some(Cic::C6105)),
        CrcCheck::Unavailable(_)
    ));
    let mut changed = be.clone();
    changed[0x5000] ^= 1;
    let rom = Rom::new(&changed).unwrap();
    assert!(matches!(rom.check_crc(Some(Cic::C6103)), CrcCheck::Bad(_)));
    let short = Rom::new(&be[..0x2000]).unwrap();
    assert!(matches!(
        short.check_crc(Some(Cic::C6103)),
        CrcCheck::Unavailable(_)
    ));
}

fn samples() -> Vec<Vec<u8>> {
    let mut repeat = Vec::new();
    for _ in 0..40 {
        repeat.extend_from_slice(b"abcabcabcabd");
    }
    vec![
        Vec::new(),
        vec![7],
        b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_vec(),
        repeat,
        filler(9, 3000),
        [filler(3, 500), vec![0; 700], filler(3, 500)].concat(),
    ]
}

#[test]
fn every_container_round_trips_including_overlapping_references() {
    for f in [Format::Yay0, Format::Yaz0, Format::Mio0] {
        for data in samples() {
            let stream = encode(f, &data);
            assert_eq!(Format::of(&stream), Some(f));
            assert_eq!(f.decode(&stream).unwrap(), data, "{f:?} {}", data.len());
        }
    }
    let one = encode(Format::Yaz0, &[5; 40]);
    assert_eq!(compress::decode_yaz0(&one).unwrap(), vec![5; 40]);
    assert!(one.len() < 30, "the run is one overlapping reference");
}

#[test]
fn truncated_streams_are_errors_at_every_length() {
    for f in [Format::Yay0, Format::Yaz0, Format::Mio0] {
        let data = [filler(5, 400), vec![1; 300]].concat();
        let stream = encode(f, &data);
        for cut in 0..stream.len() {
            assert!(f.decode(&stream[..cut]).is_err(), "{f:?} cut at {cut}");
        }
    }
}

#[test]
fn malformed_streams_are_errors() {
    let mut s = encode(Format::Yaz0, b"hello hello hello");
    s[0] = b'X';
    assert_eq!(compress::decode_yaz0(&s), Err(Error::Invalid("wrong tag")));
    let mut head = b"Yaz0".to_vec();
    head.extend_from_slice(&10u32.to_be_bytes());
    head.extend_from_slice(&[0; 8]);
    head.extend_from_slice(&[0x00, 0x00, 0x00, 0x00]);
    assert_eq!(
        compress::decode_yaz0(&head),
        Err(Error::Invalid("back-reference before the start"))
    );
    let mut big = b"Yaz0".to_vec();
    big.extend_from_slice(&u32::MAX.to_be_bytes());
    big.extend_from_slice(&[0; 8]);
    assert!(matches!(
        compress::decode_yaz0(&big),
        Err(Error::Invalid(_))
    ));
    let mut y = encode(Format::Yay0, b"abcabcabcabcabc");
    y[8..12].copy_from_slice(&4u32.to_be_bytes());
    assert!(compress::decode_yay0(&y).is_err());
    assert_eq!(compress::decode_mio0(&[0; 4]), Err(Error::TooShort(4)));
}

#[test]
fn scan_finds_every_valid_stream_and_skips_a_bare_tag() {
    let mut image = filler(11, 0x3000);
    let a = [filler(1, 200), vec![0; 300]].concat();
    let b = vec![9u8; 500];
    let c = filler(2, 90);
    let mut at = Vec::new();
    for (off, f, d) in [
        (0x100, Format::Yaz0, &a),
        (0x801, Format::Yay0, &b),
        (0x1400, Format::Mio0, &c),
    ] {
        let s = encode(f, d);
        image[off..off + s.len()].copy_from_slice(&s);
        at.push((off, f, d.len()));
    }
    image[0x2000..0x2004].copy_from_slice(b"Yaz0");
    let found: Vec<_> = Rom::new(&synth::rom(&Spec::default())).unwrap().streams();
    assert!(found.is_empty());
    let got: Vec<_> = relift_n64_formats::scan::scan(&image)
        .into_iter()
        .map(|s| (s.offset, s.kind, s.decoded_size))
        .collect();
    assert_eq!(got, at);
}

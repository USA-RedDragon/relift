//! The ZBIC and Zstandard decoder: interpolative table descriptions made by
//! the reference encoder, a ZBIC frame of synthetic data and plain zstd
//! frames of the `zstd` crate through the same
//! block decoder.

use std::io::Write;

use relift_switch_formats::zbic::{Flavor, ZBIC_MAGIC, decompress, read_counts_interpolative};

const MIXED: &[u8] = include_bytes!("zbic/mixed.zbic");
const MIXED_SIZE: usize = 140_000;

fn mixed(n: usize, mut seed: u32) -> Vec<u8> {
    let mut next = move || {
        seed = seed.wrapping_mul(1_103_515_245).wrapping_add(12_345);
        seed >> 8
    };
    let mut out = b"relift".to_vec();
    while out.len() < n {
        if next() % 10 < 7 {
            let len = 3 + (next() % 38) as usize;
            let reach = [8, 100, 5000, 100_000][(next() % 4) as usize].min(out.len());
            let from = out.len() - 1 - (next() as usize % reach);
            for i in 0..len {
                let b = out[from + i];
                out.push(b);
            }
        } else {
            for _ in 0..=next() % 6 {
                let i = (next() % 36).min(next() % 36) as usize;
                out.push(b"etaoinshrdlucmfwypvbgkjqxz0123456789"[i]);
            }
        }
    }
    out.truncate(n);
    out
}

fn hex(s: &str) -> Vec<u8> {
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
        .collect()
}

#[test]
fn interpolative_descriptions_of_the_reference_encoder() {
    let cases: &[(&str, u32, &[i16])] = &[
        ("03607e06", 5, &[31, 1]),
        ("0461629f01", 5, &[30, 1, 1]),
        ("06674ac9778609", 5, &[24, 1, 1, 1, 1, 1, 1, 1, 1]),
        ("0360de04", 5, &[23, 9]),
        ("07ffdcb597240cd2", 6, &[41, 4, 2, 4, 0, 1, 3, 3, 3, 3]),
        ("04fc3e3b01", 8, &[193, 63]),
        (
            "0ce5f64b36dbfc145d66771529",
            9,
            &[273, 24, 37, 24, 24, 26, 27, 25, 23, 29],
        ),
        (
            "115e8c7422b24f16a2608dffd4636d8f0602",
            12,
            &[2247, 203, 186, 228, 208, 197, 204, 203, 227, 193],
        ),
        ("85a6fc903130", 5, &[19, -1, 4, 3, -1, 1, 2, -1]),
        ("84569ba944", 6, &[42, 8, -1, 13]),
        ("87ff5f4205a75eae", 8, &[153, 31, 21, 19, 32]),
        ("86317fb1aed102", 9, &[355, 80, 77]),
        (
            "8c9694a1a4f0efd22925594d0e",
            12,
            &[2356, 293, 287, 323, -1, 288, 267, 281],
        ),
        (
            "958669dc82e3b3e9dc57732caf77f68a306cc15dd602",
            7,
            &[
                60, 0, 2, -1, 1, 2, 1, 2, 2, 2, 1, 2, 2, 2, 1, 2, 3, -1, 3, 2, -1, 4, 1, 0, 2, 3,
                5, -1, 2, -1, -1, 0, 0, 5, 3, -1, 3, 3,
            ],
        ),
        (
            "1297973b828c1974d24a4f92fab1a22fd7d003",
            7,
            &[
                71, 1, 2, 1, 0, 3, 2, 2, 0, 1, 2, 2, 3, 0, 2, 3, 3, 0, 1, 0, 3, 1, 1, 3, 2, 1, 1,
                2, 2, 2, 2, 4, 0, 0, 1, 1, 0, 0, 3,
            ],
        ),
    ];
    for &(h, log, counts) in cases {
        let mut b = hex(h);
        let n = b.len();
        b.push(0x5A);
        let (l, c, used) = read_counts_interpolative(&b, 255).expect(h);
        assert_eq!((l, c.as_slice(), used), (log, counts, n), "{h}");
        assert!(read_counts_interpolative(&b, counts.len() - 2).is_err());
        assert!(read_counts_interpolative(&b[..n], 255).is_err());
    }
}

#[test]
fn zbic_frame_of_synthetic_data() {
    let want = mixed(MIXED_SIZE, 1);
    assert_eq!(MIXED[..4], ZBIC_MAGIC);
    assert_eq!(decompress(Flavor::Zbic, MIXED, MIXED_SIZE).unwrap(), want);
    assert!(decompress(Flavor::Zstd, MIXED, MIXED_SIZE).is_err());
    assert!(decompress(Flavor::Zbic, MIXED, MIXED_SIZE - 1).is_err());
    assert!(decompress(Flavor::Zbic, &MIXED[..MIXED.len() - 1], MIXED_SIZE).is_err());
    let mut longer = MIXED.to_vec();
    longer.push(0);
    assert!(decompress(Flavor::Zbic, &longer, MIXED_SIZE).is_err());
    let mut seed = 0x9E37_79B9u32;
    for _ in 0..400 {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        let mut bad = MIXED.to_vec();
        let at = seed as usize % bad.len();
        bad[at] ^= 1 << (seed >> 29);
        if let Ok(out) = decompress(Flavor::Zbic, &bad, MIXED_SIZE) {
            assert_eq!(out.len(), MIXED_SIZE);
        }
    }
}

fn zstd_frame(data: &[u8], level: i32, checksum: bool) -> Vec<u8> {
    let mut e = zstd::stream::Encoder::new(Vec::new(), level).unwrap();
    e.include_checksum(checksum).unwrap();
    e.include_contentsize(true).unwrap();
    e.set_pledged_src_size(Some(data.len() as u64)).unwrap();
    e.write_all(data).unwrap();
    e.finish().unwrap()
}

#[test]
fn zstd_frames_through_the_same_decoder() {
    let skewed: Vec<u8> = mixed(300_000, 9)
        .iter()
        .map(|&b| b.wrapping_mul(3))
        .collect();
    let noise: Vec<u8> = {
        let mut s = 0x2545_F491u32;
        (0..200_000)
            .map(|_| {
                s ^= s << 13;
                s ^= s >> 17;
                s ^= s << 5;
                (s >> 24) as u8
            })
            .collect()
    };
    let inputs = [
        Vec::new(),
        b"a".to_vec(),
        vec![9u8; 400_000],
        mixed(1000, 3),
        mixed(600_000, 4),
        skewed,
        noise,
    ];
    for d in &inputs {
        for level in [1, 3, 9, 19] {
            for checksum in [false, true] {
                let f = zstd_frame(d, level, checksum);
                let out = decompress(Flavor::Zstd, &f, d.len()).unwrap_or_else(|e| {
                    panic!("{} bytes at level {level}: {e}", d.len());
                });
                assert!(out == *d, "{} bytes at level {level}", d.len());
                if checksum && !d.is_empty() {
                    let mut bad = f.clone();
                    let n = bad.len();
                    bad[n - 1] ^= 1;
                    assert!(decompress(Flavor::Zstd, &bad, d.len()).is_err());
                }
            }
        }
    }
}

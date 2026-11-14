use super::*;

fn varint(out: &mut Vec<u8>, mut v: u64) {
    loop {
        let x = (v & 0x7f) as u8;
        v >>= 7;
        if v == 0 {
            out.push(0x80 | x);
            return;
        }
        out.push(x);
        v -= 1;
    }
}

fn seal(mut p: Vec<u8>, input: &[u8], output: &[u8]) -> Vec<u8> {
    p.extend_from_slice(&crc32(input).to_le_bytes());
    p.extend_from_slice(&crc32(output).to_le_bytes());
    let c = crc32(&p);
    p.extend_from_slice(&c.to_le_bytes());
    p
}

/// A UPS patch from `a` to `b`, one hunk per run of differing bytes.
fn ups_of(a: &[u8], b: &[u8]) -> Vec<u8> {
    let mut p = b"UPS1".to_vec();
    varint(&mut p, a.len() as u64);
    varint(&mut p, b.len() as u64);
    let mut at = 0u64;
    for r in diff(a, b) {
        varint(&mut p, r.start - at);
        for i in r.clone() {
            let i = i as usize;
            p.push(a.get(i).copied().unwrap_or(0) ^ b[i]);
        }
        p.push(0);
        at = r.end + 1;
    }
    seal(p, a, b)
}

#[test]
fn crc32_matches_the_standard_check_value() {
    assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
}

#[test]
fn ips_writes_records_runs_and_truncates() {
    let input = vec![0u8; 16];
    let mut p = b"PATCH".to_vec();
    p.extend_from_slice(&[0, 0, 2, 0, 2, 0xaa, 0xbb]);
    p.extend_from_slice(&[0, 0, 8, 0, 0, 0, 3, 0x11]);
    p.extend_from_slice(b"EOF");
    let r = apply(&input, &p).expect("applies");
    assert_eq!(r.kind, Kind::Ips);
    assert_eq!(
        &r.data[..12],
        &[0, 0, 0xaa, 0xbb, 0, 0, 0, 0, 0x11, 0x11, 0x11, 0]
    );
    assert_eq!(r.written, vec![2..4, 8..11]);
    p.extend_from_slice(&[0, 0, 10]);
    assert_eq!(apply(&input, &p).expect("applies").data.len(), 10);
}

#[test]
fn ips_grows_the_file_past_its_end() {
    let mut p = b"PATCH".to_vec();
    p.extend_from_slice(&[0, 0, 4, 0, 2, 1, 2]);
    p.extend_from_slice(b"EOF");
    assert_eq!(
        apply(&[9, 9], &p).expect("applies").data,
        [9, 9, 0, 0, 1, 2]
    );
}

#[test]
fn ips32_takes_32_bit_offsets() {
    let input = vec![0u8; 0x0100_0010];
    let mut p = b"IPS32".to_vec();
    p.extend_from_slice(&[0x01, 0, 0, 4, 0, 1, 0x5a]);
    p.extend_from_slice(b"EEOF");
    let r = apply(&input, &p).expect("applies");
    assert_eq!(r.kind, Kind::Ips32);
    assert_eq!(r.data[0x0100_0004], 0x5a);
    assert_eq!(r.written, vec![0x0100_0004..0x0100_0005]);
}

#[test]
fn a_truncated_ips_is_an_error() {
    let mut p = b"PATCH".to_vec();
    p.extend_from_slice(&[0, 0, 2, 0, 9, 1]);
    assert_eq!(apply(&[0; 4], &p), Err(Error::Truncated));
    assert_eq!(apply(&[0; 4], b"HELLO"), Err(Error::UnknownFormat));
}

#[test]
fn ups_round_trips_and_reports_a_wrong_base() {
    let a: Vec<u8> = (0..64).collect();
    let mut b = a.clone();
    b[3] = 0xff;
    b[40..44].copy_from_slice(&[1, 2, 3, 4]);
    b.extend_from_slice(&[7, 7]);
    let p = ups_of(&a, &b);
    let r = apply(&a, &p).expect("applies");
    assert_eq!(r.data, b);
    assert!(!r.base_mismatch);
    assert_eq!(r.declared_base, Some(crc32(&a)));
    assert_eq!(r.written, vec![3..4, 40..44, 64..66]);
    let mut other = a.clone();
    other[60] = 0x42;
    let err = apply(&other, &p);
    assert_eq!(err, Err(Error::OutputChecksum));
    let mut damaged = p.clone();
    damaged[6] ^= 1;
    assert_eq!(apply(&a, &damaged), Err(Error::PatchChecksum));
}

#[test]
fn ups_on_a_wrong_base_that_still_produces_the_output_is_flagged() {
    let a = vec![0u8; 8];
    let b = vec![1u8; 8];
    let p = ups_of(&a, &b);
    let mut base = a.clone();
    base[0] = 9;
    let mut p2 = p.clone();
    let n = p2.len();
    let wrong = crc32(&[1, 2, 3]).to_le_bytes();
    p2[n - 12..n - 8].copy_from_slice(&wrong);
    let c = crc32(&p2[..n - 4]).to_le_bytes();
    p2[n - 4..].copy_from_slice(&c);
    let r = apply(&a, &p2).expect("applies");
    assert!(r.base_mismatch);
}

#[test]
fn bps_runs_every_action() {
    let src: Vec<u8> = b"abcdefghij".to_vec();
    let want: Vec<u8> = b"abcXYghijXYXYXY".to_vec();
    let mut p = b"BPS1".to_vec();
    varint(&mut p, src.len() as u64);
    varint(&mut p, want.len() as u64);
    varint(&mut p, 0);
    varint(&mut p, 2 << 2);
    varint(&mut p, (1 << 2) | 1);
    p.extend_from_slice(b"XY");
    varint(&mut p, (3 << 2) | 2);
    varint(&mut p, 6 << 1);
    varint(&mut p, (1 << 2) | 3);
    varint(&mut p, 3 << 1);
    varint(&mut p, (3 << 2) | 3);
    varint(&mut p, 4 << 1);
    let p = seal(p, &src, &want);
    let r = apply(&src, &p).expect("applies");
    assert_eq!(r.data, want);
    assert_eq!(r.kind, Kind::Bps);
    assert!(!r.base_mismatch);
    assert_eq!(r.written, vec![3..15]);
}

#[test]
fn bps_reading_outside_the_source_is_an_error() {
    let mut p = b"BPS1".to_vec();
    varint(&mut p, 2);
    varint(&mut p, 4);
    varint(&mut p, 0);
    varint(&mut p, (3 << 2) | 2);
    varint(&mut p, 0);
    let p = seal(p, &[1, 2], &[1, 2, 3, 4]);
    assert_eq!(apply(&[1, 2], &p), Err(Error::OutOfRange));
}

#[test]
fn ranges_merge_and_diff() {
    assert_eq!(merge(vec![5..8, 0..2, 1..3, 8..9]), vec![0..3, 5..9]);
    assert_eq!(diff(&[1, 2, 3], &[1, 0, 3, 4]), vec![1..2, 3..4]);
}

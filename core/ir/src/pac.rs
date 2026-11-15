//! The meaning of [`crate::Op::Pac`]: QARMA5 (QARMA-64 with five rounds,
//! S-box σ2), the cipher of the Arm architecture's `ComputePACQARMA`, from
//! the round structure the QARMA paper and the Arm pseudocode define.

const SUB: [u8; 16] = [
    0xb, 0x6, 0x8, 0xf, 0xc, 0x0, 0x9, 0xe, 0x3, 0x7, 0x4, 0x5, 0xd, 0x2, 0x1, 0xa,
];
const INV_SUB: [u8; 16] = [
    0x5, 0xe, 0xd, 0x8, 0xa, 0xb, 0x1, 0x9, 0x2, 0x6, 0xf, 0x0, 0x4, 0xc, 0x7, 0x3,
];
/// Cell `i` of the shuffled state is cell `SHUFFLE[i]` of the input.
const SHUFFLE: [u8; 16] = [13, 6, 11, 0, 7, 12, 1, 10, 8, 3, 14, 5, 2, 9, 4, 15];
const INV_SHUFFLE: [u8; 16] = [3, 6, 12, 9, 14, 11, 1, 4, 8, 13, 7, 2, 5, 0, 10, 15];
/// The tweak update: cell `i` is cell `TWEAK[i]` of the input, passed
/// through the cell LFSR where `TWEAK_ROT[i]` is set.
const TWEAK: [u8; 16] = [4, 5, 6, 7, 11, 2, 3, 8, 12, 13, 14, 15, 0, 1, 10, 9];
const TWEAK_ROT: [bool; 16] = [
    false, false, true, false, true, false, false, true, false, false, false, true, true, false,
    true, true,
];
const INV_TWEAK: [u8; 16] = [12, 13, 5, 6, 0, 1, 2, 3, 7, 15, 14, 4, 8, 9, 10, 11];
const INV_TWEAK_ROT: [bool; 16] = [
    true, false, false, false, false, false, true, false, true, true, true, true, false, false,
    false, true,
];
const RC: [u64; 5] = [
    0,
    0x1319_8a2e_0370_7344,
    0xa409_3822_299f_31d0,
    0x082e_fa98_ec4e_6c89,
    0x4528_21e6_38d0_1377,
];
const ALPHA: u64 = 0xc0ac_29b7_c97c_50dd;

fn cell(x: u64, i: usize) -> u64 {
    (x >> (4 * i)) & 0xf
}

fn sub(x: u64, t: &[u8; 16]) -> u64 {
    (0..16).fold(0, |o, i| o | u64::from(t[cell(x, i) as usize]) << (4 * i))
}

fn shuffle(x: u64, p: &[u8; 16]) -> u64 {
    (0..16).fold(0, |o, i| o | cell(x, usize::from(p[i])) << (4 * i))
}

fn rot_cell(c: u64, n: u32) -> u64 {
    ((c << n) | (c >> (4 - n))) & 0xf
}

fn mult(s: u64) -> u64 {
    let mut o = 0;
    for i in 0..4 {
        let (c0, c4, c8, c12) = (cell(s, i), cell(s, i + 4), cell(s, i + 8), cell(s, i + 12));
        let t0 = rot_cell(c8, 1) ^ rot_cell(c4, 2) ^ rot_cell(c0, 1);
        let t1 = rot_cell(c12, 1) ^ rot_cell(c4, 1) ^ rot_cell(c0, 2);
        let t2 = rot_cell(c12, 2) ^ rot_cell(c8, 1) ^ rot_cell(c0, 1);
        let t3 = rot_cell(c12, 1) ^ rot_cell(c8, 2) ^ rot_cell(c4, 1);
        o |= t3 << (4 * i) | t2 << (4 * (i + 4)) | t1 << (4 * (i + 8)) | t0 << (4 * (i + 12));
    }
    o
}

fn tweak_rot(c: u64) -> u64 {
    ((c ^ (c >> 1)) & 1) << 3 | ((c >> 3) & 1) << 2 | ((c >> 2) & 1) << 1 | ((c >> 1) & 1)
}

fn tweak_inv_rot(c: u64) -> u64 {
    ((c >> 2) & 1) << 3 | ((c >> 1) & 1) << 2 | (c & 1) << 1 | ((c ^ (c >> 3)) & 1)
}

fn tweak_shuffle(x: u64, p: &[u8; 16], r: &[bool; 16], f: fn(u64) -> u64) -> u64 {
    (0..16).fold(0, |o, i| {
        let c = cell(x, usize::from(p[i]));
        let c = if r[i] { f(c) } else { c };
        o | c << (4 * i)
    })
}

/// `ComputePAC(data, modifier, key0, key1)` with QARMA5: `key0` is the high
/// half of the 128-bit key register pair (`APxxKeyHi_EL1`), `key1` the low.
pub fn qarma5(data: u64, modifier: u64, key0: u64, key1: u64) -> u64 {
    const N: usize = 4;
    let modk0 = (key0 & 1) << 63 | (key0 >> 2) << 1 | ((key0 >> 63) ^ (key0 >> 1)) & 1;
    let mut tw = modifier;
    let mut w = data ^ key0;
    for (i, rc) in RC.iter().enumerate() {
        w ^= key1 ^ tw ^ rc;
        if i > 0 {
            w = mult(shuffle(w, &SHUFFLE));
        }
        w = sub(w, &SUB);
        tw = tweak_shuffle(tw, &TWEAK, &TWEAK_ROT, tweak_rot);
    }
    w ^= modk0 ^ tw;
    w = sub(mult(shuffle(w, &SHUFFLE)), &SUB);
    w = mult(shuffle(w, &SHUFFLE)) ^ key1;
    w = sub(shuffle(w, &INV_SHUFFLE), &INV_SUB);
    w = shuffle(mult(w), &INV_SHUFFLE) ^ key0 ^ tw;
    for i in 0..=N {
        w = sub(w, &INV_SUB);
        if i < N {
            w = shuffle(mult(w), &INV_SHUFFLE);
        }
        tw = tweak_shuffle(tw, &INV_TWEAK, &INV_TWEAK_ROT, tweak_inv_rot);
        w ^= RC[N - i] ^ key1 ^ tw ^ ALPHA;
    }
    w ^ modk0
}

#[cfg(test)]
mod tests {
    use super::qarma5;

    /// QARMA64_5 with σ2, Appendix A.1 of Avanzi, "The QARMA Block Cipher
    /// Family", IACR ToSC 2017(1): P, T, w0, k0 to C.
    /// The values runtime/cpus/arm64/tests/test_pac.c checks the runtime's
    /// QARMA5 against.
    #[test]
    fn gives_the_runtime_test_values() {
        assert_eq!(qarma5(0, 0, 0, 0), 0x7624_3b95_3592_993d);
        assert_eq!(
            qarma5(
                0x71_0000_1234,
                0x71_7fff_f000,
                0x0123_4567_89ab_cdef,
                0xfedc_ba98_7654_3210
            ),
            0xd58d_a1b4_f1ba_5e26
        );
    }

    #[test]
    fn matches_the_paper_test_vector() {
        assert_eq!(
            qarma5(
                0xfb62_3599_da6e_8127,
                0x477d_469d_ec0b_8762,
                0x84be_85ce_9804_e94b,
                0xec28_02d4_e0a4_88e9
            ),
            0xc003_b939_99b3_3765
        );
    }
}

//! `SM4E` and `SM4EKEY` through the generated templates and the SM4 S-box
//! intrinsic, run in the `relift-ir` interpreter on the example of GB/T 32907
//! (appendix A): the key schedule's 32 round keys and the encryption of the
//! plaintext that equals the key.

use relift_cpu_arm64::cpu::{Cpu, Va};
use relift_cpu_arm64::decode::Features;
use relift_cpu_arm64::lift::{lift, system::InterpIntrinsics};
use relift_cpu_arm64::regs;
use relift_ir::interp::{self, FlatMemory, State};

const ALL: Cpu = Cpu::new(Features::ALL, Va::BITS39);

const KEY: [u32; 4] = [0x0123_4567, 0x89ab_cdef, 0xfedc_ba98, 0x7654_3210];
const FK: [u32; 4] = [0xa3b1_bac6, 0x56aa_3350, 0x677d_9197, 0xb270_22dc];

const ROUND_KEYS: [u32; 32] = [
    0xf121_86f9,
    0x4166_2b61,
    0x5a6a_b19a,
    0x7ba9_2077,
    0x3673_60f4,
    0x776a_0c61,
    0xb6bb_89b3,
    0x2476_3151,
    0xa520_307c,
    0xb758_4dbd,
    0xc307_53ed,
    0x7ee5_5b57,
    0x6988_608c,
    0x30d8_95b7,
    0x44ba_14af,
    0x1044_95a1,
    0xd120_b428,
    0x73b5_5fa3,
    0xcc87_4966,
    0x9224_4439,
    0xe89e_641f,
    0x98ca_015a,
    0xc715_9060,
    0x99e1_fd2e,
    0xb79b_d80c,
    0x1d21_15b0,
    0x0e22_8aeb,
    0xf178_0c81,
    0x428d_3654,
    0x6229_3496,
    0x01cf_72e5,
    0x9124_a012,
];

const CIPHERTEXT: [u32; 4] = [0x681e_df34, 0xd206_965e, 0x86b3_e94f, 0x536e_4246];

fn vec4(w: [u32; 4]) -> u128 {
    w.iter()
        .enumerate()
        .fold(0, |v, (i, x)| v | u128::from(*x) << (32 * i))
}

fn words(v: u128) -> [u32; 4] {
    [0, 1, 2, 3].map(|i| (v >> (32 * i)) as u32)
}

fn run(word: u32, regs_in: &[(u32, u128)]) -> u128 {
    let b = lift(word, 0x4000, &ALL).expect("lifts");
    let mut s = State::new(regs::regs().len());
    for (n, v) in regs_in {
        s.regs[regs::vreg(*n).index()] = *v;
    }
    let mut m = FlatMemory {
        base: 0,
        bytes: Vec::new(),
    };
    interp::run(&b, &mut s, &mut m, &mut InterpIntrinsics).expect("runs");
    s.regs[regs::vreg(0).index()]
}

fn sm4ekey(n: u128, m: u128) -> u128 {
    run(0xce60_c800 | 2 << 16 | 1 << 5, &[(1, n), (2, m)])
}

fn sm4e(d: u128, n: u128) -> u128 {
    run(0xcec0_8400 | 1 << 5, &[(0, d), (1, n)])
}

#[test]
fn gbt_32907_key_schedule_and_encryption() {
    let mut k = [0u32; 4];
    for i in 0..4 {
        k[i] = KEY[i] ^ FK[i];
    }
    let mut window = vec4(k);
    let mut rks = Vec::new();
    for g in 0..8u32 {
        let ck = [0, 1, 2, 3].map(|j| {
            let i = 4 * g + j;
            u32::from_be_bytes([0, 1, 2, 3].map(|t| (28 * i + 7 * t) as u8))
        });
        window = sm4ekey(window, vec4(ck));
        rks.extend(words(window));
    }
    assert_eq!(rks, ROUND_KEYS);

    let mut state = vec4(KEY);
    for g in 0..8 {
        let rk = vec4([0, 1, 2, 3].map(|j| ROUND_KEYS[4 * g + j]));
        state = sm4e(state, rk);
    }
    let [x32, x33, x34, x35] = words(state);
    assert_eq!([x35, x34, x33, x32], CIPHERTEXT);
}

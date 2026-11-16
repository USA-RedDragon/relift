use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::translate;

fn word(op: u32, form: u32, fields: &[(u32, u32, u64)]) -> [u8; 16] {
    let mut w: u128 =
        u128::from(op) | (u128::from(form) << 9) | (7u128 << 12) | (7u128 << 110) | (7u128 << 113);
    for &(lo, len, v) in fields {
        let m = ((1u128 << len) - 1) << lo;
        w = (w & !m) | ((u128::from(v) << lo) & m);
    }
    w.to_le_bytes()
}

fn finish(code: &mut Vec<u8>) {
    code.extend(word(0x14d, 4, &[(87, 3, 7)]));
    code.extend(word(0x147, 4, &[(34, 48, 0xffff_ffff_fffc), (87, 3, 7)]));
}

fn spirv_ops(words: &[u32]) -> Vec<u32> {
    let mut ops = Vec::new();
    let mut k = 5;
    while k < words.len() {
        ops.push(words[k] & 0xffff);
        k += ((words[k] >> 16) as usize).max(1);
    }
    ops
}

fn text(p: &Program) -> Vec<String> {
    p.decode()
        .into_iter()
        .map(|d| d.map(|i| i.to_string()).unwrap_or_default())
        .collect()
}

fn run(words: &[[u8; 16]], expect: &[&str]) -> relift_gpu_sm86::translate::Shader {
    let mut b: Vec<u8> = words.iter().flatten().copied().collect();
    finish(&mut b);
    let p = Program::parse(&b);
    let t = text(&p);
    for (n, e) in expect.iter().enumerate() {
        assert!(t[n].contains(e), "{} lacks {e}", t[n]);
    }
    translate(&p).unwrap_or_else(|e| panic!("{e}"))
}

const SIZE_U8: u64 = 0;
const SIZE_S16: u64 = 3;

fn ldg(size: u64, d: u64, a: u64, off: u64) -> [u8; 16] {
    word(
        0x181,
        1,
        &[
            (16, 8, d),
            (24, 8, a),
            (40, 24, off),
            (72, 1, 1),
            (73, 3, size),
            (81, 3, 7),
        ],
    )
}

fn stg(size: u64, a: u64, off: u64, s: u64) -> [u8; 16] {
    word(
        0x186,
        1,
        &[
            (24, 8, a),
            (32, 8, s),
            (40, 24, off),
            (72, 1, 1),
            (73, 3, size),
        ],
    )
}

#[test]
fn sub_word_global_access() {
    let s = run(
        &[ldg(SIZE_S16, 2, 4, 2), stg(SIZE_U8, 4, 1, 2)],
        &["S16", "STG.E.EF.U8"],
    );
    assert!(s.interface.global_memory);
    let ops = spirv_ops(&s.spirv);
    assert!(ops.contains(&202), "no signed bit-field extract");
    assert!(ops.contains(&240) && ops.contains(&241), "no atomic and/or");
}

#[test]
fn shared_memory_sizes_come_from_offsets() {
    let lds = word(
        0x184,
        4,
        &[(16, 8, 4), (24, 8, 3), (40, 24, 0x40), (73, 3, 6)],
    );
    let sts = word(
        0x188,
        1,
        &[(24, 8, 3), (32, 8, 2), (40, 24, 0x10), (73, 3, SIZE_U8)],
    );
    let s = run(&[lds, sts], &["LDS.128", "STS.U8"]);
    assert_eq!(s.interface.shared_bytes, 0x50);
    assert!(!s.interface.global_memory);
}

#[test]
fn local_memory_sizes_come_from_offsets() {
    let ldl = word(
        0x183,
        4,
        &[(16, 8, 4), (24, 8, 255), (40, 24, 0x60), (73, 3, 5)],
    );
    let stl = word(
        0x187,
        1,
        &[(24, 8, 255), (32, 8, 4), (40, 24, 0x20), (73, 3, 5)],
    );
    let s = run(&[stl, ldl], &["STL.EF.64", "LDL.EF.64"]);
    assert_eq!(s.interface.local_bytes, 0x68);
}

#[test]
fn global_atomics_use_the_device_scope() {
    let red = word(
        0x18e,
        1,
        &[
            (24, 8, 4),
            (32, 8, 2),
            (40, 24, 8),
            (72, 1, 1),
            (87, 3, 0),
            (90, 1, 1),
        ],
    );
    let atom = word(
        0x1a8,
        1,
        &[
            (16, 8, 6),
            (24, 8, 4),
            (32, 8, 2),
            (40, 24, 8),
            (72, 1, 1),
            (81, 3, 7),
            (87, 4, 2),
        ],
    );
    let s = run(&[red, atom], &["RED.E.ADD", "ATOMG.E.MAX"]);
    let ops = spirv_ops(&s.spirv);
    assert!(ops.contains(&234), "no atomic add");
    assert!(ops.contains(&239), "no atomic unsigned max");
}

#[test]
fn counting_atomics_loop_on_compare_exchange() {
    let atom = word(
        0x1a8,
        1,
        &[
            (16, 8, 6),
            (24, 8, 4),
            (32, 8, 2),
            (40, 24, 8),
            (72, 1, 1),
            (81, 3, 7),
            (87, 4, 3),
        ],
    );
    let s = run(&[atom], &["ATOMG.E.INC"]);
    let ops = spirv_ops(&s.spirv);
    assert!(ops.contains(&230), "no compare-exchange");
    assert!(ops.contains(&246), "no loop");
}

#[test]
fn warp_shuffle_vote_and_barrier() {
    let shfl = word(
        0x189,
        1,
        &[
            (16, 8, 2),
            (24, 8, 3),
            (32, 8, 4),
            (58, 2, 3),
            (64, 8, 5),
            (81, 3, 7),
        ],
    );
    let vote = word(0x006, 1, &[(16, 8, 6), (72, 2, 1), (81, 3, 7), (87, 3, 7)]);
    let bar = word(0x11d, 5, &[(77, 3, 0), (54, 4, 0)]);
    let s = run(&[shfl, vote, bar], &["SHFL.BFLY", "VOTE.ANY", "BAR.SYNC"]);
    let ops = spirv_ops(&s.spirv);
    assert!(ops.contains(&345), "no shuffle");
    assert!(ops.contains(&335), "no any");
    assert!(ops.contains(&339), "no ballot");
    assert!(ops.contains(&224), "no control barrier");
}

#[test]
fn lane_masks_read_the_subgroup_invocation() {
    let s2r = word(0x119, 1, &[(16, 8, 2), (72, 8, 57)]);
    let s = run(&[s2r], &["SR_LTMASK"]);
    assert!(!spirv_ops(&s.spirv).is_empty());
}

#[test]
fn match_any_compares_every_lane() {
    let m = word(0x1a1, 1, &[(16, 8, 4), (24, 8, 2), (79, 1, 1)]);
    let s = run(&[m], &["MATCH.ANY"]);
    let shuffles = spirv_ops(&s.spirv).iter().filter(|&&o| o == 345).count();
    assert_eq!(shuffles, 64);
}

#[test]
fn ldsm_gathers_rows_across_lanes() {
    let ldsm = |shape: u64, count: u64| {
        word(
            0x83b,
            4,
            &[
                (16, 8, 8),
                (24, 8, 4),
                (40, 24, 0x40),
                (72, 2, count),
                (78, 2, shape),
            ],
        )
    };
    for (shape, count, name, shuffles) in [
        (0, 2, "LDSM.16.M88.4 R8", 4),
        (1, 2, "LDSM.16.MT88.4 R8", 8),
        (0, 1, "LDSM.16.M88.2 R8", 2),
        (1, 0, "LDSM.16.MT88 R8", 2),
    ] {
        let s = run(&[ldsm(shape, count)], &[name]);
        let ops = spirv_ops(&s.spirv);
        assert_eq!(
            ops.iter().filter(|o| **o == 345).count(),
            shuffles,
            "{name}"
        );
        assert!(s.interface.shared_bytes >= 0x50, "{name}");
    }
}

/// Ids of the `u32` zero constants in a module.
fn u32_zeros(words: &[u32]) -> Vec<u32> {
    let mut u32s = Vec::new();
    let mut zeros = Vec::new();
    let mut k = 5;
    while k < words.len() {
        let len = ((words[k] >> 16) as usize).max(1);
        let w = &words[k..k + len];
        match w[0] & 0xffff {
            21 if w[2] == 32 && w[3] == 0 => u32s.push(w[1]),
            43 if u32s.contains(&w[1]) && w[3] == 0 => zeros.push(w[2]),
            _ => {}
        }
        k += len;
    }
    zeros
}

#[test]
fn wide_atomics_read_both_words_of_rz_as_zero() {
    let cas = word(
        0x1a9,
        1,
        &[
            (16, 8, 4),
            (24, 8, 6),
            (32, 8, 255),
            (64, 8, 8),
            (72, 1, 1),
            (73, 4, 2),
            (81, 3, 7),
        ],
    );
    let s = run(&[cas], &[".64 PT, R4, [R6], RZ, R8"]);
    let zeros = u32_zeros(&s.spirv);
    let mut k = 5;
    let mut rz_pair = false;
    while k < s.spirv.len() {
        let len = ((s.spirv[k] >> 16) as usize).max(1);
        let w = &s.spirv[k..k + len];
        if w[0] & 0xffff == 80 && len == 5 && zeros.contains(&w[3]) && zeros.contains(&w[4]) {
            rz_pair = true;
        }
        k += len;
    }
    assert!(rz_pair, "the RZ pair is not zero in both words");
}

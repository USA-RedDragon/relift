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
    assert!(
        ops.contains(&230) && ops.contains(&246),
        "no compare-and-swap loop"
    );
    assert!(!ops.contains(&240) && !ops.contains(&241), "atomic and/or");
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

#[test]
fn tuples_past_rz_are_rejected() {
    for (d, ok) in [(252, true), (253, false)] {
        let mut b: Vec<u8> = ldg(6, d, 4, 0).to_vec();
        finish(&mut b);
        let p = Program::parse(&b);
        assert!(text(&p)[0].ends_with(&format!(".128 R{d}, [R4]")));
        assert_eq!(translate(&p).is_ok(), ok, "R{d}");
    }
}

use relift_gpu_sm86::translate::{HostProfile, Options, translate_with};

const ORDER_STRONG_SM: u64 = 5;
const ORDER_STRONG_GPU: u64 = 7;
const ORDER_STRONG_SYS: u64 = 10;
const SIZE_32: u64 = 4;
const SIZE_64: u64 = 5;

const OP_LOAD: u32 = 61;
const OP_ATOMIC_LOAD: u32 = 227;
const OP_ATOMIC_STORE: u32 = 228;
const OP_MEMORY_BARRIER: u32 = 225;
const OP_CONSTANT: u32 = 43;

fn ordered(w: [u8; 16], order: u64) -> [u8; 16] {
    let mut v = u128::from_le_bytes(w);
    v = (v & !(0xf << 77)) | (u128::from(order) << 77);
    v.to_le_bytes()
}

fn membar(scope: u64, kind: u64) -> [u8; 16] {
    word(0x192, 4, &[(76, 3, scope), (79, 2, kind)])
}

fn bar() -> [u8; 16] {
    word(0x11d, 5, &[(77, 3, 0), (54, 4, 0)])
}

fn on(words: &[[u8; 16]], host: HostProfile) -> relift_gpu_sm86::translate::Shader {
    let mut b: Vec<u8> = words.iter().flatten().copied().collect();
    finish(&mut b);
    let o = Options {
        host,
        ..Options::default()
    };
    translate_with(&Program::parse(&b), &o).unwrap_or_else(|e| panic!("{e}"))
}

/// The instructions with `opcode`, as whole words.
fn insts(words: &[u32], opcode: u32) -> Vec<Vec<u32>> {
    let mut v = Vec::new();
    let mut k = 5;
    while k < words.len() {
        let n = ((words[k] >> 16) as usize).max(1);
        if words[k] & 0xffff == opcode {
            v.push(words[k..k + n].to_vec());
        }
        k += n;
    }
    v
}

/// The value of the 32-bit OpConstant `id`.
fn constant(words: &[u32], id: u32) -> Option<u32> {
    insts(words, OP_CONSTANT)
        .into_iter()
        .find(|i| i[2] == id)
        .map(|i| i[3])
}

/// The scopes of the atomic loads and stores (as constant values).
fn atomic_scopes(words: &[u32]) -> Vec<u32> {
    let mut s: Vec<u32> = insts(words, OP_ATOMIC_LOAD)
        .iter()
        .map(|i| i[4])
        .chain(insts(words, OP_ATOMIC_STORE).iter().map(|i| i[2]))
        .filter_map(|id| constant(words, id))
        .collect();
    s.sort();
    s.dedup();
    s
}

const SCOPE_DEVICE: u32 = 1;
const SCOPE_WORKGROUP: u32 = 2;

/// A CTA-strong load meets only writes of its own workgroup, so it is
/// plain in a program that writes no global memory and a workgroup
/// atomic in one that does.
#[test]
fn cta_strong_loads_are_atomic_only_beside_global_writes() {
    let ld = ordered(ldg(SIZE_32, 2, 4, 0), ORDER_STRONG_SM);
    let s = on(&[ld], HostProfile::nvidia());
    assert!(insts(&s.spirv, OP_ATOMIC_LOAD).is_empty());
    let aligned = |i: &Vec<u32>| i.len() > 4 && i[4] == 2;
    assert_eq!(
        insts(&s.spirv, OP_LOAD)
            .iter()
            .filter(|i| aligned(i))
            .count(),
        1
    );
    let st = ordered(stg(SIZE_32, 4, 8, 2), ORDER_STRONG_SM);
    let s = on(&[ld, st], HostProfile::nvidia());
    assert_eq!(insts(&s.spirv, OP_ATOMIC_LOAD).len(), 1);
    assert_eq!(insts(&s.spirv, OP_ATOMIC_STORE).len(), 1);
    assert_eq!(atomic_scopes(&s.spirv), [SCOPE_WORKGROUP]);
    assert!(s.interface.host.inexact.is_empty());
}

/// GPU-strong accesses are device atomics; a strong 64-bit one is one
/// 64-bit atomic where the host has them, and listed inexact elsewhere.
#[test]
fn gpu_strong_accesses_are_device_atomics() {
    let ld = ordered(ldg(SIZE_32, 2, 4, 0), ORDER_STRONG_GPU);
    let s = on(&[ld], HostProfile::nvidia());
    assert_eq!(insts(&s.spirv, OP_ATOMIC_LOAD).len(), 1);
    assert_eq!(atomic_scopes(&s.spirv), [SCOPE_DEVICE]);
    let ld64 = ordered(ldg(SIZE_64, 2, 4, 0), ORDER_STRONG_GPU);
    let s = on(&[ld64], HostProfile::nvidia());
    assert_eq!(insts(&s.spirv, OP_ATOMIC_LOAD).len(), 1);
    assert!(s.interface.host.inexact.is_empty());
    let s = on(&[ld64], HostProfile::minimal());
    assert_eq!(insts(&s.spirv, OP_ATOMIC_LOAD).len(), 2);
    assert!(s.interface.host.inexact.iter().any(|l| l.contains(".64")));
    let sys = ordered(ldg(SIZE_32, 2, 4, 0), ORDER_STRONG_SYS);
    let s = on(&[sys], HostProfile::nvidia());
    assert!(s.interface.host.inexact.iter().any(|l| l.contains("SYS")));
}

/// Weak accesses take part in synchronization through fences: with a
/// device fence they are device atomics, with a barrier and global writes
/// workgroup atomics, and plain otherwise.
#[test]
fn weak_accesses_are_coherent_where_a_fence_orders_them() {
    let ld = ldg(SIZE_32, 2, 4, 0);
    let st = stg(SIZE_32, 4, 8, 2);
    let s = on(&[ld, st], HostProfile::nvidia());
    assert!(insts(&s.spirv, OP_ATOMIC_LOAD).is_empty());
    let s = on(&[ld, bar(), st], HostProfile::nvidia());
    assert_eq!(atomic_scopes(&s.spirv), [SCOPE_WORKGROUP]);
    let s = on(&[ld, bar()], HostProfile::nvidia());
    assert!(insts(&s.spirv, OP_ATOMIC_LOAD).is_empty());
    let s = on(&[ld, membar(2, 2), st], HostProfile::nvidia());
    assert_eq!(atomic_scopes(&s.spirv), [SCOPE_DEVICE]);
    assert!(s.interface.host.inexact.is_empty());
}

/// MEMBAR orders image memory too; SC and SYS fences are listed inexact,
/// VC is rejected.
#[test]
fn fences_report_what_vulkan_cannot_order() {
    let s = on(&[membar(2, 2)], HostProfile::nvidia());
    let b = insts(&s.spirv, OP_MEMORY_BARRIER);
    assert_eq!(b.len(), 1);
    assert_eq!(constant(&s.spirv, b[0][1]), Some(SCOPE_DEVICE));
    let sem = constant(&s.spirv, b[0][2]).unwrap_or(0);
    assert_eq!(sem & 0x848, 0x848, "semantics {sem:#x}");
    assert!(s.interface.host.inexact.is_empty());
    let s = on(&[membar(2, 0)], HostProfile::nvidia());
    assert!(
        s.interface
            .host
            .inexact
            .iter()
            .any(|l| l.contains("MEMBAR.SC"))
    );
    let s = on(&[membar(3, 2)], HostProfile::nvidia());
    assert!(
        s.interface
            .host
            .inexact
            .iter()
            .any(|l| l.contains("MEMBAR.SYS"))
    );
    let mut b: Vec<u8> = membar(5, 2).to_vec();
    finish(&mut b);
    assert!(translate(&Program::parse(&b)).is_err());
}

/// Generic accesses are global ones, which misses the shared and local
/// windows: listed inexact.
#[test]
fn generic_accesses_are_listed_inexact() {
    let ld = word(0x180, 1, &[(16, 8, 2), (24, 8, 4), (72, 1, 1), (73, 3, 4)]);
    let s = run(&[ld], &["LD.E"]);
    assert!(
        s.interface
            .host
            .inexact
            .iter()
            .any(|l| l.contains("generic"))
    );
}

/// Without buffer device addresses, the segment index stops at the bound
/// segment count, where the runtime puts a trap segment.
#[test]
fn segment_index_is_bounded() {
    let s = on(&[ldg(SIZE_32, 2, 4, 0)], HostProfile::minimal());
    let spec = insts(&s.spirv, 50);
    let count = spec.iter().find(|i| {
        insts(&s.spirv, 71)
            .iter()
            .any(|d| d[1] == i[2] && d[2] == 1 && d[3] == 6)
    });
    assert!(count.is_some(), "no segment count constant");
    assert!(!insts(&s.spirv, 169).is_empty(), "no select");
}

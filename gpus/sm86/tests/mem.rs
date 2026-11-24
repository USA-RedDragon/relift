mod common;

use common::translate;
use relift_gpu_nv::program::Program;

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
    p.code
        .clone()
        .into_iter()
        .map(|d| d.map(|i| i.to_string()).unwrap_or_default())
        .collect()
}

fn run(words: &[[u8; 16]], expect: &[&str]) -> relift_gpu_nv::translate::Shader {
    let mut b: Vec<u8> = words.iter().flatten().copied().collect();
    finish(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
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
fn only_global_stores_mark_the_program_as_writing() {
    let loads = run(&[ldg(SIZE_S16, 2, 4, 2)], &["S16"]);
    assert!(loads.interface.global_memory);
    assert!(!loads.interface.global_store);
    let stores = run(&[stg(SIZE_U8, 4, 1, 2)], &["STG.E.EF.U8"]);
    assert!(stores.interface.global_store);
}

/// On a host whose guest memory is mirrored, a global store also sets its
/// page's bit in the written-page bitmap (an `OpAtomicOr` through the
/// address in the push constants); elsewhere the program is as it was.
#[test]
fn global_stores_mark_their_pages_where_memory_is_mirrored() {
    use relift_gpu_nv::translate::{HostProfile, Options};
    let mut b: Vec<u8> = [stg(SIZE_U8, 4, 1, 2)].iter().flatten().copied().collect();
    finish(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    for h in HostProfile::presets() {
        let o = Options {
            host: h.clone(),
            ..Options::default()
        };
        let s = common::translate_with(&p, &o).unwrap_or_else(|e| panic!("{}: {e}", h.name));
        assert!(s.interface.global_store, "{}", h.name);
        let marks = h.mirror_writes && h.bda;
        assert_eq!(s.interface.marks_written_pages, marks, "{}", h.name);
        assert_eq!(spirv_ops(&s.spirv).contains(&242), marks, "{}", h.name);
    }
    assert!(
        HostProfile::presets()
            .iter()
            .any(|h| h.mirror_writes && h.bda)
    );
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
    assert_eq!(s.interface.shared_declared, None);
    assert_eq!(s.interface.shared_beyond_declared, 0);
}

#[test]
fn constant_shared_accesses_are_checked_against_the_declared_size() {
    let lds = word(
        0x184,
        4,
        &[(16, 8, 4), (24, 8, 3), (40, 24, 0x40), (73, 3, 6)],
    );
    let mut b: Vec<u8> = lds.to_vec();
    finish(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let with = |declared: u32| {
        let o = relift_gpu_nv::translate::Options {
            shared_declared: Some(declared),
            ..Default::default()
        };
        relift_gpu_nv::translate::translate_with(&p, &o)
            .unwrap_or_else(|e| panic!("{e}"))
            .interface
    };
    let fits = with(0x50);
    assert_eq!(fits.shared_declared, Some(0x50));
    assert_eq!(fits.shared_beyond_declared, 0);
    let short = with(0x48);
    assert_eq!(short.shared_bytes, 0x50, "the extent is the same");
    assert_eq!(
        short.shared_beyond_declared, 8,
        "LDS.128 at 0x40 reaches 0x50"
    );
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
        let p = relift_gpu_sm86::program::parse(&b);
        assert!(text(&p)[0].ends_with(&format!(".128 R{d}, [R4]")));
        assert_eq!(translate(&p).is_ok(), ok, "R{d}");
    }
}

use common::translate_with;
use relift_gpu_nv::translate::{Atomic64Path, HostProfile, Options};

const ORDER_STRONG_SM: u64 = 5;
const ORDER_STRONG_GPU: u64 = 7;
const ORDER_STRONG_SYS: u64 = 10;
const SIZE_32: u64 = 4;
const SIZE_64: u64 = 5;
const SIZE_128: u64 = 6;

const OP_LOAD: u32 = 61;
const OP_ATOMIC_LOAD: u32 = 227;
const OP_ATOMIC_STORE: u32 = 228;
const OP_ATOMIC_COMPARE_EXCHANGE: u32 = 230;
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

fn on(words: &[[u8; 16]], host: HostProfile) -> relift_gpu_nv::translate::Shader {
    let mut b: Vec<u8> = words.iter().flatten().copied().collect();
    finish(&mut b);
    let o = Options {
        host,
        ..Options::default()
    };
    translate_with(&relift_gpu_sm86::program::parse(&b), &o).unwrap_or_else(|e| panic!("{e}"))
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

/// GPU-strong accesses are device atomics. Each 64-bit element of a strong
/// 64- or 128-bit one is single-copy atomic: one 64-bit atomic where the
/// host has them, and a read or write of both words under the lock of the
/// 64-bit atomics elsewhere.
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
    let ld128 = ordered(ldg(SIZE_128, 4, 8, 0), ORDER_STRONG_GPU);
    let s = on(&[ld128], HostProfile::nvidia());
    assert_eq!(insts(&s.spirv, OP_ATOMIC_LOAD).len(), 2);
    assert!(s.interface.host.inexact.is_empty());
    let s = on(&[ld64], HostProfile::minimal());
    assert_eq!(s.interface.host.atomics64, Atomic64Path::Locked);
    assert_eq!(insts(&s.spirv, OP_ATOMIC_COMPARE_EXCHANGE).len(), 1);
    assert!(
        insts(&s.spirv, OP_ATOMIC_STORE).len() == 1,
        "a locked load writes only the lock"
    );
    assert!(s.interface.host.inexact.is_empty());
    let st128 = ordered(stg(SIZE_128, 4, 8, 0), ORDER_STRONG_GPU);
    let s = on(&[st128], HostProfile::minimal());
    assert_eq!(insts(&s.spirv, OP_ATOMIC_COMPARE_EXCHANGE).len(), 2);
    assert_eq!(insts(&s.spirv, OP_ATOMIC_STORE).len(), 6);
    assert!(s.interface.host.inexact.is_empty());
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

/// MEMBAR orders image memory too; SC and SYS fences are listed inexact.
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
}

/// NVIDIA's compiler gives GLSL's groupMemoryBarrier `MEMBAR.SC.VC`: a
/// workgroup fence in a compute program that keeps weak accesses at
/// workgroup scope, a device fence elsewhere.
#[test]
fn vc_fences_are_workgroup_fences() {
    let ld = ldg(SIZE_32, 2, 4, 0);
    let st = stg(SIZE_32, 4, 8, 2);
    let s = on(&[ld, membar(5, 2), st], HostProfile::nvidia());
    let b = insts(&s.spirv, OP_MEMORY_BARRIER);
    assert_eq!(b.len(), 1);
    assert_eq!(constant(&s.spirv, b[0][1]), Some(SCOPE_WORKGROUP));
    assert_eq!(atomic_scopes(&s.spirv), [SCOPE_WORKGROUP]);
    assert!(s.interface.host.inexact.is_empty());
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

const ACCESS_CHAIN: u32 = 65;
const CONVERT_U_TO_PTR: u32 = 120;
const DECORATE: u32 = 71;
const DECO_BINDING: u32 = 33;

/// A vertex program: `ULDC.64 UR4, c[0x0][slot base]` then
/// `LDG.E.128 R4, [R5.U32+UR4+0x20]`.
fn vertex_slot_load(base: u64) -> relift_gpu_nv::translate::Shader {
    let mut w = [0u32; 32];
    w[0] = 1 | (4 << 5) | (1 << 10);
    let mut b: Vec<u8> = w.iter().flat_map(|x| x.to_le_bytes()).collect();
    b.extend(word(
        0x0b9,
        5,
        &[(16, 3, 4), (38, 9, base), (54, 1, 0), (73, 3, 5)],
    ));
    b.extend(word(
        0x181,
        4,
        &[
            (16, 3, 4),
            (24, 3, 5),
            (32, 3, 4),
            (40, 6, 0x20),
            (72, 1, 1),
            (73, 3, 6),
            (81, 3, 7),
            (91, 1, 1),
        ],
    ));
    finish(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let t = text(&p);
    assert!(t[0].contains("ULDC.64 UR4, c[0x0]"), "{}", t[0]);
    assert!(t[1].contains("LDG.E.EF.128"), "{}", t[1]);
    translate(&p).unwrap_or_else(|e| panic!("{e}"))
}

fn decorated_binding(words: &[u32], binding: u32) -> bool {
    let mut k = 5;
    while k < words.len() {
        let n = ((words[k] >> 16) as usize).max(1);
        if words[k] & 0xffff == DECORATE && words[k + 2] == DECO_BINDING && words[k + 3] == binding
        {
            return true;
        }
        k += n;
    }
    false
}

#[test]
fn a_vertex_load_from_a_storage_slot_base_reads_the_slot_descriptor() {
    let s = vertex_slot_load(0x130);
    assert_eq!(
        s.interface
            .storage_slots
            .iter()
            .copied()
            .collect::<Vec<_>>(),
        vec![2]
    );
    let ops = spirv_ops(&s.spirv);
    assert!(
        !ops.contains(&CONVERT_U_TO_PTR),
        "the load keeps a buffer device address"
    );
    assert!(ops.contains(&ACCESS_CHAIN));
    assert!(decorated_binding(&s.spirv, 64));
    let other = vertex_slot_load(0x100);
    assert_eq!(other.interface.storage_slots.len(), 16, "no slot search");
    assert!(spirv_ops(&other.spirv).contains(&CONVERT_U_TO_PTR));
}

const SPH_VERTEX: u32 = 1 | (4 << 5) | (1 << 10);
const SPH_FRAGMENT: u32 = 2 | (4 << 5) | (5 << 10);

/// The program of `sph0` (the first header word) with `body` and an exit.
fn staged(sph0: u32, body: &[[u8; 16]], expect: &[&str]) -> relift_gpu_nv::translate::Shader {
    let mut w = [0u32; 32];
    w[0] = sph0;
    let mut b: Vec<u8> = w.iter().flat_map(|x| x.to_le_bytes()).collect();
    b.extend(body.iter().flatten());
    finish(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let t = text(&p);
    for (n, e) in expect.iter().enumerate() {
        assert!(t[n].contains(e), "{} lacks {e}", t[n]);
    }
    translate(&p).unwrap_or_else(|e| panic!("{e}"))
}

/// `ULDC.64 URu, c[0x0][off]`.
fn uldc64(u: u64, off: u64) -> [u8; 16] {
    word(
        0x0b9,
        5,
        &[(16, 6, u), (38, 16, off), (54, 5, 0), (73, 3, 5)],
    )
}

/// An instruction word written out as the 16 bytes NVIDIA's compiler
/// emits (little endian), for forms the field helpers do not build.
fn hex_word(h: &str) -> [u8; 16] {
    let mut b = [0u8; 16];
    for (k, o) in b.iter_mut().enumerate() {
        *o = u8::from_str_radix(&h[2 * k..2 * k + 2], 16).unwrap();
    }
    b
}

/// `LEA R16, P0, R49, c[0x0][0x560], 0x2`.
const LEA_R16_P0_C0_560: &str = "117a103100580100ff10800700c80f00";
/// `IADD3.X R17, RZ, c[0x0][0x564], RZ, P0, !PT`.
const IADD3X_R17_P0_C0_564: &str = "107a11ff00590100ffe47f0000ca0f00";
/// `LDG.E.CONSTANT R13, [R16]`.
const LDG_R13_R16: &str = "81730d100000000000891e0000a20200";
/// `STG.E.STRONG.SM [R13.U32+UR6], R14`.
const STG_R13_UR6_R14: &str = "8679000d0e00000006a9100800e20f00";

fn has_psb(s: &relift_gpu_nv::translate::Shader) -> bool {
    spirv_ops(&s.spirv).contains(&CONVERT_U_TO_PTR)
}

/// Whether the program searches its stage's slots at run time for an
/// access no route takes: every slot recorded, a selection on the hit, an
/// access chain into the slot array decorated NonUniform, and the buffer
/// device address only on the miss.
fn searches(s: &relift_gpu_nv::translate::Shader) -> bool {
    let ops = spirv_ops(&s.spirv);
    s.interface.storage_slots.len() == 16
        && ops.contains(&247)
        && insts(&s.spirv, DECORATE).iter().any(|d| d[2] == 5300)
        && decorated_binding(&s.spirv, 64)
        && has_psb(s)
}

fn slots_of(s: &relift_gpu_nv::translate::Shader) -> Vec<u32> {
    s.interface.storage_slots.iter().copied().collect()
}

/// A fragment program's load from the slot base in a uniform register
/// pair reads fragment slot `k` (c[0] row 0x51 plus `k`) through a
/// descriptor; a word outside the fragment rows keeps the address.
#[test]
fn a_fragment_load_from_a_storage_slot_base_reads_the_slot_descriptor() {
    let load = word(
        0x181,
        4,
        &[
            (16, 8, 4),
            (24, 8, 5),
            (32, 6, 4),
            (40, 24, 0x20),
            (72, 1, 1),
            (73, 3, 6),
            (81, 3, 7),
            (91, 1, 1),
        ],
    );
    let s = staged(
        SPH_FRAGMENT,
        &[uldc64(4, 0x530), load],
        &["ULDC.64 UR4, c[0x0][0x530]", "LDG"],
    );
    assert_eq!(slots_of(&s), vec![2]);
    assert!(!has_psb(&s), "the load keeps a buffer device address");
    assert!(decorated_binding(&s.spirv, 64));
    let vertex_row = staged(SPH_FRAGMENT, &[uldc64(4, 0x130), load], &[]);
    assert!(searches(&vertex_row));
}

/// A store through a slot base goes through the slot's descriptor, and
/// does not make the program a global writer (the runtime marks the
/// slot's span written from the stores to the storage buffer).
#[test]
fn a_fragment_store_through_a_storage_slot_base_writes_the_slot_descriptor() {
    let s = staged(
        SPH_FRAGMENT,
        &[uldc64(6, 0x550), hex_word(STG_R13_UR6_R14)],
        &[
            "ULDC.64 UR6, c[0x0][0x550]",
            "STG.E.STRONG.SM [R13.U32+UR6], R14",
        ],
    );
    assert_eq!(slots_of(&s), vec![4]);
    assert!(!has_psb(&s));
    assert!(!s.interface.global_store);
    assert!(spirv_ops(&s.spirv).contains(&62), "no store");
}

/// A register pair made from slot 5's base words with LEA and IADD3.X
/// (its carry) is the slot's base plus a 32-bit offset: the load reads the
/// slot's descriptor at the low word less the base's.
#[test]
fn a_load_through_a_register_pair_made_from_a_slot_base_reads_the_slot() {
    let s = staged(
        SPH_FRAGMENT,
        &[
            hex_word(LEA_R16_P0_C0_560),
            hex_word(IADD3X_R17_P0_C0_564),
            hex_word(LDG_R13_R16),
        ],
        &[
            "LEA R16, P0, R49, c[0x0][0x560], 0x2",
            "IADD3.X R17, RZ, c[0x0][0x564], RZ, P0, !PT",
            "LDG.E.CONSTANT R13, [R16]",
        ],
    );
    assert_eq!(slots_of(&s), vec![5]);
    assert!(!has_psb(&s));
    assert!(
        spirv_ops(&s.spirv).contains(&130),
        "no subtraction of the base"
    );
}

/// The same pair in a vertex program is not a vertex slot (0x560 is past
/// the vertex rows), nor is a pair whose high word is not made from the
/// next word: those accesses search the slots at run time.
#[test]
fn a_register_pair_from_another_stages_rows_searches_the_slots() {
    let s = staged(
        SPH_VERTEX,
        &[
            hex_word(LEA_R16_P0_C0_560),
            hex_word(IADD3X_R17_P0_C0_564),
            hex_word(LDG_R13_R16),
        ],
        &[],
    );
    assert!(searches(&s));
    let s = staged(
        SPH_FRAGMENT,
        &[hex_word(LEA_R16_P0_C0_560), hex_word(LDG_R13_R16)],
        &[],
    );
    assert!(searches(&s));
}

/// An atomic through a slot base pair is a storage buffer atomic.
#[test]
fn an_atomic_through_a_slot_base_pair_uses_the_slot_descriptor() {
    let red = word(
        0x18e,
        1,
        &[
            (24, 8, 16),
            (32, 8, 2),
            (40, 24, 8),
            (72, 1, 1),
            (87, 3, 0),
            (90, 1, 1),
        ],
    );
    let s = staged(
        SPH_FRAGMENT,
        &[
            hex_word(LEA_R16_P0_C0_560),
            hex_word(IADD3X_R17_P0_C0_564),
            red,
        ],
        &["LEA", "IADD3.X", "RED.E.ADD"],
    );
    assert_eq!(slots_of(&s), vec![5]);
    assert!(!has_psb(&s));
    assert!(spirv_ops(&s.spirv).contains(&234), "no atomic add");
}

/// `ULDC.64 UR4, c[0x0][0x80]`.
const ULDC64_UR4_C0_80: &str = "b97a040000200000000a000000e40f00";
/// `LDG.E.STRONG.SM R0, [UR4+0x24]`.
const LDG_R0_UR4_24: &str = "817900ff0424000000a91e0c00a20e00";
/// `UIADD3 UR4, UP0, UR4, 0x24, URZ`.
const UIADD3_UR4_UP0_24: &str = "90780404240000003fe0f10f00e20f00";
/// `UIADD3.X UR5, URZ, UR5, URZ, UP0, !UPT`.
const UIADD3X_UR5_UP0: &str = "9072053f050000003fe47f0800e20f00";
/// `LDG.E.STRONG.SM R23, [UR4+-0x4]`.
const LDG_R23_UR4_M4: &str = "817917ff04fcffff00a91e0c00a40e00";
/// `IMAD.WIDE.U32 R10, R0, R11, c[0x0][0x270]`.
const IMAD_WIDE_R10_C0_270: &str = "25760a00009c00000b008e0700ca0f00";
/// `LDG.E.U16 R9, [R10.64+0x6]`.
const LDG_U16_R9_R10_6: &str = "8179090a0406000000151e0c00a80e00";

fn compute_bases(s: &relift_gpu_nv::translate::Shader) -> Vec<u32> {
    s.interface.compute_bases.iter().copied().collect()
}

/// A compute program's loads from a base in its c[0] go through the
/// base's descriptor (binding 128 plus the offset in doublewords), counted
/// from the start the base table gives, so an offset below the base, or a
/// constant added to the base pair with its carry, reaches the same bytes.
#[test]
fn compute_loads_from_a_c0_base_use_the_base_descriptor() {
    let s = run(
        &[
            hex_word(ULDC64_UR4_C0_80),
            hex_word(LDG_R0_UR4_24),
            hex_word(UIADD3_UR4_UP0_24),
            hex_word(UIADD3X_UR5_UP0),
            hex_word(LDG_R23_UR4_M4),
        ],
        &[
            "ULDC.64 UR4, c[0x0][0x80]",
            "LDG.E.STRONG.SM R0, [UR4+0x24]",
            "UIADD3 UR4, UP0",
            "UIADD3.X UR5",
            "[UR4+-0x4]",
        ],
    );
    assert_eq!(compute_bases(&s), vec![0x80]);
    assert!(s.interface.storage_slots.is_empty());
    assert!(!has_psb(&s), "a load keeps a buffer device address");
    assert!(decorated_binding(&s.spirv, 128 + 0x80 / 8));
    assert!(decorated_binding(&s.spirv, 80), "no base table");
}

/// A pair made by `IMAD.WIDE.U32` from a c[0] base is that base plus an
/// offset.
#[test]
fn compute_loads_through_a_wide_product_plus_a_base_use_the_base_descriptor() {
    let s = run(
        &[hex_word(IMAD_WIDE_R10_C0_270), hex_word(LDG_U16_R9_R10_6)],
        &[
            "IMAD.WIDE.U32 R10, R0, R11, c[0x0][0x270]",
            "LDG.E.U16 R9, [R10.64+0x6]",
        ],
    );
    assert_eq!(compute_bases(&s), vec![0x270]);
    assert!(!has_psb(&s));
}

/// A compute base pair whose high word is not the next c[0] word keeps
/// the buffer device address.
#[test]
fn compute_loads_through_other_pairs_keep_the_address() {
    let s = run(&[hex_word(UIADD3_UR4_UP0_24), hex_word(LDG_R0_UR4_24)], &[]);
    assert!(compute_bases(&s).is_empty());
    assert!(has_psb(&s));
}

/// Plain loads and stores, sub-word stores and atomics through addresses no
/// route takes, in a graphics stage, all search the slots.
#[test]
fn graphics_accesses_off_the_slot_routes_search_the_slots() {
    let s = staged(SPH_FRAGMENT, &[ldg(SIZE_S16, 2, 4, 2)], &["S16"]);
    assert!(searches(&s), "sub-word load");
    let s = staged(SPH_FRAGMENT, &[stg(SIZE_U8, 4, 1, 2)], &["STG.E.EF.U8"]);
    assert!(searches(&s), "sub-word store");
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
    let s = staged(SPH_FRAGMENT, &[red], &["RED.E.ADD"]);
    assert!(searches(&s), "atomic");
    assert!(spirv_ops(&s.spirv).contains(&234));
}

/// Compute programs keep the buffer device address for an address no
/// route proves: no slot or pool search.
#[test]
fn compute_accesses_off_the_routes_keep_the_address() {
    let s = run(&[ldg(SIZE_S16, 2, 4, 2)], &["S16"]);
    assert!(s.interface.storage_slots.is_empty());
    assert!(!s.interface.pools);
    assert!(has_psb(&s));
    assert!(!spirv_ops(&s.spirv).contains(&247));
}

/// Whether the program searches the pool table: the table and the array
/// at set 4, a NonUniform access chain, and the buffer device address
/// only on the miss.
fn searches_pools(s: &relift_gpu_nv::translate::Shader) -> bool {
    let set4: Vec<u32> = insts(&s.spirv, DECORATE)
        .iter()
        .filter(|d| d[2] == 34 && d[3] == 4)
        .map(|d| d[1])
        .collect();
    let bound = |b: u32| {
        insts(&s.spirv, DECORATE)
            .iter()
            .any(|d| d[2] == DECO_BINDING && d[3] == b && set4.contains(&d[1]))
    };
    bound(0) && bound(1) && insts(&s.spirv, DECORATE).iter().any(|d| d[2] == 5300) && has_psb(s)
}

/// Graphics accesses that miss the slots search the pools too, in every
/// kind of access: a load, a sub-word store and an atomic.
#[test]
fn graphics_slot_misses_search_the_pools() {
    let s = staged(SPH_FRAGMENT, &[ldg(SIZE_S16, 2, 4, 2)], &["S16"]);
    assert!(searches(&s) && searches_pools(&s));
    let s = staged(SPH_FRAGMENT, &[stg(SIZE_U8, 4, 1, 2)], &["STG.E.EF.U8"]);
    assert!(searches(&s) && searches_pools(&s));
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
    let s = staged(SPH_FRAGMENT, &[red], &["RED.E.ADD"]);
    assert!(searches_pools(&s));
}

/// Where guest memory is mirrored, a graphics store marks its pages on
/// every path, so one that goes through a slot or pool descriptor is
/// copied back too: two marks (first and last byte) on each of the slot,
/// pool and address paths.
#[test]
fn stores_through_slots_and_pools_mark_their_pages() {
    use relift_gpu_nv::translate::{HostProfile, Options};
    let h = HostProfile::presets()
        .into_iter()
        .find(|h| h.mirror_writes && h.bda)
        .expect("a mirroring preset");
    let mut b: Vec<u8> = [0u32; 32]
        .iter()
        .enumerate()
        .flat_map(|(k, _)| if k == 0 { SPH_FRAGMENT } else { 0 }.to_le_bytes())
        .collect();
    b.extend(stg(SIZE_32, 4, 8, 2));
    finish(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let o = Options {
        host: h,
        ..Options::default()
    };
    let s = common::translate_with(&p, &o).unwrap_or_else(|e| panic!("{e}"));
    assert!(s.interface.marks_written_pages);
    let marks = spirv_ops(&s.spirv).iter().filter(|&&o| o == 242).count();
    assert_eq!(marks, 6, "page marks");
}

mod common;

use common::{translate, translate_with};
use relift_gpu_nv::program::{Program, Stage};
use relift_gpu_nv::translate::pseudo::{IPA_MUL, IPA_SAT};
use relift_gpu_nv::translate::{A2C_SPEC, ALPHA_FUNC_SPEC, HostProfile, Interp, Kind, Options};
use relift_gpu_nv::{Inst, Operand, RegMods};

fn word(op: u32, form: u32, fields: &[(u32, u32, u64)]) -> [u8; 16] {
    let mut w: u128 =
        u128::from(op) | (u128::from(form) << 9) | (7u128 << 12) | (7u128 << 110) | (7u128 << 113);
    for &(lo, len, v) in fields {
        let m = ((1u128 << len) - 1) << lo;
        w = (w & !m) | ((u128::from(v) << lo) & m);
    }
    w.to_le_bytes()
}

fn exit_and_halt(code: &mut Vec<u8>) {
    code.extend(word(0x14d, 4, &[(87, 3, 7)]));
    code.extend(word(0x147, 4, &[(34, 48, 0xffff_ffff_fffc), (87, 3, 7)]));
}

fn sph(kind: u32, stage: u32, extra: &[(usize, u32)]) -> Vec<u8> {
    let mut w = [0u32; 32];
    w[0] = kind | (4 << 5) | (stage << 10);
    for &(i, v) in extra {
        w[i] = v;
    }
    w.iter().flat_map(|x| x.to_le_bytes()).collect()
}

fn header_ok(words: &[u32]) {
    assert_eq!(words[0], 0x0723_0203);
    assert!(words[3] > 1);
}

#[test]
fn vertex_transform() {
    let mut b = sph(1, 1, &[]);
    b.extend(word(
        0x121,
        1,
        &[(16, 8, 8), (24, 8, 255), (40, 11, 0x80), (74, 2, 3)],
    ));
    b.extend(word(
        0x020,
        5,
        &[
            (16, 8, 0),
            (24, 8, 8),
            (32, 27, (3 << 22) | (0x10 << 6)),
            (84, 3, 4),
        ],
    ));
    b.extend(word(
        0x023,
        5,
        &[
            (16, 8, 0),
            (24, 8, 9),
            (32, 27, (3 << 22) | (0x14 << 6)),
            (64, 8, 0),
        ],
    ));
    b.extend(word(
        0x021,
        4,
        &[(16, 8, 1), (24, 8, 10), (32, 32, 0x3f80_0000)],
    ));
    b.extend(word(
        0x122,
        1,
        &[(24, 8, 255), (32, 8, 0), (40, 11, 0x70), (74, 2, 3)],
    ));
    b.extend(word(0x122, 1, &[(24, 8, 255), (32, 8, 8), (40, 11, 0x90)]));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(p.stage(), Stage::Vertex);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    header_ok(&s.spirv);
    assert_eq!(
        s.interface.cbufs.iter().copied().collect::<Vec<_>>(),
        vec![3]
    );
    assert!(s.interface.inputs.contains_key(&0));
    assert!(s.interface.outputs.contains(&1));
}

#[test]
fn fragment_perspective_color() {
    let mut b = sph(2, 5, &[(6, 0xaa), (18, 0xf)]);
    b.extend(word(
        0x126,
        1,
        &[
            (16, 8, 4),
            (64, 8, 0x7c >> 2),
            (24, 8, 255),
            (32, 8, 255),
            (81, 3, 7),
        ],
    ));
    for c in 0..4u64 {
        b.extend(word(
            0x126,
            1,
            &[
                (16, 8, c),
                (64, 8, (0x80 >> 2) + c),
                (24, 8, 255),
                (32, 8, 255),
                (81, 3, 7),
            ],
        ));
    }
    b.extend(word(0x108, 1, &[(16, 8, 4), (32, 8, 4), (74, 6, 4)]));
    for c in 0..4u64 {
        b.extend(word(
            0x020,
            1,
            &[(16, 8, c), (24, 8, 4), (32, 8, c), (84, 3, 4)],
        ));
    }
    exit_and_halt(&mut b);
    let s = translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
    header_ok(&s.spirv);
    assert_eq!(s.interface.inputs.get(&0), Some(&Some(Interp::Perspective)));
    assert!(s.interface.color_targets.contains(&0));
}

#[test]
fn rejects_what_it_cannot_translate() {
    let mut b = Vec::new();
    b.extend(word(0x150, 4, &[(24, 8, 4), (86, 1, 1), (87, 3, 7)]));
    exit_and_halt(&mut b);
    assert!(translate(&relift_gpu_sm86::program::parse(&b)).is_err());
}

fn bra(at: usize, to: usize, pred: Option<u64>) -> [u8; 16] {
    let rel = (to as i64 * 16 - (at as i64 * 16 + 16)) / 4;
    let mut f = vec![(34, 48, (rel as u64) & 0xffff_ffff_ffff), (87, 3, 7)];
    if let Some(p) = pred {
        f.push((12, 3, p));
    }
    word(0x147, 4, &f)
}

fn bra_mode(at: usize, to: usize, mode: u64) -> [u8; 16] {
    let rel = (to as i64 * 16 - (at as i64 * 16 + 16)) / 4;
    word(
        0x147,
        4,
        &[
            (32, 2, mode),
            (34, 48, (rel as u64) & 0xffff_ffff_ffff),
            (87, 3, 7),
        ],
    )
}

fn unsupported() -> [u8; 16] {
    word(0x150, 4, &[(24, 8, 4), (86, 1, 1), (87, 3, 7)])
}

#[test]
fn divergent_branch_never_leaves_the_fast_path() {
    let mut b = Vec::new();
    b.extend(bra_mode(0, 3, 2));
    b.extend(word(0x14d, 4, &[(87, 3, 7)]));
    b.extend(word(0x14d, 4, &[(87, 3, 7)]));
    b.extend(unsupported());
    exit_and_halt(&mut b);
    translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
}

#[test]
fn converged_branch_always_leaves_the_slow_path() {
    let mut b = Vec::new();
    b.extend(bra_mode(0, 2, 3));
    b.extend(unsupported());
    b.extend(word(0x14d, 4, &[(87, 3, 7)]));
    exit_and_halt(&mut b);
    translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
}

#[test]
fn exclusive_warpsync_is_rejected() {
    let sync = |exclusive: u64| word(0x148, 1, &[(32, 8, 14), (86, 1, exclusive), (87, 3, 7)]);
    let mut b = Vec::new();
    b.extend(sync(0));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[0], "WARPSYNC R14");
    translate(&p).unwrap_or_else(|e| panic!("{e}"));
    let mut b = Vec::new();
    b.extend(sync(1));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[0], "WARPSYNC.EXCLUSIVE R14");
    let e = translate(&p).err().map(|e| e.0);
    assert_eq!(e.as_deref(), Some("WARPSYNC.EXCLUSIVE"));
}

fn spirv_has(words: &[u32], opcode: u32) -> bool {
    let mut k = 5;
    while k < words.len() {
        let n = (words[k] >> 16) as usize;
        if words[k] & 0xffff == opcode {
            return true;
        }
        k += n.max(1);
    }
    false
}

#[test]
fn compute_loop_is_structured() {
    let mut b = Vec::new();
    b.extend(word(0x119, 1, &[(16, 8, 0), (72, 8, 33)]));
    b.extend(word(0x002, 4, &[(16, 8, 1), (32, 32, 0), (72, 4, 0xf)]));
    b.extend(word(
        0x010,
        4,
        &[
            (16, 8, 1),
            (24, 8, 1),
            (32, 32, 1),
            (64, 8, 255),
            (81, 3, 7),
            (84, 3, 7),
        ],
    ));
    b.extend(word(
        0x00c,
        1,
        &[
            (24, 8, 1),
            (32, 8, 0),
            (73, 1, 1),
            (76, 3, 1),
            (81, 3, 0),
            (84, 3, 7),
            (87, 3, 7),
        ],
    ));
    b.extend(bra(4, 2, Some(0)));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(p.stage(), Stage::Compute);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    header_ok(&s.spirv);
    assert!(spirv_has(&s.spirv, 246), "no OpLoopMerge");
}

fn bssy(n: u64, at: usize, to: usize) -> [u8; 16] {
    word(0x145, 4, &[(16, 4, n), (34, 48, rel(at, to))])
}

fn bsync(n: u64) -> [u8; 16] {
    word(0x141, 4, &[(16, 4, n), (87, 3, 7)])
}

fn vote_any() -> [u8; 16] {
    word(0x006, 1, &[(16, 8, 6), (72, 2, 1), (81, 3, 7), (87, 3, 7)])
}

/// `BSSY B0` to `to`, then `@P0 BRA 3` over a MOV (a VOTE when `inside`),
/// then a VOTE when `warp`.
fn region_with(to: usize, inside: bool, warp: bool) -> Program {
    let mut b = Vec::new();
    b.extend(bssy(0, 0, to));
    b.extend(bra(1, 3, Some(0)));
    if inside {
        b.extend(vote_any());
    } else {
        b.extend(mov_imm(1, 1));
    }
    b.extend(bsync(0));
    if warp {
        b.extend(vote_any());
    }
    exit_and_halt(&mut b);
    relift_gpu_sm86::program::parse(&b)
}

fn region(to: usize, warp: bool) -> Program {
    region_with(to, false, warp)
}

#[test]
fn bssy_must_reconverge_at_the_merge() {
    let p = region(3, true);
    assert_eq!(text(&p)[0], "BSSY B0, 0x30");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("bssy_region", &s.spirv);
    assert!(s.interface.host.max_reconvergence);
    translate(&region(2, false)).unwrap_or_else(|e| panic!("{e}"));
}

/// A BSSY whose target is no merge, with warp operations after it, is
/// funneled there: the lanes that go to the target and those that branch
/// past it meet at a new block that switches on which way each came, so the
/// lanes for the target enter it together, as after the guest's BSYNC.
#[test]
fn bssy_off_the_merge_is_funneled_to_its_target() {
    let p = region_with(2, true, true);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("bssy_funnel", &s.spirv);
    assert!(s.interface.host.max_reconvergence);
    assert!(spirv_has(&s.spirv, 251), "no OpSwitch");
    assert!(s.interface.host.inexact.is_empty());
}

/// A BSSY whose target is no merge of the structure is harmless when no
/// warp operation runs between it and the next merge: the lanes are
/// together again before anything can tell.
#[test]
fn bssy_off_the_merges_is_harmless_without_warp_operations() {
    let s = translate(&region(2, true)).unwrap_or_else(|e| panic!("{e}"));
    assert!(s.interface.host.max_reconvergence);
}

/// A loop left through `@P0 BREAK B0; @P0 BRA 4`, with a VOTE after the
/// loop and before (`early`) or after the BSYNC.
fn break_loop(early: bool) -> Program {
    let mut b = Vec::new();
    b.extend(bssy(0, 0, if early { 5 } else { 4 }));
    b.extend(word(0x142, 4, &[(12, 3, 0), (16, 4, 0), (87, 3, 7)]));
    b.extend(bra(2, 4, Some(0)));
    b.extend(bra(3, 1, None));
    if early {
        b.extend(vote_any());
        b.extend(bsync(0));
    } else {
        b.extend(bsync(0));
        b.extend(vote_any());
    }
    exit_and_halt(&mut b);
    relift_gpu_sm86::program::parse(&b)
}

#[test]
fn break_must_not_skip_warp_operations() {
    let p = break_loop(false);
    assert_eq!(text(&p)[1], "@P0 BREAK B0");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("break_loop", &s.spirv);
    assert!(s.interface.host.max_reconvergence);
    let e = translate(&break_loop(true)).err().map(|e| e.0);
    assert_eq!(
        e.as_deref(),
        Some("warp operation at 0x40 between BREAK and its BSYNC")
    );
}

/// `BSSY B1` around a loop in `BSSY B0` that `@P0 BREAK B0` leaves for
/// `out`, with a VOTE after `BSYNC B1`.
fn break_to_outer(out: usize) -> Program {
    let mut b = Vec::new();
    b.extend(bssy(1, 0, 8));
    b.extend(bssy(0, 1, 6));
    b.extend(word(0x142, 4, &[(12, 3, 0), (16, 4, 0), (87, 3, 7)]));
    b.extend(bra(3, out, Some(0)));
    b.extend(bra(4, 2, Some(1)));
    b.extend(bsync(0));
    b.extend(mov_imm(1, 1));
    b.extend(bsync(1));
    b.extend(vote_any());
    exit_and_halt(&mut b);
    relift_gpu_sm86::program::parse(&b)
}

/// Lanes that break out of an inner region and wait at the BSYNC of the
/// region around it are back with the rest before the warp operation.
#[test]
fn break_may_wait_at_an_outer_bsync() {
    let p = break_to_outer(7);
    assert_eq!(text(&p)[7], "BSYNC B1");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert!(s.interface.host.max_reconvergence);
    let e = translate(&break_to_outer(8)).err().map(|e| e.0);
    assert_eq!(
        e.as_deref(),
        Some("warp operation at 0x80 between BREAK and its BSYNC")
    );
}

fn bmov_from(dst: u64, state: u64) -> [u8; 16] {
    word(0x155, 1, &[(16, 8, dst), (24, 6, state), (87, 3, 7)])
}

#[test]
fn bmov_reads_the_active_mask_and_rejects_other_state() {
    let mut b = Vec::new();
    b.extend(bmov_from(2, 26));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[0], "BMOV.32 R2, MACTIVE");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("bmov_active", &s.spirv);
    assert_eq!(s.interface.host.warp_ops, 1);
    assert!(spirv_has(&s.spirv, 339), "no OpGroupNonUniformBallot");
    for (state, name) in [(24, "MEXITED"), (16, "THREAD_STATE_ENUM.0"), (1, "B1")] {
        let mut b = Vec::new();
        b.extend(bmov_from(2, state));
        exit_and_halt(&mut b);
        let e = translate(&relift_gpu_sm86::program::parse(&b))
            .err()
            .map(|e| e.0);
        assert_eq!(e, Some(format!("BMOV R2, {name}")));
    }
    let mut b = Vec::new();
    b.extend(word(0x156, 1, &[(24, 6, 1), (32, 8, 2), (87, 3, 7)]));
    b.extend(word(
        0x006,
        1,
        &[(12, 3, 0), (16, 8, 6), (72, 2, 1), (81, 3, 7), (87, 3, 7)],
    ));
    b.extend(bsync(1));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[0], "BMOV.32 B1, R2");
    let e = translate(&p).err().map(|e| e.0);
    assert_eq!(
        e.as_deref(),
        Some("BMOV changes a convergence barrier at 0x0")
    );
}

fn bmov_save(n: u64) -> [u8; 16] {
    word(0x155, 7, &[(16, 4, n), (24, 6, 26), (87, 3, 7)])
}

fn bmov_active_from(n: u64, pquad: bool) -> [u8; 16] {
    word(
        0x156,
        7,
        &[
            (16, 4, n),
            (24, 6, 26),
            (84, 1, u64::from(pquad)),
            (87, 3, 7),
        ],
    )
}

/// The active mask saved in a barrier, widened to whole quads and given
/// back around derivative work, as a fragment program does inside a
/// branch: `BMOV B15, MACTIVE; BMOV.PQUAD MACTIVE, B15; ...; BMOV MACTIVE,
/// B15`. The restore changes nothing; the widening is exact where the warp
/// has not diverged and reported where it has. A write of `MACTIVE` from a
/// barrier that does not hold the saved lanes is rejected.
#[test]
fn bmov_mactive_from_a_saved_barrier() {
    let pair = |b: &mut Vec<u8>| {
        b.extend(bmov_save(15));
        b.extend(bmov_active_from(15, true));
        b.extend(vote_any());
        b.extend(bmov_active_from(15, false));
    };
    let mut b = Vec::new();
    pair(&mut b);
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let t = text(&p);
    assert_eq!(t[0], "BMOV.32 B15, MACTIVE");
    assert_eq!(t[1], "BMOV.32.PQUAD MACTIVE, B15");
    assert_eq!(t[3], "BMOV.32 MACTIVE, B15");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert!(
        s.interface.host.inexact.is_empty(),
        "{:?}",
        s.interface.host.inexact
    );
    let mut b = Vec::new();
    b.extend(bssy(0, 0, 7));
    b.extend(bra(1, 6, Some(0)));
    pair(&mut b);
    b.extend(bsync(0));
    exit_and_halt(&mut b);
    let s = translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
    assert!(
        s.interface
            .host
            .inexact
            .iter()
            .any(|w| w.starts_with("BMOV.PQUAD MACTIVE in divergent")),
        "{:?}",
        s.interface.host.inexact
    );
    let mut b = Vec::new();
    b.extend(bmov_save(15));
    b.extend(bra(1, 2, Some(0)));
    b.extend(bmov_active_from(15, false));
    exit_and_halt(&mut b);
    let e = translate(&relift_gpu_sm86::program::parse(&b))
        .err()
        .map(|e| e.0);
    assert_eq!(e.as_deref(), Some("BMOV MACTIVE, B15"));
}

/// `BMOV.32.CLEAR RZ, B0`, as the compiler puts before a BSSY.
fn bmov_clear() -> [u8; 16] {
    0x000f_e200_0010_0000_0000_0000_00ff_7355u128.to_le_bytes()
}

#[test]
fn barrier_writes_set_again_before_a_read_are_harmless() {
    let mut b = Vec::new();
    b.extend(bmov_clear());
    b.extend(bssy(0, 1, 4));
    b.extend(bra(2, 4, Some(0)));
    b.extend(vote_any());
    b.extend(bsync(0));
    b.extend(vote_any());
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[0], "BMOV.32.CLEAR RZ, B0");
    translate(&p).unwrap_or_else(|e| panic!("{e}"));
    let mut b = Vec::new();
    b.extend(bssy(0, 0, 4));
    b.extend(bra(1, 3, Some(0)));
    b.extend(bmov_clear());
    b.extend(bsync(0));
    b.extend(vote_any());
    exit_and_halt(&mut b);
    let e = translate(&relift_gpu_sm86::program::parse(&b))
        .err()
        .map(|e| e.0);
    assert_eq!(
        e.as_deref(),
        Some("BMOV changes a convergence barrier at 0x20"),
        "a clear inside the region its BSYNC reads"
    );
}

#[test]
fn warpsync_orders_lanes_or_is_reported() {
    let sync = |guard: u64| {
        word(
            0x148,
            4,
            &[(12, 3, guard), (32, 32, 0xffff_ffff), (87, 3, 7)],
        )
    };
    let mut b = Vec::new();
    b.extend(sync(7));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[0], "WARPSYNC 0xffffffff");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("warpsync", &s.spirv);
    assert!(spirv_has(&s.spirv, 224), "no OpControlBarrier");
    assert!(s.interface.host.inexact.is_empty());
    let mut b = Vec::new();
    b.extend(sync(0));
    exit_and_halt(&mut b);
    let s = translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
    assert!(!spirv_has(&s.spirv, 224));
    assert_eq!(
        s.interface.host.inexact,
        vec!["WARPSYNC in divergent control flow".to_string()]
    );
}

fn vote_all(dst: u64) -> [u8; 16] {
    word(
        0x006,
        1,
        &[(16, 8, dst), (72, 2, 0), (81, 3, 7), (87, 3, 7)],
    )
}

fn warpsync_reg(r: u64) -> [u8; 16] {
    word(0x148, 1, &[(32, 8, r), (87, 3, 7)])
}

/// `@P0 BRA 4` over `VOTE.ALL R2, PT; WARPSYNC R<mask>; VOTE.ANY`: the
/// branch splits the warp, and the WARPSYNC waits for the lanes the VOTE.ALL
/// saw (`mask` 2), which run together, or for some other register.
fn warpsync_after_split(mask: u64) -> Program {
    let mut b = Vec::new();
    b.extend(bra(0, 4, Some(0)));
    b.extend(vote_all(2));
    b.extend(warpsync_reg(mask));
    b.extend(vote_any());
    exit_and_halt(&mut b);
    relift_gpu_sm86::program::parse(&b)
}

/// A WARPSYNC of the mask of the lanes running it, in divergent flow,
/// waits for the lanes that run together: a subgroup barrier where the
/// host reconverges maximally, nothing on the software warp path.
#[test]
fn warpsync_of_the_running_lanes_is_exact() {
    let p = warpsync_after_split(2);
    assert_eq!(text(&p)[1], "VOTE.ALL R2, PT, PT");
    assert_eq!(text(&p)[2], "WARPSYNC R2");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert!(s.interface.host.max_reconvergence);
    assert!(spirv_has(&s.spirv, 224), "no OpControlBarrier");
    assert!(
        s.interface.host.inexact.is_empty(),
        "{:?}",
        s.interface.host.inexact
    );
    let s = translate(&warpsync_after_split(3)).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(
        s.interface.host.inexact,
        vec!["WARPSYNC in divergent control flow".to_string()]
    );
}

#[test]
fn guarded_exit_returns_in_place() {
    let mut b = Vec::new();
    b.extend(word(
        0x00c,
        1,
        &[
            (24, 8, 1),
            (32, 8, 0),
            (76, 3, 2),
            (81, 3, 0),
            (84, 3, 7),
            (87, 3, 7),
        ],
    ));
    b.extend(word(0x14d, 4, &[(12, 3, 0), (87, 3, 7)]));
    b.extend(word(0x002, 4, &[(16, 8, 1), (32, 32, 7), (72, 4, 0xf)]));
    exit_and_halt(&mut b);
    let s = translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
    assert!(spirv_has(&s.spirv, 247), "no OpSelectionMerge");
}

#[test]
fn directed_rounding_translates() {
    for rnd in 1..4u64 {
        let mut b = Vec::new();
        for op in [0x021u32, 0x020, 0x023] {
            let mut f = vec![(16, 8, 2), (24, 8, 0), (32, 8, 1), (78, 2, rnd)];
            if op == 0x023 {
                f.push((64, 8, 3));
            }
            if op == 0x020 {
                f.push((84, 3, 4));
            }
            b.extend(word(op, 1, &f));
        }
        exit_and_halt(&mut b);
        let p = relift_gpu_sm86::program::parse(&b);
        let text: Vec<String> = p
            .code
            .clone()
            .into_iter()
            .map(|d| d.map(|i| i.to_string()).unwrap_or_default())
            .collect();
        assert!(
            text[0].contains(["M", "P", "Z"].map(|c| format!(".R{c}"))[rnd as usize - 1].as_str()),
            "{text:?}"
        );
        translate(&p).unwrap_or_else(|e| panic!("{e}"));
    }
}

#[test]
fn mufu_forms_translate_with_and_without_the_model() {
    const F_DIV: u32 = 136;
    let names = [
        "COS", "SIN", "EX2", "LG2", "RCP", "RSQ", "RCP64H", "RSQ64H", "SQRT", "TANH",
    ];
    for (k, name) in names.iter().enumerate() {
        for half in [false, true] {
            let mut b = Vec::new();
            b.extend(word(
                0x108,
                1,
                &[
                    (16, 8, 4),
                    (32, 8, 5),
                    (74, 6, k as u64),
                    (73, 1, u64::from(half)),
                ],
            ));
            exit_and_halt(&mut b);
            let p = relift_gpu_sm86::program::parse(&b);
            let shown = text(&p)[0].clone();
            let want = format!("MUFU.{name}{}", if half { ".F16" } else { "" });
            assert!(shown.starts_with(&want), "{shown}");
            for exact_mufu in [false, true] {
                let r = translate_with(
                    &p,
                    &Options {
                        exact_mufu,
                        ..Options::default()
                    },
                );
                if half && name.ends_with("64H") {
                    assert!(r.is_err(), "{want}");
                    continue;
                }
                let s = r.unwrap_or_else(|e| panic!("{want}: {e}"));
                header_ok(&s.spirv);
                dump(&format!("mufu_{name}_{half}_{exact_mufu}"), &s.spirv);
                assert_eq!(
                    spirv_has(&s.spirv, F_DIV),
                    *name == "RCP" && !exact_mufu,
                    "{want} exact {exact_mufu}"
                );
            }
        }
    }
}

fn text(p: &Program) -> Vec<String> {
    p.code
        .clone()
        .into_iter()
        .map(|d| {
            d.map(|i| i.to_string())
                .unwrap_or_else(|e| format!("?? {e}"))
        })
        .collect()
}

fn dump(name: &str, words: &[u32]) {
    if let Some(dir) = std::env::var_os("RELIFT_SM86_DUMP") {
        let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        let path = std::path::Path::new(&dir).join(format!("{name}.spv"));
        relift_cache::write_if_changed(&path, &bytes).unwrap_or_else(|e| panic!("{name}: {e}"));
    }
}

fn decorated(words: &[u32], deco: u32) -> bool {
    let mut k = 5;
    while k < words.len() {
        let n = (words[k] >> 16) as usize;
        if words[k] & 0xffff == 71 && words.get(k + 2) == Some(&deco) {
            return true;
        }
        k += n.max(1);
    }
    false
}

fn builtin_used(words: &[u32], b: u32) -> bool {
    let mut k = 5;
    while k < words.len() {
        let n = (words[k] >> 16) as usize;
        if words[k] & 0xffff == 71 && words.get(k + 2) == Some(&11) && words.get(k + 3) == Some(&b)
        {
            return true;
        }
        k += n.max(1);
    }
    false
}

fn ipa(dst: u64, attr: u32, loc: u64, freq: u64) -> [u8; 16] {
    word(
        0x126,
        1,
        &[
            (16, 8, dst),
            (64, 8, u64::from(attr >> 2)),
            (24, 8, 255),
            (32, 8, 255),
            (76, 2, loc),
            (78, 2, freq),
            (81, 3, 7),
        ],
    )
}

fn mov_imm(dst: u64, v: u32) -> [u8; 16] {
    word(
        0x002,
        4,
        &[(16, 8, dst), (32, 32, u64::from(v)), (72, 4, 0xf)],
    )
}

fn rel(at: usize, to: usize) -> u64 {
    (((to as i64 * 16 - (at as i64 * 16 + 16)) / 4) as u64) & 0xffff_ffff_ffff
}

fn isetp(pdst: u64, a: u64, b: u64, cmp: u64) -> [u8; 16] {
    word(
        0x00c,
        1,
        &[
            (24, 8, a),
            (32, 8, b),
            (73, 1, 1),
            (76, 3, cmp),
            (81, 3, pdst),
            (84, 3, 7),
            (87, 3, 7),
        ],
    )
}

#[test]
fn fragment_inputs_follow_the_sph() {
    let mut b = sph(2, 5, &[(6, 0x0f | (0xaa << 8) | (0x55 << 16)), (18, 0xf)]);
    b.extend(ipa(0, 0x80, 1, 0));
    b.extend(ipa(1, 0x84, 1, 0));
    b.extend(ipa(4, 0x7c, 0, 0));
    b.extend(ipa(2, 0x90, 0, 0));
    b.extend(ipa(3, 0x90, 1, 0));
    b.extend(ipa(5, 0xa0, 0, 1));
    b.extend(ipa(6, 0x3fc, 0, 1));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let t = text(&p);
    assert_eq!(t[0], "IPA.PASS.CENTROID R0, a[0x80]");
    assert_eq!(t[5], "IPA.CONSTANT R5, a[0xa0]");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("fragment_inputs", &s.spirv);
    let i = &s.interface.inputs;
    assert_eq!(i.get(&0), Some(&Some(Interp::Linear)));
    assert_eq!(i.get(&1), Some(&Some(Interp::Perspective)));
    assert_eq!(i.get(&2), Some(&Some(Interp::Flat)));
    assert!(decorated(&s.spirv, 16), "no Centroid");
    assert!(decorated(&s.spirv, 13), "no NoPerspective");
    assert!(spirv_has(&s.spirv, 12), "no InterpolateAtCentroid");
    assert!(builtin_used(&s.spirv, 17), "no FrontFacing");
}

/// Maxwell's `IPA.MUL` and `IPA` with `.SAT` reach the translator as
/// pseudo-instructions in place of a word; the multiplier may be the
/// destination.
#[test]
fn maxwell_ipa_pseudo_instructions() {
    let mut b = sph(2, 5, &[(6, 0xaa), (18, 0xf)]);
    b.extend(ipa(0, 0x7c, 0, 0));
    for _ in 0..3 {
        b.extend(word(0x118, 4, &[]));
    }
    exit_and_halt(&mut b);
    let mut p = relift_gpu_sm86::program::parse(&b);
    let reg = |n| Operand::Reg(n, RegMods::default());
    let attr = |offset| Operand::Attr {
        index: None,
        offset,
        patch: false,
    };
    let pseudo = |mods: &[&'static str], ops: Vec<Operand>| {
        let mut i = Inst::new("IPA");
        i.mods = mods.to_vec();
        i.ops = ops;
        i
    };
    p.code[1] = Ok(pseudo(&["PASS", IPA_MUL], vec![reg(1), attr(0x80), reg(0)]));
    p.code[2] = Ok(pseudo(
        &["PASS", IPA_MUL, IPA_SAT],
        vec![reg(0), attr(0x84), reg(0)],
    ));
    p.code[3] = Ok(pseudo(&["PASS", IPA_SAT], vec![reg(2), attr(0x88)]));
    let t = text(&p);
    assert_eq!(t[1], "IPA.PASS.MUL R1, a[0x80], R0");
    assert_eq!(t[2], "IPA.PASS.MUL.SAT R0, a[0x84], R0");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("maxwell_ipa", &s.spirv);
    assert_eq!(s.interface.inputs.get(&0), Some(&Some(Interp::Perspective)));
    p.code[1] = Ok(pseudo(&["PASS", IPA_MUL], vec![reg(1), attr(0x80)]));
    let e = translate(&p).err().expect("a MUL without its multiplier");
    assert!(e.0.contains("pseudo IPA form"), "{}", e.0);
}

#[test]
fn fragment_outputs_take_four_registers_per_target() {
    let mut b = sph(2, 5, &[(18, 0x31), (19, 3)]);
    exit_and_halt(&mut b);
    let s = translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
    dump("fragment_outputs", &s.spirv);
    assert_eq!(
        s.interface
            .color_targets
            .iter()
            .copied()
            .collect::<Vec<_>>(),
        vec![0, 1]
    );
    assert!(s.interface.writes_depth && s.interface.writes_sample_mask);
    assert!(builtin_used(&s.spirv, 22) && builtin_used(&s.spirv, 20));
}

#[test]
fn vertex_system_values() {
    let mut b = sph(1, 1, &[(18, 0x3)]);
    b.extend(word(
        0x121,
        1,
        &[(16, 8, 0), (24, 8, 255), (32, 8, 255), (40, 11, 0x2fc)],
    ));
    b.extend(word(
        0x121,
        1,
        &[(16, 8, 1), (24, 8, 255), (32, 8, 255), (40, 11, 0x2f8)],
    ));
    for a in [0x5c, 0x6c, 0x2c0, 0x2c4] {
        b.extend(word(
            0x122,
            1,
            &[(24, 8, 255), (32, 8, 0), (40, 11, a), (64, 8, 255)],
        ));
    }
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[2], "AST a[0x5c], R0");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("vertex_system_values", &s.spirv);
    assert_eq!(s.interface.clip_distances, 2);
    for bi in [42, 43, 1, 3] {
        assert!(builtin_used(&s.spirv, bi), "built-in {bi}");
    }
}

/// The instance id counts from 0 in each draw (NVK builds InstanceIndex
/// as the instance id plus BaseInstance), so it reads InstanceIndex minus
/// BaseInstance; the vertex id is VertexIndex.
#[test]
fn instance_id_leaves_out_the_base_instance() {
    let mut b = sph(1, 1, &[]);
    b.extend(word(
        0x121,
        1,
        &[(16, 8, 1), (24, 8, 255), (32, 8, 255), (40, 11, 0x2f8)],
    ));
    exit_and_halt(&mut b);
    let s = translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
    dump("instance_id", &s.spirv);
    assert!(builtin_used(&s.spirv, 43) && builtin_used(&s.spirv, 4425));
    assert!(s.interface.builtins.contains(&4425));
    assert_eq!(count_op(&s.spirv, 130), 1, "one ISub");
    assert!(
        s.spirv.windows(2).any(|w| w == [0x0002_0011, 4427]),
        "no DrawParameters"
    );
}

/// Each render target's output has the numeric type of its format and
/// takes the register bits unchanged.
#[test]
fn integer_render_targets_get_integer_outputs() {
    let mut b = sph(2, 5, &[(18, 0x111)]);
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let o = Options {
        color_kinds: [(1, Kind::Uint), (2, Kind::Sint)].into(),
        ..Options::default()
    };
    let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{e}"));
    dump("integer_targets", &s.spirv);
    // OpTypeInt/OpTypeFloat ids, then the element type of each output
    // variable's pointer, by Location.
    let w = &s.spirv;
    let mut ty = std::collections::BTreeMap::new();
    let mut ptr = std::collections::BTreeMap::new();
    let mut vec = std::collections::BTreeMap::new();
    let mut var = std::collections::BTreeMap::new();
    let mut loc = std::collections::BTreeMap::new();
    let mut k = 5;
    while k < w.len() {
        let n = (w[k] >> 16) as usize;
        match w[k] & 0xffff {
            21 => {
                ty.insert(w[k + 1], if w[k + 3] == 1 { "i32" } else { "u32" });
            }
            22 => {
                ty.insert(w[k + 1], "f32");
            }
            23 => {
                vec.insert(w[k + 1], w[k + 2]);
            }
            32 if w[k + 2] == 3 => {
                ptr.insert(w[k + 1], w[k + 3]);
            }
            59 if w[k + 3] == 3 => {
                var.insert(w[k + 2], w[k + 1]);
            }
            71 if w[k + 2] == 30 => {
                loc.insert(w[k + 3], w[k + 1]);
            }
            _ => {}
        }
        k += n.max(1);
    }
    let kind = |t: u32| ty[&vec[&ptr[&var[&loc[&t]]]]];
    assert_eq!([kind(0), kind(1), kind(2)], ["f32", "u32", "i32"]);
}

fn count_op(words: &[u32], opcode: u32) -> usize {
    let mut k = 5;
    let mut n = 0;
    while k < words.len() {
        if words[k] & 0xffff == opcode {
            n += 1;
        }
        k += ((words[k] >> 16) as usize).max(1);
    }
    n
}

fn call(at: usize, to: usize) -> [u8; 16] {
    word(0x144, 4, &[(34, 48, rel(at, to)), (86, 1, 1), (87, 3, 7)])
}

fn ret(at: usize, reg: u64) -> [u8; 16] {
    word(
        0x150,
        4,
        &[(24, 8, reg), (34, 48, rel(at, 0)), (86, 1, 1), (87, 3, 7)],
    )
}

fn exit() -> [u8; 16] {
    word(0x14d, 4, &[(87, 3, 7)])
}

fn halt(at: usize) -> [u8; 16] {
    bra(at, at, None)
}

fn calls(sites: usize) -> Vec<u8> {
    let mut b = Vec::new();
    for k in 0..2 {
        if k < sites {
            b.extend(mov_imm(4, 0x20 * (k as u32 + 1)));
            b.extend(call(2 * k + 1, 5));
        } else {
            b.extend(word(0x118, 4, &[]));
            b.extend(word(0x118, 4, &[]));
        }
    }
    b.extend(exit());
    b.extend(word(
        0x010,
        4,
        &[
            (16, 8, 1),
            (24, 8, 1),
            (32, 32, 1),
            (64, 8, 255),
            (81, 3, 7),
            (84, 3, 7),
        ],
    ));
    b.extend(ret(6, 4));
    b.extend(halt(7));
    b
}

#[test]
fn call_is_inlined_at_each_site() {
    let one =
        translate(&relift_gpu_sm86::program::parse(&calls(1))).unwrap_or_else(|e| panic!("{e}"));
    let p = relift_gpu_sm86::program::parse(&calls(2));
    let t = text(&p);
    assert_eq!(t[1], "CALL.REL.NOINC 0x50");
    assert_eq!(t[6], "RET.REL.NODEC R4 0x0");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("call", &s.spirv);
    assert_eq!(
        count_op(&s.spirv, 128),
        2 * count_op(&one.spirv, 128),
        "callee not inlined twice"
    );
}

#[test]
fn call_without_return_address_is_rejected() {
    let mut b = Vec::new();
    b.extend(call(0, 2));
    b.extend(exit());
    b.extend(ret(2, 4));
    b.extend(halt(3));
    let e = translate(&relift_gpu_sm86::program::parse(&b))
        .err()
        .map(|e| e.0);
    assert_eq!(e.as_deref(), Some("CALL without its return address"));
}

#[test]
fn return_address_must_be_in_the_register_ret_reads() {
    let mut b = calls(1);
    b[..16].copy_from_slice(&mov_imm(5, 0x20));
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[0], "MOV R5, 0x20");
    let e = translate(&p).err().map(|e| e.0);
    assert_eq!(e.as_deref(), Some("CALL without its return address"));
    let mut b = calls(1);
    b[5 * 16..6 * 16].copy_from_slice(&word(0x002, 1, &[(16, 8, 6), (32, 8, 4), (72, 4, 0xf)]));
    b[6 * 16..7 * 16].copy_from_slice(&ret(6, 6));
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[5], "MOV R6, R4");
    translate(&p).unwrap_or_else(|e| panic!("{e}"));
    b[..16].copy_from_slice(&mov_imm(5, 0x20));
    let e = translate(&relift_gpu_sm86::program::parse(&b))
        .err()
        .map(|e| e.0);
    assert_eq!(e.as_deref(), Some("CALL without its return address"));
}

#[test]
fn brx_becomes_a_switch() {
    let mut b = Vec::new();
    b.extend(word(0x145, 4, &[(16, 4, 0), (34, 48, rel(0, 7))]));
    b.extend(mov_imm(2, 0x50));
    b.extend(word(
        0x149,
        4,
        &[(24, 8, 2), (34, 48, rel(2, 0)), (87, 3, 7)],
    ));
    b.extend(mov_imm(1, 1));
    b.extend(bra(4, 7, None));
    b.extend(mov_imm(1, 2));
    b.extend(bra(6, 7, None));
    b.extend(word(0x141, 4, &[(16, 4, 0), (87, 3, 7)]));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let t = text(&p);
    assert!(t[2].starts_with("BRX R2 -0x30"), "{}", t[2]);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("brx", &s.spirv);
    assert!(spirv_has(&s.spirv, 251), "no OpSwitch");
    let w = &s.spirv;
    let mut k = 5;
    let mut checked = false;
    while k < w.len() {
        let n = (w[k] >> 16) as usize;
        if w[k] & 0xffff == 251 {
            let default = w[k + 2];
            let cases: Vec<u32> = w[k + 3..k + n].chunks(2).map(|c| c[1]).collect();
            assert_eq!(cases.len(), 2, "{:?}", &w[k..k + n]);
            assert!(!cases.contains(&default), "default falls into a case");
            let at = (5..w.len())
                .find(|&j| w[j] == (2 << 16 | 248) && w[j + 1] == default)
                .expect("default label");
            assert_eq!(w[at + 2] & 0xffff, 253, "default does not return");
            checked = true;
        }
        k += n.max(1);
    }
    assert!(checked);
}

#[test]
fn loop_with_two_exits_dispatches_after_it() {
    let mut b = Vec::new();
    b.extend(mov_imm(0, 0));
    b.extend(word(
        0x010,
        4,
        &[
            (16, 8, 0),
            (24, 8, 0),
            (32, 32, 1),
            (64, 8, 255),
            (81, 3, 7),
            (84, 3, 7),
        ],
    ));
    b.extend(isetp(0, 0, 8, 2));
    b.extend(bra(3, 8, Some(0)));
    b.extend(isetp(1, 0, 9, 2));
    b.extend(bra(5, 10, Some(1)));
    b.extend(bra(6, 1, None));
    b.extend(word(0x118, 4, &[]));
    b.extend(mov_imm(1, 1));
    b.extend(bra(9, 11, None));
    b.extend(mov_imm(1, 2));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("loop_two_exits", &s.spirv);
    assert!(spirv_has(&s.spirv, 246), "no OpLoopMerge");
    assert!(spirv_has(&s.spirv, 251), "no exit dispatch");
}

#[test]
fn kill_demotes_and_goes_on() {
    let mut b = sph(2, 5, &[(6, 0xaa), (18, 0xf)]);
    b.extend(ipa(0, 0x70, 0, 0));
    b.extend(mov_imm(9, 0x4200_0000));
    b.extend(isetp(0, 0, 9, 1));
    b.extend(word(0x15b, 4, &[(12, 3, 0), (87, 3, 7)]));
    b.extend(ipa(1, 0x80, 0, 0));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[3], "@P0 KILL");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("kill", &s.spirv);
    assert!(s.interface.kills);
    assert!(spirv_has(&s.spirv, 5380), "no OpDemoteToHelperInvocation");
    assert!(!spirv_has(&s.spirv, 252), "OpKill");
}

#[test]
fn indexed_ipa_selects_among_the_sph_inputs() {
    let mut b = sph(2, 5, &[(6, 0x00aa_aaaa), (18, 0x1)]);
    b.extend(word(
        0x126,
        2,
        &[
            (16, 8, 0),
            (64, 8, 0x84 >> 2),
            (24, 6, 4),
            (91, 1, 1),
            (32, 8, 255),
            (81, 3, 7),
        ],
    ));
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[0], "IPA.PASS R0, a[UR4+0x84]");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("indexed_ipa", &s.spirv);
    assert_eq!(s.interface.inputs.len(), 3);
    assert_eq!(count_op(&s.spirv, 169), 12, "one select per component");
}

#[test]
fn quad_swizzle_add_translates_in_both_stages() {
    for fragment in [false, true] {
        let mut b = if fragment {
            sph(2, 5, &[(18, 0xf)])
        } else {
            Vec::new()
        };
        b.extend(word(
            0x022,
            4,
            &[
                (16, 8, 0),
                (24, 8, 1),
                (64, 8, 2),
                (32, 8, 0xe4),
                (80, 1, 1),
            ],
        ));
        exit_and_halt(&mut b);
        let p = relift_gpu_sm86::program::parse(&b);
        assert!(p.code[0].as_ref().is_ok_and(|i| i.mnemonic == "FSWZADD"));
        translate(&p).unwrap_or_else(|e| panic!("{e}"));
    }
}

// SM86 kernels from nvcc -arch=sm_86 -O3 for BAR placement, each storing
// a shared word another thread wrote:
//   UNIFORM_EXIT:   if (blockIdx.x >= n) return; sm[t] = t; __syncthreads();
//   LANE_EXIT:      if (t & 1) return; sm[t] = t; __syncthreads();
//   TWO_SITES:      if (t & 16) { sm[t] += n; __syncthreads(); } else __syncthreads();
//   UNIFORM_BRANCH: if (n > 4) __syncthreads();
//   VARYING_LOOP:   for (i = 0; i < (t & 7); i++) { sm[t] += sm[255 - t]; __syncthreads(); }
//   UNIFORM_LOOP:   for (i = 0; i < n; i++) { sm[t] += sm[255 - t]; __syncthreads(); }
// On the RTX 3090 a lane that exited counts as arrived (LANE_EXIT syncs
// the rest) and TWO_SITES hangs. Vulkan needs every invocation of the
// workgroup at the same OpControlBarrier, so only the uniform ones
// translate.
const UNIFORM_EXIT: [u128; 16] = [
    0x000fe400078e00ff00000a00ff017624,
    0x000e2400000025000000000000027919,
    0x001fda0003f0607000005a0002007a0c,
    0x000fea0003800000000000000000094d,
    0x000e2200000021000000000000077919,
    0x000fe20000000f000000000400037802,
    0x000fe40000000a000000460000047ab9,
    0x001fe200000048000000000707007388,
    0x000fe20007ffe1ff000000ff07007810,
    0x000fe400078e02070000010002027824,
    0x000fe400000100000000000000007b1d,
    0x000fc800078e00030000580002027625,
    0x000e2800000048000000000000057984,
    0x001fe2000c1019040000000502007986,
    0x000fea0003800000000000000000794d,
    0x000fc0000383fffffffffff000007947,
];
const LANE_EXIT: [u128; 17] = [
    0x000fe400078e00ff00000a00ff017624,
    0x000e2400000021000000000000047919,
    0x001fc800078ec0ff0000000104007812,
    0x000fda0003f05070000000010000780c,
    0x000fea0003800000000000000000894d,
    0x000fe200000048000000000404007388,
    0x000fe20007ffe1ff000000ff04007810,
    0x000fe20000000a000000460000047ab9,
    0x000fe20000000f000000000400077802,
    0x000fec00000100000000000000007b1d,
    0x000e2800000025000000000000037919,
    0x000e6200000048000000000000057984,
    0x001fc800078e02040000010003027824,
    0x000fca00078e00070000580002027625,
    0x002fe2000c1019040000000502007986,
    0x000fea0003800000000000000000794d,
    0x000fc0000383fffffffffff000007947,
];
const TWO_SITES: [u128; 20] = [
    0x000fe400078e00ff00000a00ff017624,
    0x000e2200000021000000000000077919,
    0x000fc60000000f000000000400037802,
    0x000e6200000025000000000000027919,
    0x001fc6000780c0ff0000001007ff7812,
    0x000fe200000048000000000707007388,
    0x000fe20007ffe1ff000000ff07047810,
    0x002fc800078e02070000010002027824,
    0x000fc800078e00030000580002027625,
    0x000fe20007ffe0ff00005a0007000a10,
    0x000fe80003800000ffffffff00000948,
    0x000fe800000048000000000007000388,
    0x000fec00000100000000000000000b1d,
    0x000fe20003800000ffffffff00008948,
    0x000fe20000000a000000460000047ab9,
    0x000fec00000100000000000000008b1d,
    0x000e2800000048000000000004057984,
    0x001fe2000c1019040000000502007986,
    0x000fea0003800000000000000000794d,
    0x000fc0000383fffffffffff000007947,
];
const UNIFORM_BRANCH: [u128; 16] = [
    0x000fe400078e00ff00000a00ff017624,
    0x000e2200000021000000000000047919,
    0x000fe200078e00ff00005a00ff007624,
    0x000fe20000000f000000000400077802,
    0x000fe20000000a000000460000047ab9,
    0x000e6400000025000000000000037919,
    0x000fe40003f06070000000050000780c,
    0x001fe200000048000000000404007388,
    0x000fd40007ffe1ff000000ff04007810,
    0x000fe200000100000000000000000b1d,
    0x002fca00078e40ff0000000403027211,
    0x000fe200078e00070000580002027625,
    0x000e2800000048000000000000057984,
    0x001fe2000c1019040000000502007986,
    0x000fea0003800000000000000000794d,
    0x000fc0000383fffffffffff000007947,
];
const VARYING_LOOP: [u128; 25] = [
    0x000fe400078e00ff00000a00ff017624,
    0x000e2200000021000000000000097919,
    0x000fe200078e00ff00000004ff037424,
    0x000fe40000000a000000460000047ab9,
    0x000e6200000025000000000000027919,
    0x001fc60007ffe1ff000000ff09047810,
    0x0001e200000048000000000909007388,
    0x000fe4000780c0ff0000000709007812,
    0x002fe200078e40ff0000000902027211,
    0x00006800000048000000000004057984,
    0x000fcc00078e00030000580002027625,
    0x000fea0003800000000000a000008947,
    0x001fc600078e00ff000000ffff077224,
    0x001e2200000048000000000009067984,
    0x000fe20003800000ffffffff00007948,
    0x000fc80007ffe0ff0000000107077810,
    0x000fe40003f06070000000000700720c,
    0x003fca0007ffe0ff0000000506067210,
    0x0001e800000048000000000609007388,
    0x000fec00000100000000000000007b1d,
    0x00006200000048000000000004057984,
    0x000fea000383ffffffffff7000008947,
    0x003fe2000c1019040000000502007986,
    0x000fea0003800000000000000000794d,
    0x000fc0000383fffffffffff000007947,
];
const UNIFORM_LOOP: [u128; 24] = [
    0x000fe400078e00ff00000a00ff017624,
    0x000e2200000021000000000000077919,
    0x000fe20003f0527000005a00ff007a0c,
    0x000fe200078e00ff00000004ff037424,
    0x000fe20000000a000000460000047ab9,
    0x000e6200000025000000000000027919,
    0x001fc60007ffe1ff000000ff07007810,
    0x0001e200000048000000000707007388,
    0x002fc600078e40ff0000000702027211,
    0x00006400000048000000000000057984,
    0x000fe200078e00030000580002027625,
    0x000fea00038000000000009000008947,
    0x000fc800078e00ff000000ffff047224,
    0x006ea200000048000000000007067984,
    0x000fc80007ffe0ff0000000104047810,
    0x000fe40003f0607000005a0004007a0c,
    0x006fca0007ffe0ff0000000506067210,
    0x0003e800000048000000000607007388,
    0x000fec00000100000000000000007b1d,
    0x0002a200000048000000000000057984,
    0x000fea000383ffffffffff8000008947,
    0x006fe2000c1019040000000502007986,
    0x000fea0003800000000000000000794d,
    0x000fc0000383fffffffffff000007947,
];

/// `if (g >= n) return; ... if (g != 0) for (k < iters) { __syncthreads(); ...
/// __syncthreads(); }` from nvcc for SM86: a varying EXIT before BARs, then
/// a varying branch that skips a loop of BARs.
const SKIPPED_BARS: [u128; 81] = [
    0x000fe400078e00ff00000a00ff017624,
    0x000e2800000025000000000000007919,
    0x000e24000000210000000000000b7919,
    0x001fca00078e020b0000004000007824,
    0x000fda0003f0607000005c0000007a0c,
    0x000fea0003800000000000000000094d,
    0x000fe200078e00ff00000004ff037424,
    0x000fc60000000a000000460000047ab9,
    0x000fca00078e00030000580000027625,
    0x000ea2000c1e19000000000402047981,
    0x000fe20003f0527000005d00ff007a0c,
    0x000fe60003800000000003e000007945,
    0x000fe20004702670000000ff0000720c,
    0x0041d80000004800000000040b007388,
    0x000fea0003800000000003a000000947,
    0x000fca00078e00ff00005d00ff027624,
    0x040fe40007ffe0ffffffffff02037810,
    0x000fe400078ec0ff0000000302027812,
    0x000fe20003f26070000000030300780c,
    0x000fe200078e00ff000000ffff037224,
    0x000fd60003f05270000000ff0200720c,
    0x000fea00038000000000025000009947,
    0x000fe20007ffe0ff000000040b057810,
    0x000fe200078e00ff000000ffff037224,
    0x000fe40007ffe0ff80005d00020c7a10,
    0x000fe20003800000ffffffff00007948,
    0x000fe200000100000000000000007b1d,
    0x000fe200078e00ff0000000405067824,
    0x000fe40007ffe0ff0000000403037810,
    0x000fe40007ffe0ff0000000405057810,
    0x040fe40007ffe0fffffffff406077810,
    0x000fc40007ffe0fffffffff806087810,
    0x000fe400078ec0ff000000fc07077812,
    0x000fe400078ec0ff000000fc08087812,
    0x040fe40007ffe0fffffffffc060a7810,
    0x000fe400078ec0ff000000fc06067812,
    0x000fe200078ec0ff000000fc0a0a7812,
    0x000e6400000008000000000007077984,
    0x003fc400078e02070000000304047824,
    0x000fec00000100000000000000007b1d,
    0x000fe80000004800000000040b007388,
    0x000fec00000100000000000000007b1d,
    0x00006400000008000000000008097984,
    0x001fc400078e0203000000010c087824,
    0x000fe600000100000000000000007b1d,
    0x000fe20003f25270000000ff0800720c,
    0x002fca00078e02090000000304097824,
    0x000fe80000004800000000090b007388,
    0x000fec00000100000000000000007b1d,
    0x000e240000000800000000000a0a7984,
    0x001fc400078e020a0000000309077824,
    0x000fec00000100000000000000007b1d,
    0x000fe80000004800000000070b007388,
    0x000fec00000100000000000000007b1d,
    0x000e2400000008000000000006067984,
    0x001fc400078e02060000000307047824,
    0x000fec00000100000000000000007b1d,
    0x0001e20000004800000000040b007388,
    0x000fea000383fffffffffde000001947,
    0x000fea0003800000000000d000008947,
    0x000fc60007ffe003000000010b037810,
    0x000fe20003800000ffffffff00007948,
    0x000fe200000100000000000000007b1d,
    0x000fe200078e00ff0000000403057824,
    0x000fc40007ffe0ffffffffff02027810,
    0x000fe40007ffe0ff0000000103037810,
    0x000fe400078ec0ff000000fc05057812,
    0x000fc80003f05270000000ff0200720c,
    0x000e6400000008000000000005057984,
    0x003fe400078e02050000000304047824,
    0x000fec00000100000000000000007b1d,
    0x0001e20000004800000000040b007388,
    0x000fea000383ffffffffff4000000947,
    0x000fea00038000000000000000007941,
    0x000fe200078e00ff010000000b057824,
    0x000fc800078010ff00005a0000027a11,
    0x000fe400000f14ff00005b0000037a11,
    0x000fca00078e3cff0000000504057212,
    0x000fe2000c1019040000000502007986,
    0x000fea0003800000000000000000794d,
    0x000fc0000383fffffffffff000007947,
];

fn cuda(words: &[u128]) -> Program {
    let b: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    relift_gpu_sm86::program::parse(&b)
}

#[test]
fn bar_runs_only_where_the_whole_workgroup_does() {
    let ok: [(&str, &[u128]); 5] = [
        ("uniform exit", &UNIFORM_EXIT),
        ("uniform branch", &UNIFORM_BRANCH),
        ("uniform loop", &UNIFORM_LOOP),
        ("lane exit", &LANE_EXIT),
        ("skipped bars", &SKIPPED_BARS),
    ];
    for (name, words) in ok {
        let p = cuda(words);
        assert!(text(&p).iter().any(|t| t.contains("BAR.SYNC")), "{name}");
        let s = translate(&p).unwrap_or_else(|e| panic!("{name}: {e}"));
        assert!(spirv_has(&s.spirv, 224), "{name}: no OpControlBarrier");
    }
    let rejected: [(&str, &[u128], &str); 2] = [
        (
            "two sites",
            &TWO_SITES,
            "BAR at 0xc0 in non-uniform control flow",
        ),
        (
            "varying loop",
            &VARYING_LOOP,
            "BAR at 0x130 in non-uniform control flow",
        ),
    ];
    for (name, words, why) in rejected {
        let e = translate(&cuda(words)).err().map(|e| e.0);
        assert_eq!(e.as_deref(), Some(why), "{name}");
    }
}

/// UNIFORM_LOOP with its loop's first LDS made a texture load from R1 and
/// R9 into R6, R7 and R10: the load writes its destinations (Rd = R6 and
/// Rd2 = R10, pairs), not the registers after its sources, so the counter
/// in R4 stays uniform and the BAR still runs where the whole workgroup
/// does.
#[test]
fn texture_sources_leave_a_uniform_loop_counter_uniform() {
    let mut words = UNIFORM_LOOP;
    words[13] = 0x000f_c200_009e_070a_3000_1e09_0106_7b66;
    let p = cuda(&words);
    let t = text(&p);
    assert!(
        t.iter().any(|l| l.contains("TLD.SCR.LZ R10, R6, R1, R9")),
        "{t:?}"
    );
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert!(spirv_has(&s.spirv, 224), "no OpControlBarrier");
}

fn bra_not(at: usize, to: usize, p: u64) -> [u8; 16] {
    let mut w = bra(at, to, Some(p));
    w[1] |= 0x80;
    w
}

/// Sections that each may leave early to one of two shared tails, which
/// the next section's code skips: `@!P0 BRA s; @P1 BRA x1; s: @P0 BRA x2`.
fn early_exit_chain(sections: usize) -> Vec<u8> {
    let mut b = Vec::new();
    b.extend(isetp(0, 0, 1, 4));
    b.extend(isetp(1, 2, 3, 4));
    let x1 = 2 + 3 * sections;
    for k in 0..sections {
        let a = 2 + 3 * k;
        b.extend(bra_not(a, a + 2, 0));
        b.extend(bra(a + 1, x1, Some(1)));
        b.extend(bra(a + 2, x1 + 2, Some(0)));
    }
    b.extend(mov_imm(5, 1));
    b.extend(bra(x1 + 1, x1 + 3, None));
    b.extend(mov_imm(5, 2));
    b.extend(exit());
    b.extend(halt(x1 + 4));
    b
}

#[test]
fn shared_tails_are_dispatched_not_copied() {
    let p = relift_gpu_sm86::program::parse(&early_exit_chain(2));
    assert_eq!(text(&p)[2], "@!P0 BRA 0x40");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(count_op(&s.spirv, 251), 0, "small chains are copied");
    let p = relift_gpu_sm86::program::parse(&early_exit_chain(40));
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("early_exit_chain", &s.spirv);
    assert!(count_op(&s.spirv, 251) >= 39);
    assert!(s.spirv.len() < 20_000, "{} words", s.spirv.len());
}

fn spirv_count(words: &[u32], opcode: u32) -> usize {
    let mut k = 5;
    let mut n = 0;
    while k < words.len() {
        if words[k] & 0xffff == opcode {
            n += 1;
        }
        k += ((words[k] >> 16) as usize).max(1);
    }
    n
}

fn iadd_imm(dst: u64, a: u64, v: u32) -> [u8; 16] {
    word(
        0x010,
        4,
        &[
            (16, 8, dst),
            (24, 8, a),
            (32, 32, u64::from(v)),
            (64, 8, 255),
            (81, 3, 7),
            (84, 3, 7),
        ],
    )
}

/// Within a block, registers are SSA values: a register written and read
/// again is neither stored nor loaded in between, and a guarded add is a
/// select, not a branch.
#[test]
fn registers_stay_values_within_a_block() {
    let mut b = Vec::new();
    b.extend(mov_imm(1, 5));
    for k in 0..8 {
        b.extend(iadd_imm(2 + k, 1 + k, 3));
    }
    let mut guarded = iadd_imm(1, 9, 1);
    guarded[1] &= 0x8f;
    b.extend(guarded);
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert_eq!(text(&p)[9], "@P0 IADD3 R1, R9, 0x1, RZ");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    let mut vars = 0;
    let mut k = 5;
    while k < s.spirv.len() {
        let n = ((s.spirv[k] >> 16) as usize).max(1);
        if s.spirv[k] & 0xffff == 59 && s.spirv[k + 3] == 7 {
            vars += 1;
        }
        k += n;
    }
    assert_eq!(
        spirv_count(&s.spirv, 62),
        vars,
        "registers are stored only where the entry zeroes them"
    );
    assert!(
        spirv_count(&s.spirv, 61) <= 1,
        "only the predicate is loaded"
    );
    assert_eq!(spirv_count(&s.spirv, 247), 0, "no selection for the guard");
}

/// Whether `words` decorates some id with `deco` and, if given, the first operand `value`.
fn decorates(words: &[u32], deco: u32, value: Option<u32>) -> bool {
    let mut k = 5;
    while k < words.len() {
        let n = (words[k] >> 16) as usize;
        if words[k] & 0xffff == 71
            && n >= 4
            && words[k + 2] == deco
            && value.is_none_or(|v| words[k + 3] == v)
        {
            return true;
        }
        k += n.max(1);
    }
    false
}

/// Whether `words` declares a variable of storage class `class`.
fn has_variable_in(words: &[u32], class: u32) -> bool {
    let mut k = 5;
    while k < words.len() {
        let n = (words[k] >> 16) as usize;
        if words[k] & 0xffff == 59 && n >= 4 && words[k + 3] == class {
            return true;
        }
        k += n.max(1);
    }
    false
}

const BUILTIN_POINT_SIZE: u32 = 1;

fn vertex_storing(attribute: u64) -> Vec<u8> {
    let mut b = sph(1, 1, &[]);
    b.extend(word(
        0x021,
        4,
        &[(16, 8, 1), (24, 8, 10), (32, 32, 0x3f80_0000)],
    ));
    b.extend(word(
        0x122,
        1,
        &[(24, 8, 255), (32, 8, 10), (40, 11, attribute), (74, 2, 3)],
    ));
    exit_and_halt(&mut b);
    b
}

#[test]
fn vertex_without_a_point_size_gets_the_fixed_one() {
    let p = relift_gpu_sm86::program::parse(&vertex_storing(0x70));
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("vertex_fixed_point_size", &s.spirv);
    assert!(decorates(&s.spirv, 11, Some(BUILTIN_POINT_SIZE)));
    assert!(has_variable_in(&s.spirv, 9), "no push constant block");
}

#[test]
fn vertex_that_stores_a_point_size_keeps_its_own() {
    let p = relift_gpu_sm86::program::parse(&vertex_storing(0x6c));
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert!(decorates(&s.spirv, 11, Some(BUILTIN_POINT_SIZE)));
    assert!(!has_variable_in(&s.spirv, 9), "push constant block");
}

#[test]
fn fragment_alpha_is_tested_by_a_specialized_function() {
    let mut b = sph(2, 5, &[(6, 0xaa), (18, 0xf)]);
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("fragment_alpha_test", &s.spirv);
    assert!(
        decorates(&s.spirv, 1, Some(ALPHA_FUNC_SPEC)),
        "no alpha function constant"
    );
    assert!(has_variable_in(&s.spirv, 9), "no push constant block");
    assert!(spirv_has(&s.spirv, 5380), "no OpDemoteToHelperInvocation");
    assert!(s.interface.kills);
}

/// A fragment program that writes the flat input attribute 0 (a[0x80] to
/// a[0x8c]) to render target 0, its w as the alpha: the program the
/// runtime's alpha to coverage test (test_nvn_a2c.c) draws, from the module
/// this dumps as `fragment_a2c` for the lavapipe profile.
fn flat_alpha_fragment() -> Vec<u8> {
    let mut b = sph(2, 5, &[(6, 0x55), (18, 0xf)]);
    for c in 0..4u64 {
        b.extend(word(
            0x126,
            1,
            &[
                (16, 8, c),
                (64, 8, (0x80 >> 2) + c),
                (24, 8, 255),
                (32, 8, 255),
                (78, 2, 1),
                (81, 3, 7),
            ],
        ));
    }
    exit_and_halt(&mut b);
    b
}

#[test]
fn fragment_alpha_to_coverage_is_a_specialized_mask() {
    let p = relift_gpu_sm86::program::parse(&flat_alpha_fragment());
    let lavapipe = Options {
        host: HostProfile::lavapipe(),
        ..Options::default()
    };
    let s = translate_with(&p, &lavapipe).unwrap_or_else(|e| panic!("{e}"));
    dump("fragment_a2c", &s.spirv);
    assert!(
        decorates(&s.spirv, 1, Some(A2C_SPEC)),
        "no alpha to coverage constant"
    );
    assert!(s.interface.writes_sample_mask);
    let p = relift_gpu_sm86::program::parse(&{
        let mut b = sph(2, 5, &[(6, 0xaa), (18, 0x7)]);
        exit_and_halt(&mut b);
        b
    });
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert!(
        !decorates(&s.spirv, 1, Some(A2C_SPEC)),
        "no alpha written, no coverage"
    );
}

/// Whether every store to the `gl_SampleMask` output of `words` sits in a
/// block entered by a conditional branch (so a draw whose constants fold the
/// branch away stores nothing).
fn sample_mask_stores_are_conditional(words: &[u32]) -> bool {
    let mut var = None;
    let mut k = 5;
    while k < words.len() {
        let n = (words[k] >> 16) as usize;
        // OpDecorate <id> BuiltIn SampleMask
        if words[k] & 0xffff == 71 && n >= 4 && words[k + 2] == 11 && words[k + 3] == 20 {
            var = Some(words[k + 1]);
        }
        k += n.max(1);
    }
    let Some(var) = var else { return true };
    let (mut pointers, mut stores, mut prev, mut before_label) = (Vec::new(), 0, 0, 0);
    let mut k = 5;
    while k < words.len() {
        let (op, n) = (words[k] & 0xffff, (words[k] >> 16) as usize);
        if op == 65 && words[k + 3] == var {
            pointers.push(words[k + 2]);
        }
        if op == 248 {
            before_label = prev;
        }
        if op == 62 && pointers.contains(&words[k + 1]) {
            stores += 1;
            // OpBranchConditional is 250
            if before_label != 250 {
                return false;
            }
        }
        prev = op;
        k += n.max(1);
    }
    stores > 0
}

#[test]
fn fragment_alpha_to_coverage_stores_the_mask_only_when_set() {
    let p = relift_gpu_sm86::program::parse(&flat_alpha_fragment());
    let lavapipe = Options {
        host: HostProfile::lavapipe(),
        ..Options::default()
    };
    let s = translate_with(&p, &lavapipe).unwrap_or_else(|e| panic!("{e}"));
    assert!(
        sample_mask_stores_are_conditional(&s.spirv),
        "a store to gl_SampleMask outside a branch on the alpha to coverage constant: the 3090 draws nothing for a program that stores the mask"
    );
}

#[test]
fn fragment_without_alpha_has_no_alpha_test() {
    let mut b = sph(2, 5, &[(6, 0xaa), (18, 0x7)]);
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert!(!decorates(&s.spirv, 1, Some(ALPHA_FUNC_SPEC)));
    assert!(!has_variable_in(&s.spirv, 9), "push constant block");
    assert!(!s.interface.kills);
}

#[test]
fn specialization_constant_ranges_do_not_overlap() {
    use relift_gpu_nv::translate::{TESS_LEVEL_SPEC, TEXTURE_DIM_SPEC, TEXTURE_KIND_SPEC};
    let ranges = [
        (TEXTURE_KIND_SPEC, TEXTURE_KIND_SPEC + 0x100),
        (TESS_LEVEL_SPEC, TESS_LEVEL_SPEC + 6),
        (TEXTURE_DIM_SPEC, TEXTURE_DIM_SPEC + 0x100),
        (ALPHA_FUNC_SPEC, ALPHA_FUNC_SPEC + 1),
    ];
    for (i, a) in ranges.iter().enumerate() {
        for b in &ranges[i + 1..] {
            assert!(a.1 <= b.0 || b.1 <= a.0, "{a:x?} overlaps {b:x?}");
        }
    }
}

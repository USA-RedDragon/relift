use relift_gpu_nv::program::Program;
use relift_gpu_nv::translate::{HostProfile, Options, WarpPath, translate_with};

fn word(op: u32, form: u32, fields: &[(u32, u32, u64)]) -> [u8; 16] {
    let mut w: u128 =
        u128::from(op) | (u128::from(form) << 9) | (7u128 << 12) | (7u128 << 110) | (7u128 << 113);
    for &(lo, len, v) in fields {
        let m = ((1u128 << len) - 1) << lo;
        w = (w & !m) | ((u128::from(v) << lo) & m);
    }
    w.to_le_bytes()
}

fn sph(stage: u32, targets: u32) -> Vec<u8> {
    let mut w = [0u32; 32];
    w[0] = 2 | (4 << 5) | (stage << 10);
    w[18] = targets;
    w.iter().flat_map(|x| x.to_le_bytes()).collect()
}

fn exit_and_halt(code: &mut Vec<u8>) {
    code.extend(word(0x14d, 4, &[(87, 3, 7)]));
    code.extend(word(0x147, 4, &[(34, 48, 0xffff_ffff_fffc), (87, 3, 7)]));
}

/// `@P0 EXIT`: the rest runs in divergent control flow.
fn guarded_exit() -> [u8; 16] {
    word(0x14d, 4, &[(12, 3, 0), (87, 3, 7)])
}

fn s2r(d: u64, sr: u64) -> [u8; 16] {
    word(0x119, 0, &[(16, 8, d), (72, 8, sr), (87, 3, 7)])
}

/// `VOTE.ANY Rd, PT, PT`: the ballot of the active lanes.
fn ballot(d: u64) -> [u8; 16] {
    word(0x006, 0, &[(16, 8, d), (72, 2, 1), (81, 3, 7), (87, 3, 7)])
}

fn flo(d: u64, a: u64) -> [u8; 16] {
    word(0x100, 1, &[(16, 8, d), (32, 8, a), (81, 3, 7), (87, 3, 7)])
}

fn popc(d: u64, a: u64) -> [u8; 16] {
    word(0x109, 1, &[(16, 8, d), (32, 8, a), (87, 3, 7)])
}

/// `ISETP.EQ.U32.AND Pd, PT, Ra, Rb, PT`.
fn isetp_eq(p: u64, a: u64, b: u64) -> [u8; 16] {
    word(
        0x00c,
        1,
        &[
            (81, 3, p),
            (84, 3, 7),
            (24, 8, a),
            (32, 8, b),
            (76, 3, 2),
            (73, 1, 0),
            (87, 3, 7),
        ],
    )
}

/// `@P0 RED.E.ADD.STRONG.GPU [Ra], Rb`.
fn red_add(a: u64, b: u64) -> [u8; 16] {
    word(
        0x18e,
        4,
        &[
            (12, 3, 0),
            (24, 8, a),
            (32, 8, b),
            (64, 8, 0x86),
            (72, 8, 0xe1),
            (80, 8, 0x10),
        ],
    )
}

fn mov_imm(d: u64, v: u64) -> [u8; 16] {
    word(
        0x002,
        4,
        &[(16, 8, d), (32, 32, v), (72, 4, 0xf), (87, 3, 7)],
    )
}

/// `ISETP.NE.U32.AND P1, PT, R5, c[0x3][0x10], PT`.
fn isetp_cbuf() -> [u8; 16] {
    word(
        0x00c,
        5,
        &[
            (81, 3, 1),
            (84, 3, 7),
            (24, 8, 5),
            (40, 14, 0x10 >> 2),
            (54, 5, 3),
            (76, 3, 5),
            (73, 1, 0),
            (87, 3, 7),
        ],
    )
}

/// `VOTE.ANY P2, P1`.
fn vote_any_pred() -> [u8; 16] {
    word(
        0x006,
        0,
        &[(16, 8, 255), (72, 2, 1), (81, 3, 2), (87, 3, 1)],
    )
}

/// The lane counts ballots and the elected lane adds them: aggregated.
fn aggregated(value: u64) -> Program {
    let mut b = Vec::new();
    b.extend(guarded_exit());
    b.extend(mov_imm(6, 0x100));
    b.extend(mov_imm(7, 0));
    b.extend(s2r(1, 0));
    b.extend(ballot(2));
    b.extend(flo(3, 2));
    b.extend(popc(4, 2));
    b.extend(isetp_eq(0, 1, 3));
    b.extend(red_add(6, value));
    exit_and_halt(&mut b);
    relift_gpu_sm86::program::parse(&b)
}

fn on(p: &Program, host: HostProfile) -> relift_gpu_nv::translate::Shader {
    translate_with(
        p,
        &Options {
            host,
            ..Options::default()
        },
    )
    .unwrap_or_else(|e| panic!("{e}"))
}

fn text_of(p: &Program) -> Vec<String> {
    p.code
        .clone()
        .into_iter()
        .map(|d| d.map(|i| i.to_string()).unwrap_or_else(|e| e.to_string()))
        .collect()
}

#[test]
fn the_encodings_are_the_intended_instructions() {
    let t = text_of(&aggregated(4));
    assert_eq!(
        &t[..9],
        [
            "@P0 EXIT",
            "MOV R6, 0x100",
            "MOV R7, 0x0",
            "S2R R1, SR_LANEID",
            "VOTE.ANY R2, PT, PT",
            "FLO.U32 R3, R2",
            "POPC R4, R2",
            "ISETP.EQ.U32.AND P0, PT, R1, R3, PT",
            "@P0 RED.E.ADD.STRONG.GPU [R6], R4",
        ]
    );
}

#[test]
fn aggregated_atomics_are_exact_on_any_subgroup_size() {
    let p = aggregated(4);
    for h in [
        HostProfile::lavapipe(),
        HostProfile::mali(),
        HostProfile::amd_pro(),
    ] {
        let s = on(&p, h);
        assert!(
            s.interface.host.inexact.is_empty(),
            "{:?}",
            s.interface.host
        );
        assert!(matches!(
            s.interface.host.warp,
            WarpPath::Invariant | WarpPath::Subgroup
        ));
    }
    assert_eq!(
        on(&p, HostProfile::lavapipe()).interface.host.warp,
        WarpPath::Invariant
    );
}

/// A ballot stored whole depends on the warp, so a compute program with
/// one runs in software warps, not on the host's own subgroups.
#[test]
fn a_ballot_stored_whole_takes_software_warps() {
    let p = aggregated(2);
    let s = on(&p, HostProfile::lavapipe());
    assert_eq!(s.interface.host.warp, WarpPath::Simt);
    assert!(s.interface.host.inexact.is_empty());
}

#[test]
fn votes_on_draw_uniform_predicates_run_per_lane() {
    let mut b = sph(5, 0xf);
    b.extend(mov_imm(5, 11));
    b.extend(isetp_cbuf());
    b.extend(vote_any_pred());
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    let t = text_of(&p);
    assert_eq!(t[1], "ISETP.NE.U32.AND P1, PT, R5, c[0x3][0x10], PT");
    assert_eq!(t[2], "VOTE.ANY P2, P1");
    for h in [
        HostProfile::lavapipe(),
        HostProfile::anv(),
        HostProfile::mali(),
    ] {
        let s = on(&p, h);
        assert_eq!(s.interface.host.warp_invariant, 1);
        assert_eq!(s.interface.host.warp_ops, 0);
        assert!(s.interface.host.inexact.is_empty());
    }
}

/// `TEX R2, R5, R8, R4, 2D` with mask `xy`: the first two components go
/// to the destination at bits 16 (R5, R6), the text names bits 64 first.
fn tex_into_r5() -> [u8; 16] {
    word(
        0x160,
        5,
        &[
            (16, 8, 5),
            (64, 8, 2),
            (24, 8, 8),
            (32, 8, 4),
            (40, 14, 4),
            (54, 5, 3),
            (60, 1, 1),
            (61, 3, 1),
            (72, 4, 3),
            (81, 3, 7),
            (84, 3, 1),
            (87, 3, 1),
        ],
    )
}

#[test]
fn a_texel_written_over_a_uniform_register_is_not_uniform() {
    let mut b = sph(5, 0xf);
    b.extend(mov_imm(5, 11));
    b.extend(tex_into_r5());
    b.extend(isetp_cbuf());
    b.extend(vote_any_pred());
    exit_and_halt(&mut b);
    let p = relift_gpu_sm86::program::parse(&b);
    assert!(
        text_of(&p)[1].starts_with("TEX.SCR.LZ R2, R5,"),
        "{:?}",
        text_of(&p)
    );
    let s = on(&p, HostProfile::lavapipe());
    assert_eq!(s.interface.host.warp_invariant, 0);
    assert_eq!(s.interface.host.warp_ops, 1);
}

/// `PLOP3.LUT Pd, PT, PT, PT, UP0, lut, 0x0`: a copy of UP0 (lut 0x80) or
/// of its negation (lut 0x40).
fn plop3_of_up0(d: u64, lut: u64) -> [u8; 16] {
    word(
        0x01c,
        4,
        &[
            (81, 3, d),
            (84, 3, 7),
            (87, 3, 7),
            (77, 3, 7),
            (67, 1, 1),
            (68, 3, 0),
            (72, 5, lut >> 3),
            (64, 3, lut & 7),
        ],
    )
}

/// The shape of the compiler's branch on a uniform predicate: `VOTEU.ANY
/// UP0, P1` of a draw-uniform P1, copied into P0 and P5 (its negation);
/// R0, which holds the lane id, is replaced where UP0 is false by a guarded move and where it is
/// true on the fall-through path; then P0 is reused before the output.
/// The guarded move and the branch name different copies of UP0, so only
/// following the copies shows R0 replaced on every path.
fn vote_copies() -> Program {
    let mut b = sph(5, 0x1);
    b.extend(s2r(0, 0));
    b.extend(mov_imm(5, 11));
    b.extend(isetp_cbuf());
    b.extend(word(
        0x086,
        0,
        &[(16, 6, 63), (72, 2, 1), (81, 3, 0), (87, 3, 1)],
    ));
    b.extend(plop3_of_up0(5, 0x40));
    b.extend(plop3_of_up0(0, 0x80));
    b.extend(word(
        0x002,
        1,
        &[(12, 4, 8), (16, 8, 0), (32, 8, 8), (72, 4, 0xf)],
    ));
    b.extend(word(0x947, 4, &[(12, 4, 5), (34, 48, 4), (87, 3, 7)]));
    b.extend(word(
        0x002,
        1,
        &[(16, 8, 0), (32, 8, 9), (72, 4, 0xf), (87, 3, 7)],
    ));
    b.extend(isetp_eq(0, 2, 3));
    exit_and_halt(&mut b);
    relift_gpu_sm86::program::parse(&b)
}

#[test]
fn guards_follow_predicate_copies() {
    let p = vote_copies();
    let t = text_of(&p);
    assert_eq!(
        &t[3..9],
        [
            "VOTEU.ANY UP0, P1",
            "PLOP3.LUT P5, PT, PT, PT, UP0, 0x40, 0x0",
            "PLOP3.LUT P0, PT, PT, PT, UP0, 0x80, 0x0",
            "@!P0 MOV R0, R8",
            "@P5 BRA 0x90",
            "MOV R0, R9",
        ],
        "{t:?}"
    );
    let s = on(&p, HostProfile::lavapipe());
    assert_eq!(
        s.interface.host.warp,
        WarpPath::Invariant,
        "{:?}",
        s.interface.host
    );
    assert!(s.interface.host.inexact.is_empty());
}

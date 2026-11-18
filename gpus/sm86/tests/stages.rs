use relift_gpu_nv::program::{Program, Stage};
use relift_gpu_nv::translate::{
    GsInput, Options, Pipeline, TESS_LEVEL_SPEC, TessDomain, TessOutput, TessSpacing, translate,
    translate_with,
};

const RZ: u64 = 255;

/// One instruction from its 12-bit opcode and form, with no guard and
/// default scheduling.
fn w(op: u32, fields: &[(u32, u32, u64)]) -> [u8; 16] {
    let mut x: u128 = u128::from(op) | (7u128 << 12) | (7u128 << 110) | (7u128 << 113);
    for &(lo, len, v) in fields {
        let m = ((1u128 << len) - 1) << lo;
        x = (x & !m) | ((u128::from(v) << lo) & m);
    }
    x.to_le_bytes()
}

fn s2r(d: u64, sr: u64) -> [u8; 16] {
    w(0x919, &[(16, 8, d), (72, 8, sr)])
}

const SR_LANEID: u64 = 0;
const SR_INVOCATION_ID: u64 = 17;
const SR_INVOCATION_INFO: u64 = 29;

fn prmt_imm(d: u64, a: u64, imm: u64) -> [u8; 16] {
    w(0x816, &[(16, 8, d), (24, 8, a), (32, 32, imm), (64, 8, RZ)])
}

fn imad_imm(d: u64, a: u64, b: u64, imm: u64) -> [u8; 16] {
    w(
        0x424,
        &[
            (16, 8, d),
            (24, 8, a),
            (64, 8, b),
            (32, 32, imm),
            (72, 8, 2),
            (80, 8, 0x8e),
            (88, 3, 7),
        ],
    )
}

fn imad(d: u64, a: u64, b: u64, c: u64) -> [u8; 16] {
    w(
        0x224,
        &[
            (16, 8, d),
            (24, 8, a),
            (32, 8, b),
            (64, 8, c),
            (72, 8, 2),
            (80, 8, 0x8e),
            (88, 3, 7),
        ],
    )
}

fn iadd3(d: u64, a: u64, b: u64, negb: bool) -> [u8; 16] {
    w(
        0x210,
        &[
            (16, 8, d),
            (24, 8, a),
            (32, 8, b),
            (63, 1, u64::from(negb)),
            (64, 8, RZ),
            (72, 8, 0xe0),
            (80, 8, 0xff),
            (88, 3, 7),
        ],
    )
}

fn isberd(d: u64, a: u64, imm: u64, mods: u64) -> [u8; 16] {
    w(
        0x923,
        &[(16, 8, d), (24, 8, a), (40, 16, imm), (74, 6, mods)],
    )
}

fn isbewr(a: u64, s: u64) -> [u8; 16] {
    w(0x927, &[(24, 8, a), (32, 8, s), (79, 1, 1)])
}

fn mov_imm(d: u64, v: u64) -> [u8; 16] {
    w(0x802, &[(16, 8, d), (32, 32, v), (72, 4, 0xf)])
}

fn size(n: u64) -> u64 {
    n - 1
}

fn ald(d: u64, off: u64, vtx: u64, n: u64, o: bool, p: bool) -> [u8; 16] {
    w(
        0x321,
        &[
            (16, 8, d),
            (24, 8, RZ),
            (32, 8, vtx),
            (40, 11, off),
            (74, 2, size(n)),
            (76, 1, u64::from(p)),
            (79, 1, u64::from(o)),
        ],
    )
}

fn ast(off: u64, s: u64, vtx: u64, n: u64, p: bool) -> [u8; 16] {
    w(
        0x322,
        &[
            (24, 8, RZ),
            (32, 8, s),
            (40, 11, off),
            (64, 8, vtx),
            (74, 2, size(n)),
            (76, 1, u64::from(p)),
        ],
    )
}

fn fmul(d: u64, a: u64, b: u64) -> [u8; 16] {
    w(0x220, &[(16, 8, d), (24, 8, a), (32, 8, b), (84, 3, 4)])
}

fn ffma(d: u64, a: u64, b: u64, c: u64) -> [u8; 16] {
    w(0x223, &[(16, 8, d), (24, 8, a), (32, 8, b), (64, 8, c)])
}

fn fadd(d: u64, a: u64, b: u64) -> [u8; 16] {
    w(0x221, &[(16, 8, d), (24, 8, a), (32, 8, b)])
}

fn out(kind: u64, d: u64, a: u64) -> [u8; 16] {
    w(0x324, &[(16, 8, d), (24, 8, a), (32, 8, RZ), (78, 2, kind)])
}

const OUT_FINAL: u64 = 0;
const OUT_EMIT: u64 = 1;
const OUT_CUT: u64 = 2;

fn bar_sync() -> [u8; 16] {
    w(0xb1d, &[])
}

fn exit() -> [u8; 16] {
    w(0x94d, &[(87, 3, 7)])
}

fn halt() -> [u8; 16] {
    w(0x947, &[(34, 48, 0xffff_ffff_fffc), (87, 3, 7)])
}

fn sph(stage: u32, extra: &[(usize, u32)]) -> Vec<u8> {
    let mut s = [0u32; 32];
    s[0] = 1 | (4 << 5) | (stage << 10);
    for &(i, v) in extra {
        s[i] |= v;
    }
    s.iter().flat_map(|x| x.to_le_bytes()).collect()
}

fn program(stage: u32, extra: &[(usize, u32)], code: &[[u8; 16]]) -> Program {
    let mut b = sph(stage, extra);
    for c in code {
        b.extend(c);
    }
    b.extend(exit());
    b.extend(halt());
    relift_gpu_sm86::program::parse(&b)
}

fn dump(name: &str, words: &[u32]) {
    if let Some(dir) = std::env::var_os("RELIFT_SM86_DUMP") {
        let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        let path = std::path::Path::new(&dir).join(format!("{name}.spv"));
        relift_cache::write_if_changed(&path, &bytes).unwrap_or_else(|e| panic!("{name}: {e}"));
    }
}

/// The instructions of a module: (opcode, operands).
fn insts(words: &[u32]) -> Vec<(u32, Vec<u32>)> {
    let mut v = Vec::new();
    let mut k = 5;
    while k < words.len() {
        let n = (words[k] >> 16) as usize;
        v.push((words[k] & 0xffff, words[k + 1..k + n.max(1)].to_vec()));
        k += n.max(1);
    }
    v
}

fn count(words: &[u32], opcode: u32) -> usize {
    insts(words).iter().filter(|(o, _)| *o == opcode).count()
}

fn model(words: &[u32]) -> u32 {
    insts(words)
        .iter()
        .find(|(o, _)| *o == 15)
        .expect("entry point")
        .1[0]
}

/// The stage's execution modes, without the float controls (SignedZeroInfNanPreserve).
fn modes(words: &[u32]) -> Vec<Vec<u32>> {
    insts(words)
        .into_iter()
        .filter(|(o, a)| *o == 16 && a.get(1) != Some(&4461))
        .map(|(_, a)| a[1..].to_vec())
        .collect()
}

fn builtin(words: &[u32], b: u32) -> bool {
    insts(words)
        .iter()
        .any(|(o, a)| *o == 71 && a.get(1) == Some(&11) && a.get(2) == Some(&b))
}

fn decorated(words: &[u32], d: u32) -> usize {
    insts(words)
        .iter()
        .filter(|(o, a)| *o == 71 && a.get(1) == Some(&d))
        .count()
}

fn spec_ids(words: &[u32]) -> Vec<u32> {
    insts(words)
        .iter()
        .filter(|(o, a)| *o == 71 && a.get(1) == Some(&1))
        .map(|(_, a)| a[2])
        .collect()
}

/// The handle of input vertex `k` the way the hardware's programs build
/// it: `ISBERD [byte2 * byte0 + k]` of SR_INVOCATION_INFO.
fn handles(info: u64, dsts: &[u64]) -> Vec<[u8; 16]> {
    let mut v = vec![
        s2r(info, SR_INVOCATION_INFO),
        prmt_imm(2, info, 0x7772),
        prmt_imm(3, info, 0x7770),
    ];
    for (k, &d) in dsts.iter().enumerate() {
        v.push(imad_imm(d, 2, 3, k as u64));
        v.push(isberd(d, d, 0, 0));
    }
    v
}

#[test]
fn tess_eval_reads_patch_vertices_through_handles() {
    let mut code = vec![s2r(4, SR_LANEID), ald(4, 0x2f0, 4, 2, true, false)];
    code.extend(handles(0, &[6, 7, 8]));
    code.push(mov_imm(9, 0x8000_0000));
    for base in [0x70, 0x80] {
        for (k, h) in [6, 7, 8, 9].into_iter().enumerate() {
            code.push(ald(12 + 4 * k as u64, base, h, 4, false, false));
        }
        for c in 0..4 {
            code.push(fmul(28 + c, 12 + c, 4));
            code.push(ffma(28 + c, 16 + c, 5, 28 + c));
            code.push(ffma(28 + c, 20 + c, 5, 28 + c));
            code.push(fadd(28 + c, 28 + c, 24 + c));
        }
        code.push(ast(base, 28, RZ, 4, false));
    }
    code.extend([
        ald(12, 0x70, RZ, 1, false, true),
        ald(13, 0x0, RZ, 1, false, true),
        ald(14, 0x10, RZ, 1, false, true),
        ald(15, 0x60, RZ, 1, false, false),
        ast(0x90, 12, RZ, 4, false),
    ]);
    let p = program(3, &[], &code);
    assert_eq!(p.stage(), Stage::TessEval);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("tess_eval", &s.spirv);
    assert_eq!(model(&s.spirv), 2);
    assert_eq!(modes(&s.spirv), vec![vec![22], vec![1], vec![5]]);
    for b in [0, 7, 11, 12, 13, 14] {
        assert!(builtin(&s.spirv, b), "built-in {b}");
    }
    assert!(
        !builtin(&s.spirv, 41),
        "the TessCoord lane needs no subgroup"
    );
    assert_eq!(
        s.interface.patch_inputs.iter().copied().collect::<Vec<_>>(),
        vec![5]
    );
    assert_eq!(
        s.interface.inputs.keys().copied().collect::<Vec<_>>(),
        vec![0]
    );
    assert_eq!(
        s.interface.outputs.iter().copied().collect::<Vec<_>>(),
        vec![0, 1]
    );
    assert_eq!(
        decorated(&s.spirv, 15),
        3,
        "patch input and both level arrays"
    );
    assert_eq!(s.interface.host.warp_ops, 0);
}

#[test]
fn tess_eval_follows_the_pipeline_state() {
    let p = program(
        3,
        &[],
        &[
            ald(0, 0x2f0, RZ, 2, true, false),
            ast(0x70, 0, RZ, 2, false),
        ],
    );
    let o = Options {
        pipeline: Pipeline {
            tess_domain: TessDomain::Quads,
            tess_spacing: TessSpacing::FractionalOdd,
            tess_output: TessOutput::Points,
            ..Pipeline::default()
        },
        ..Options::default()
    };
    let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(modes(&s.spirv), vec![vec![24], vec![3], vec![10], vec![5]]);
    let o = Options {
        pipeline: Pipeline {
            tess_domain: TessDomain::Isolines,
            tess_spacing: TessSpacing::FractionalEven,
            tess_output: TessOutput::TrianglesCw,
            ..Pipeline::default()
        },
        ..Options::default()
    };
    let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(modes(&s.spirv), vec![vec![25], vec![2], vec![4]]);
}

#[test]
fn tess_control_writes_its_vertex_and_the_patch() {
    let mut code = vec![s2r(0, SR_INVOCATION_ID), s2r(5, SR_LANEID)];
    code.extend([
        s2r(1, SR_INVOCATION_INFO),
        prmt_imm(2, 1, 0x7772),
        prmt_imm(3, 1, 0x7770),
        imad(4, 2, 3, 0),
        isberd(4, 4, 0, 0),
        ald(8, 0x70, 4, 4, false, false),
        ald(12, 0x80, 4, 4, false, false),
        ast(0x70, 8, 0, 4, false),
        ast(0x80, 12, 0, 4, false),
        bar_sync(),
        iadd3(6, 5, 0, true),
        ald(16, 0x80, 6, 4, true, false),
        ast(0x70, 16, RZ, 4, true),
        mov_imm(20, 0x4000_0000),
        ast(0x0, 20, RZ, 1, true),
        ast(0x10, 20, RZ, 1, true),
        ald(21, 0x70, RZ, 1, true, true),
    ]);
    let p = program(2, &[(2, 4 << 24)], &code);
    assert_eq!(p.stage(), Stage::TessControl);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("tess_control", &s.spirv);
    assert_eq!(model(&s.spirv), 1);
    assert_eq!(modes(&s.spirv), vec![vec![26, 4]]);
    assert_eq!(s.interface.output_vertices, 4);
    assert_eq!(count(&s.spirv, 224), 1, "one control barrier");
    assert!(builtin(&s.spirv, 8) && builtin(&s.spirv, 14));
    assert_eq!(
        s.interface
            .patch_outputs
            .iter()
            .copied()
            .collect::<Vec<_>>(),
        vec![5]
    );
    assert_eq!(
        s.interface.outputs.iter().copied().collect::<Vec<_>>(),
        vec![0]
    );
}

#[test]
fn tess_control_without_output_vertices_passes_the_patch_through() {
    let p = program(2, &[(1, 6 << 24)], &[]);
    let o = Options {
        pipeline: Pipeline {
            patch_vertices: 4,
            tcs_passthrough: 0b101,
            ..Pipeline::default()
        },
        ..Options::default()
    };
    let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{e}"));
    dump("tess_control_passthrough", &s.spirv);
    assert_eq!(modes(&s.spirv), vec![vec![26, 4]]);
    assert_eq!(
        s.interface.outputs.iter().copied().collect::<Vec<_>>(),
        vec![0, 2]
    );
    let mut ids = spec_ids(&s.spirv);
    ids.sort_unstable();
    assert_eq!(
        ids,
        (TESS_LEVEL_SPEC..TESS_LEVEL_SPEC + 6).collect::<Vec<_>>()
    );
    assert!(builtin(&s.spirv, 11) && builtin(&s.spirv, 12) && builtin(&s.spirv, 0));
}

fn geometry_body(code: &mut Vec<[u8; 16]>) {
    code.extend(handles(0, &[4, 5, 6]));
    code.extend([mov_imm(1, 0), ald(16, 0x60, RZ, 1, false, false)]);
    for _ in 0..2 {
        for v in [4, 5, 6] {
            code.extend([
                ald(8, 0x70, v, 4, false, false),
                ald(12, 0x80, v, 4, false, false),
                ast(0x70, 8, 1, 4, false),
                ast(0x80, 12, 1, 4, false),
                ast(0x60, 16, 1, 1, false),
                ast(0x64, RZ, 1, 1, false),
                out(OUT_EMIT, 1, 1),
            ]);
        }
        code.push(out(OUT_CUT, 1, 1));
    }
}

const GS_SPH: [(usize, u32); 4] = [(0, 1 << 28), (2, 1 << 24), (3, 7 << 24), (4, 6)];

#[test]
fn geometry_emits_and_cuts() {
    let mut code = Vec::new();
    geometry_body(&mut code);
    code.push(out(OUT_FINAL, RZ, 1));
    let p = program(4, &GS_SPH, &code);
    assert_eq!(p.stage(), Stage::Geometry);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("geometry", &s.spirv);
    assert_eq!(model(&s.spirv), 3);
    assert_eq!(
        modes(&s.spirv),
        vec![vec![0, 1], vec![22], vec![26, 6], vec![29]]
    );
    assert_eq!(count(&s.spirv, 218), 6);
    assert_eq!(count(&s.spirv, 219), 2);
    assert!(builtin(&s.spirv, 7) && builtin(&s.spirv, 9));
    let o = Options {
        pipeline: Pipeline {
            gs_input: GsInput::LinesAdjacency,
            ..Pipeline::default()
        },
        ..Options::default()
    };
    let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(modes(&s.spirv)[1], vec![21]);
}

fn lepc(d: u64) -> [u8; 16] {
    w(0x34e, &[(16, 8, d)])
}

fn atexit(code: &mut Vec<[u8; 16]>, handler: usize) {
    code.extend([
        lepc(20),
        mov_imm(22, 16 * handler as u64),
        w(
            0x210,
            &[
                (16, 8, 20),
                (24, 8, 20),
                (32, 8, 22),
                (64, 8, RZ),
                (72, 8, 0xe0),
                (80, 8, 0xf1),
                (88, 3, 7),
            ],
        ),
        w(
            0x210,
            &[
                (16, 8, 21),
                (24, 8, 21),
                (32, 8, RZ),
                (64, 8, RZ),
                (72, 8, 0xe4),
                (80, 8, 0x7f),
            ],
        ),
        w(0x357, &[(32, 8, 20)]),
    ]);
}

#[test]
fn geometry_atexit_that_only_finishes_is_dropped() {
    for (tail, ok) in [
        (out(OUT_FINAL, RZ, 1), true),
        (ast(0x70, 8, 1, 1, false), false),
    ] {
        let mut code = Vec::new();
        atexit(&mut code, 0);
        geometry_body(&mut code);
        code.push(exit());
        let at = code.len();
        code.extend([tail, w(0x957, &[])]);
        code[1] = mov_imm(22, 16 * at as u64);
        let p = program(4, &GS_SPH, &code);
        match translate(&p) {
            Ok(s) => {
                assert!(ok);
                assert_eq!(count(&s.spirv, 218), 6);
            }
            Err(e) => {
                assert!(!ok, "{e}");
                assert_eq!(e.0, "ATEXIT handler");
            }
        }
    }
}

#[test]
fn other_stage_buffer_accesses_are_refused() {
    let p = program(3, &[], &[isberd(0, RZ, 0, 0b01_1110)]);
    let e = translate(&p).err().expect("ATTR.SKEW access");
    assert!(e.0.starts_with("stage buffer ISBERD."), "{e}");
    let p = program(1, &[], &[isbewr(0, 1)]);
    assert_eq!(
        translate(&p).err().map(|e| e.0),
        Some("stage buffer ISBEWR".into())
    );
    let p = relift_gpu_sm86::program::parse(&[isberd(0, 1, 0, 0), exit(), halt()].concat());
    assert_eq!(
        translate(&p).err().map(|e| e.0),
        Some("ISBERD outside a tessellation or geometry stage".into())
    );
}

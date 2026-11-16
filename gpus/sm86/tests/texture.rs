mod common;

use common::translate;
use relift_gpu_sm86::program::{Program, Stage};
use relift_gpu_sm86::translate::{
    Dim, ImageDim, ImageKind, Kind, SAMPLER_BINDING, TEXTURE_DIM_SPEC, TEXTURE_KIND_SPEC,
    image_binding, texture_binding,
};

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

fn sph_fragment() -> Vec<u8> {
    let mut w = [0u32; 32];
    w[0] = 2 | (4 << 5) | (5 << 10);
    w[18] = 0xf;
    w.iter().flat_map(|x| x.to_le_bytes()).collect()
}

fn mov(rd: u64, imm: u64) -> [u8; 16] {
    word(0x002, 4, &[(16, 8, rd), (32, 32, imm), (72, 4, 0xf)])
}

/// A texture instruction: opcode `op` (0x160 TEX, 0x163 TLD4, 0x166 TLD,
/// 0x169 TMML, 0x16c TXD, 0x16f TXQ) bound to `c[3]` byte 0x10, Rd R0,
/// Rd2 R2, Ra R8, Rb R4.
fn tex(op: u32, fields: &[(u32, u32, u64)]) -> [u8; 16] {
    let mut f = vec![
        (16, 8, 0),
        (64, 8, 2),
        (24, 8, 8),
        (32, 8, 4),
        (40, 14, 4),
        (54, 5, 3),
        (72, 4, 0xf),
        (81, 3, 7),
        (84, 3, 1),
    ];
    f.extend_from_slice(fields);
    word(op, 5, &f)
}

fn insts(words: &[u32], opcode: u32) -> Vec<Vec<u32>> {
    let mut out = Vec::new();
    let mut k = 5;
    while k < words.len() {
        let n = ((words[k] >> 16) as usize).max(1);
        if words[k] & 0xffff == opcode {
            out.push(words[k..k + n].to_vec());
        }
        k += n;
    }
    out
}

fn compute(body: &[[u8; 16]]) -> relift_gpu_sm86::translate::Shader {
    let mut b = Vec::new();
    for w in body {
        b.extend(w);
    }
    exit_and_halt(&mut b);
    translate(&Program::parse(&b)).unwrap_or_else(|e| panic!("{e}"))
}

#[test]
fn bound_texture_uses_the_pools() {
    let s = compute(&[tex(0x160, &[(60, 1, 1), (61, 3, 1), (87, 3, 1)])]);
    assert!(s.interface.textures.contains(&(3, 16)));
    assert_eq!(
        s.interface.texture_kinds.get(&(3, 16)),
        Some(&TEXTURE_KIND_SPEC)
    );
    for b in [
        SAMPLER_BINDING,
        texture_binding(Kind::Float, Dim::D2),
        texture_binding(Kind::Sint, Dim::D2),
        texture_binding(Kind::Uint, Dim::D2),
    ] {
        assert!(s.interface.texture_bindings.contains(&b), "binding {b}");
    }
    assert_eq!(insts(&s.spirv, 88).len(), 3);
    assert!(!insts(&s.spirv, 52).is_empty());
}

#[test]
fn depth_compare_reads_floats_only() {
    let s = compute(&[tex(
        0x160,
        &[(60, 1, 1), (61, 3, 5), (78, 1, 1), (87, 3, 1)],
    )]);
    assert!(s.interface.texture_kinds.is_empty());
    assert_eq!(insts(&s.spirv, 90).len(), 1);
}

#[test]
fn bindless_index_is_non_uniform() {
    let mut w = tex(0x161, &[(59, 1, 1), (61, 3, 1), (87, 3, 1)]);
    w[1] = (w[1] & !0x0e) | (1 << 1);
    let s = compute(&[w]);
    assert!(s.interface.textures.is_empty());
    let decos = insts(&s.spirv, 71);
    assert!(decos.iter().any(|d| d.get(2) == Some(&5300)));
}

#[test]
fn uniform_register_handle_is_uniform() {
    let w = word(
        0x160,
        7,
        &[
            (64, 8, 2),
            (24, 8, 8),
            (32, 8, 4),
            (40, 6, 6),
            (60, 1, 1),
            (61, 3, 1),
            (72, 4, 0xf),
            (81, 3, 7),
            (84, 3, 1),
            (87, 3, 1),
        ],
    );
    let s = compute(&[w]);
    assert!(s.interface.textures.is_empty());
    let decos = insts(&s.spirv, 71);
    assert!(decos.iter().all(|d| d.get(2) != Some(&5300)));
    assert_eq!(insts(&s.spirv, 88).len(), 1);
}

#[test]
fn constant_offsets_stay_constant() {
    let s = compute(&[
        mov(4, 0xf1),
        tex(0x160, &[(61, 3, 1), (76, 1, 1), (87, 3, 1)]),
    ]);
    let samples = insts(&s.spirv, 88);
    assert!(samples.iter().all(|i| i[5] & 8 != 0));
}

/// `@P0 BRA` forward by `skip` instructions.
fn bra_p0(skip: u64) -> [u8; 16] {
    word(0x147, 4, &[(12, 4, 0), (34, 48, 4 * skip), (87, 3, 7)])
}

/// An offset register that moves set before a branch reaches on every
/// path is a constant; one that a path reaches unset is not.
#[test]
fn offsets_set_in_an_earlier_block_stay_constant() {
    let t = tex(0x160, &[(61, 3, 1), (76, 1, 1), (87, 3, 1)]);
    let s = compute(&[mov(4, 0xf1), bra_p0(1), mov(9, 0), t]);
    let samples = insts(&s.spirv, 88);
    assert!(!samples.is_empty() && samples.iter().all(|i| i[5] & 8 != 0));
    let s = compute(&[mov(4, 0xf1), bra_p0(1), mov(4, 0xf1), t]);
    assert!(insts(&s.spirv, 88).iter().all(|i| i[5] & 8 != 0));
    for body in [
        vec![bra_p0(1), mov(4, 0xf1), t],
        vec![mov(4, 0xf1), bra_p0(1), mov(4, 0x12), t],
        vec![
            mov(4, 0xf1),
            tex(
                0x160,
                &[
                    (16, 8, 3),
                    (64, 8, 255),
                    (32, 8, 255),
                    (61, 3, 1),
                    (72, 4, 3),
                    (87, 3, 1),
                ],
            ),
            t,
        ],
        vec![
            mov(4, 0xf1),
            word(
                0x002,
                4,
                &[(12, 4, 0), (16, 8, 4), (32, 32, 0x12), (72, 4, 0xf)],
            ),
            t,
        ],
    ] {
        let mut b: Vec<u8> = body.iter().flatten().copied().collect();
        exit_and_halt(&mut b);
        let e = translate(&Program::parse(&b)).err().map(|e| e.to_string());
        assert!(e.is_some_and(|e| e.contains("offsets")), "{body:?}");
    }
}

#[test]
fn run_time_sample_offsets_are_rejected() {
    for w in [
        tex(0x160, &[(61, 3, 1), (76, 1, 1), (87, 3, 1)]),
        tex(0x16c, &[(61, 3, 1), (76, 1, 1)]),
    ] {
        let mut b = w.to_vec();
        exit_and_halt(&mut b);
        let e = translate(&Program::parse(&b)).err().map(|e| e.to_string());
        assert!(e.is_some_and(|e| e.contains("offsets")));
    }
}

#[test]
fn fetch_gather_and_queries() {
    let s = compute(&[
        tex(0x166, &[(61, 3, 5), (87, 3, 3)]),
        tex(0x163, &[(61, 3, 1), (76, 2, 2), (87, 2, 1)]),
        tex(0x16c, &[(61, 3, 1)]),
    ]);
    assert_eq!(insts(&s.spirv, 95).len(), 3);
    assert_eq!(insts(&s.spirv, 96).len(), 12);
    assert!(
        s.interface
            .texture_bindings
            .contains(&texture_binding(Kind::Uint, Dim::Array2))
    );
}

#[test]
fn fragment_queries_the_lod() {
    let mut b = sph_fragment();
    b.extend(tex(0x169, &[(61, 3, 3), (72, 4, 3)]));
    b.extend(tex(0x160, &[(61, 3, 1), (87, 3, 2)]));
    exit_and_halt(&mut b);
    let s = translate(&Program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(insts(&s.spirv, 105).len(), 1);
    let implicit = insts(&s.spirv, 87);
    assert_eq!(implicit.len(), 3);
    assert!(implicit.iter().all(|i| i[5] == 1));
}

#[test]
fn half_results_pack_in_pairs() {
    let s = compute(&[tex(0x160, &[(61, 3, 1), (79, 2, 1), (87, 3, 1)])]);
    assert!(!insts(&s.spirv, 196).is_empty());
}

fn surface_words() -> ([u8; 16], [u8; 16]) {
    let sust = word(
        0xb9b,
        0,
        &[
            (24, 8, 8),
            (32, 8, 4),
            (40, 14, 0x12),
            (54, 5, 2),
            (59, 2, 0),
            (61, 3, 3),
            (72, 4, 0xf),
            (84, 3, 1),
        ],
    );
    let suld = word(
        0xb99,
        0,
        &[
            (16, 8, 0),
            (24, 8, 8),
            (40, 14, 0x13),
            (54, 5, 2),
            (59, 2, 0),
            (61, 3, 3),
            (72, 1, 1),
            (73, 3, 4),
            (81, 3, 7),
            (84, 3, 1),
        ],
    );
    (sust, suld)
}

/// A program that writes surfaces must see the other invocations' surface
/// writes a barrier orders, so its storage images are Coherent; one that
/// only reads them needs nothing.
#[test]
fn written_storage_images_are_coherent() {
    const COHERENT: u32 = 23;
    let (sust, suld) = surface_words();
    let coherent = |s: &relift_gpu_sm86::translate::Shader| {
        insts(&s.spirv, 71).iter().any(|d| d[2] == COHERENT)
    };
    assert!(coherent(&compute(&[sust, suld])));
    assert!(!coherent(&compute(&[suld])));
}

#[test]
fn surfaces_are_storage_images() {
    let sust = word(
        0xb9b,
        0,
        &[
            (24, 8, 8),
            (32, 8, 4),
            (40, 14, 0x12),
            (54, 5, 2),
            (59, 2, 0),
            (61, 3, 3),
            (72, 4, 0xf),
            (84, 3, 1),
        ],
    );
    let suld = word(
        0xb99,
        0,
        &[
            (16, 8, 0),
            (24, 8, 8),
            (40, 14, 0x13),
            (54, 5, 2),
            (59, 2, 0),
            (61, 3, 3),
            (72, 1, 1),
            (73, 3, 4),
            (81, 3, 7),
            (84, 3, 1),
        ],
    );
    let s = compute(&[sust, suld]);
    assert_eq!(insts(&s.spirv, 99).len(), 3);
    assert_eq!(insts(&s.spirv, 98).len(), 1);
    assert!(
        s.interface
            .texture_bindings
            .contains(&image_binding(ImageKind::Raw32, ImageDim::D2))
    );
    assert!(s.interface.textures.contains(&(2, 0x48)));
}

#[test]
fn surface_atomics_return_the_old_value() {
    let suatom = word(
        0x593,
        0,
        &[
            (16, 8, 0),
            (24, 8, 8),
            (32, 8, 4),
            (40, 14, 0x12),
            (54, 5, 2),
            (59, 2, 0),
            (61, 3, 3),
            (72, 1, 1),
            (73, 3, 0),
            (81, 3, 7),
            (84, 3, 1),
            (87, 4, 0),
        ],
    );
    let s = compute(&[suatom]);
    assert_eq!(insts(&s.spirv, 60).len(), 1);
    assert_eq!(insts(&s.spirv, 234).len(), 1);
    assert!(
        s.interface
            .texture_bindings
            .contains(&image_binding(ImageKind::Atomic32, ImageDim::D2))
    );
}

#[test]
fn rejects_texture_forms_it_cannot_translate() {
    for w in [
        tex(0x166, &[(61, 3, 1), (77, 1, 1), (87, 3, 1)]),
        tex(0x16f, &[(62, 2, 2)]),
        tex(0x16c, &[(61, 3, 1), (87, 1, 1)]),
        tex(0x169, &[(61, 3, 1), (72, 4, 7)]),
    ] {
        let mut b = w.to_vec();
        exit_and_halt(&mut b);
        assert!(translate(&Program::parse(&b)).is_err());
    }
}

fn sph_vertex() -> Vec<u8> {
    let mut w = [0u32; 32];
    w[0] = 1 | (4 << 5) | (1 << 10);
    w.iter().flat_map(|x| x.to_le_bytes()).collect()
}

fn program(head: Vec<u8>, body: &[[u8; 16]]) -> Program {
    let mut b = head;
    for w in body {
        b.extend(w);
    }
    exit_and_halt(&mut b);
    Program::parse(&b)
}

#[test]
fn txq_takes_the_dimension_the_handle_is_sampled_with() {
    let s = compute(&[
        tex(0x166, &[(61, 3, 5), (87, 3, 3)]),
        tex(0x16f, &[(62, 2, 0)]),
    ]);
    let sizes = insts(&s.spirv, 103);
    assert!(!sizes.is_empty());
    assert!(!insts(&s.spirv, 106).is_empty());
    for body in [
        vec![tex(0x16f, &[(62, 2, 0)])],
        vec![
            tex(0x166, &[(61, 3, 5), (87, 3, 3)]),
            tex(0x166, &[(61, 3, 2), (87, 3, 3)]),
            tex(0x16f, &[(62, 2, 0)]),
        ],
    ] {
        let s = translate(&program(Vec::new(), &body)).unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(
            s.interface.texture_dims.get(&(3, 16)),
            Some(&TEXTURE_DIM_SPEC),
            "the runtime gives the dimension"
        );
        assert_eq!(insts(&s.spirv, SWITCH).len(), 1);
        assert_eq!(
            insts(&s.spirv, 103).len(),
            7,
            "a size query per mipmapped dimension"
        );
    }
    let mut w = tex(0x170, &[(62, 2, 0)]);
    w[1] = (w[1] & !0x0e) | (1 << 1);
    let e = translate(&program(Vec::new(), &[w]))
        .err()
        .map(|e| e.to_string());
    assert!(
        e.is_some_and(|e| e.contains("TXQ")),
        "a handle from a register"
    );
}

const SWITCH: u32 = 251;

/// NVIDIA's compiler queries the LOD of a 2D array texture with `TMML 2D`
/// (`textureQueryLod` of a `sampler2DArray`), so TMML's dimension does not
/// conflict with the array the handle is sampled as, and the query reads
/// the array view.
#[test]
fn tmml_takes_the_array_view_the_handle_is_sampled_with() {
    let mut b = sph_fragment();
    b.extend(tex(0x169, &[(61, 3, 1), (72, 4, 3)]));
    b.extend(tex(0x160, &[(61, 3, 5), (87, 3, 2)]));
    b.extend(tex(0x16f, &[(62, 2, 0)]));
    exit_and_halt(&mut b);
    let s = translate(&Program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(insts(&s.spirv, 105).len(), 1);
    assert!(!insts(&s.spirv, 103).is_empty());
    let float = &s.interface.texture_bindings;
    assert!(float.contains(&texture_binding(Kind::Float, Dim::Array2)));
    assert!(!float.contains(&texture_binding(Kind::Float, Dim::D2)));
    let mut b = sph_fragment();
    b.extend(tex(0x169, &[(61, 3, 2), (72, 4, 3)]));
    b.extend(tex(0x160, &[(61, 3, 5), (87, 3, 2)]));
    b.extend(tex(0x16f, &[(62, 2, 0)]));
    exit_and_halt(&mut b);
    let s = translate(&Program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
    assert!(
        s.interface.texture_dims.contains_key(&(3, 16)),
        "a 3D TMML on a 2D array leaves the dimension to the runtime"
    );
}

const EXECUTION_MODE: u32 = 16;
const DERIVATIVE_GROUP_LINEAR: u32 = 5290;

#[test]
fn implicit_lods_take_quad_derivatives_in_compute() {
    for (body, sample, op) in [
        (tex(0x160, &[(61, 3, 1), (87, 3, 0)]), 87, None),
        (tex(0x160, &[(61, 3, 1), (87, 3, 2)]), 87, Some(1)),
        (tex(0x169, &[(61, 3, 1), (72, 4, 3)]), 105, None),
    ] {
        let s = compute(&[body]);
        assert!(s.interface.compute_derivatives);
        let i = insts(&s.spirv, sample);
        assert_eq!(i.len(), if sample == 87 { 3 } else { 1 });
        if let Some(bias) = op {
            assert!(i.iter().all(|i| i[5] == bias));
        }
        assert!(
            insts(&s.spirv, EXECUTION_MODE)
                .iter()
                .any(|m| m[2] == DERIVATIVE_GROUP_LINEAR)
        );
        let v = program(sph_vertex(), &[body]);
        assert_eq!(v.stage(), Stage::Vertex);
        assert!(translate(&v).is_err());
    }
    let s = compute(&[tex(0x160, &[(61, 3, 1), (87, 3, 1)])]);
    assert!(!s.interface.compute_derivatives);
    assert!(insts(&s.spirv, 87).is_empty());
}

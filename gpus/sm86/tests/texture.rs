mod common;

use common::translate;
use relift_gpu_nv::program::{Program, Stage};
use relift_gpu_nv::translate::{
    Dim, HandleSource, ImageDim, ImageKind, KIND_AT_RUN_TIME, Kind, SAMPLER_BINDING,
    TEXTURE_DIM_SPEC, TEXTURE_DIMS_BINDING, TEXTURE_FETCH_KIND_SPEC, image_binding,
    texture_binding,
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

fn compute(body: &[[u8; 16]]) -> relift_gpu_nv::translate::Shader {
    let mut b = Vec::new();
    for w in body {
        b.extend(w);
    }
    exit_and_halt(&mut b);
    translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"))
}

#[test]
fn bound_texture_uses_the_pools() {
    let s = compute(&[tex(0x160, &[(60, 1, 1), (61, 3, 1), (87, 3, 1)])]);
    assert!(s.interface.textures.contains(&(3, 16)));
    assert!(!s.interface.bindless);
    assert!(!s.interface.register_handles);
    assert!(s.interface.texture_kinds.is_empty());
    assert_eq!(
        s.interface.texture_fetch_kinds.get(&(3, 16)),
        Some(&TEXTURE_FETCH_KIND_SPEC)
    );
    for b in [
        SAMPLER_BINDING,
        texture_binding(Kind::Float, Dim::D2),
        texture_binding(Kind::Sint, Dim::D2),
        texture_binding(Kind::Uint, Dim::D2),
        TEXTURE_DIMS_BINDING,
    ] {
        assert!(s.interface.texture_bindings.contains(&b), "binding {b}");
    }
    assert_eq!(insts(&s.spirv, 88).len(), 3);
    assert!(
        insts(&s.spirv, 71)
            .iter()
            .all(|d| d[2] != 1 || !(0x100..0x200).contains(&d[3]))
    );
    let kind = insts(&s.spirv, 71)
        .into_iter()
        .find(|d| d[2] == 1 && d[3] == TEXTURE_FETCH_KIND_SPEC)
        .expect("the fetch kind constant")[1];
    assert!(
        insts(&s.spirv, 50)
            .iter()
            .any(|c| c[2] == kind && c[3] == KIND_AT_RUN_TIME),
        "the kind defaults to reading it at run time"
    );
    assert!(!insts(&s.spirv, 194).is_empty());
    assert!(
        insts(&s.spirv, 59).iter().any(|v| v.get(3) == Some(&6)),
        "the kind is kept in a private variable filled at entry"
    );
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
    assert!(s.interface.bindless);
    assert!(s.interface.register_handles);
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
    assert!(s.interface.bindless);
    assert!(s.interface.register_handles);
    let decos = insts(&s.spirv, 71);
    assert!(decos.iter().all(|d| d.get(2) != Some(&5300)));
    assert_eq!(insts(&s.spirv, 88).len(), 1);
}

#[test]
fn a_handle_loaded_from_a_bank_names_its_words() {
    let uldc = word(
        0x0b9,
        5,
        &[
            (16, 6, 6),
            (24, 6, 63),
            (38, 16, 0x10),
            (54, 5, 3),
            (73, 3, 4),
        ],
    );
    let ur = word(
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
    let s = compute(&[uldc, ur]);
    assert!(!s.interface.register_handles);
    let found: Vec<_> = s.interface.handle_sources.iter().copied().collect();
    assert_eq!(
        found,
        [HandleSource {
            bank: 3,
            offset: 0x10,
            indexed: false
        }]
    );
    let ldc = word(
        0x182,
        5,
        &[
            (16, 8, 4),
            (24, 8, 2),
            (38, 16, 0x20),
            (54, 5, 4),
            (73, 3, 4),
        ],
    );
    let mut b = tex(0x161, &[(59, 1, 1), (61, 3, 1), (87, 3, 1)]);
    b[1] = (b[1] & !0x0e) | (1 << 1);
    let s = compute(&[ldc, b]);
    assert!(!s.interface.register_handles);
    let found: Vec<_> = s.interface.handle_sources.iter().copied().collect();
    assert_eq!(
        found,
        [HandleSource {
            bank: 4,
            offset: 0x20,
            indexed: true
        }]
    );
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
        let e = translate(&relift_gpu_sm86::program::parse(&b))
            .err()
            .map(|e| e.to_string());
        assert!(e.is_some_and(|e| e.contains("offsets")), "{body:?}");
    }
}

/// Instructions between the move and the sample that write the registers
/// right below the offset register, but not it, keep it constant: a
/// texture result of three components (Rd, Rd + 1, Rd2, so Rd2 + 1 keeps
/// its value, as on an RTX 3090), one of four F16 components (Rd and Rd2
/// only) and a 32-bit constant load.
#[test]
fn writes_next_to_the_offset_keep_it_constant() {
    let t = tex(0x160, &[(61, 3, 1), (76, 1, 1), (87, 3, 1)]);
    let three = tex(
        0x160,
        &[(64, 8, 3), (32, 8, 255), (61, 3, 1), (72, 4, 7), (87, 3, 1)],
    );
    let f16 = tex(
        0x160,
        &[(64, 8, 3), (32, 8, 255), (61, 3, 1), (79, 2, 1), (87, 3, 1)],
    );
    let ldc = word(
        0x182,
        5,
        &[
            (16, 8, 3),
            (24, 8, 255),
            (38, 16, 0x20),
            (54, 5, 3),
            (73, 3, 4),
        ],
    );
    for w in [three, f16, ldc] {
        let s = compute(&[mov(4, 0xf1), w, t]);
        assert!(insts(&s.spirv, 88).iter().any(|i| i[5] & 8 != 0));
    }
}

/// A BRX reaches only the cases the translation gives it, so a later BRX
/// does not bring another value of the offset register into a block
/// before it.
#[test]
fn a_later_brx_does_not_reach_an_earlier_sample() {
    let t = tex(0x160, &[(61, 3, 1), (76, 1, 1), (87, 3, 1)]);
    let exit = word(0x14d, 4, &[(87, 3, 7)]);
    let brx = word(
        0x149,
        4,
        &[
            (24, 8, 2),
            (34, 48, (-28i64 as u64) & 0xffff_ffff_ffff),
            (87, 3, 7),
        ],
    );
    let s = compute(&[
        mov(4, 0xf1),
        bra_p0(3),
        t,
        exit,
        mov(4, 0x12),
        mov(2, 0x70),
        brx,
    ]);
    assert!(insts(&s.spirv, 88).iter().any(|i| i[5] & 8 != 0));
}

#[test]
fn run_time_sample_offsets_are_rejected() {
    for w in [
        tex(0x160, &[(61, 3, 1), (76, 1, 1), (87, 3, 1)]),
        tex(0x16c, &[(61, 3, 1), (76, 1, 1)]),
    ] {
        let mut b = w.to_vec();
        exit_and_halt(&mut b);
        let e = translate(&relift_gpu_sm86::program::parse(&b))
            .err()
            .map(|e| e.to_string());
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
    let s = translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
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
    let coherent =
        |s: &relift_gpu_nv::translate::Shader| insts(&s.spirv, 71).iter().any(|d| d[2] == COHERENT);
    assert!(coherent(&compute(&[sust, suld])));
    assert!(!coherent(&compute(&[suld])));
}

/// SUST has no sparse fault predicate: the bits SULD uses for one hold
/// something else, and a store must leave P0 as it was. SULD with one
/// writes it (as resident).
#[test]
fn surface_stores_leave_the_predicates_alone() {
    let (sust, _) = surface_words();
    let stores_false = |s: &relift_gpu_nv::translate::Shader| {
        let fals: Vec<u32> = insts(&s.spirv, 42).iter().map(|i| i[2]).collect();
        insts(&s.spirv, 62).iter().any(|i| fals.contains(&i[2]))
    };
    assert!(!stores_false(&compute(&[sust])));
    let suld_p0 = word(
        0xb99,
        0,
        &[
            (16, 8, 0),
            (24, 8, 8),
            (40, 14, 0x13),
            (54, 5, 2),
            (61, 3, 3),
            (72, 1, 1),
            (73, 3, 4),
            (81, 3, 0),
            (84, 3, 1),
        ],
    );
    assert!(stores_false(&compute(&[suld_p0])));
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
    assert_eq!(insts(&s.spirv, 99).len(), 4);
    assert_eq!(insts(&s.spirv, 98).len(), 1);
    assert!(
        s.interface
            .texture_bindings
            .contains(&image_binding(ImageKind::Raw32, ImageDim::D2))
    );
    assert!(s.interface.textures.contains(&(2, 0x48)));
}

/// The instruction defining SPIR-V id `id`, if any.
fn def(words: &[u32], id: u32) -> Option<Vec<u32>> {
    let mut k = 5;
    while k < words.len() {
        let n = ((words[k] >> 16) as usize).max(1);
        let w = &words[k..k + n];
        let op = w[0] & 0xffff;
        let at = match op {
            43 | 61 | 80 | 81 | 98 => 2,
            _ => 0,
        };
        if at > 0 && w.get(at) == Some(&id) {
            return Some(w.to_vec());
        }
        k += n;
    }
    None
}

/// Whether id is the u32 constant v.
fn is_const(words: &[u32], id: u32, v: u32) -> bool {
    def(words, id).is_some_and(|w| w[0] & 0xffff == 43 && w[3] == v)
}

/// A formatted surface access of an RGB9E5 image moves its 32-bit texels unchanged through the raw array, as the
/// console does (it has no shared-exponent surface encoder; programs pack the format themselves): when the kind
/// constant is `SURFACE_KIND_RGB9E5`, a store writes its first data register as the texel, and a load returns the
/// texel in the first register, 0 in the next two and 1 in the last, as an R32UI load does.
#[test]
fn formatted_surface_accesses_of_rgb9e5_move_raw_texels() {
    const SPEC_CONSTANT_OP: u32 = 52;
    const IMAGE_READ: u32 = 98;
    const IMAGE_WRITE: u32 = 99;
    const LOAD: u32 = 61;
    const COMPOSITE_CONSTRUCT: u32 = 80;
    const COMPOSITE_EXTRACT: u32 = 81;
    let (sust, _) = surface_words();
    let suld_p = word(
        0xb97,
        0,
        &[
            (16, 8, 0),
            (24, 8, 8),
            (40, 14, 0x13),
            (54, 5, 2),
            (59, 2, 0),
            (61, 3, 3),
            (72, 4, 0xf),
            (81, 3, 7),
            (84, 3, 1),
        ],
    );
    let s = compute(&[sust, suld_p]);
    common::validate(&s.spirv);
    let w = &s.spirv;
    assert!(
        s.interface
            .texture_bindings
            .contains(&image_binding(ImageKind::Raw32, ImageDim::D2))
    );
    let k = w
        .windows(4)
        .any(|x| x[0] == (4 << 16 | 43) && x[3] == relift_gpu_nv::translate::SURFACE_KIND_RGB9E5);
    assert!(k, "a constant of the RGB9E5 kind");
    assert!(!insts(w, SPEC_CONSTANT_OP).is_empty());
    let raw_store = insts(w, IMAGE_WRITE).iter().any(|i| {
        def(w, i[3]).is_some_and(|t| {
            t[0] & 0xffff == COMPOSITE_CONSTRUCT
                && def(w, t[3]).is_some_and(|x| x[0] & 0xffff == LOAD)
                && t[4..7].iter().all(|&z| is_const(w, z, 0))
        })
    });
    assert!(raw_store, "a store of the first data register unchanged");
    let raw_load = insts(w, COMPOSITE_CONSTRUCT).iter().any(|t| {
        t.len() == 7
            && def(w, t[3]).is_some_and(|x| {
                x[0] & 0xffff == COMPOSITE_EXTRACT
                    && x[4] == 0
                    && def(w, x[3]).is_some_and(|r| r[0] & 0xffff == IMAGE_READ)
            })
            && is_const(w, t[4], 0)
            && is_const(w, t[5], 0)
            && is_const(w, t[6], 1)
    });
    assert!(raw_load, "a load of the texel unchanged, as R32UI");
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
        assert!(translate(&relift_gpu_sm86::program::parse(&b)).is_err());
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
    relift_gpu_sm86::program::parse(&b)
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
    let s = translate(&program(Vec::new(), &[w])).unwrap_or_else(|e| panic!("{e}"));
    assert!(
        s.interface.texture_bindings.contains(&TEXTURE_DIMS_BINDING),
        "a handle from a register reads the runtime's dimension table"
    );
    assert!(s.interface.texture_dims.is_empty());
    assert_eq!(insts(&s.spirv, SWITCH).len(), 1);
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
    let s = translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
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
    let s = translate(&relift_gpu_sm86::program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
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

/// `CALL.REL.NOINC` at line `at` to line `to`.
fn call_rel(at: usize, to: usize) -> [u8; 16] {
    let rel = (to as i64 - at as i64 - 1) * 4;
    word(
        0x144,
        4,
        &[
            (34, 48, (rel as u64) & 0xffff_ffff_ffff),
            (86, 1, 1),
            (87, 3, 7),
        ],
    )
}

/// `RET.REL.NODEC R20` at line `at`, to the address R20 holds.
fn ret_rel(at: usize) -> [u8; 16] {
    let rel = -(16 * (at as i64 + 1)) / 4;
    word(
        0x150,
        4,
        &[
            (24, 8, 20),
            (34, 48, (rel as u64) & 0xffff_ffff_ffff),
            (86, 1, 1),
            (87, 3, 7),
        ],
    )
}

/// An offset set before a call to a subroutine that does not write it
/// stays constant after the call; one the subroutine changes at run time
/// does not.
#[test]
fn offsets_are_followed_through_calls() {
    let t = tex(0x160, &[(61, 3, 1), (76, 1, 1), (87, 3, 1)]);
    let exit = word(0x14d, 4, &[(87, 3, 7)]);
    let iadd = word(
        0x010,
        4,
        &[
            (16, 8, 4),
            (24, 8, 4),
            (32, 32, 1),
            (64, 8, 255),
            (81, 3, 7),
            (84, 3, 7),
        ],
    );
    let program = |sub: [u8; 16]| {
        let mut b = Vec::new();
        for w in [
            mov(4, 0xf1),
            mov(20, 0x30),
            call_rel(2, 5),
            t,
            exit,
            sub,
            ret_rel(6),
        ] {
            b.extend(w);
        }
        exit_and_halt(&mut b);
        b
    };
    let s = translate(&relift_gpu_sm86::program::parse(&program(mov(9, 0))))
        .unwrap_or_else(|e| panic!("{e}"));
    assert!(insts(&s.spirv, 88).iter().any(|i| i[5] & 8 != 0));
    let e = translate(&relift_gpu_sm86::program::parse(&program(iadd)))
        .err()
        .map(|e| e.to_string());
    assert!(e.is_some_and(|e| e.contains("offsets")));
}

/// Trace mode's `handle` condition records the handle word of a texture read through a register handle (one loaded
/// from memory, as a material table's): the module stores a record with destination `TRACE_HANDLE`. A bound texture
/// (handle from a constant bank) records nothing, and neither does any other write.
#[test]
fn trace_mode_records_register_handles_of_texture_reads() {
    use relift_gpu_nv::translate::{Options, TRACE_HANDLE, Trace, translate_with};
    const STORE: u32 = 62;
    const CONSTANT: u32 = 43;
    let opts = Options {
        trace: Some(Trace::parse("handle").unwrap()),
        ..Options::default()
    };
    let build = |w: [u8; 16]| {
        let mut b = Vec::new();
        b.extend(w);
        exit_and_halt(&mut b);
        translate_with(&relift_gpu_sm86::program::parse(&b), &opts)
            .unwrap_or_else(|e| panic!("{e}"))
    };
    let stores_handle = |s: &relift_gpu_nv::translate::Shader| {
        let consts: Vec<u32> = insts(&s.spirv, CONSTANT)
            .iter()
            .filter(|c| c.len() == 4 && c[3] == TRACE_HANDLE)
            .map(|c| c[2])
            .collect();
        insts(&s.spirv, STORE)
            .iter()
            .any(|st| consts.contains(&st[2]))
    };
    let mut w = tex(0x161, &[(59, 1, 1), (61, 3, 1), (87, 3, 1)]);
    w[1] = (w[1] & !0x0e) | (1 << 1);
    let bindless = build(w);
    common::validate(&bindless.spirv);
    assert!(bindless.interface.trace_words > 0, "a trace buffer");
    assert!(stores_handle(&bindless), "a record of the handle word");
    let uniform = build(word(
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
    ));
    common::validate(&uniform.spirv);
    assert!(
        stores_handle(&uniform),
        "a record of a uniform register handle"
    );
    let bound = build(tex(0x160, &[(60, 1, 1), (61, 3, 1), (87, 3, 1)]));
    assert!(
        !stores_handle(&bound),
        "no handle record for a bound texture"
    );
    assert_eq!(bound.interface.trace_words, 0, "nothing traced");
    assert!(Trace::parse("handle").unwrap().handle);
}

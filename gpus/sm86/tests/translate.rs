use relift_gpu_sm86::program::{Program, Stage};
use relift_gpu_sm86::translate::{Interp, Options, translate, translate_with};

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
    let p = Program::parse(&b);
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
    let s = translate(&Program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
    header_ok(&s.spirv);
    assert_eq!(s.interface.inputs.get(&0), Some(&Some(Interp::Perspective)));
    assert!(s.interface.color_targets.contains(&0));
}

#[test]
fn rejects_what_it_cannot_translate() {
    let mut b = Vec::new();
    b.extend(word(0x150, 4, &[(24, 8, 4), (86, 1, 1), (87, 3, 7)]));
    exit_and_halt(&mut b);
    assert!(translate(&Program::parse(&b)).is_err());
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
    translate(&Program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
}

#[test]
fn converged_branch_always_leaves_the_slow_path() {
    let mut b = Vec::new();
    b.extend(bra_mode(0, 2, 3));
    b.extend(unsupported());
    b.extend(word(0x14d, 4, &[(87, 3, 7)]));
    exit_and_halt(&mut b);
    translate(&Program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
}

#[test]
fn exclusive_warpsync_is_rejected() {
    let sync = |exclusive: u64| word(0x148, 1, &[(32, 8, 14), (86, 1, exclusive), (87, 3, 7)]);
    let mut b = Vec::new();
    b.extend(sync(0));
    exit_and_halt(&mut b);
    let p = Program::parse(&b);
    assert_eq!(text(&p)[0], "WARPSYNC R14");
    translate(&p).unwrap_or_else(|e| panic!("{e}"));
    let mut b = Vec::new();
    b.extend(sync(1));
    exit_and_halt(&mut b);
    let p = Program::parse(&b);
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
    let p = Program::parse(&b);
    assert_eq!(p.stage(), Stage::Compute);
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    header_ok(&s.spirv);
    assert!(spirv_has(&s.spirv, 246), "no OpLoopMerge");
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
    let s = translate(&Program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
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
        let p = Program::parse(&b);
        let text: Vec<String> = p
            .decode()
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
            let p = Program::parse(&b);
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
    p.decode()
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
    let p = Program::parse(&b);
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

#[test]
fn fragment_outputs_take_four_registers_per_target() {
    let mut b = sph(2, 5, &[(18, 0x31), (19, 3)]);
    exit_and_halt(&mut b);
    let s = translate(&Program::parse(&b)).unwrap_or_else(|e| panic!("{e}"));
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
    let p = Program::parse(&b);
    assert_eq!(text(&p)[2], "AST a[0x5c], R0");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    dump("vertex_system_values", &s.spirv);
    assert_eq!(s.interface.clip_distances, 2);
    for bi in [42, 43, 1, 3] {
        assert!(builtin_used(&s.spirv, bi), "built-in {bi}");
    }
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
    let one = translate(&Program::parse(&calls(1))).unwrap_or_else(|e| panic!("{e}"));
    let p = Program::parse(&calls(2));
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
    let e = translate(&Program::parse(&b)).err().map(|e| e.0);
    assert_eq!(e.as_deref(), Some("CALL without its return address"));
}

#[test]
fn return_address_must_be_in_the_register_ret_reads() {
    let mut b = calls(1);
    b[..16].copy_from_slice(&mov_imm(5, 0x20));
    let p = Program::parse(&b);
    assert_eq!(text(&p)[0], "MOV R5, 0x20");
    let e = translate(&p).err().map(|e| e.0);
    assert_eq!(e.as_deref(), Some("CALL without its return address"));
    let mut b = calls(1);
    b[5 * 16..6 * 16].copy_from_slice(&word(0x002, 1, &[(16, 8, 6), (32, 8, 4), (72, 4, 0xf)]));
    b[6 * 16..7 * 16].copy_from_slice(&ret(6, 6));
    let p = Program::parse(&b);
    assert_eq!(text(&p)[5], "MOV R6, R4");
    translate(&p).unwrap_or_else(|e| panic!("{e}"));
    b[..16].copy_from_slice(&mov_imm(5, 0x20));
    let e = translate(&Program::parse(&b)).err().map(|e| e.0);
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
    let p = Program::parse(&b);
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
    let p = Program::parse(&b);
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
    let p = Program::parse(&b);
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
    let p = Program::parse(&b);
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
        let p = Program::parse(&b);
        assert!(
            p.decode()[0]
                .as_ref()
                .is_ok_and(|i| i.mnemonic == "FSWZADD")
        );
        translate(&p).unwrap_or_else(|e| panic!("{e}"));
    }
}

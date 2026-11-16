use relift_gpu_sm86::program::{Program, Stage};
use relift_gpu_sm86::translate::{Interp, translate};

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
    let mut b = sph(1, 1, &[]);
    b.extend(word(
        0x002,
        5,
        &[(16, 8, 0), (32, 27, 0x10 << 4), (72, 4, 0xf)],
    ));
    exit_and_halt(&mut b);
    assert!(translate(&Program::parse(&b)).is_err());
}

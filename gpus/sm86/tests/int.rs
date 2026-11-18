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

fn program(body: &[[u8; 16]]) -> Program {
    let mut b = Vec::new();
    for w in body {
        b.extend(w);
    }
    b.extend(word(0x14d, 4, &[(87, 3, 7)]));
    b.extend(word(0x147, 4, &[(34, 48, 0xffff_ffff_fffc), (87, 3, 7)]));
    relift_gpu_sm86::program::parse(&b)
}

fn check(name: &str, w: [u8; 16], text: &str) {
    let p = program(&[w]);
    let first = p.code.clone().into_iter().next().and_then(Result::ok);
    let shown = first.map(|i| i.to_string()).unwrap_or_default();
    assert!(
        shown.starts_with(text),
        "{name}: decoded as {shown:?}, wanted {text:?}"
    );
    let s = translate(&p).unwrap_or_else(|e| panic!("{name}: {e}"));
    assert_eq!(s.spirv[0], 0x0723_0203, "{name}");
}

#[test]
fn integer_arithmetic() {
    check(
        "iadd3 carries",
        word(
            0x010,
            1,
            &[
                (16, 8, 6),
                (24, 8, 4),
                (32, 8, 5),
                (64, 8, 255),
                (81, 3, 1),
                (84, 3, 2),
            ],
        ),
        "IADD3 R6, P1, P2, R4, R5, RZ",
    );
    check(
        "iadd3 negated",
        word(
            0x010,
            1,
            &[
                (16, 8, 6),
                (24, 8, 4),
                (32, 8, 5),
                (64, 8, 255),
                (72, 1, 1),
                (81, 3, 7),
                (84, 3, 7),
            ],
        ),
        "IADD3 R6, -R4, R5, RZ",
    );
    check(
        "imad wide",
        word(
            0x025,
            1,
            &[
                (16, 8, 6),
                (24, 8, 4),
                (32, 8, 5),
                (64, 8, 8),
                (73, 1, 1),
                (81, 3, 7),
            ],
        ),
        "IMAD.WIDE R6, R4, R5, R8",
    );
    check(
        "imad hi carry",
        word(
            0x027,
            1,
            &[(16, 8, 6), (24, 8, 4), (32, 8, 5), (64, 8, 8), (81, 3, 1)],
        ),
        "IMAD.HI",
    );
    check(
        "lea hi sx32",
        word(
            0x011,
            1,
            &[
                (16, 8, 6),
                (24, 8, 4),
                (32, 8, 5),
                (64, 8, 255),
                (75, 5, 3),
                (80, 1, 1),
                (73, 1, 1),
                (81, 3, 7),
                (87, 3, 7),
            ],
        ),
        "LEA.HI.SX32",
    );
    check(
        "shf u64",
        word(
            0x019,
            1,
            &[
                (16, 8, 6),
                (24, 8, 4),
                (32, 8, 5),
                (64, 8, 8),
                (76, 1, 1),
                (73, 2, 0),
            ],
        ),
        "SHF.R.S64",
    );
    check(
        "isetp extended",
        word(
            0x00c,
            1,
            &[
                (76, 3, 5),
                (72, 1, 1),
                (81, 3, 1),
                (84, 3, 7),
                (24, 8, 4),
                (32, 8, 5),
                (87, 3, 7),
                (68, 3, 7),
            ],
        ),
        "ISETP.NE",
    );
}

#[test]
fn bits_and_predicates() {
    check(
        "lop3 with predicate",
        word(
            0x012,
            1,
            &[
                (16, 8, 255),
                (24, 8, 4),
                (32, 8, 5),
                (64, 8, 6),
                (72, 8, 0x96),
                (81, 3, 1),
                (87, 3, 7),
            ],
        ),
        "LOP3.LUT P1",
    );
    check(
        "prmt",
        word(0x016, 1, &[(16, 8, 6), (24, 8, 4), (32, 8, 5), (64, 8, 8)]),
        "PRMT",
    );
    check(
        "popc",
        word(0x109, 1, &[(16, 8, 6), (32, 8, 4)]),
        "POPC R6, R4",
    );
    check(
        "flo",
        word(0x100, 1, &[(16, 8, 6), (32, 8, 4), (73, 1, 0), (74, 1, 1)]),
        "FLO",
    );
}

#[test]
fn conversions() {
    check(
        "f2i 64",
        word(
            0x105,
            1,
            &[(16, 8, 6), (32, 8, 4), (72, 1, 1), (75, 2, 3), (84, 3, 2)],
        ),
        "F2I",
    );
    check(
        "i2f 64",
        word(
            0x112,
            1,
            &[(16, 8, 6), (32, 8, 4), (74, 1, 1), (84, 3, 3), (75, 3, 2)],
        ),
        "I2F",
    );
    check(
        "f2f half to single",
        word(0x104, 1, &[(16, 8, 6), (32, 8, 4), (75, 3, 2), (84, 3, 1)]),
        "F2F",
    );
    check(
        "f2fp pack",
        word(
            0x03e,
            1,
            &[(16, 8, 6), (24, 8, 4), (32, 8, 5), (64, 8, 255)],
        ),
        "F2FP",
    );
    check(
        "f2fp merge",
        word(0x03e, 1, &[(16, 8, 6), (32, 8, 5), (64, 8, 4), (78, 1, 1)]),
        "F2FP.MERGE_C",
    );
}

#[test]
fn half_arithmetic() {
    check(
        "hadd2 single result",
        word(
            0x030,
            1,
            &[
                (16, 8, 6),
                (24, 8, 255),
                (32, 8, 4),
                (72, 1, 1),
                (78, 1, 1),
                (60, 2, 3),
            ],
        ),
        "HADD2.F32",
    );
    check(
        "hfma2 swizzled",
        word(
            0x031,
            1,
            &[
                (16, 8, 6),
                (24, 8, 4),
                (32, 8, 5),
                (64, 8, 8),
                (74, 2, 2),
                (60, 2, 3),
                (77, 1, 1),
            ],
        ),
        "HFMA2.SAT",
    );
    check(
        "hfma2 negated high half",
        word(
            0x031,
            1,
            &[(16, 8, 6), (24, 8, 4), (32, 8, 5), (64, 8, 8), (86, 1, 1)],
        ),
        "HFMA2 R6, R4, R5.H0_NH1",
    );
    check(
        "hset2",
        word(
            0x033,
            1,
            &[(16, 8, 6), (24, 8, 4), (32, 8, 5), (76, 4, 4), (87, 3, 7)],
        ),
        "HSET2",
    );
    check(
        "hmnmx2",
        word(0x040, 1, &[(16, 8, 6), (24, 8, 4), (32, 8, 5), (87, 3, 7)]),
        "HMNMX2",
    );
}

#[test]
fn uniform_datapath() {
    check(
        "uiadd3",
        word(
            0x090,
            1,
            &[
                (16, 6, 4),
                (24, 6, 5),
                (32, 6, 6),
                (64, 6, 63),
                (81, 3, 1),
                (84, 3, 7),
            ],
        ),
        "UIADD3",
    );
    check(
        "ushf",
        word(
            0x099,
            1,
            &[(16, 6, 4), (24, 6, 5), (32, 6, 6), (64, 6, 63), (76, 1, 1)],
        ),
        "USHF",
    );
    check(
        "ulop3",
        word(
            0x092,
            1,
            &[
                (16, 6, 4),
                (24, 6, 5),
                (32, 6, 6),
                (64, 6, 63),
                (72, 8, 0xc0),
                (81, 3, 7),
            ],
        ),
        "ULOP3.LUT",
    );
    check(
        "r2ur",
        word(0x1c2, 1, &[(16, 6, 4), (24, 8, 5), (81, 3, 7)]),
        "R2UR UR4, R5",
    );
}

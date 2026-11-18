use relift_gpu_sm53::field::Kind;
use relift_gpu_sm53::form::ENCODINGS;
use relift_gpu_sm53::lower::table::{Row, forms, markdown, row};
use relift_gpu_sm53::lower::{Lowered, lower};
use relift_gpu_sm53::{Inst, Op, Src};

const PATH: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/LOWERING.md");

#[test]
fn lowering_md_is_current() {
    let want = markdown();
    if std::env::var_os("RELIFT_BLESS").is_some() {
        relift_cache::write_if_changed(std::path::Path::new(PATH), want.as_bytes())
            .expect("LOWERING.md is writable");
    }
    let have = std::fs::read_to_string(PATH).unwrap_or_default();
    assert!(
        have == want,
        "gpus/sm53/LOWERING.md is stale; run RELIFT_BLESS=1 cargo test -p relift-gpu-sm53 --test lowering"
    );
}

#[test]
fn every_encoding_has_a_row_with_its_reason() {
    for e in &ENCODINGS {
        assert!(forms().contains(&(e.op, e.src)));
        match row(e.op, e.src) {
            Row::Exact { to, evidence } => assert!(!to.is_empty() && !evidence.is_empty()),
            Row::Pseudo { name, evidence } => assert!(!name.is_empty() && !evidence.is_empty()),
            Row::Split {
                to,
                pseudo,
                evidence,
            } => assert!(!to.is_empty() && !pseudo.is_empty() && !evidence.is_empty()),
            Row::Stop { reason } => assert!(!reason.is_empty()),
        }
    }
}

#[test]
fn a_program_names_each_form_that_stops_once() {
    let mut b = 0x1234_5678u32.to_le_bytes().to_vec();
    b.resize(0x30, 0);
    let masked = 0x0100_0000_0007_0000u64;
    let mov = masked | 0xf000;
    let exit = 0xe300_0000_0007_000fu64;
    for w in [0u64, masked, mov, masked, 0, exit, exit, exit] {
        b.extend(w.to_le_bytes());
    }
    let p = relift_gpu_sm53::Program::parse(&b).expect("parses");
    let s = relift_gpu_sm53::lower::stops(&p);
    let names: Vec<(&str, u64, &str)> = s.iter().map(|x| (x.op.name(), x.at, x.reason)).collect();
    assert_eq!(
        names,
        [
            ("MOV", 8, "MOV32I with a lane mask other than 0xf"),
            ("EXIT", 40, "no semantic check yet")
        ]
    );
}

/// A Maxwell word of the encoding `op`/`src` with `fields` set (by
/// Ryujinx's field names) and the guard predicate PT.
fn maxwell(op: Op, src: Src, fields: &[(&str, u64)]) -> Inst {
    let e = ENCODINGS
        .iter()
        .find(|e| (e.op, e.src) == (op, src))
        .expect("an encoding");
    let mut w = e.bits;
    for &(name, v) in fields.iter().chain(&[("Pred", 7)]) {
        let f = e.form.field(name).expect("a field");
        let &[(shift, mask)] = f.parts else {
            panic!("{name} has several parts");
        };
        let v = if f.kind == Kind::Bool && v != 0 {
            mask
        } else {
            v & mask
        };
        w = (w & !(mask << shift)) | (v << shift);
    }
    Inst::decode(w, 0).expect("decodes")
}

fn sm86_text(l: &Lowered) -> String {
    match l {
        Lowered::Word(w) => relift_gpu_sm86::decode(*w, 0)
            .map(|i| i.to_string())
            .unwrap_or_else(|e| format!("?? {e}")),
        Lowered::Pseudo(i) => format!("pseudo {i}"),
        Lowered::Stop(r) => format!("stop: {r}"),
    }
}

#[test]
fn checked_forms_lower_to_these_sm86_instructions() {
    let rz = ("SrcA", 255);
    let cases: &[(Inst, &str)] = &[
        (
            maxwell(
                Op::Mov,
                Src::Imm32,
                &[("Dest", 3), ("Imm32", 0x3f80_0000), ("QuadMask", 0xf)],
            ),
            "MOV R3, 0x3f800000",
        ),
        (
            maxwell(Op::Mov, Src::Imm32, &[("Dest", 3), ("QuadMask", 0x3)]),
            "stop: MOV32I with a lane mask other than 0xf",
        ),
        (
            maxwell(
                Op::Mov,
                Src::CBuf,
                &[
                    ("Dest", 1),
                    ("CbufSlot", 3),
                    ("CbufOffset", 0x44),
                    ("QuadMask", 0xf),
                ],
            ),
            "MOV R1, c[0x3][0x110]",
        ),
        (
            maxwell(
                Op::Mov,
                Src::CBuf,
                &[("Dest", 1), ("CbufOffset", 0x2000), ("QuadMask", 0xf)],
            ),
            "stop: constant offset at or above 0x8000 (SM86 reads it as negative)",
        ),
        (
            maxwell(
                Op::Ald,
                Src::None,
                &[("Dest", 4), rz, ("SrcB", 255), ("Imm11", 0x84)],
            ),
            "ALD R4, a[0x84]",
        ),
        (
            maxwell(
                Op::Ald,
                Src::None,
                &[
                    ("Dest", 4),
                    rz,
                    ("SrcB", 9),
                    ("Imm11", 0x90),
                    ("AlSize", 3),
                    ("O", 1),
                ],
            ),
            "ALD.O.128 R4, a[0x90], R9",
        ),
        (
            maxwell(
                Op::Ald,
                Src::None,
                &[("Dest", 4), ("SrcA", 2), ("SrcB", 255), ("Imm11", 0x84)],
            ),
            "stop: ALD with a register in the attribute address",
        ),
    ];
    for (i, want) in cases {
        assert_eq!(sm86_text(&lower(i)), *want, "{:016x}", i.word);
    }
}

fn ipa(op: u64, msi: u64, sat: u64, b: u64, attr: u64) -> Inst {
    maxwell(
        Op::Ipa,
        Src::None,
        &[
            ("Dest", 2),
            ("SrcA", 255),
            ("SrcB", b),
            ("SrcC", 255),
            ("IpaOp", op),
            ("Msi", msi),
            ("Sat", sat),
            ("Imm10", attr),
            ("SrcPred", 7),
        ],
    )
}

#[test]
fn ipa_lowers_by_case() {
    let cases: &[(Inst, &str)] = &[
        (ipa(0, 0, 0, 255, 0x7c), "IPA.PASS R2, a[0x7c]"),
        (ipa(0, 1, 0, 255, 0x80), "IPA.PASS.CENTROID R2, a[0x80]"),
        (ipa(2, 0, 0, 255, 0x3fc), "IPA.CONSTANT R2, a[0x3fc]"),
        (ipa(1, 0, 0, 0, 0x84), "pseudo IPA.PASS.MUL R2, a[0x84], R0"),
        (
            ipa(1, 1, 1, 0, 0x84),
            "pseudo IPA.PASS.CENTROID.MUL.SAT R2, a[0x84], R0",
        ),
        (
            ipa(2, 0, 1, 255, 0x90),
            "pseudo IPA.CONSTANT.SAT R2, a[0x90]",
        ),
        (ipa(3, 0, 0, 255, 0x80), "stop: IPA.SC"),
        (
            ipa(0, 2, 0, 255, 0x80),
            "stop: IPA at an offset or a sample",
        ),
        (
            ipa(0, 0, 0, 1, 0x80),
            "stop: IPA with a multiplier register but no .MUL",
        ),
    ];
    for (i, want) in cases {
        assert_eq!(sm86_text(&lower(i)), *want, "{:016x}", i.word);
    }
}

#[test]
fn every_checked_row_lowers_its_encodings() {
    for e in &ENCODINGS {
        if matches!(row(e.op, e.src), Row::Stop { .. }) {
            continue;
        }
        let i = Inst::decode(e.bits, 0).expect("decodes");
        assert_ne!(
            lower(&i),
            Lowered::Stop("no lowering for a checked row"),
            "{}",
            e.op.name()
        );
    }
}

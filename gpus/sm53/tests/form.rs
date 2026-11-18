use relift_gpu_sm53::bundle::{BUNDLE, is_sched, sched, slots};
use relift_gpu_sm53::form::ENCODINGS;
use relift_gpu_sm53::{Op, Src, classify};

/// A word with the top bits `op` (16 bits) and zeros elsewhere.
fn op16(op: u64) -> u64 {
    op << 48
}

#[test]
fn every_encoding_classifies_as_itself_or_a_more_specific_one() {
    for e in &ENCODINGS {
        let got = classify(e.bits).expect("its own fixed bits match");
        assert!(
            got.op == e.op && got.src == e.src || got.fixed() > e.fixed(),
            "{:?}/{:?} decoded as {:?}/{:?}",
            e.op,
            e.src,
            got.op,
            got.src
        );
    }
}

#[test]
fn no_two_encodings_tie() {
    for (i, a) in ENCODINGS.iter().enumerate() {
        for b in &ENCODINGS[i + 1..] {
            let common = a.mask & b.mask;
            let overlap = a.bits & common == b.bits & common;
            assert!(
                !overlap || a.fixed() != b.fixed() || (a.op, a.src) == (b.op, b.src),
                "{:?}/{:?} and {:?}/{:?} match the same words with as many fixed bits",
                a.op,
                a.src,
                b.op,
                b.src
            );
        }
    }
}

/// The opcodes Mesa NAK's SM50 encoder (sm50.rs, MIT) writes in bits 48 to
/// 63, and what each is. NAK's bound-texture TEX (0x0380) is left out: it
/// is the LOP3 constant-bank form in both other tables, and envytools
/// gives TEX as 0xc038.
const NAK: &[(u64, Op, Src)] = &[
    (0x0800, Op::Fadd, Src::Imm32),
    (0x5c58, Op::Fadd, Src::Reg),
    (0x3858, Op::Fadd, Src::Imm),
    (0x4c58, Op::Fadd, Src::CBuf),
    (0x5980, Op::Ffma, Src::Reg),
    (0x3280, Op::Ffma, Src::Imm),
    (0x4980, Op::Ffma, Src::CBuf),
    (0x5180, Op::Ffma, Src::RegCBuf),
    (0x1e00, Op::Fmul, Src::Imm32),
    (0x5c68, Op::Fmul, Src::Reg),
    (0x5c90, Op::Rro, Src::Reg),
    (0x5080, Op::Mufu, Src::None),
    (0x5bb0, Op::Fsetp, Src::Reg),
    (0x5c70, Op::Dadd, Src::Reg),
    (0x5c00, Op::Bfe, Src::Reg),
    (0x1c00, Op::Iadd, Src::Imm32),
    (0x5c10, Op::Iadd, Src::Reg),
    (0x5a00, Op::Imad, Src::Reg),
    (0x1fc0, Op::Imul, Src::Imm32),
    (0x5b60, Op::Isetp, Src::Reg),
    (0x0400, Op::Lop, Src::Imm32),
    (0x5c40, Op::Lop, Src::Reg),
    (0x5c48, Op::Shl, Src::Reg),
    (0x5c28, Op::Shr, Src::Reg),
    (0x5ca8, Op::F2f, Src::Reg),
    (0x5cb0, Op::F2i, Src::Reg),
    (0x5cb8, Op::I2f, Src::Reg),
    (0x5ce0, Op::I2i, Src::Reg),
    (0x5c98, Op::Mov, Src::Reg),
    (0x0100, Op::Mov, Src::Imm32),
    (0x5bc0, Op::Prmt, Src::Reg),
    (0x5ca0, Op::Sel, Src::Reg),
    (0xef10, Op::Shfl, Src::None),
    (0x5090, Op::Psetp, Src::None),
    (0xdeb8, Op::TexB, Src::None),
    (0xdc38, Op::Tld, Src::None),
    (0xc838, Op::Tld4, Src::None),
    (0xdf48, Op::Txq, Src::None),
    (0xeb00, Op::SuldB, Src::None),
    (0xeb20, Op::SustB, Src::None),
    (0xeac0, Op::SuatomCas, Src::None),
    (0xea60, Op::Suatom, Src::None),
    (0xef90, Op::Ldc, Src::None),
    (0xebf8, Op::Red, Src::None),
    (0xed00, Op::Atom, Src::None),
    (0xec00, Op::Atoms, Src::None),
    (0xefd8, Op::Ald, Src::None),
    (0xeff0, Op::Ast, Src::None),
    (0xe000, Op::Ipa, Src::None),
    (0xef98, Op::Membar, Src::None),
    (0xe240, Op::Bra, Src::None),
    (0xe290, Op::Ssy, Src::None),
    (0xf0f8, Op::Sync, Src::None),
    (0xe340, Op::Brk, Src::None),
    (0xe2a0, Op::Pbk, Src::None),
    (0xe350, Op::Cont, Src::None),
    (0xe2b0, Op::Pcnt, Src::None),
    (0xe300, Op::Exit, Src::None),
    (0xf0a8, Op::Bar, Src::None),
    (0xefd0, Op::Isberd, Src::None),
    (0xe330, Op::Kil, Src::None),
    (0x50b0, Op::Nop, Src::None),
    (0xf0c8, Op::S2r, Src::None),
    (0x50d8, Op::Vote, Src::None),
    (0xfbe0, Op::Out, Src::Reg),
    (0xf6e0, Op::Out, Src::Imm),
    (0xebe0, Op::Out, Src::CBuf),
];

#[test]
fn nak_opcodes_name_the_same_instructions() {
    for &(op, want, src) in NAK {
        let e = classify(op16(op)).unwrap_or_else(|| panic!("{op:#06x} unknown"));
        assert_eq!((e.op, e.src), (want, src), "{op:#06x}");
    }
}

#[test]
fn envytools_tex_is_tex() {
    assert_eq!(classify(op16(0xc038)).map(|e| e.op), Some(Op::Tex));
}

#[test]
fn names_split_variant_words() {
    assert_eq!(Op::TexsF16.name(), "TEXS_F16");
    assert_eq!(Op::ShfL.name(), "SHF_L");
    assert_eq!(Op::Ffma.name(), "FFMA");
}

#[test]
fn scheduling_fields_follow_nak() {
    let f = 0xf | 1 << 4 | 2 << 5 | 3 << 8 | 0x2a << 11 | 0x9 << 17;
    let s = sched(f << 21, 1);
    assert_eq!(
        (s.stall, s.yield_, s.wr_bar, s.rd_bar, s.wait_mask, s.reuse),
        (0xf, true, 2, 3, 0x2a, 0x9)
    );
    assert_eq!(sched(f << 21, 0).stall, 0);
}

#[test]
fn slots_skip_scheduling_words() {
    let mut code = vec![0u8; 2 * BUNDLE];
    for k in 0..8u64 {
        code[8 * k as usize..8 * k as usize + 8].copy_from_slice(&(k + 100).to_le_bytes());
    }
    let got: Vec<(u64, u64)> = slots(&code).map(|s| (s.at, s.word)).collect();
    assert_eq!(
        got,
        [
            (8, 101),
            (16, 102),
            (24, 103),
            (40, 105),
            (48, 106),
            (56, 107)
        ]
    );
    assert!(is_sched(0) && is_sched(32) && !is_sched(8));
}

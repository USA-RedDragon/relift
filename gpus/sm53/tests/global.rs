//! Maxwell global accesses through a driver storage buffer slot's base:
//! the address pair made with IADD.CC and IADD.X (or ISCADD.CC) from the
//! slot's c[0] words lowers to the SM86 carry forms, and the translator
//! then reaches the slot through its descriptor, not a buffer device
//! address.

use relift_gpu_sm53::field::Kind;
use relift_gpu_sm53::form::ENCODINGS;
use relift_gpu_sm53::program::{CODE_HEADER_LEN, CODE_MAGIC, SPH_LEN};
use relift_gpu_sm53::{Op, Program, Src};

const EXIT: u64 = 0xe300_0000_0007_000f;
const NOP: u64 = 0x50b0_0000_0007_0f00;
const CONVERT_U_TO_PTR: u32 = 120;

/// The word of the encoding `op`/`src` with `fields` set (Ryujinx's names)
/// and the guard PT.
fn word(op: Op, src: Src, fields: &[(&str, u64)]) -> u64 {
    let e = ENCODINGS
        .iter()
        .find(|e| (e.op, e.src) == (op, src))
        .expect("an encoding");
    let mut w = e.bits;
    for &(name, v) in fields.iter().chain(&[("Pred", 7)]) {
        let f = e.form.field(name).expect("a field");
        for &(shift, mask) in f.parts {
            let v = if f.kind == Kind::Bool && v != 0 {
                mask
            } else {
                v & mask
            };
            w = (w & !(mask << shift)) | (v << shift);
        }
    }
    w
}

/// A program with an SPH of stage `stage` (1 vertex, 5 pixel) and `code`
/// three instructions to a bundle, then EXIT.
fn program(stage: u32, code: &[u64]) -> Program {
    let mut b = CODE_MAGIC.to_le_bytes().to_vec();
    b.resize(CODE_HEADER_LEN, 0);
    let kind = if stage == 5 { 2u32 } else { 1 };
    b.extend((kind | 3 << 5 | stage << 10).to_le_bytes());
    b.resize(CODE_HEADER_LEN + SPH_LEN, 0);
    let mut all = code.to_vec();
    all.push(EXIT);
    while !all.len().is_multiple_of(3) {
        all.push(NOP);
    }
    for c in all.chunks(3) {
        b.extend(0u64.to_le_bytes());
        for w in c {
            b.extend(w.to_le_bytes());
        }
    }
    Program::parse(&b).expect("parses")
}

fn translated(p: &Program) -> relift_gpu_nv::translate::Shader {
    let l = relift_gpu_sm53::lower::program(p).unwrap_or_else(|e| panic!("stops: {e:?}"));
    relift_gpu_nv::translate::translate(&l).unwrap_or_else(|e| panic!("translate: {e:?}"))
}

fn ops(words: &[u32]) -> Vec<u32> {
    let mut v = Vec::new();
    let mut k = 5;
    while k < words.len() {
        v.push(words[k] & 0xffff);
        k += ((words[k] >> 16) as usize).max(1);
    }
    v
}

/// `IADD.CC Rd, Ra, c[0][off]`.
fn iadd_cc(d: u64, a: u64, off: u64) -> u64 {
    word(
        Op::Iadd,
        Src::CBuf,
        &[
            ("Dest", d),
            ("SrcA", a),
            ("CbufSlot", 0),
            ("CbufOffset", off / 4),
            ("WriteCC", 1),
        ],
    )
}

/// `IADD.X Rd, RZ, c[0][off]`.
fn iadd_x(d: u64, off: u64) -> u64 {
    word(
        Op::Iadd,
        Src::CBuf,
        &[
            ("Dest", d),
            ("SrcA", 255),
            ("CbufSlot", 0),
            ("CbufOffset", off / 4),
            ("X", 1),
        ],
    )
}

fn ldg(d: u64, a: u64, size: u64) -> u64 {
    word(
        Op::Ldg,
        Src::None,
        &[
            ("Dest", d),
            ("SrcA", a),
            ("E", 1),
            ("LsSize", size),
            ("Imm24", 0),
        ],
    )
}

fn stg(d: u64, a: u64, off: u64) -> u64 {
    word(
        Op::Stg,
        Src::None,
        &[
            ("Dest", d),
            ("SrcA", a),
            ("E", 1),
            ("LsSize", 4),
            ("Imm24", off),
        ],
    )
}

/// A fragment load from fragment slot 0 (c[0] 0x510): the carry pair
/// lowers to IADD3 with a carry predicate the program does not name and
/// IADD3.X, and the load reads the slot's descriptor.
#[test]
fn a_fragment_load_through_a_carried_slot_pair_reads_the_slot() {
    let p = program(5, &[iadd_cc(2, 8, 0x510), iadd_x(3, 0x514), ldg(4, 2, 6)]);
    let l = relift_gpu_sm53::lower::program(&p).expect("lowers");
    let t: Vec<String> = l
        .code
        .iter()
        .take(3)
        .map(|i| i.as_ref().unwrap().to_string())
        .collect();
    assert_eq!(t[0], "IADD3 R2, P6, R8, c[0x0][0x510], RZ");
    assert_eq!(t[1], "IADD3.X R3, RZ, c[0x0][0x514], RZ, P6, !PT");
    assert_eq!(t[2], "LDG.E.128 R4, [R2.64]");
    let s = translated(&p);
    assert_eq!(
        s.interface
            .storage_slots
            .iter()
            .copied()
            .collect::<Vec<_>>(),
        vec![0]
    );
    assert!(!ops(&s.spirv).contains(&CONVERT_U_TO_PTR));
}

/// A store through the vertex slot 2 pair made by ISCADD.CC writes the
/// slot's descriptor.
#[test]
fn a_vertex_store_through_an_iscadd_pair_writes_the_slot() {
    let iscadd = word(
        Op::Iscadd,
        Src::CBuf,
        &[
            ("Dest", 2),
            ("SrcA", 9),
            ("CbufSlot", 0),
            ("CbufOffset", 0x130 / 4),
            ("Imm5", 2),
            ("WriteCC", 1),
        ],
    );
    let p = program(1, &[iscadd, iadd_x(3, 0x134), stg(5, 2, 4)]);
    let l = relift_gpu_sm53::lower::program(&p).expect("lowers");
    assert_eq!(
        l.code[0].as_ref().unwrap().to_string(),
        "LEA R2, P6, R9, c[0x0][0x130], 0x2"
    );
    assert_eq!(
        l.code[2].as_ref().unwrap().to_string(),
        "STG.E [R2.64+0x4], R5"
    );
    let s = translated(&p);
    assert_eq!(
        s.interface
            .storage_slots
            .iter()
            .copied()
            .collect::<Vec<_>>(),
        vec![2]
    );
    assert!(!s.interface.global_store);
}

/// The carry predicate is the highest one the program leaves unnamed: a
/// program that guards with P6 carries in P5.
#[test]
fn the_carry_predicate_avoids_the_programs_own() {
    let guarded = (ldg(4, 2, 4) & !(0xf << 16)) | (6 << 16);
    let p = program(5, &[iadd_cc(2, 8, 0x510), iadd_x(3, 0x514), guarded]);
    let l = relift_gpu_sm53::lower::program(&p).expect("lowers");
    assert_eq!(
        l.code[0].as_ref().unwrap().to_string(),
        "IADD3 R2, P5, R8, c[0x0][0x510], RZ"
    );
}

/// An address outside the slots still lowers and translates (the slot
/// search finds it at run time); a 32-bit address stops.
#[test]
fn other_addresses_search_and_a_32_bit_one_stops() {
    let add = word(
        Op::Iadd,
        Src::Reg,
        &[("Dest", 2), ("SrcA", 8), ("SrcB", 10), ("WriteCC", 1)],
    );
    let addx = word(
        Op::Iadd,
        Src::Reg,
        &[("Dest", 3), ("SrcA", 9), ("SrcB", 11), ("X", 1)],
    );
    let s = translated(&program(5, &[add, addx, ldg(4, 2, 4)]));
    assert_eq!(s.interface.storage_slots.len(), 16);
    let short = word(
        Op::Ldg,
        Src::None,
        &[("Dest", 4), ("SrcA", 2), ("E", 0), ("LsSize", 4)],
    );
    let stops = relift_gpu_sm53::lower::stops(&program(5, &[short]));
    assert_eq!(stops[0].reason, "global access through a 32-bit address");
}

/// A negated operand without a carry is IADD3's negated operand; with one
/// it stops.
#[test]
fn a_negated_iadd_operand_lowers_without_a_carry() {
    let sub = word(
        Op::Iadd,
        Src::Reg,
        &[("Dest", 2), ("SrcA", 8), ("SrcB", 10), ("AvgMode", 1)],
    );
    let l = relift_gpu_sm53::lower::program(&program(5, &[sub])).expect("lowers");
    assert_eq!(
        l.code[0].as_ref().unwrap().to_string(),
        "IADD3 R2, R8, -R10, RZ"
    );
    let neg_imm = word(
        Op::Iadd,
        Src::Imm,
        &[("Dest", 2), ("SrcA", 8), ("Imm20", 4), ("AvgMode", 1)],
    );
    let l = relift_gpu_sm53::lower::program(&program(5, &[neg_imm])).expect("lowers");
    assert_eq!(
        l.code[0].as_ref().unwrap().to_string(),
        "IADD3 R2, R8, 0xfffffffc, RZ"
    );
    let carried = word(
        Op::Iadd,
        Src::Reg,
        &[
            ("Dest", 2),
            ("SrcA", 8),
            ("SrcB", 10),
            ("AvgMode", 2),
            ("WriteCC", 1),
        ],
    );
    let stops = relift_gpu_sm53::lower::stops(&program(5, &[carried]));
    assert_eq!(
        stops[0].reason,
        "IADD with a negated operand and a condition code carry"
    );
}

/// XMAD lowers to the translator's pseudo-instruction with its halves,
/// signs, shift, merge and C mode, which its known answers check
/// (`gpus/sm86/tests/kat.rs`); `.CC` stops.
#[test]
fn xmad_lowers_to_its_pseudo_instruction() {
    let x = word(
        Op::Xmad,
        Src::Reg,
        &[
            ("Dest", 2),
            ("SrcA", 8),
            ("SrcB", 9),
            ("SrcC", 10),
            ("HiloA", 1),
            ("Psl", 1),
            ("XmadCop", 4),
        ],
    );
    let l = relift_gpu_sm53::lower::program(&program(5, &[x])).expect("lowers");
    assert_eq!(
        l.code[0].as_ref().unwrap().to_string(),
        "XMAD.H1A.PSL.CBCC R2, R8, R9, R10"
    );
    let rc = word(
        Op::Xmad,
        Src::RegCBuf,
        &[
            ("Dest", 2),
            ("SrcA", 8),
            ("SrcC", 9),
            ("CbufSlot", 0),
            ("CbufOffset", 0x40),
            ("ASigned", 1),
        ],
    );
    let l = relift_gpu_sm53::lower::program(&program(5, &[rc])).expect("lowers");
    assert_eq!(
        l.code[0].as_ref().unwrap().to_string(),
        "XMAD.ASIGNED R2, R8, R9, c[0x0][0x100]"
    );
    let cc = word(
        Op::Xmad,
        Src::Reg,
        &[
            ("Dest", 2),
            ("SrcA", 8),
            ("SrcB", 9),
            ("SrcC", 10),
            ("WriteCC", 1),
        ],
    );
    assert_eq!(
        relift_gpu_sm53::lower::stops(&program(5, &[cc]))[0].reason,
        "XMAD with a condition code carry"
    );
}

/// LEA and LEA.HI lower to the SM86 forms of the same operands; with a
/// carry they stop.
#[test]
fn lea_lowers_to_the_sm86_lea() {
    let lea = word(
        Op::Lea,
        Src::Reg,
        &[
            ("Dest", 2),
            ("SrcA", 8),
            ("SrcB", 9),
            ("ImmU5", 3),
            ("DestPred", 7),
        ],
    );
    let hi = word(
        Op::LeaHi,
        Src::CBuf,
        &[
            ("Dest", 3),
            ("SrcA", 8),
            ("SrcC", 10),
            ("CbufSlot", 0),
            ("CbufOffset", 0x154 / 4),
            ("ImmU5", 3),
            ("DestPred", 7),
        ],
    );
    let p = program(5, &[lea, hi]);
    let l = relift_gpu_sm53::lower::program(&p).expect("lowers");
    assert_eq!(
        l.code[0].as_ref().unwrap().to_string(),
        "LEA R2, R8, R9, 0x3"
    );
    assert_eq!(
        l.code[1].as_ref().unwrap().to_string(),
        "LEA.HI R3, R8, c[0x0][0x154], R10, 0x3"
    );
    translated(&p);
    let cc = word(
        Op::Lea,
        Src::Reg,
        &[
            ("Dest", 2),
            ("SrcA", 8),
            ("SrcB", 9),
            ("WriteCC", 1),
            ("DestPred", 7),
        ],
    );
    assert_eq!(
        relift_gpu_sm53::lower::stops(&program(5, &[cc]))[0].reason,
        "LEA with a condition code carry"
    );
}

//! The lowering of each Maxwell form the table does not stop: the SM86
//! word of an exact row, the pseudo-instruction of a pseudo row, or the
//! named stop of a case the evidence does not cover.

use relift_gpu_sm86::translate::pseudo::{IPA_MUL, IPA_SAT};
use relift_gpu_sm86::{Inst as Inst86, Operand, RegMods, Word};

use super::flow::{self, Flow};
use crate::form::{Op, Src};
use crate::inst::Inst;

/// What one Maxwell instruction becomes.
#[derive(Clone, Debug, PartialEq)]
pub enum Lowered {
    /// One SM86 word that behaves the same.
    Word(Word),
    /// A pseudo-instruction for the SM86 translator (see
    /// `relift_gpu_sm86::translate::pseudo`).
    Pseudo(Inst86),
    /// Not translated, and why.
    Stop(&'static str),
}

const RZ: u64 = 255;
const PT: u64 = 7;

/// An SM86 word: opcode `op` (12 bits), the guard of `i`, no scoreboard
/// (barriers 7, as nvdisasm prints none), then `fields` as (low bit,
/// length, value).
fn word(i: &Inst, op: u32, fields: &[(u32, u32, u64)]) -> Word {
    let (p, not) = i.guard().unwrap_or((7, false));
    let mut w = u128::from(op)
        | (u128::from(p) << 12)
        | (u128::from(not) << 15)
        | (7u128 << 110)
        | (7u128 << 113);
    for &(lo, len, v) in fields {
        let m = ((1u128 << len) - 1) << lo;
        w = (w & !m) | ((u128::from(v) << lo) & m);
    }
    Word(w)
}

fn f(i: &Inst, name: &str) -> u64 {
    i.get(name).unwrap_or(0)
}

/// A relative SM86 branch field: the offset in bytes from the instruction
/// after index `from` to index `to` (bits 34 to 81, in words of 4 bytes).
fn rel(from: usize, to: usize) -> (u32, u32, u64) {
    let off = 16 * (to as i64 - from as i64 - 1);
    (34, 48, ((off / 4) as u64) & ((1 << 48) - 1))
}

/// The lowering of `i`, if its form has one; `None` for a form the table
/// stops. The control forms need the program's control-flow pass `f`.
pub fn lower(i: &Inst, f: Option<&Flow>) -> Option<Lowered> {
    Some(match (i.op(), i.enc.src) {
        (Op::Nop | Op::Depbar, _) => Lowered::Word(word(i, 0x918, &[])),
        (Op::Bra, _) => bra(i),
        (Op::Exit, _) => exit(i),
        (Op::Kil, _) => kil(i),
        (Op::Ssy | Op::Pbk, _) => push(i, f),
        (Op::Sync | Op::Brk, _) => pop(i, f),
        (Op::Mov, Src::Imm32) => mov32i(i),
        (Op::Mov, Src::CBuf) => mov_c(i),
        (Op::Ald, _) => ald(i),
        (Op::Ipa, _) => ipa(i),
        _ => return None,
    })
}

/// The guard of a control word: its own predicate field (bits 87 to 90)
/// holds PT, the guard sits where every SM86 instruction has it.
const NO_PRED: (u32, u32, u64) = (87, 4, 7);

/// `BRA` with the condition code test T: SM86 `BRA` (0x947) to the same
/// instruction under the same guard.
fn bra(i: &Inst) -> Lowered {
    if f(i, "Ccc") != 15 {
        return Lowered::Stop("branch on a condition code test");
    }
    if f(i, "Ca") != 0 || f(i, "Lmt") != 0 || f(i, "U") != 0 {
        return Lowered::Stop("BRA with .CA, .LMT or .U");
    }
    let Some(t) = flow::branch_target(i.at, i.word) else {
        return Lowered::Stop("branch target outside the code");
    };
    Lowered::Word(word(i, 0x947, &[NO_PRED, rel(flow::index(i.at), t)]))
}

/// `EXIT` with the condition code test T: SM86 `EXIT` (0x94d).
fn exit(i: &Inst) -> Lowered {
    if f(i, "Ccc") != 15 {
        return Lowered::Stop("EXIT on a condition code test");
    }
    if f(i, "KeepRefCnt") != 0 {
        return Lowered::Stop("EXIT.KEEPREFCOUNT");
    }
    Lowered::Word(word(i, 0x94d, &[NO_PRED]))
}

/// `KIL` with the condition code test T: SM86 `KILL` (0x95b).
fn kil(i: &Inst) -> Lowered {
    if f(i, "Ccc") != 15 {
        return Lowered::Stop("KIL on a condition code test");
    }
    Lowered::Word(word(i, 0x95b, &[NO_PRED]))
}

/// `SSY` and `PBK`: SM86 `BSSY` (0x945) on the barrier of the push's stack
/// depth, to the same target.
fn push(i: &Inst, f: Option<&Flow>) -> Lowered {
    let k = flow::index(i.at);
    let Some(&(b, t)) = f.and_then(|f| f.pushes.get(&k)) else {
        return Lowered::Stop("push outside the control-flow pass");
    };
    Lowered::Word(word(i, 0x945, &[NO_PRED, (16, 4, u64::from(b)), rel(k, t)]))
}

/// `SYNC` and `BRK`: SM86 `BRA` to the one target the pass found.
fn pop(i: &Inst, f: Option<&Flow>) -> Lowered {
    let k = flow::index(i.at);
    let Some(&t) = f.and_then(|f| f.pops.get(&k)) else {
        return Lowered::Stop("pop outside the control-flow pass");
    };
    Lowered::Word(word(i, 0x947, &[NO_PRED, rel(k, t)]))
}

/// `MOV32I Rd, imm`: SM86 `MOV Rd, imm` (form 0x802) with the full lane
/// mask.
fn mov32i(i: &Inst) -> Lowered {
    if f(i, "QuadMask") != 0xf {
        return Lowered::Stop("MOV32I with a lane mask other than 0xf");
    }
    Lowered::Word(word(
        i,
        0x802,
        &[(16, 8, f(i, "Dest")), (32, 32, f(i, "Imm32")), (72, 4, 0xf)],
    ))
}

/// `MOV Rd, c[bank][offset]`: SM86 `MOV Rd, c[bank][offset]` (form 0xa02),
/// the offset in bytes (Maxwell holds it in words).
fn mov_c(i: &Inst) -> Lowered {
    if f(i, "QuadMask") != 0xf {
        return Lowered::Stop("MOV with a lane mask other than 0xf");
    }
    let off = f(i, "CbufOffset") << 2;
    if off >= 0x8000 {
        return Lowered::Stop("constant offset at or above 0x8000 (SM86 reads it as negative)");
    }
    Lowered::Word(word(
        i,
        0xa02,
        &[
            (16, 8, f(i, "Dest")),
            (38, 16, off),
            (54, 5, f(i, "CbufSlot")),
            (72, 4, 0xf),
        ],
    ))
}

/// `ALD Rd, a[imm], Rv`: SM86 `ALD` (0x321) with the same offset, vertex
/// register, size and the `.P` and `.O` flags.
fn ald(i: &Inst) -> Lowered {
    if f(i, "SrcA") != RZ {
        return Lowered::Stop("ALD with a register in the attribute address");
    }
    Lowered::Word(word(
        i,
        0x321,
        &[
            (16, 8, f(i, "Dest")),
            (24, 8, RZ),
            (32, 8, f(i, "SrcB")),
            (40, 11, f(i, "Imm11")),
            (74, 2, f(i, "AlSize")),
            (76, 1, f(i, "P")),
            (79, 1, f(i, "O")),
        ],
    ))
}

/// `IPA`: SM86 `IPA` (0x326) for `.PASS` and `.CONSTANT` at the center or
/// the centroid; with `.MUL` or `.SAT` the pseudo-instruction `IPA` with
/// those modifiers and the multiplier as its third operand.
fn ipa(i: &Inst) -> Lowered {
    if f(i, "Idx") != 0 {
        return Lowered::Stop("indexed IPA");
    }
    if f(i, "SrcA") != RZ || f(i, "SrcC") != RZ {
        return Lowered::Stop("IPA with a register in the address or an offset register");
    }
    if f(i, "SrcPred") != PT || f(i, "SrcPredInv") != 0 {
        return Lowered::Stop("IPA with a source predicate other than PT");
    }
    let off = f(i, "Imm10");
    if !off.is_multiple_of(4) {
        return Lowered::Stop("IPA of an unaligned attribute");
    }
    let centroid = match f(i, "Msi") {
        0 => false,
        1 => true,
        _ => return Lowered::Stop("IPA at an offset or a sample"),
    };
    let (freq, mul) = match f(i, "IpaOp") {
        0 => (0, false),
        1 => (0, true),
        2 => (1, false),
        _ => return Lowered::Stop("IPA.SC"),
    };
    let b = f(i, "SrcB");
    if !mul && b != RZ {
        return Lowered::Stop("IPA with a multiplier register but no .MUL");
    }
    let sat = f(i, "Sat") != 0;
    if !mul && !sat {
        return Lowered::Word(word(
            i,
            0x326,
            &[
                (16, 8, f(i, "Dest")),
                (24, 8, RZ),
                (32, 8, RZ),
                (64, 8, off >> 2),
                (76, 2, u64::from(centroid)),
                (78, 2, freq),
                (81, 3, PT),
            ],
        ));
    }
    let mut p = Inst86::new("IPA");
    p.guard = match i.guard() {
        Some((7, false)) | None => None,
        Some((n, not)) => Some(Operand::Pred(n, not)),
    };
    p.mods.push(if freq == 1 { "CONSTANT" } else { "PASS" });
    if centroid {
        p.mods.push("CENTROID");
    }
    if mul {
        p.mods.push(IPA_MUL);
    }
    if sat {
        p.mods.push(IPA_SAT);
    }
    p.ops
        .push(Operand::Reg(f(i, "Dest") as u8, RegMods::default()));
    p.ops.push(Operand::Attr {
        index: None,
        offset: off as u32,
        patch: false,
    });
    if mul {
        p.ops.push(Operand::Reg(b as u8, RegMods::default()));
    }
    Lowered::Pseudo(p)
}

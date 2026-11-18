//! The lowering of each Maxwell form the table does not stop: the SM86
//! word of an exact row, the pseudo-instruction of a pseudo row, or the
//! named stop of a case the evidence does not cover.

use relift_gpu_sm86::translate::pseudo::{
    I2F_IABS, I2F_INEG, IPA_MUL, IPA_SAT, MUFU_SAT, RRO_EX2, RRO_SINCOS,
};
use relift_gpu_sm86::{CBank, Inst as Inst86, Operand, RegMods, Word};

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
        (Op::Fmul, _) => fmul(i),
        (Op::Fadd, _) => fadd(i),
        (Op::Ffma, _) => ffma(i),
        (Op::Mufu, _) => mufu(i),
        (Op::Rro, _) => rro(i),
        (Op::Mov, Src::Imm32) => mov32i(i),
        (Op::Mov, Src::CBuf) => mov_c(i),
        (Op::Mov, Src::Reg) => mov_r(i),
        (Op::Fmnmx, _) => fmnmx(i),
        (Op::Fsetp, _) => fsetp(i),
        (Op::I2f, _) => i2f(i),
        (Op::F2i, _) => f2i(i),
        (Op::F2f, _) => f2f(i),
        (Op::Ast, _) => ast(i),
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

/// Fields of an SM86 word: (low bit, length, value).
type Fields = Vec<(u32, u32, u64)>;

/// The second source of an SM86 ALU word in the form of `src`: the
/// fields of a register (bits 32 to 39), a 32-bit immediate (32 to 63), or
/// a constant (offset in bytes 38 to 53, bank 54 to 58), the 9-bit opcode
/// `op` given its form (1, 4 or 5) and the negation and absolute value
/// bits 63 and 62 (folded into an immediate).
fn src_b(i: &Inst, neg: bool, abs: bool) -> Result<(u32, Fields), &'static str> {
    let flags = |v: &mut Fields| {
        v.push((63, 1, u64::from(neg)));
        v.push((62, 1, u64::from(abs)));
    };
    Ok(match i.enc.src {
        Src::Reg => {
            let mut v = vec![(32, 8, f(i, "SrcB"))];
            flags(&mut v);
            (1, v)
        }
        Src::Imm | Src::Imm32 => {
            let mut bits = if i.enc.src == Src::Imm {
                f(i, "Imm20") << 12
            } else {
                f(i, "Imm32")
            };
            if abs {
                bits &= 0x7fff_ffff;
            }
            if neg {
                bits ^= 0x8000_0000;
            }
            (4, vec![(32, 32, bits)])
        }
        Src::CBuf => {
            let off = f(i, "CbufOffset") << 2;
            if off >= 0x8000 {
                return Err("constant offset at or above 0x8000 (SM86 reads it as negative)");
            }
            let mut v = vec![(38, 16, off), (54, 5, f(i, "CbufSlot"))];
            flags(&mut v);
            (5, v)
        }
        _ => return Err("no SM86 operand form"),
    })
}

/// The SM86 opcode of the 9-bit operation `op` in form `form`.
fn opc(op: u32, form: u32) -> u32 {
    op | (form << 9)
}

/// The SM86 scale of `FMUL` (bits 84 to 86) for Maxwell's: none, D2, D4,
/// D8, M8, M4, M2 there are 4, 3, 2, 1, 7, 6, 5 on SM86.
fn fmul_scale(s: u64) -> Option<u64> {
    [4, 3, 2, 1, 7, 6, 5].get(s as usize).copied()
}

/// The flush and rounding fields shared by FMUL, FADD and FFMA: `.FTZ`
/// (bit 80), the rounding mode (78, the same four values) and `.SAT`
/// (77). Maxwell's `.FMZ` stops: the SM86 translator does not model it.
fn modes(i: &Inst, ftz_field: &str) -> Result<Fields, &'static str> {
    if f(i, "WriteCC") != 0 {
        return Err("float arithmetic that writes the condition code");
    }
    let ftz = match f(i, ftz_field) {
        0 => 0,
        1 => 1,
        _ => return Err("float arithmetic with .FMZ"),
    };
    Ok(vec![
        (80, 1, ftz),
        (78, 2, f(i, "RoundMode")),
        (77, 1, f(i, "Sat")),
    ])
}

/// `FMUL`: SM86 `FMUL` (0x20) with the same rounding, flush, saturation and
/// scale; Maxwell's `.NEG` negates the product, here on the first source.
fn fmul(i: &Inst) -> Lowered {
    let mut v = match modes(i, "Fmz") {
        Ok(v) => v,
        Err(r) => return Lowered::Stop(r),
    };
    let Some(scale) = fmul_scale(f(i, "Scale")) else {
        return Lowered::Stop("FMUL with an invalid scale");
    };
    let (form, b) = match src_b(i, false, false) {
        Ok(x) => x,
        Err(r) => return Lowered::Stop(r),
    };
    v.extend(b);
    v.extend([
        (16, 8, f(i, "Dest")),
        (24, 8, f(i, "SrcA")),
        (72, 1, f(i, "NegA")),
        (84, 3, scale),
    ]);
    Lowered::Word(word(i, opc(0x020, form), &v))
}

/// `FADD`: SM86 `FADD` (0x21) with the same modes and operand negation
/// and absolute values; its forms put the second source in slot C (reg 1,
/// immediate 2, constant 3) at the same bits as slot B.
fn fadd(i: &Inst) -> Lowered {
    let mut v = match modes(i, "Ftz") {
        Ok(v) => v,
        Err(r) => return Lowered::Stop(r),
    };
    let (form, b) = match src_b(i, f(i, "NegB") != 0, f(i, "AbsB") != 0) {
        Ok(x) => x,
        Err(r) => return Lowered::Stop(r),
    };
    v.extend(b);
    v.extend([
        (16, 8, f(i, "Dest")),
        (24, 8, f(i, "SrcA")),
        (72, 1, f(i, "NegA")),
        (73, 1, f(i, "AbsA")),
    ]);
    let form = match form {
        1 => 1,
        4 => 2,
        _ => 3,
    };
    Lowered::Word(word(i, opc(0x021, form), &v))
}

/// `FFMA`: SM86 `FFMA` (0x23), `a * b + c` with the same modes; Maxwell's
/// `.NEG` of the product goes on the first source, `.NEG` of the addend on
/// the third. `FFMA32I` adds its destination; the register-constant form
/// takes the register as `b` and the constant as `c` (SM86 form 3).
fn ffma(i: &Inst) -> Lowered {
    let mut v = match modes(i, "Fmz") {
        Ok(v) => v,
        Err(r) => return Lowered::Stop(r),
    };
    v.extend([
        (16, 8, f(i, "Dest")),
        (24, 8, f(i, "SrcA")),
        (72, 1, f(i, "NegA")),
    ]);
    let neg_c = f(i, "NegC");
    let form = if i.enc.src == Src::RegCBuf {
        let off = f(i, "CbufOffset") << 2;
        if off >= 0x8000 {
            return Lowered::Stop("constant offset at or above 0x8000 (SM86 reads it as negative)");
        }
        v.extend([
            (64, 8, f(i, "SrcC")),
            (38, 16, off),
            (54, 5, f(i, "CbufSlot")),
            (63, 1, neg_c),
        ]);
        3
    } else {
        let (form, b) = match src_b(i, false, false) {
            Ok(x) => x,
            Err(r) => return Lowered::Stop(r),
        };
        v.extend(b);
        let c = if i.enc.src == Src::Imm32 {
            f(i, "Dest")
        } else {
            f(i, "SrcC")
        };
        v.extend([(64, 8, c), (75, 1, neg_c)]);
        form
    };
    Lowered::Word(word(i, opc(0x023, form), &v))
}

/// `MUFU`: SM86 `MUFU` (0x308) of the same function with the operand's
/// negation and absolute value; `.SAT`, which SM86 lacks, is the
/// pseudo-instruction `MUFU.SAT`. The 64-bit forms stop.
fn mufu(i: &Inst) -> Lowered {
    let kind = f(i, "MufuOp");
    if kind > 8 || kind == 6 || kind == 7 {
        return Lowered::Stop("MUFU.RCP64H, MUFU.RSQ64H or an invalid function");
    }
    let w = word(
        i,
        0x308,
        &[
            (16, 8, f(i, "Dest")),
            (32, 8, f(i, "SrcA")),
            (63, 1, f(i, "NegA")),
            (62, 1, f(i, "AbsA")),
            (74, 6, kind),
        ],
    );
    if f(i, "Sat") == 0 {
        return Lowered::Word(w);
    }
    let Ok(mut p) = relift_gpu_sm86::decode(w, 0) else {
        return Lowered::Stop("MUFU that SM86 does not decode");
    };
    p.mods.push(MUFU_SAT);
    Lowered::Pseudo(p)
}

/// `RRO`: the pseudo-instruction `RRO.SINCOS` or `RRO.EX2` of the operand
/// with its negation and absolute value.
fn rro(i: &Inst) -> Lowered {
    let neg = f(i, "NegB") != 0;
    let abs = f(i, "AbsB") != 0;
    let mods = RegMods {
        neg,
        abs,
        ..RegMods::default()
    };
    let src = match i.enc.src {
        Src::Reg => Operand::Reg(f(i, "SrcB") as u8, mods),
        Src::Imm => {
            let mut bits = f(i, "Imm20") << 12;
            if abs {
                bits &= 0x7fff_ffff;
            }
            if neg {
                bits ^= 0x8000_0000;
            }
            Operand::F32(bits as u32, false)
        }
        Src::CBuf => {
            let off = f(i, "CbufOffset") << 2;
            if off >= 0x8000 {
                return Lowered::Stop(
                    "constant offset at or above 0x8000 (SM86 reads it as negative)",
                );
            }
            Operand::CBuf {
                bank: CBank::Imm(f(i, "CbufSlot") as u32),
                index: None,
                offset: off as i64,
                mods,
                space: false,
            }
        }
        _ => return Lowered::Stop("RRO form"),
    };
    let mut p = Inst86::new("RRO");
    p.guard = match i.guard() {
        Some((7, false)) | None => None,
        Some((n, not)) => Some(Operand::Pred(n, not)),
    };
    p.mods.push(if f(i, "RroOp") != 0 {
        RRO_EX2
    } else {
        RRO_SINCOS
    });
    p.ops
        .push(Operand::Reg(f(i, "Dest") as u8, RegMods::default()));
    p.ops.push(src);
    Lowered::Pseudo(p)
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

/// `MOV Rd, Rs`: SM86 `MOV Rd, Rs` (form 0x202) with the full lane mask.
fn mov_r(i: &Inst) -> Lowered {
    if f(i, "QuadMask") != 0xf {
        return Lowered::Stop("MOV with a lane mask other than 0xf");
    }
    Lowered::Word(word(
        i,
        0x202,
        &[(16, 8, f(i, "Dest")), (32, 8, f(i, "SrcA")), (72, 4, 0xf)],
    ))
}

/// The SM86 fields of a source predicate (bits 87 to 89, negation 90).
fn src_pred(i: &Inst) -> [(u32, u32, u64); 2] {
    [(87, 3, f(i, "SrcPred")), (90, 1, f(i, "SrcPredInv"))]
}

/// `FMNMX`: SM86 `FMNMX` (0x09) with the same operands, their negation and
/// absolute values, `.FTZ` and the predicate that picks the minimum.
fn fmnmx(i: &Inst) -> Lowered {
    if f(i, "WriteCC") != 0 {
        return Lowered::Stop("FMNMX that writes the condition code");
    }
    let (form, b) = match src_b(i, f(i, "NegB") != 0, f(i, "AbsB") != 0) {
        Ok(x) => x,
        Err(r) => return Lowered::Stop(r),
    };
    let mut v = b;
    v.extend([
        (16, 8, f(i, "Dest")),
        (24, 8, f(i, "SrcA")),
        (72, 1, f(i, "NegA")),
        (73, 1, f(i, "AbsA")),
        (80, 1, f(i, "Ftz")),
    ]);
    v.extend(src_pred(i));
    Lowered::Word(word(i, opc(0x009, form), &v))
}

/// `FSETP`: SM86 `FSETP` (0x0b) with the same comparison, `.FTZ`, boolean
/// operation, both destinations and operand modifiers. Maxwell's bit 47 is
/// `.FTZ` (envydis); Ryujinx's form also names it the condition code bit,
/// which FSETP does not write.
fn fsetp(i: &Inst) -> Lowered {
    let bop = f(i, "Bop");
    if bop == 3 {
        return Lowered::Stop("FSETP with an invalid boolean operation");
    }
    let (form, b) = match src_b(i, f(i, "NegB") != 0, f(i, "AbsB") != 0) {
        Ok(x) => x,
        Err(r) => return Lowered::Stop(r),
    };
    let mut v = b;
    v.extend([
        (81, 3, f(i, "DestPred")),
        (84, 3, f(i, "DestPredInv")),
        (24, 8, f(i, "SrcA")),
        (72, 1, f(i, "NegA")),
        (73, 1, f(i, "AbsA")),
        (76, 4, f(i, "FComp")),
        (80, 1, f(i, "Ftz")),
        (74, 2, bop),
    ]);
    v.extend(src_pred(i));
    Lowered::Word(word(i, opc(0x00b, form), &v))
}

/// The source of a conversion in the SM86 field layout (bits 32 to 63 for
/// a register or constant, with negation 63 and absolute value 62) and
/// its form; an integer immediate is Maxwell's 20 bits sign-extended, a
/// float immediate the top 20 bits of the float.
fn cvt_src(i: &Inst, neg: bool, abs: bool, float: bool) -> Result<(u32, Fields), &'static str> {
    if i.enc.src == Src::Imm && !float {
        if neg || abs {
            return Err("integer immediate with a modifier");
        }
        let v = f(i, "Imm20");
        let v = if v & 0x8_0000 != 0 {
            v | 0xfff0_0000
        } else {
            v
        };
        return Ok((4, vec![(32, 32, v)]));
    }
    src_b(i, neg, abs)
}

/// The pseudo-instruction of the SM86 word `w` with the modifiers
/// `extra`.
fn pseudo_of(w: Word, extra: &[&'static str]) -> Lowered {
    let Ok(mut p) = relift_gpu_sm86::decode(w, 0) else {
        return Lowered::Stop("a lowered word SM86 does not decode");
    };
    p.mods.extend(extra);
    Lowered::Pseudo(p)
}

/// `I2F`: SM86 `I2F` (0x106) to F32 with the same source size, signedness,
/// byte or half selection and rounding mode. Maxwell's integer `.ABS` and
/// `.NEG` of the source have no SM86 form: the pseudo-instruction
/// `I2F.IABS` or `I2F.INEG`.
fn i2f(i: &Inst) -> Lowered {
    if f(i, "WriteCC") != 0 {
        return Lowered::Stop("I2F that writes the condition code");
    }
    if f(i, "DstFmt") != 2 {
        return Lowered::Stop("I2F to F16 or F64");
    }
    let src = f(i, "ISrcFmt");
    let (size, signed) = (src & 3, src >> 2);
    if size == 3 {
        return Lowered::Stop("I2F from a 64-bit integer");
    }
    let sel = f(i, "ByteSel");
    let sel = match (size, sel) {
        (_, 0) => 0,
        (0, s) => s,
        (1, 2) => 1,
        _ => return Lowered::Stop("I2F with a byte selection its source size does not have"),
    };
    let (neg, abs) = (f(i, "NegB") != 0, f(i, "AbsB") != 0);
    if (neg || abs) && size != 2 {
        return Lowered::Stop("I2F of an 8 or 16-bit integer with .ABS or .NEG");
    }
    let (form, mut v) = match cvt_src(i, false, false, false) {
        Ok(x) => x,
        Err(r) => return Lowered::Stop(r),
    };
    if (neg || abs) && form != 1 {
        return Lowered::Stop("I2F of a constant with .ABS or .NEG");
    }
    v.retain(|x| x.0 != 62 && x.0 != 63);
    v.extend([
        (16, 8, f(i, "Dest")),
        (60, 2, sel),
        (74, 1, signed),
        (75, 3, 2),
        (78, 2, f(i, "RoundMode")),
        (84, 2, size),
    ]);
    let w = word(i, opc(0x106, form), &v);
    if !neg && !abs {
        return Lowered::Word(w);
    }
    let mut extra = Vec::new();
    if abs {
        extra.push(I2F_IABS);
    }
    if neg {
        extra.push(I2F_INEG);
    }
    pseudo_of(w, &extra)
}

/// `F2I`: SM86 `F2I.NTZ` (0x105) from F32 with the same destination size
/// and signedness, rounding, `.FTZ` and the operand's negation and
/// absolute value. `.NTZ` makes a NaN zero, as Maxwell's F2I gives it
/// (inferred, s2-console-questions).
fn f2i(i: &Inst) -> Lowered {
    if f(i, "WriteCC") != 0 {
        return Lowered::Stop("F2I that writes the condition code");
    }
    if f(i, "SrcFmt") != 2 || f(i, "Sh") != 0 {
        return Lowered::Stop("F2I from F16 or F64");
    }
    let dst = f(i, "IDstFmt");
    let (form, mut v) = match cvt_src(i, f(i, "NegB") != 0, f(i, "AbsB") != 0, true) {
        Ok(x) => x,
        Err(r) => return Lowered::Stop(r),
    };
    v.extend([
        (16, 8, f(i, "Dest")),
        (72, 1, dst >> 2),
        (75, 2, dst & 3),
        (77, 1, 1),
        (78, 2, f(i, "RoundMode")),
        (80, 1, f(i, "Ftz")),
        (84, 3, 2),
    ]);
    Lowered::Word(word(i, opc(0x105, form), &v))
}

/// `F2F` from F32 to F32 with an integer rounding: SM86 `FRND` (0x107)
/// with the same rounding, `.FTZ` and operand modifiers. Other F2F forms
/// stop.
fn f2f(i: &Inst) -> Lowered {
    if f(i, "WriteCC") != 0 {
        return Lowered::Stop("F2F that writes the condition code");
    }
    if f(i, "SrcFmt") != 2 || f(i, "DstFmt") != 2 || f(i, "Sh") != 0 {
        return Lowered::Stop("F2F between formats");
    }
    let mode = f(i, "RoundMode");
    if mode < 4 {
        return Lowered::Stop("F2F from F32 to F32 without an integer rounding");
    }
    if f(i, "Sat") != 0 {
        return Lowered::Stop("F2F with .SAT");
    }
    let (form, mut v) = match cvt_src(i, f(i, "NegB") != 0, f(i, "AbsB") != 0, true) {
        Ok(x) => x,
        Err(r) => return Lowered::Stop(r),
    };
    v.extend([
        (16, 8, f(i, "Dest")),
        (75, 3, 2),
        (78, 2, mode & 3),
        (80, 1, f(i, "Ftz")),
        (84, 3, 2),
    ]);
    Lowered::Word(word(i, opc(0x107, form), &v))
}

/// `AST a[imm], Rs, Rv`: SM86 `AST` (0x322) with the same offset, data and
/// vertex registers, size and `.P`.
fn ast(i: &Inst) -> Lowered {
    if f(i, "SrcA") != RZ {
        return Lowered::Stop("AST with a register in the attribute address");
    }
    Lowered::Word(word(
        i,
        0x322,
        &[
            (24, 8, RZ),
            (32, 8, f(i, "SrcB")),
            (40, 11, f(i, "Imm11")),
            (64, 8, f(i, "SrcC")),
            (74, 2, f(i, "AlSize")),
            (76, 1, f(i, "P")),
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

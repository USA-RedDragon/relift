//! The lowering of each Maxwell form the table does not stop: the
//! instruction of the SM86 form of an exact row, the pseudo-instruction of
//! a pseudo row, or the named stop of a case the evidence does not cover.
//!
//! Each instruction is built as `relift-gpu-sm86`'s decoder gives that SM86
//! form: the same mnemonic, modifiers in nvdisasm's order, operands and
//! named fields, so the translator runs it as it runs the SM86 form.

use relift_gpu_nv::fields::{Fields, Handle, Tex};
use relift_gpu_nv::translate::pseudo::{
    I2F_IABS, I2F_INEG, IPA_MUL, IPA_SAT, MUFU_SAT, RRO_EX2, RRO_SINCOS, XMAD,
};
use relift_gpu_nv::{CBank, Inst as Inst86, Operand, RegMods};

use super::flow::{self, Flow};
use crate::form::{Op, Src};
use crate::inst::Inst;

/// What one Maxwell instruction becomes.
#[derive(Clone, Debug, PartialEq)]
pub enum Lowered {
    /// An instruction of the SM86 form that behaves the same.
    Exact(Inst86),
    /// A pseudo-instruction for the translator (see
    /// `relift_gpu_nv::translate::pseudo`).
    Pseudo(Inst86),
    /// Not translated, and why.
    Stop(&'static str),
}

const RZ: u64 = 255;
const PT: u64 = 7;

/// The stop of a constant offset the SM86 field cannot hold.
const HIGH_OFFSET: &str = "constant offset at or above 0x8000 (SM86 reads it as negative)";

fn f(i: &Inst, name: &str) -> u64 {
    i.get(name).unwrap_or(0)
}

/// An instruction `name` under the guard of `i`.
fn inst(i: &Inst, name: &'static str) -> Inst86 {
    let mut p = Inst86::new(name);
    p.guard = match i.guard() {
        Some((7, false)) | None => None,
        Some((n, not)) => Some(Operand::Pred(n, not)),
    };
    p
}

/// `p` with each of `mods` that is not empty.
fn with(mut p: Inst86, mods: &[&'static str]) -> Inst86 {
    p.mods.extend(mods.iter().filter(|m| !m.is_empty()));
    p
}

fn reg(n: u64) -> Operand {
    Operand::Reg(n as u8, RegMods::default())
}

fn reg_m(n: u64, neg: bool, abs: bool) -> Operand {
    Operand::Reg(
        n as u8,
        RegMods {
            neg,
            abs,
            ..RegMods::default()
        },
    )
}

fn pred(n: u64, not: bool) -> Operand {
    Operand::Pred(n as u8, not)
}

/// `c[bank][offset]` with an offset in bytes below 0x8000.
fn cbuf(bank: u64, offset: u64, neg: bool, abs: bool) -> Operand {
    Operand::CBuf {
        bank: CBank::Imm(bank as u32),
        index: None,
        offset: offset as i64,
        mods: RegMods {
            neg,
            abs,
            ..RegMods::default()
        },
        space: false,
    }
}

/// The constant of a Maxwell constant source: bank and byte offset.
fn cbuf_of(i: &Inst, neg: bool, abs: bool) -> Result<Operand, &'static str> {
    let off = f(i, "CbufOffset") << 2;
    if off >= 0x8000 {
        return Err(HIGH_OFFSET);
    }
    Ok(cbuf(f(i, "CbufSlot"), off, neg, abs))
}

/// The byte offset of instruction index `t` as a branch target.
fn target(t: usize) -> Operand {
    Operand::Target(16 * t as i64)
}

/// The float second source of an ALU form: a register (`SrcB`), the 32-bit
/// immediate (Maxwell's 20 bits are the top of the float) with the
/// negation and absolute value folded into its bits, or a constant, each
/// with `neg` and `abs`.
fn src_b(i: &Inst, neg: bool, abs: bool) -> Result<Operand, &'static str> {
    Ok(match i.enc.src {
        Src::Reg => reg_m(f(i, "SrcB"), neg, abs),
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
            Operand::F32(bits as u32, false)
        }
        Src::CBuf => cbuf_of(i, neg, abs)?,
        _ => return Err("no SM86 operand form"),
    })
}

/// The lowering of `i`, if its form has one; `None` for a form the table
/// stops. The control forms need the program's control-flow pass `f`.
pub fn lower(i: &Inst, f: Option<&Flow>) -> Option<Lowered> {
    let r = match (i.op(), i.enc.src) {
        (Op::Nop | Op::Depbar, _) => Ok(inst(i, "NOP")),
        (Op::Bra, _) => bra(i),
        (Op::Exit, _) => exit(i),
        (Op::Kil, _) => kil(i),
        (Op::Ssy | Op::Pbk, _) => push(i, f),
        (Op::Sync | Op::Brk, _) => pop(i, f),
        (Op::Fmul, _) => fmul(i),
        (Op::Fadd, _) => fadd(i),
        (Op::Ffma, _) => ffma(i),
        (Op::Mufu, _) => return Some(mufu(i)),
        (Op::Rro, _) => return Some(rro(i)),
        (Op::Mov, Src::Imm32) => mov32i(i),
        (Op::Mov, Src::CBuf) => mov_c(i),
        (Op::Mov, Src::Reg) => mov_r(i),
        (Op::Fmnmx, _) => fmnmx(i),
        (Op::Fsetp, _) => fsetp(i),
        (Op::I2f, _) => return Some(i2f(i)),
        (Op::F2i, _) => f2i(i),
        (Op::F2f, _) => f2f(i),
        (Op::Ast, _) => ast(i),
        (Op::Texs, _) => texs(i),
        (Op::Tex, _) => tex(i, false),
        (Op::TexB, _) => tex(i, true),
        (Op::Tld4s, _) => tld4s(i),
        (Op::Tlds, _) => tlds(i),
        (Op::TldB, _) => tld_b(i),
        (Op::Txq, _) => txq(i, false),
        (Op::TxqB, _) => txq(i, true),
        (Op::Ald, _) => ald(i),
        (Op::Ipa, _) => return Some(ipa(i)),
        (Op::Iadd, _) => iadd(i, f),
        (Op::Iscadd, _) => iscadd(i, f),
        (Op::Lea, _) => lea(i, false),
        (Op::LeaHi, _) => lea(i, true),
        (Op::Ldg, _) => ldg(i),
        (Op::Xmad, _) => return Some(xmad(i)),
        (Op::Stg, _) => stg(i),
        _ => return None,
    };
    Some(r.map_or_else(Lowered::Stop, Lowered::Exact))
}

type Res = Result<Inst86, &'static str>;

/// The second source of an integer form: a register, the immediate
/// (Imm20 sign-extended from 20 bits, or Imm32), or a constant.
fn int_b(i: &Inst) -> Result<Operand, &'static str> {
    Ok(match i.enc.src {
        Src::Reg => reg(f(i, "SrcB")),
        Src::Imm => {
            let v = f(i, "Imm20");
            let v = if v & 0x8_0000 != 0 {
                v | 0xfff0_0000
            } else {
                v
            };
            Operand::Hex(v & 0xffff_ffff)
        }
        Src::Imm32 => Operand::Hex(f(i, "Imm32")),
        Src::CBuf => cbuf_of(i, false, false)?,
        _ => return Err("no SM86 operand form"),
    })
}

/// `o` negated when `neg`: a register or constant with its negation
/// modifier, an immediate as its two's complement.
fn negated(o: Operand, neg: bool) -> Operand {
    if !neg {
        return o;
    }
    match o {
        Operand::Reg(n, m) => Operand::Reg(n, RegMods { neg: true, ..m }),
        Operand::CBuf {
            bank,
            index,
            offset,
            mods,
            space,
        } => Operand::CBuf {
            bank,
            index,
            offset,
            mods: RegMods { neg: true, ..mods },
            space,
        },
        Operand::Hex(v) => Operand::Hex(v.wrapping_neg() & 0xffff_ffff),
        o => o,
    }
}

/// The carry predicate of a program's condition code (see
/// `flow::free_predicate`), for a form that writes or reads it.
fn cc_pred(f: Option<&Flow>) -> Result<u8, &'static str> {
    match f {
        Some(Flow { cc: Some(p), .. }) => Ok(*p),
        Some(_) => Err("condition code carry in a program that names all seven predicates"),
        None => Err("condition code carry outside a program"),
    }
}

/// `p` with the carry-out predicate fields of `IADD3`, `IMAD` and `LEA`:
/// `pd` the first (`PT` for none), the second none.
fn carry_out(mut p: Inst86, pd: u8) -> Inst86 {
    p.fields = Fields {
        pd,
        pd2: PT as u8,
        ..Fields::default()
    };
    p
}

/// `IADD Rd, Ra, B` with `.CC` and `.X`: SM86 `IADD3 Rd, Pcc, Ra, B, RZ`
/// (the carry out into the condition code's predicate where `.CC`), and
/// with `.X` `IADD3.X Rd, Pcc, Ra, B, RZ, Pcc, !PT` (the carry in from it).
/// Ryujinx's EmitIadd adds the carry flag for `.X` and sets it from the
/// unsigned wrap of the sum, which is IADD3's carry out of a two-term sum.
/// A negated operand (AvgMode NegA, NegB) is IADD3's negated operand;
/// with a carry it stops (Maxwell's carry of a + -0 is 0, IADD3's of
/// a + ~0 + 1 is 1), as do `.PO` and `.SAT`.
fn iadd(i: &Inst, fl: Option<&Flow>) -> Res {
    let avg = f(i, "AvgMode");
    if avg == 3 {
        return Err("IADD.PO");
    }
    if f(i, "Sat") != 0 {
        return Err("IADD.SAT");
    }
    let (cc_out, x) = (f(i, "WriteCC") != 0, f(i, "X") != 0);
    if avg != 0 && (cc_out || x) {
        return Err("IADD with a negated operand and a condition code carry");
    }
    let cc = if cc_out || x {
        Some(cc_pred(fl)?)
    } else {
        None
    };
    let mut p = if x {
        with(inst(i, "IADD3"), &["X"])
    } else {
        inst(i, "IADD3")
    };
    p.ops.push(reg(f(i, "Dest")));
    let out = if cc_out {
        cc.unwrap_or(PT as u8)
    } else {
        PT as u8
    };
    if cc_out {
        p.ops.push(pred(u64::from(out), false));
    }
    p.ops.push(reg_m(f(i, "SrcA"), avg == 2, false));
    p.ops.push(negated(int_b(i)?, avg == 1));
    p.ops.push(reg(RZ));
    if let (true, Some(c)) = (x, cc) {
        p.ops.push(pred(u64::from(c), false));
        p.ops.push(pred(PT, true));
    }
    Ok(carry_out(p, out))
}

/// `ISCADD Rd, Ra, B, s` with `.CC`: SM86 `LEA Rd, Pcc, Ra, B, s`, which
/// is `(Ra << s) + B` with the carry out of the add, as Ryujinx's
/// EmitIscadd computes and flags it. Negated operands stop.
fn iscadd(i: &Inst, fl: Option<&Flow>) -> Res {
    if f(i, "AvgMode") != 0 {
        return Err("ISCADD with a negated operand or .PO");
    }
    let out = if f(i, "WriteCC") != 0 {
        cc_pred(fl)?
    } else {
        PT as u8
    };
    let mut p = inst(i, "LEA");
    p.ops.push(reg(f(i, "Dest")));
    if out != PT as u8 {
        p.ops.push(pred(u64::from(out), false));
    }
    p.ops.push(reg(f(i, "SrcA")));
    p.ops.push(int_b(i)?);
    p.ops.push(Operand::Hex(f(i, "Imm5")));
    Ok(carry_out(p, out))
}

/// `XMAD Rd, A, B, C`: the translator's XMAD pseudo-instruction (see
/// `relift_gpu_nv::translate::pseudo`) with its halves, signs, shift,
/// merge and C mode as Ryujinx's XmadR, XmadI, XmadC and XmadRc read them
/// (XmadI's B is Imm16 and its low half, XmadRc's B the register SrcC and
/// C the constant, without PSL and MRG). `.X` and `.CC` stop.
fn xmad(i: &Inst) -> Lowered {
    if f(i, "X") != 0 || f(i, "WriteCC") != 0 {
        return Lowered::Stop("XMAD with a condition code carry");
    }
    let rc = i.enc.src == Src::RegCBuf;
    let (b, c) = match i.enc.src {
        Src::Reg => (reg(f(i, "SrcB")), reg(f(i, "SrcC"))),
        Src::Imm => (Operand::Hex(f(i, "Imm16")), reg(f(i, "SrcC"))),
        Src::CBuf => match cbuf_of(i, false, false) {
            Ok(b) => (b, reg(f(i, "SrcC"))),
            Err(e) => return Lowered::Stop(e),
        },
        Src::RegCBuf => match cbuf_of(i, false, false) {
            Ok(c) => (reg(f(i, "SrcC")), c),
            Err(e) => return Lowered::Stop(e),
        },
        _ => return Lowered::Stop("no SM86 operand form"),
    };
    let mode = match f(i, "XmadCop") {
        0 => "",
        1 => "CLO",
        2 => "CHI",
        3 => "CSFU",
        4 => "CBCC",
        _ => return Lowered::Stop("XMAD with an unknown C mode"),
    };
    let flag = |n: &str, m: &'static str| if f(i, n) != 0 { m } else { "" };
    let (psl, mrg) = if rc {
        ("", "")
    } else {
        (flag("Psl", "PSL"), flag("Mrg", "MRG"))
    };
    let hb = if i.enc.src == Src::Imm {
        ""
    } else {
        flag("HiloB", "H1B")
    };
    let mut p = with(
        inst(i, XMAD),
        &[
            flag("ASigned", "ASIGNED"),
            flag("BSigned", "BSIGNED"),
            flag("HiloA", "H1A"),
            hb,
            psl,
            mrg,
            mode,
        ],
    );
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(reg(f(i, "SrcA")));
    p.ops.push(b);
    p.ops.push(c);
    Lowered::Pseudo(p)
}

/// `LEA Rd, Ra, B, s` and `LEA.HI Rd, Ra, B, Rc, s`: SM86 `LEA` and
/// `LEA.HI` of the same operands, `(Ra << s) + B` and the high word of
/// `(Rc:Ra) << s` plus B, as Ryujinx's EmitLea and EmitLeaHi compute them.
/// Ryujinx does not model `.CC`, `.X` or the predicate destination, so
/// those, and `.NEG` of A, stop.
fn lea(i: &Inst, hi: bool) -> Res {
    if f(i, "WriteCC") != 0 || f(i, "X") != 0 {
        return Err("LEA with a condition code carry");
    }
    if f(i, "NegA") != 0 {
        return Err("LEA with a negated A");
    }
    if f(i, "DestPred") != PT {
        return Err("LEA with a predicate destination");
    }
    let mut p = if hi {
        with(inst(i, "LEA"), &["HI"])
    } else {
        inst(i, "LEA")
    };
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(reg(f(i, "SrcA")));
    p.ops.push(int_b(i)?);
    if hi {
        p.ops.push(reg(f(i, "SrcC")));
    }
    p.ops.push(Operand::Hex(f(i, "ImmU5")));
    Ok(carry_out(p, PT as u8))
}

/// The SM86 size modifier of a Maxwell global access size: U8, S8, U16,
/// S16, B32 (none), B64, B128.
fn ls_size(v: u64) -> Result<&'static str, &'static str> {
    Ok(match v {
        0 => "U8",
        1 => "S8",
        2 => "U16",
        3 => "S16",
        4 => "",
        5 => "64",
        6 => "128",
        _ => return Err("global access of size UB128"),
    })
}

/// The SM86 address `[Ra.64+offset]` of a Maxwell global access: a
/// register pair (`.E`) plus Imm24 sign-extended. A 32-bit address (no
/// `.E`) and an RZ base stop.
fn global_addr(i: &Inst) -> Result<Operand, &'static str> {
    if f(i, "E") == 0 {
        return Err("global access through a 32-bit address");
    }
    let ra = f(i, "SrcA");
    if ra == RZ {
        return Err("global access at an absolute address");
    }
    let v = f(i, "Imm24");
    let off = if v & 0x80_0000 != 0 {
        v as i64 - 0x100_0000
    } else {
        v as i64
    };
    let s = match off {
        0 => format!("[R{ra}.64]"),
        o if o < 0 => format!("[R{ra}.64-{:#x}]", -o),
        o => format!("[R{ra}.64+{o:#x}]"),
    };
    Ok(Operand::Sym(s))
}

/// `LDG.E Rd, [Ra+imm]`: SM86 `LDG.E Rd, [Ra.64+imm]` of the same size, a
/// weak load. Ryujinx's EmitLdg loads each word of the size at the address
/// and does not read the cache operation; `.CV` (volatile) stops.
fn ldg(i: &Inst) -> Res {
    if f(i, "CacheOp") == 3 {
        return Err("LDG.CV");
    }
    let mut p = with(inst(i, "LDG"), &["E", ls_size(f(i, "LsSize"))?]);
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(global_addr(i)?);
    Ok(p)
}

/// `STG.E [Ra+imm], Rd`: SM86 `STG.E [Ra.64+imm], Rd` of the same size, a
/// weak store, as Ryujinx's EmitStg stores the data register's words; the
/// cache operation is not read there either.
fn stg(i: &Inst) -> Res {
    let mut p = with(inst(i, "STG"), &["E", ls_size(f(i, "LsSize"))?]);
    p.ops.push(global_addr(i)?);
    p.ops.push(reg(f(i, "Dest")));
    Ok(p)
}

/// `BRA` with the condition code test T: SM86 `BRA` to the same
/// instruction under the same guard.
fn bra(i: &Inst) -> Res {
    if f(i, "Ccc") != 15 {
        return Err("branch on a condition code test");
    }
    if f(i, "Ca") != 0 || f(i, "Lmt") != 0 || f(i, "U") != 0 {
        return Err("BRA with .CA, .LMT or .U");
    }
    let Some(t) = flow::branch_target(i.at, i.word) else {
        return Err("branch target outside the code");
    };
    let mut p = inst(i, "BRA");
    p.ops.push(target(t));
    Ok(p)
}

/// `EXIT` with the condition code test T: SM86 `EXIT`.
fn exit(i: &Inst) -> Res {
    if f(i, "Ccc") != 15 {
        return Err("EXIT on a condition code test");
    }
    if f(i, "KeepRefCnt") != 0 {
        return Err("EXIT.KEEPREFCOUNT");
    }
    Ok(inst(i, "EXIT"))
}

/// `KIL` with the condition code test T: SM86 `KILL`.
fn kil(i: &Inst) -> Res {
    if f(i, "Ccc") != 15 {
        return Err("KIL on a condition code test");
    }
    Ok(inst(i, "KILL"))
}

/// `SSY` and `PBK`: SM86 `BSSY` on the barrier of the push's stack depth,
/// to the same target.
fn push(i: &Inst, f: Option<&Flow>) -> Res {
    let k = flow::index(i.at);
    let Some(&(b, t)) = f.and_then(|f| f.pushes.get(&k)) else {
        return Err("push outside the control-flow pass");
    };
    let mut p = inst(i, "BSSY");
    p.ops.push(Operand::Barrier(b));
    p.ops.push(target(t));
    Ok(p)
}

/// `SYNC` and `BRK`: SM86 `BRA` to the one target the pass found.
fn pop(i: &Inst, f: Option<&Flow>) -> Res {
    let k = flow::index(i.at);
    let Some(&t) = f.and_then(|f| f.pops.get(&k)) else {
        return Err("pop outside the control-flow pass");
    };
    let mut p = inst(i, "BRA");
    p.ops.push(target(t));
    Ok(p)
}

/// The SM86 names of the rounding modes: none, RM, RP, RZ.
const RND: [&str; 4] = ["", "RM", "RP", "RZ"];

/// The flush and rounding modifiers shared by FMUL, FADD and FFMA:
/// `.FTZ`, then the scale `scale` (FMUL's), the rounding mode and `.SAT`.
/// Maxwell's `.FMZ` stops: the translator does not model it.
fn modes(
    i: &Inst,
    ftz_field: &str,
    scale: &'static str,
) -> Result<[&'static str; 4], &'static str> {
    if f(i, "WriteCC") != 0 {
        return Err("float arithmetic that writes the condition code");
    }
    let ftz = match f(i, ftz_field) {
        0 => "",
        1 => "FTZ",
        _ => return Err("float arithmetic with .FMZ"),
    };
    let sat = if f(i, "Sat") != 0 { "SAT" } else { "" };
    Ok([ftz, scale, RND[f(i, "RoundMode") as usize & 3], sat])
}

/// The SM86 name of Maxwell's `FMUL` scale: none, D2, D4, D8, M8, M4, M2.
fn fmul_scale(s: u64) -> Option<&'static str> {
    ["", "D2", "D4", "D8", "M8", "M4", "M2"]
        .get(s as usize)
        .copied()
}

/// `FMUL`: SM86 `FMUL` with the same rounding, flush, saturation and
/// scale; Maxwell's `.NEG` negates the product, here on the first source.
fn fmul(i: &Inst) -> Res {
    let scale = fmul_scale(f(i, "Scale"));
    let m = modes(i, "Fmz", scale.unwrap_or(""))?;
    if scale.is_none() {
        return Err("FMUL with an invalid scale");
    }
    let b = src_b(i, false, false)?;
    let mut p = with(inst(i, "FMUL"), &m);
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(reg_m(f(i, "SrcA"), f(i, "NegA") != 0, false));
    p.ops.push(b);
    Ok(p)
}

/// `FADD`: SM86 `FADD` with the same modes and operand negation and
/// absolute values.
fn fadd(i: &Inst) -> Res {
    let m = modes(i, "Ftz", "")?;
    let b = src_b(i, f(i, "NegB") != 0, f(i, "AbsB") != 0)?;
    let mut p = with(inst(i, "FADD"), &m);
    p.ops.push(reg(f(i, "Dest")));
    p.ops
        .push(reg_m(f(i, "SrcA"), f(i, "NegA") != 0, f(i, "AbsA") != 0));
    p.ops.push(b);
    Ok(p)
}

/// `FFMA`: SM86 `FFMA`, `a * b + c` with the same modes; Maxwell's `.NEG`
/// of the product goes on the first source, `.NEG` of the addend on the
/// third. `FFMA32I` adds its destination; the register-constant form takes
/// the register as `b` and the constant as `c`.
fn ffma(i: &Inst) -> Res {
    let m = modes(i, "Fmz", "")?;
    let neg_c = f(i, "NegC") != 0;
    let (b, c) = if i.enc.src == Src::RegCBuf {
        (reg(f(i, "SrcC")), cbuf_of(i, neg_c, false)?)
    } else {
        let b = src_b(i, false, false)?;
        let c = if i.enc.src == Src::Imm32 {
            f(i, "Dest")
        } else {
            f(i, "SrcC")
        };
        (b, reg_m(c, neg_c, false))
    };
    let mut p = with(inst(i, "FFMA"), &m);
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(reg_m(f(i, "SrcA"), f(i, "NegA") != 0, false));
    p.ops.push(b);
    p.ops.push(c);
    Ok(p)
}

/// The SM86 names of the `MUFU` functions Maxwell shares, by Maxwell's
/// `MufuOp` (the same numbers).
const MUFU: [&str; 9] = ["COS", "SIN", "EX2", "LG2", "RCP", "RSQ", "", "", "SQRT"];

/// `MUFU`: SM86 `MUFU` of the same function with the operand's negation
/// and absolute value; `.SAT`, which SM86 lacks, is the pseudo-instruction
/// `MUFU.SAT`. The 64-bit forms stop.
fn mufu(i: &Inst) -> Lowered {
    let kind = MUFU.get(f(i, "MufuOp") as usize).copied().unwrap_or("");
    if kind.is_empty() {
        return Lowered::Stop("MUFU.RCP64H, MUFU.RSQ64H or an invalid function");
    }
    let mut p = with(inst(i, "MUFU"), &[kind]);
    p.ops.push(reg(f(i, "Dest")));
    p.ops
        .push(reg_m(f(i, "SrcA"), f(i, "NegA") != 0, f(i, "AbsA") != 0));
    if f(i, "Sat") == 0 {
        return Lowered::Exact(p);
    }
    p.mods.push(MUFU_SAT);
    Lowered::Pseudo(p)
}

/// `RRO`: the pseudo-instruction `RRO.SINCOS` or `RRO.EX2` of the operand
/// with its negation and absolute value.
fn rro(i: &Inst) -> Lowered {
    if !matches!(i.enc.src, Src::Reg | Src::Imm | Src::CBuf) {
        return Lowered::Stop("RRO form");
    }
    let src = match src_b(i, f(i, "NegB") != 0, f(i, "AbsB") != 0) {
        Ok(s) => s,
        Err(r) => return Lowered::Stop(r),
    };
    let op = if f(i, "RroOp") != 0 {
        RRO_EX2
    } else {
        RRO_SINCOS
    };
    let mut p = with(inst(i, "RRO"), &[op]);
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(src);
    Lowered::Pseudo(p)
}

/// The `MOV` of `src` to `Dest` with the full lane mask, the only mask
/// that lowers.
fn mov(i: &Inst, name: &'static str, src: impl FnOnce() -> Result<Operand, &'static str>) -> Res {
    if f(i, "QuadMask") != 0xf {
        return Err(name);
    }
    let mut p = inst(i, "MOV");
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(src()?);
    Ok(p)
}

/// `MOV32I Rd, imm`: SM86 `MOV Rd, imm`.
fn mov32i(i: &Inst) -> Res {
    mov(i, "MOV32I with a lane mask other than 0xf", || {
        Ok(Operand::Hex(f(i, "Imm32")))
    })
}

/// `MOV Rd, c[bank][offset]`: SM86 `MOV Rd, c[bank][offset]`, the offset
/// in bytes (Maxwell holds it in words).
fn mov_c(i: &Inst) -> Res {
    mov(i, "MOV with a lane mask other than 0xf", || {
        cbuf_of(i, false, false)
    })
}

/// `MOV Rd, Rs`: SM86 `MOV Rd, Rs`.
fn mov_r(i: &Inst) -> Res {
    mov(i, "MOV with a lane mask other than 0xf", || {
        Ok(reg(f(i, "SrcA")))
    })
}

/// The source predicate of `i`.
fn src_pred(i: &Inst) -> Operand {
    pred(f(i, "SrcPred"), f(i, "SrcPredInv") != 0)
}

/// `FMNMX`: SM86 `FMNMX` with the same operands, their negation and
/// absolute values, `.FTZ` and the predicate that picks the minimum.
fn fmnmx(i: &Inst) -> Res {
    if f(i, "WriteCC") != 0 {
        return Err("FMNMX that writes the condition code");
    }
    let b = src_b(i, f(i, "NegB") != 0, f(i, "AbsB") != 0)?;
    let ftz = if f(i, "Ftz") != 0 { "FTZ" } else { "" };
    let mut p = with(inst(i, "FMNMX"), &[ftz]);
    p.ops.push(reg(f(i, "Dest")));
    p.ops
        .push(reg_m(f(i, "SrcA"), f(i, "NegA") != 0, f(i, "AbsA") != 0));
    p.ops.push(b);
    p.ops.push(src_pred(i));
    Ok(p)
}

/// The SM86 names of the float comparisons, by Maxwell's `FComp` (the
/// same numbers).
const FCMP: [&str; 16] = [
    "F", "LT", "EQ", "LE", "GT", "NE", "GE", "NUM", "NAN", "LTU", "EQU", "LEU", "GTU", "NEU",
    "GEU", "T",
];

/// The SM86 names of the boolean operations: AND, OR, XOR.
const BOOL: [&str; 3] = ["AND", "OR", "XOR"];

/// `FSETP`: SM86 `FSETP` with the same comparison, `.FTZ`, boolean
/// operation, both destinations and operand modifiers. Maxwell's bit 47 is
/// `.FTZ` (envydis); Ryujinx's form also names it the condition code bit,
/// which FSETP does not write.
fn fsetp(i: &Inst) -> Res {
    let Some(bop) = BOOL.get(f(i, "Bop") as usize) else {
        return Err("FSETP with an invalid boolean operation");
    };
    let b = src_b(i, f(i, "NegB") != 0, f(i, "AbsB") != 0)?;
    let ftz = if f(i, "Ftz") != 0 { "FTZ" } else { "" };
    let mut p = with(
        inst(i, "FSETP"),
        &[FCMP[f(i, "FComp") as usize & 15], ftz, bop],
    );
    p.ops.push(pred(f(i, "DestPred"), false));
    p.ops.push(pred(f(i, "DestPredInv"), false));
    p.ops
        .push(reg_m(f(i, "SrcA"), f(i, "NegA") != 0, f(i, "AbsA") != 0));
    p.ops.push(b);
    p.ops.push(src_pred(i));
    Ok(p)
}

/// The SM86 name of an integer type, by signedness and size (8, 16, 32
/// bits); a signed 32-bit integer has none.
fn ity(signed: bool, size: u64) -> &'static str {
    match (signed, size) {
        (false, 0) => "U8",
        (true, 0) => "S8",
        (false, 1) => "U16",
        (true, 1) => "S16",
        (false, _) => "U32",
        (true, _) => "",
    }
}

/// `I2F`: SM86 `I2F` to F32 with the same source size, signedness, byte
/// or half selection and rounding mode. Maxwell's integer `.ABS` and
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
    let (size, signed) = (src & 3, src >> 2 != 0);
    if size == 3 {
        return Lowered::Stop("I2F from a 64-bit integer");
    }
    let sel = match (size, f(i, "ByteSel")) {
        (_, 0) => None,
        (0, s) => Some(relift_gpu_nv::Swz::B(s as u8)),
        (1, 2) => Some(relift_gpu_nv::Swz::H(1)),
        _ => return Lowered::Stop("I2F with a byte selection its source size does not have"),
    };
    let (neg, abs) = (f(i, "NegB") != 0, f(i, "AbsB") != 0);
    if (neg || abs) && size != 2 {
        return Lowered::Stop("I2F of an 8 or 16-bit integer with .ABS or .NEG");
    }
    let b = match i.enc.src {
        Src::Reg => reg(f(i, "SrcB")),
        Src::Imm => {
            let v = f(i, "Imm20");
            let v = if v & 0x8_0000 != 0 {
                v | 0xfff0_0000
            } else {
                v
            };
            Operand::Int(i64::from(v as u32 as i32))
        }
        Src::CBuf => match cbuf_of(i, false, false) {
            Ok(c) => c,
            Err(r) => return Lowered::Stop(r),
        },
        _ => return Lowered::Stop("no SM86 operand form"),
    };
    if (neg || abs) && i.enc.src != Src::Reg {
        return Lowered::Stop("I2F of a constant with .ABS or .NEG");
    }
    let b = match (b, sel) {
        (Operand::Reg(n, mut m), Some(s)) => {
            m.swz = s;
            Operand::Reg(n, m)
        }
        (
            Operand::CBuf {
                bank,
                index,
                offset,
                mut mods,
                space,
            },
            Some(s),
        ) => {
            mods.swz = s;
            Operand::CBuf {
                bank,
                index,
                offset,
                mods,
                space,
            }
        }
        (b, _) => b,
    };
    let mut p = with(
        inst(i, "I2F"),
        &[ity(signed, size), RND[f(i, "RoundMode") as usize & 3]],
    );
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(b);
    if !neg && !abs {
        return Lowered::Exact(p);
    }
    if abs {
        p.mods.push(I2F_IABS);
    }
    if neg {
        p.mods.push(I2F_INEG);
    }
    Lowered::Pseudo(p)
}

/// The SM86 names of the integer roundings of `F2I` and `FRND`: none,
/// FLOOR, CEIL, TRUNC.
const IRND: [&str; 4] = ["", "FLOOR", "CEIL", "TRUNC"];

/// The float source of a conversion: `src_b` with the operand's negation
/// and absolute value.
fn cvt_src(i: &Inst) -> Result<Operand, &'static str> {
    src_b(i, f(i, "NegB") != 0, f(i, "AbsB") != 0)
}

/// `F2I`: SM86 `F2I.NTZ` from F32 with the same destination size and
/// signedness, rounding, `.FTZ` and the operand's negation and absolute
/// value. `.NTZ` makes a NaN zero, as Maxwell's F2I gives it (inferred,
/// s2-console-questions).
fn f2i(i: &Inst) -> Res {
    if f(i, "WriteCC") != 0 {
        return Err("F2I that writes the condition code");
    }
    if f(i, "SrcFmt") != 2 || f(i, "Sh") != 0 {
        return Err("F2I from F16 or F64");
    }
    let dst = f(i, "IDstFmt");
    let ty = match (dst >> 2 != 0, dst & 3) {
        (false, 3) => "U64",
        (true, 3) => "S64",
        (s, n) => ity(s, n),
    };
    let b = cvt_src(i)?;
    let ftz = if f(i, "Ftz") != 0 { "FTZ" } else { "" };
    let mut p = with(
        inst(i, "F2I"),
        &[ftz, ty, IRND[f(i, "RoundMode") as usize & 3], "NTZ"],
    );
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(b);
    Ok(p)
}

/// `F2F` from F32 to F32 with an integer rounding: SM86 `FRND` with the
/// same rounding, `.FTZ` and operand modifiers. Other F2F forms stop.
fn f2f(i: &Inst) -> Res {
    if f(i, "WriteCC") != 0 {
        return Err("F2F that writes the condition code");
    }
    if f(i, "SrcFmt") != 2 || f(i, "DstFmt") != 2 || f(i, "Sh") != 0 {
        return Err("F2F between formats");
    }
    let mode = f(i, "RoundMode");
    if mode < 4 {
        return Err("F2F from F32 to F32 without an integer rounding");
    }
    if f(i, "Sat") != 0 {
        return Err("F2F with .SAT");
    }
    let b = cvt_src(i)?;
    let ftz = if f(i, "Ftz") != 0 { "FTZ" } else { "" };
    let mut p = with(inst(i, "FRND"), &[ftz, IRND[mode as usize & 3]]);
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(b);
    Ok(p)
}

/// The SM86 names of the attribute access sizes: 32, 64, 96, 128 bits.
const ASIZE: [&str; 4] = ["", "64", "96", "128"];

/// The attribute at byte offset `offset`.
fn attr(offset: u64) -> Operand {
    Operand::Attr {
        index: None,
        offset: offset as u32,
        patch: false,
    }
}

/// `AST a[imm], Rs, Rv`: SM86 `AST` with the same offset, data and vertex
/// registers, size and `.P`.
fn ast(i: &Inst) -> Res {
    if f(i, "SrcA") != RZ {
        return Err("AST with a register in the attribute address");
    }
    let p_ = if f(i, "P") != 0 { "P" } else { "" };
    let mut p = with(inst(i, "AST"), &[p_, ASIZE[f(i, "AlSize") as usize & 3]]);
    p.ops.push(attr(f(i, "Imm11")));
    p.ops.push(reg(f(i, "SrcB")));
    if f(i, "SrcC") != RZ {
        p.ops.push(reg(f(i, "SrcC")));
    }
    Ok(p)
}

/// `ALD Rd, a[imm], Rv`: SM86 `ALD` with the same offset, vertex register,
/// size and the `.P` and `.O` flags.
fn ald(i: &Inst) -> Res {
    if f(i, "SrcA") != RZ {
        return Err("ALD with a register in the attribute address");
    }
    let o = if f(i, "O") != 0 { "O" } else { "" };
    let p_ = if f(i, "P") != 0 { "P" } else { "" };
    let mut p = with(inst(i, "ALD"), &[o, p_, ASIZE[f(i, "AlSize") as usize & 3]]);
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(attr(f(i, "Imm11")));
    if f(i, "SrcB") != RZ {
        p.ops.push(reg(f(i, "SrcB")));
    }
    Ok(p)
}

/// `IPA`: SM86 `IPA` for `.PASS` and `.CONSTANT` at the center or the
/// centroid; with `.MUL` or `.SAT` the pseudo-instruction `IPA` with those
/// modifiers and the multiplier as its third operand.
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
        0 => ("PASS", false),
        1 => ("PASS", true),
        2 => ("CONSTANT", false),
        _ => return Lowered::Stop("IPA.SC"),
    };
    let b = f(i, "SrcB");
    if !mul && b != RZ {
        return Lowered::Stop("IPA with a multiplier register but no .MUL");
    }
    let sat = f(i, "Sat") != 0;
    let mut p = with(
        inst(i, "IPA"),
        &[freq, if centroid { "CENTROID" } else { "" }],
    );
    p.ops.push(reg(f(i, "Dest")));
    p.ops.push(attr(off));
    if !mul && !sat {
        return Lowered::Exact(p);
    }
    if mul {
        p.mods.push(IPA_MUL);
    }
    if sat {
        p.mods.push(IPA_SAT);
    }
    if mul {
        p.ops.push(reg(b));
    }
    Lowered::Pseudo(p)
}

/// The constant bank of bound texture handles: every NVN1 driver sets
/// Maxwell's texture constant buffer index (3D method 0x982) to 2.
const TEXTURE_BANK: u32 = 2;

/// The components each `TEXS` write mask selects, by whether `Rd2` is a
/// register (Ryujinx's TEXS table, envydis's component names).
const TEXS_MASKS: [[u8; 8]; 2] = [
    [
        0b0001, 0b0010, 0b0100, 0b1000, 0b0011, 0b1001, 0b1010, 0b1100,
    ],
    [0b0111, 0b1011, 0b1101, 0b1110, 0b1111, 0, 0, 0],
];

/// The SM86 texture dimension index of Maxwell's `TexDim`: 1D, 1D array,
/// 2D, 2D array, 3D, (3D array), cube and cube array are 0, 4, 1, 5, 2,
/// (none), 3 and 7 there.
fn tex_dim(d: u64) -> Option<u8> {
    [
        Some(0),
        Some(4),
        Some(1),
        Some(5),
        Some(2),
        None,
        Some(3),
        Some(7),
    ][d as usize & 7]
}

/// The SM86 names of the texture dimensions, by index, of `TEX`, `TLD`
/// and `TLD4`.
const DIMS: [&str; 8] = [
    "1D",
    "2D",
    "3D",
    "CUBE",
    "ARRAY_1D",
    "ARRAY_2D",
    "",
    "ARRAY_CUBE",
];
const DIMS_LD: [&str; 8] = ["1D", "2D", "3D", "", "ARRAY_1D", "ARRAY_2D", "", ""];

/// The SM86 names of the LOD modes of `TEX`: none, LZ, LB, LL.
const LOD: [&str; 4] = ["", "LZ", "LB", "LL"];

/// The SM86 names of the `TLD4` components: R, G, B, A.
const COMP: [&str; 4] = ["R", "G", "B", "A"];

/// One SM86 texture instruction before it is built: the registers, the
/// handle (bound to `c[2][4 * TidB]`, or a register with `.B`), the
/// dimension (an index into `names`), the component mask and the
/// modifiers between `.B` and `.NODEP`.
struct TexInst<'a> {
    name: &'static str,
    scr: bool,
    bindless: bool,
    mods: &'a [&'static str],
    rd: u64,
    rd2: u64,
    ra: u64,
    rb: u64,
    dim: Option<(u8, &'static [&'static str; 8])>,
    mask: u8,
    tex: Tex,
}

/// The instruction of `t` for the Maxwell `i`, as the SM86 decoder gives
/// the texture forms.
fn tex_inst(i: &Inst, t: TexInst<'_>) -> Res {
    let mut p = inst(i, t.name);
    if t.scr {
        p.mods.push("SCR");
    }
    if t.bindless {
        p.mods.push("B");
    }
    p.mods.extend(t.mods.iter().filter(|m| !m.is_empty()));
    if f(i, "Nodep") != 0 {
        p.mods.push("NODEP");
    }
    let txq = t.name == "TXQ";
    p.ops.push(reg(t.rd2));
    p.ops.push(reg(t.rd));
    p.ops.push(if t.ra == RZ && (t.bindless || !txq) {
        Operand::Sym("???255".to_string())
    } else {
        reg(t.ra)
    });
    if !txq {
        if t.rb != RZ {
            p.ops.push(reg(t.rb));
        } else if t.bindless {
            p.ops.push(Operand::Sym("???255".to_string()));
        }
    } else {
        p.ops.push(Operand::Sym("TEX_HEADER_DIMENSION".to_string()));
    }
    let tid = f(i, "TidB");
    let handle = if t.bindless {
        Handle::Reg
    } else {
        p.ops.push(Operand::Hex(u64::from(TEXTURE_BANK)));
        p.ops.push(Operand::Hex(tid));
        Handle::Bound {
            bank: TEXTURE_BANK,
            offset: 4 * tid as u32,
        }
    };
    let mut dim = 0;
    if let Some((d, names)) = t.dim {
        let n = names[usize::from(d)];
        if n.is_empty() {
            return Err("a texture dimension SM86 does not have");
        }
        p.ops.push(Operand::Sym(n.to_string()));
        dim = d;
    }
    if t.mask != 0xf || txq {
        p.ops.push(Operand::Hex(u64::from(t.mask)));
    }
    p.fields = Fields {
        rd: t.rd as u8,
        ra: t.ra as u8,
        rb: t.rb as u8,
        rc: t.rd2 as u8,
        mask: t.mask,
        tex: Tex {
            handle,
            dim,
            fault: if txq { 0 } else { PT as u8 },
            ..t.tex
        },
        ..Fields::default()
    };
    Ok(p)
}

/// `TEXS`: SM86 `TEX.SCR` on the bound handle `c[2][TidB * 4]` with the
/// target's dimension, LOD mode and depth compare, the component mask of
/// the write mask, and the same `Rd`, `Rd2`, `Ra` and `Rb`: each target's
/// sources are those `.SCR` splits over `Ra`, `Ra + 1`, `Rb` and `Rb + 1`
/// in the same order.
fn texs(i: &Inst) -> Res {
    let (dim, lod, dc) = match f(i, "Target") {
        0 => (0, 1, false),
        1 => (1, 0, false),
        2 => (1, 1, false),
        3 => (1, 3, false),
        4 => (1, 0, true),
        6 => (1, 1, true),
        7 => (5, 0, false),
        8 => (5, 1, false),
        10 => (2, 0, false),
        11 => (2, 1, false),
        12 => (3, 0, false),
        13 => (3, 3, false),
        _ => {
            return Err("TEXS with an explicit LOD and a depth compare or an array depth compare");
        }
    };
    let rd2 = f(i, "Dest2");
    let mask = TEXS_MASKS[usize::from(rd2 != RZ)][f(i, "WMask") as usize & 7];
    if mask == 0 {
        return Err("TEXS with an empty write mask");
    }
    tex_inst(
        i,
        TexInst {
            name: "TEX",
            scr: true,
            bindless: false,
            mods: &[LOD[lod as usize], if dc { "DC" } else { "" }],
            rd: f(i, "Dest"),
            rd2,
            ra: f(i, "SrcA"),
            rb: f(i, "SrcB"),
            dim: Some((dim, &DIMS)),
            mask,
            tex: Tex {
                lod,
                dc,
                scr: true,
                ..Tex::default()
            },
        },
    )
}

/// The second destination of an SM86 texture instruction that writes
/// Maxwell's four consecutive registers from `Rd`: `Rd + 2`, or RZ with
/// `Rd`.
fn rd2_of(rd: u64) -> Option<u64> {
    match rd {
        RZ => Some(RZ),
        r if r + 3 < RZ => Some(r + 2),
        _ => None,
    }
}

/// `TEX` and `TEX.B`: SM86 `TEX` (with a register handle for `.B`) with
/// the same dimension, LOD mode (none, LZ, LB, LL), offsets, depth compare
/// and component mask. Both read the array index and coordinates from `Ra`
/// on and the handle, LOD or bias, offsets and reference from `Rb` on, in
/// the same order; Maxwell writes the components to `Rd` on, which SM86
/// writes to `Rd`, `Rd + 1` and its `Rd2`, `Rd2 + 1`.
fn tex(i: &Inst, bindless: bool) -> Res {
    let (lod, aoffi, lc) = if bindless {
        (f(i, "Lodb"), f(i, "Aoffib"), f(i, "Lcb"))
    } else {
        (f(i, "Lod"), f(i, "Aoffi"), f(i, "Lc"))
    };
    if lod > 3 || lc != 0 {
        return Err("TEX with .LC or an array LOD mode");
    }
    if f(i, "Ndv") != 0 || f(i, "DestPred") != PT {
        return Err("TEX with .NDV or a sparse predicate");
    }
    let Some(dim) = tex_dim(f(i, "Dim")) else {
        return Err("TEX of a 3D array");
    };
    let rd = f(i, "Dest");
    let Some(rd2) = rd2_of(rd) else {
        return Err("TEX writing past R254");
    };
    let (aoffi, dc) = (aoffi != 0, f(i, "Dc") != 0);
    tex_inst(
        i,
        TexInst {
            name: "TEX",
            scr: false,
            bindless,
            mods: &[
                LOD[lod as usize],
                if aoffi { "AOFFI" } else { "" },
                if dc { "DC" } else { "" },
            ],
            rd,
            rd2,
            ra: f(i, "SrcA"),
            rb: f(i, "SrcB"),
            dim: Some((dim, &DIMS)),
            mask: f(i, "WMask") as u8 & 0xf,
            tex: Tex {
                lod: lod as u8,
                aoffi,
                dc,
                ..Tex::default()
            },
        },
    )
}

/// `TLD4S`: SM86 `TLD4.SCR` of a 2D texture on the bound handle with the
/// same component, offsets (six bits per coordinate) and depth compare;
/// its sources are those `.SCR` splits over `Ra`, `Ra + 1`, `Rb` and
/// `Rb + 1` (coordinates, offsets, reference), as Ryujinx's TLD4S reads
/// them. It writes four components (two with `Rd2` RZ), as TEXS's mask 4.
fn tld4s(i: &Inst) -> Res {
    let rd2 = f(i, "Dest2");
    let comp = f(i, "TexComp") as u8 & 3;
    let offsets = f(i, "Aoffi") as u8 & 3;
    let dc = f(i, "Dc") != 0;
    let off = match offsets {
        1 => "AOFFI",
        2 => "PTP",
        3 => return Err("TLD4S offsets"),
        _ => "",
    };
    tex_inst(
        i,
        TexInst {
            name: "TLD4",
            scr: true,
            bindless: false,
            mods: &[COMP[usize::from(comp)], off, if dc { "DC" } else { "" }],
            rd: f(i, "Dest"),
            rd2,
            ra: f(i, "SrcA"),
            rb: f(i, "SrcB"),
            dim: Some((1, &DIMS)),
            mask: TEXS_MASKS[usize::from(rd2 != RZ)][4],
            tex: Tex {
                component: comp,
                offsets,
                dc,
                scr: true,
                ..Tex::default()
            },
        },
    )
}

/// The SM86 names of the LOD modes of `TLD`: LZ, LL.
fn tld_lod(ll: bool) -> (u8, &'static str) {
    if ll { (3, "LL") } else { (1, "LZ") }
}

/// `TLDS`: SM86 `TLD.SCR` on the bound handle with the target's
/// dimension, LOD mode, offsets and sample; each target's sources are
/// those `.SCR` splits over `Ra`, `Ra + 1`, `Rb`, `Rb + 1` in Ryujinx's
/// order. The 2D array target, whose layer Ryujinx reads from `Ra` after
/// the coordinates in `Rb`, stops.
fn tlds(i: &Inst) -> Res {
    let (dim, ll, aoffi, ms) = match f(i, "Target") {
        0 => (0, false, false, false),
        1 => (0, true, false, false),
        2 => (1, false, false, false),
        4 => (1, false, true, false),
        5 => (1, true, false, false),
        6 => (1, false, false, true),
        7 => (2, false, false, false),
        12 => (1, true, true, false),
        _ => return Err("TLDS of a 2D array"),
    };
    let rd2 = f(i, "Dest2");
    let mask = TEXS_MASKS[usize::from(rd2 != RZ)][f(i, "WMask") as usize & 7];
    if mask == 0 {
        return Err("TLDS with an empty write mask");
    }
    let (lod, lname) = tld_lod(ll);
    tex_inst(
        i,
        TexInst {
            name: "TLD",
            scr: true,
            bindless: false,
            mods: &[
                lname,
                if aoffi { "AOFFI" } else { "" },
                if ms { "MS" } else { "" },
            ],
            rd: f(i, "Dest"),
            rd2,
            ra: f(i, "SrcA"),
            rb: f(i, "SrcB"),
            dim: Some((dim, &DIMS_LD)),
            mask,
            tex: Tex {
                lod,
                aoffi,
                ms,
                scr: true,
                ..Tex::default()
            },
        },
    )
}

/// `TLD.B`: SM86 `TLD.B` with the same dimension, LOD mode (LZ or LL),
/// offsets and sample; both read the layer and coordinates from `Ra` on
/// and the handle, LOD, offsets and sample from `Rb` on. `.CL` stops.
fn tld_b(i: &Inst) -> Res {
    if f(i, "Cl") != 0 || f(i, "DestPred") != PT {
        return Err("TLD with .CL or a sparse predicate");
    }
    let Some(dim) = tex_dim(f(i, "Dim")) else {
        return Err("TLD of a 3D array");
    };
    let rd = f(i, "Dest");
    let Some(rd2) = rd2_of(rd) else {
        return Err("TLD writing past R254");
    };
    let (aoffi, ms) = (f(i, "Toff") != 0, f(i, "Ms") != 0);
    let (lod, lname) = tld_lod(f(i, "Lod") != 0);
    tex_inst(
        i,
        TexInst {
            name: "TLD",
            scr: false,
            bindless: true,
            mods: &[
                lname,
                if aoffi { "AOFFI" } else { "" },
                if ms { "MS" } else { "" },
            ],
            rd,
            rd2,
            ra: f(i, "SrcA"),
            rb: f(i, "SrcB"),
            dim: Some((dim, &DIMS_LD)),
            mask: f(i, "WMask") as u8 & 0xf,
            tex: Tex {
                lod,
                aoffi,
                ms,
                ..Tex::default()
            },
        },
    )
}

/// `TXQ` and `TXQ.B` of the dimensions: SM86 `TXQ` (with a register
/// handle for `.B`) `TEX_HEADER_DIMENSION` with the same component mask;
/// both read the handle (bindless) and then the level from `Ra` on. Other
/// queries stop.
fn txq(i: &Inst, bindless: bool) -> Res {
    if f(i, "TexQuery") != 1 {
        return Err("TXQ other than the dimensions");
    }
    let rd = f(i, "Dest");
    let Some(rd2) = rd2_of(rd) else {
        return Err("TXQ writing past R254");
    };
    tex_inst(
        i,
        TexInst {
            name: "TXQ",
            scr: false,
            bindless,
            mods: &[],
            rd,
            rd2,
            ra: f(i, "SrcA"),
            rb: RZ,
            dim: None,
            mask: f(i, "WMask") as u8 & 0xf,
            tex: Tex::default(),
        },
    )
}

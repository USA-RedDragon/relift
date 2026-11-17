//! Which registers and predicates hold the same value in every invocation
//! of a workgroup, one instruction at a time.
//!
//! A value is uniform when it comes from constants, the constant banks or
//! the workgroup's ID through arithmetic on uniform values, written where
//! every invocation runs. Everything else (thread and lane IDs, memory,
//! warp operations, clocks, instructions this module does not model) is
//! taken to vary, except a shared memory load at a uniform address that
//! every invocation runs between two BARs no shared memory write lies
//! between (the cfg module finds those): it reads the same word in each.
//! The answer is conservative: a register said to be uniform is; one said
//! to vary may not.
//!
//! For the arithmetic families the destinations the instruction certainly
//! writes take the result's uniformity, so a register reused for a uniform
//! value is uniform again. Every other register or predicate an
//! instruction may write (the second register of a 64-bit form whose
//! result may be 32 bits, any operand of an instruction this module does
//! not model) varies if its old value or the result does.

use crate::{CBank, Inst, Operand};

const P0: usize = 256;
const UR0: usize = 264;
const UP0: usize = 328;
const SLOTS: usize = 336;

/// The registers and predicates that may vary between invocations.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(super) struct Varying([u64; SLOTS.div_ceil(64)]);

impl Varying {
    fn get(&self, s: usize) -> bool {
        self.0[s / 64] >> (s % 64) & 1 == 1
    }

    fn set(&mut self, s: usize, v: bool) {
        if v {
            self.0[s / 64] |= 1 << (s % 64);
        } else {
            self.0[s / 64] &= !(1 << (s % 64));
        }
    }

    /// Adds what varies in `o`; true if that changed anything.
    pub(super) fn join(&mut self, o: &Varying) -> bool {
        let old = self.0;
        for (a, b) in self.0.iter_mut().zip(o.0) {
            *a |= b;
        }
        old != self.0
    }

    /// Whether every predicate of a branch condition is uniform.
    pub(super) fn uniform_all(&self, conds: &[Operand]) -> bool {
        let mut s = Vec::new();
        for c in conds {
            slots(c, 1, &mut s);
        }
        s.iter().all(|s| !self.get(*s))
    }

    pub(super) fn uniform_reg(&self, r: u8, ureg: bool) -> bool {
        match (ureg, r) {
            (false, 255) | (true, 63) => true,
            (false, r) => !self.get(r as usize),
            (true, r) => !self.get(UR0 + r as usize),
        }
    }
}

/// Registers and predicates an operand names, nested ones included, with
/// registers widened to `w` consecutive registers.
fn slots(o: &Operand, w: usize, out: &mut Vec<usize>) {
    match o {
        Operand::Reg(255, _) | Operand::UReg(63, _) => {}
        Operand::Reg(r, _) => {
            out.extend((*r as usize..(*r as usize + w).min(255)).filter(|s| *s < 255))
        }
        Operand::UReg(r, _) => {
            out.extend((*r as usize..(*r as usize + w).min(63)).map(|r| UR0 + r));
        }
        Operand::Pred(7, _) | Operand::UPred(7, _) => {}
        Operand::Pred(p, _) => out.push(P0 + *p as usize),
        Operand::UPred(p, _) => out.push(UP0 + *p as usize),
        Operand::Sym(s) if s == "PR" => out.extend(P0..P0 + 7),
        Operand::Sym(s) if s == "UPR" => out.extend(UP0..UP0 + 7),
        Operand::CBuf { bank, index, .. } => {
            if let CBank::UReg(u) = bank {
                slots(&Operand::UReg(*u, Default::default()), 1, out);
            }
            if let Some(i) = index {
                slots(i, 1, out);
            }
        }
        Operand::Attr { index: Some(i), .. } => slots(i, 1, out),
        Operand::Mem { base, ubase, .. } => {
            for b in [base, ubase].into_iter().flatten() {
                slots(b, 2, out);
            }
        }
        _ => {}
    }
}

/// Instructions that an invocation running a converted region it skips
/// (see the cfg module) must not carry out: everything but arithmetic and
/// the instructions that only order or mark control flow.
pub(super) fn gated(i: &Inst) -> bool {
    !arithmetic(i.mnemonic)
        && !matches!(
            i.mnemonic,
            "NOP"
                | "BAR"
                | "BRA"
                | "BSSY"
                | "BSYNC"
                | "WARPSYNC"
                | "DEPBAR"
                | "ERRBAR"
                | "CCTL"
                | "CCTLL"
                | "NANOSLEEP"
                | "YIELD"
                | "LDGDEPBAR"
        )
}

/// Instructions that write nothing a later instruction reads.
fn writes_nothing(m: &str) -> bool {
    matches!(
        m,
        "NOP"
            | "BAR"
            | "BRA"
            | "BRX"
            | "BRXU"
            | "EXIT"
            | "CALL"
            | "RET"
            | "BSSY"
            | "BSYNC"
            | "BREAK"
            | "WARPSYNC"
            | "DEPBAR"
            | "MEMBAR"
            | "ERRBAR"
            | "CCTL"
            | "CCTLL"
            | "NANOSLEEP"
            | "YIELD"
            | "KILL"
            | "STG"
            | "STS"
            | "STL"
            | "ST"
            | "RED"
            | "SUST"
            | "SURED"
            | "LDGDEPBAR"
    )
}

/// Pure arithmetic whose destinations come first (see the module notes).
fn arithmetic(m: &str) -> bool {
    matches!(
        m,
        "MOV"
            | "IMAD"
            | "IADD3"
            | "IADD"
            | "IMUL"
            | "ISCADD"
            | "LOP3"
            | "LOP"
            | "SHF"
            | "SHL"
            | "SHR"
            | "LEA"
            | "ISETP"
            | "SEL"
            | "IABS"
            | "IMNMX"
            | "POPC"
            | "FLO"
            | "BREV"
            | "PRMT"
            | "BMSK"
            | "SGXT"
            | "PLOP3"
            | "P2R"
            | "R2P"
            | "IDP"
            | "FADD"
            | "FMUL"
            | "FFMA"
            | "FSETP"
            | "FSET"
            | "FSEL"
            | "FMNMX"
            | "FCHK"
            | "MUFU"
            | "F2I"
            | "I2F"
            | "F2F"
            | "I2I"
            | "I2FP"
            | "F2FP"
            | "FRND"
            | "HADD2"
            | "HMUL2"
            | "HFMA2"
            | "HSETP2"
            | "HSET2"
            | "HMNMX2"
            | "DADD"
            | "DMUL"
            | "DFMA"
            | "DSETP"
            | "DMNMX"
            | "LDC"
            | "ULDC"
            | "UMOV"
            | "UIADD3"
            | "UIMAD"
            | "ULOP3"
            | "USHF"
            | "ULEA"
            | "UISETP"
            | "USEL"
            | "UPLOP3"
            | "UPRMT"
            | "USGXT"
            | "UFLO"
            | "UPOPC"
            | "UBMSK"
            | "UBREV"
            | "UP2UR"
            | "R2UR"
            | "S2R"
            | "S2UR"
            | "CS2R"
    )
}

/// The width of source registers: pairs for 64-bit forms, quads for
/// 128-bit ones. Reading too many registers only loses precision.
fn source_width(i: &Inst) -> usize {
    if i.mods.iter().any(|m| m.contains("128")) {
        4
    } else if i.mnemonic.starts_with('D') || i.mods.iter().any(|m| m.contains("64") || *m == "WIDE")
    {
        2
    } else {
        1
    }
}

/// The registers a definite destination certainly writes: pairs only
/// where the result is 64 bits wide.
fn dest_width(i: &Inst) -> usize {
    let wide = |m: &&str| m.contains("64");
    match i.mnemonic {
        "IMAD" | "UIMAD" if i.has("WIDE") => 2,
        "LDC" | "ULDC" if i.mods.iter().any(wide) => 2,
        "DADD" | "DMUL" | "DFMA" | "DMNMX" => 2,
        "F2F" | "F2I" | "I2F" | "I2I" | "FRND" => {
            let ty = i
                .mods
                .iter()
                .find(|m| m.starts_with(['F', 'U', 'S']) && m.len() <= 3);
            if ty.is_some_and(wide) { 2 } else { 1 }
        }
        "CS2R" if !i.has("32") => 2,
        _ => 1,
    }
}

/// The operand positions an arithmetic instruction certainly writes.
fn definite(i: &Inst) -> Vec<usize> {
    let pred = |k: usize| matches!(i.ops.get(k), Some(Operand::Pred(..) | Operand::UPred(..)));
    let reg = |k: usize| matches!(i.ops.get(k), Some(Operand::Reg(..) | Operand::UReg(..)));
    match i.mnemonic {
        "ISETP" | "UISETP" | "FSETP" | "DSETP" | "HSETP2" | "FCHK" => {
            (0..i.ops.len()).take_while(|k| pred(*k)).take(2).collect()
        }
        "PLOP3" | "UPLOP3" => vec![0, 1],
        "LOP3" | "ULOP3" if pred(0) => vec![0, 1],
        "IADD3" | "UIADD3" | "LEA" | "ULEA" | "IMAD" | "UIMAD" => {
            let mut v = vec![0];
            v.extend((1..i.ops.len()).take_while(|k| pred(*k)));
            v
        }
        "R2P" => Vec::new(),
        _ if reg(0) || pred(0) => vec![0],
        _ => Vec::new(),
    }
}

/// How many registers from each operand an instruction this module does
/// not model may write: loads and warp operations name their width, the
/// rest (textures, surfaces, matrix operations) are taken as quads.
fn unknown_width(i: &Inst) -> usize {
    match i.mnemonic {
        "LDG" | "LDS" | "LDL" | "LD" | "ATOM" | "ATOMS" | "ATOMG" | "SHFL" | "VOTE" | "VOTEU"
        | "MATCH" | "REDUX" | "BMOV" | "S2UR" => source_width(i),
        _ => 4,
    }
}

fn special_varies(i: &Inst) -> bool {
    matches!(i.mnemonic, "S2R" | "S2UR" | "CS2R")
        && !i
            .ops
            .iter()
            .any(|o| matches!(o, Operand::Sym(s) if s == "SRZ" || s.starts_with("SR_CTAID.")))
}

fn guarded(i: &Inst) -> bool {
    matches!(i.guard, Some(Operand::Pred(p, _)) | Some(Operand::UPred(p, _)) if p != 7)
}

fn top_level(o: &Operand) -> bool {
    matches!(
        o,
        Operand::Reg(..)
            | Operand::UReg(..)
            | Operand::Pred(..)
            | Operand::UPred(..)
            | Operand::Sym(_)
    )
}

/// A load `i` that reads the same word in every invocation that runs it
/// at a uniform address: its destination is uniform if its address is.
/// Gives whether the address was uniform (and `v` updated).
pub(super) fn broadcast(i: &Inst, v: &mut Varying) -> bool {
    let mut read = Vec::new();
    for o in i.ops.iter().skip(1) {
        slots(o, 1, &mut read);
    }
    let Some(dest) = i.ops.first() else {
        return false;
    };
    if read.iter().any(|s| v.get(*s)) || !matches!(dest, Operand::Reg(..)) {
        return false;
    }
    let mut out = Vec::new();
    slots(dest, source_width(i), &mut out);
    for s in out {
        v.set(s, false);
    }
    true
}

/// Updates `v` for instruction `i`; `partial` when only some invocations
/// may run it.
///
/// A definite destination takes the result's uniformity (or keeps a
/// varying old value under a guard); a possible one varies if either its
/// old value or the result does.
pub(super) fn step(i: &Inst, partial: bool, v: &mut Varying) {
    if writes_nothing(i.mnemonic) {
        return;
    }
    let known = arithmetic(i.mnemonic);
    let defs = if known { definite(i) } else { Vec::new() };
    let sw = source_width(i);
    let dw = dest_width(i);
    let mut read = Vec::new();
    let mut sure = Vec::new();
    let mut maybe = Vec::new();
    for (k, o) in i.ops.iter().enumerate() {
        if defs.contains(&k) {
            slots(o, dw, &mut sure);
            if sw > dw {
                // A 64-bit form whose result may still be one register.
                let mut wide = Vec::new();
                slots(o, sw, &mut wide);
                maybe.extend(wide.into_iter().filter(|s| !sure.contains(s)));
            }
        } else {
            slots(o, sw, &mut read);
            if !known && top_level(o) {
                // Anything else may write any register or predicate it
                // names; addresses are only read.
                slots(o, unknown_width(i), &mut maybe);
            } else if i.mnemonic == "R2P" && top_level(o) {
                slots(o, 1, &mut maybe);
            }
        }
    }
    if let Some(g) = &i.guard {
        slots(g, 1, &mut read);
    }
    let varies = !known || partial || special_varies(i) || read.iter().any(|s| v.get(*s));
    let keep = guarded(i);
    for s in sure {
        let old = v.get(s);
        v.set(s, varies || (keep && old));
    }
    for s in maybe {
        let old = v.get(s);
        v.set(s, varies || old);
    }
}

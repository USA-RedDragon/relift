//! Lifting of A64 encodings to `relift-ir`, one function per encoding id.
//!
//! [`lift`] uses the template generated from the Sail model ([`sail`]) when
//! the encoding has one. A few encodings also keep a hand-written lifter
//! ([`lifter`], [`lift_hand`]): the forms the differential checker was
//! brought up on, kept as a cross-check of the generator. The system
//! instructions with no template ([`system`]) are lifted by hand.

mod arith;
mod branch;
mod mem;
pub mod sail;
pub mod system;

use std::fmt;

use relift_ir::{Block, Builder, CmpOp, Exit, Flow, Next, Value, Width};

use crate::decode::{Encoding, Insn, decode};
use crate::regs::{self, C, N, SP, V, Z};

/// Why a word could not be lifted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LiftError {
    /// No encoding matches the word.
    Unallocated(u32),
    /// The encoding has no lifter yet.
    Unsupported(Encoding),
    /// The field values make the instruction UNDEFINED.
    Undefined(Encoding),
    /// The field values make the instruction CONSTRAINED UNPREDICTABLE; the
    /// lifter refuses rather than pick one of the permitted behaviors.
    Unpredictable(Encoding),
}

impl fmt::Display for LiftError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LiftError::Unallocated(w) => write!(f, "{w:#010x} is unallocated"),
            LiftError::Unsupported(e) => write!(f, "{} has no lifter", e.name()),
            LiftError::Undefined(e) => write!(f, "{} is UNDEFINED with these fields", e.name()),
            LiftError::Unpredictable(e) => {
                write!(f, "{} is UNPREDICTABLE with these fields", e.name())
            }
        }
    }
}

impl std::error::Error for LiftError {}

/// How a lifted instruction ends.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum End {
    /// Falls through to the next instruction.
    Next,
    /// Leaves through this exit.
    Exit(Exit),
}

/// A lifting function: emits the instruction's operations into the builder.
pub type LiftFn = fn(Insn, &mut Builder) -> Result<End, LiftError>;

/// Lifts the word `word` at guest address `pc` to a one-instruction block.
pub fn lift(word: u32, pc: u64) -> Result<Block, LiftError> {
    let insn = decode(word).ok_or(LiftError::Unallocated(word))?;
    lift_insn(insn, pc)
}

/// Lifts an already decoded instruction at `pc`: with its generated template
/// when it has one, else with its hand-written lifter.
pub fn lift_insn(insn: Insn, pc: u64) -> Result<Block, LiftError> {
    if let Some(f) = system::lifter(insn.encoding) {
        return run_lifter(f, insn, pc);
    }
    if sail::has_template(insn.encoding) {
        return sail::lift_insn(insn, pc);
    }
    hand_insn(insn, pc)
}

/// Lifts `word` at `pc` with its hand-written lifter only.
pub fn lift_hand(word: u32, pc: u64) -> Result<Block, LiftError> {
    let insn = decode(word).ok_or(LiftError::Unallocated(word))?;
    hand_insn(insn, pc)
}

/// Lifts an already decoded instruction with its hand-written lifter.
fn hand_insn(insn: Insn, pc: u64) -> Result<Block, LiftError> {
    let f = lifter(insn.encoding).ok_or(LiftError::Unsupported(insn.encoding))?;
    run_lifter(f, insn, pc)
}

/// Runs the lifting function `f` on `insn` at `pc`.
fn run_lifter(f: LiftFn, insn: Insn, pc: u64) -> Result<Block, LiftError> {
    let mut b = Builder::new(regs::regs(), Width::W64, pc);
    let exit = match f(insn, &mut b)? {
        End::Next => Exit {
            next: Next::Const(pc.wrapping_add(4)),
            flow: Flow::Jump,
        },
        End::Exit(e) => e,
    };
    Ok(b.finish(4, exit))
}

/// The encodings that lift, in specification order.
pub fn supported() -> impl Iterator<Item = Encoding> {
    Encoding::all()
        .filter(|e| system::lifter(*e).is_some() || sail::has_template(*e) || lifter(*e).is_some())
}

/// The hand-written lifting function for `enc`, if there is one.
pub fn lifter(enc: Encoding) -> Option<LiftFn> {
    arith::lifter(enc)
        .or_else(|| branch::lifter(enc))
        .or_else(|| mem::lifter(enc))
}

/// The operand width selected by an `sf` field.
fn sf_width(insn: &Insn) -> Width {
    if insn.get("sf") == 1 {
        Width::W64
    } else {
        Width::W32
    }
}

/// Reads `x<n>` (`xzr` for 31) truncated to `w`.
fn read_x(b: &mut Builder, n: u32, w: Width) -> Value {
    if n == 31 {
        return b.konst(w, 0);
    }
    let v = b.get(regs::x(n));
    b.trunc(v, w)
}

/// Reads `x<n>` (`sp` for 31) truncated to `w`.
fn read_x_sp(b: &mut Builder, n: u32, w: Width) -> Value {
    let v = if n == 31 {
        b.get(SP)
    } else {
        b.get(regs::x(n))
    };
    b.trunc(v, w)
}

/// Writes `x<n>` zero-extended to 64 bits; 31 discards the value.
fn write_x(b: &mut Builder, n: u32, v: Value) {
    if n == 31 {
        return;
    }
    let v = b.zext(v, Width::W64);
    b.set(regs::x(n), v);
}

/// Writes `x<n>` zero-extended to 64 bits; 31 writes `sp`.
fn write_x_sp(b: &mut Builder, n: u32, v: Value) {
    let v = b.zext(v, Width::W64);
    if n == 31 {
        b.set(SP, v);
    } else {
        b.set(regs::x(n), v);
    }
}

/// Sets N and Z from `result`, and C and V from the given bits.
fn set_flags(b: &mut Builder, result: Value, carry: Value, overflow: Value) {
    let w = b.width(result);
    let n = b.bit(result, w.bits() - 1);
    let zero = b.konst(w, 0);
    let z = b.cmp(CmpOp::Eq, result, zero);
    b.set(N, n);
    b.set(Z, z);
    b.set(C, carry);
    b.set(V, overflow);
}

/// Whether condition code `cond` holds for the entry flags, as one bit.
fn condition_holds(b: &mut Builder, cond: u32) -> Value {
    let base = match cond >> 1 {
        0 => b.get(Z),
        1 => b.get(C),
        2 => b.get(N),
        3 => b.get(V),
        4 => {
            let c = b.get(C);
            let z = b.get(Z);
            let nz = b.not(z);
            b.and(c, nz)
        }
        5 => {
            let n = b.get(N);
            let v = b.get(V);
            b.cmp(CmpOp::Eq, n, v)
        }
        6 => {
            let n = b.get(N);
            let v = b.get(V);
            let ge = b.cmp(CmpOp::Eq, n, v);
            let z = b.get(Z);
            let nz = b.not(z);
            b.and(ge, nz)
        }
        _ => b.konst(Width::W1, 1),
    };
    if cond & 1 == 1 && cond != 0b1111 {
        b.not(base)
    } else {
        base
    }
}

/// Sign-extends the low `bits` bits of `v` to an `i64`.
fn sext_imm(v: u32, bits: u32) -> i64 {
    let s = 64 - bits;
    ((u64::from(v) << s) as i64) >> s
}

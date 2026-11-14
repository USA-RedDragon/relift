//! The SM83 register file the lifter declares to `relift-ir`.
//!
//! Slots: `a`, `b`, `c`, `d`, `e`, `h`, `l` (indexes 0 to 6, eight bits), `sp`
//! (7, sixteen bits), then the flags `fz`, `fn`, `fh`, `fc` (8 to 11) as
//! one-bit slots. The flag byte `f` has no slot: its low nibble is always
//! zero, so `f` is built from the four flags where an instruction reads it.

use relift_ir::{Reg, RegDecl, RegFile, Width};

use crate::Reg8;

/// Accumulator.
pub const A: Reg = Reg(0);
/// `b`.
pub const B: Reg = Reg(1);
/// `c`.
pub const C: Reg = Reg(2);
/// `d`.
pub const D: Reg = Reg(3);
/// `e`.
pub const E: Reg = Reg(4);
/// `h`.
pub const H: Reg = Reg(5);
/// `l`.
pub const L: Reg = Reg(6);
/// Stack pointer.
pub const SP: Reg = Reg(7);
/// Zero flag, bit 7 of `f`.
pub const FZ: Reg = Reg(8);
/// Subtract flag, bit 6 of `f`.
pub const FN: Reg = Reg(9);
/// Half-carry flag, bit 5 of `f`.
pub const FH: Reg = Reg(10);
/// Carry flag, bit 4 of `f`.
pub const FC: Reg = Reg(11);

const fn decl(name: &'static str, width: Width) -> RegDecl {
    RegDecl { name, width }
}

static DECLS: [RegDecl; 12] = [
    decl("a", Width::W8),
    decl("b", Width::W8),
    decl("c", Width::W8),
    decl("d", Width::W8),
    decl("e", Width::W8),
    decl("h", Width::W8),
    decl("l", Width::W8),
    decl("sp", Width::W16),
    decl("fz", Width::W1),
    decl("fn", Width::W1),
    decl("fh", Width::W1),
    decl("fc", Width::W1),
];

/// The register file.
pub fn regs() -> RegFile {
    RegFile::new("sm83", &DECLS)
}

/// The slot of an 8-bit register.
pub const fn reg8(r: Reg8) -> Reg {
    match r {
        Reg8::A => A,
        Reg8::B => B,
        Reg8::C => C,
        Reg8::D => D,
        Reg8::E => E,
        Reg8::H => H,
        Reg8::L => L,
    }
}

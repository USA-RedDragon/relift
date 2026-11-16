//! The Z80 register file the lifter declares to `relift-ir`: one slot per
//! register of `cpus/z80/sail/z80.sail`, in the order of the model.
//!
//! The eight flags of F are one-bit slots (bit 7 to bit 0 of F: S Z Y H X
//! P/V N C), so F is built only where an instruction reads it whole. The
//! shadow pairs are 16 bits: only `EX AF,AF'` and `EXX` touch them. `q`,
//! `ei` and `p` are the model's Q, EIDelay and LDAIR. `ret` is read only by
//! the interrupt entries: the address the interrupt returns to, which the
//! runtime stores before it runs an entry.

use relift_ir::{Reg, RegDecl, RegFile, Width};

const fn d(name: &'static str, width: Width) -> RegDecl {
    RegDecl { name, width }
}

/// The slots: name and width.
pub static DECLS: [RegDecl; 32] = [
    d("a", Width::W8),
    d("fs", Width::W1),
    d("fz", Width::W1),
    d("fy", Width::W1),
    d("fh", Width::W1),
    d("fx", Width::W1),
    d("fp", Width::W1),
    d("fn", Width::W1),
    d("fc", Width::W1),
    d("b", Width::W8),
    d("c", Width::W8),
    d("d", Width::W8),
    d("e", Width::W8),
    d("h", Width::W8),
    d("l", Width::W8),
    d("ix", Width::W16),
    d("iy", Width::W16),
    d("sp", Width::W16),
    d("wz", Width::W16),
    d("i", Width::W8),
    d("r", Width::W8),
    d("af_", Width::W16),
    d("bc_", Width::W16),
    d("de_", Width::W16),
    d("hl_", Width::W16),
    d("iff1", Width::W1),
    d("iff2", Width::W1),
    d("im", Width::new(2)),
    d("q", Width::W1),
    d("ei", Width::W1),
    d("p", Width::W1),
    d("ret", Width::W16),
];

/// Accumulator.
pub const A: Reg = Reg(0);
/// Sign flag, bit 7 of F.
pub const FS: Reg = Reg(1);
/// Zero flag, bit 6.
pub const FZ: Reg = Reg(2);
/// Undocumented flag Y, bit 5.
pub const FY: Reg = Reg(3);
/// Half-carry flag, bit 4.
pub const FH: Reg = Reg(4);
/// Undocumented flag X, bit 3.
pub const FX: Reg = Reg(5);
/// Parity/overflow flag, bit 2.
pub const FP: Reg = Reg(6);
/// Subtract flag, bit 1.
pub const FN: Reg = Reg(7);
/// Carry flag, bit 0.
pub const FC: Reg = Reg(8);
/// `b`.
pub const B: Reg = Reg(9);
/// `c`.
pub const C: Reg = Reg(10);
/// `d`.
pub const D: Reg = Reg(11);
/// `e`.
pub const E: Reg = Reg(12);
/// `h`.
pub const H: Reg = Reg(13);
/// `l`.
pub const L: Reg = Reg(14);
/// `ix`.
pub const IX: Reg = Reg(15);
/// `iy`.
pub const IY: Reg = Reg(16);
/// Stack pointer.
pub const SP: Reg = Reg(17);
/// MEMPTR.
pub const WZ: Reg = Reg(18);
/// Interrupt vector base.
pub const I: Reg = Reg(19);
/// Refresh counter.
pub const R: Reg = Reg(20);
/// `af'`.
pub const AF_: Reg = Reg(21);
/// `bc'`.
pub const BC_: Reg = Reg(22);
/// `de'`.
pub const DE_: Reg = Reg(23);
/// `hl'`.
pub const HL_: Reg = Reg(24);
/// Interrupt enable flip-flop 1.
pub const IFF1: Reg = Reg(25);
/// Interrupt enable flip-flop 2.
pub const IFF2: Reg = Reg(26);
/// Interrupt mode, 0 to 2.
pub const IM: Reg = Reg(27);
/// The previous instruction set the flags.
pub const Q: Reg = Reg(28);
/// The previous instruction was `EI`: no maskable interrupt is taken
/// after it.
pub const EIDELAY: Reg = Reg(29);
/// The previous instruction was `LD A,I` or `LD A,R`: an interrupt taken
/// after it clears P/V.
pub const LDAIR: Reg = Reg(30);
/// The address an interrupt entry returns to.
pub const PC: Reg = Reg(31);

/// The register file.
pub fn regs() -> RegFile {
    RegFile::new("z80", &DECLS)
}

/// The slots of F's bits, bit 7 first.
pub const F_BITS: [Reg; 8] = [FS, FZ, FY, FH, FX, FP, FN, FC];

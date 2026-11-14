//! The A64 register file the lifter declares to `relift-ir`.
//!
//! Slots: `x0` to `x30` (indexes 0 to 30), `sp` (31), the flags `n`, `z`,
//! `c`, `v` as one-bit slots (32 to 35), `v0` to `v31` as 128-bit slots (36 to
//! 67), then `fpcr`, `fpsr`, `tpidr_el0` and `tpidrro_el0`.

use relift_ir::{Reg, RegDecl, RegFile, Width};

const fn decl(name: &'static str, width: Width) -> RegDecl {
    RegDecl { name, width }
}

static DECLS: [RegDecl; 73] = [
    decl("x0", Width::W64),
    decl("x1", Width::W64),
    decl("x2", Width::W64),
    decl("x3", Width::W64),
    decl("x4", Width::W64),
    decl("x5", Width::W64),
    decl("x6", Width::W64),
    decl("x7", Width::W64),
    decl("x8", Width::W64),
    decl("x9", Width::W64),
    decl("x10", Width::W64),
    decl("x11", Width::W64),
    decl("x12", Width::W64),
    decl("x13", Width::W64),
    decl("x14", Width::W64),
    decl("x15", Width::W64),
    decl("x16", Width::W64),
    decl("x17", Width::W64),
    decl("x18", Width::W64),
    decl("x19", Width::W64),
    decl("x20", Width::W64),
    decl("x21", Width::W64),
    decl("x22", Width::W64),
    decl("x23", Width::W64),
    decl("x24", Width::W64),
    decl("x25", Width::W64),
    decl("x26", Width::W64),
    decl("x27", Width::W64),
    decl("x28", Width::W64),
    decl("x29", Width::W64),
    decl("x30", Width::W64),
    decl("sp", Width::W64),
    decl("n", Width::W1),
    decl("z", Width::W1),
    decl("c", Width::W1),
    decl("v", Width::W1),
    decl("v0", Width::W128),
    decl("v1", Width::W128),
    decl("v2", Width::W128),
    decl("v3", Width::W128),
    decl("v4", Width::W128),
    decl("v5", Width::W128),
    decl("v6", Width::W128),
    decl("v7", Width::W128),
    decl("v8", Width::W128),
    decl("v9", Width::W128),
    decl("v10", Width::W128),
    decl("v11", Width::W128),
    decl("v12", Width::W128),
    decl("v13", Width::W128),
    decl("v14", Width::W128),
    decl("v15", Width::W128),
    decl("v16", Width::W128),
    decl("v17", Width::W128),
    decl("v18", Width::W128),
    decl("v19", Width::W128),
    decl("v20", Width::W128),
    decl("v21", Width::W128),
    decl("v22", Width::W128),
    decl("v23", Width::W128),
    decl("v24", Width::W128),
    decl("v25", Width::W128),
    decl("v26", Width::W128),
    decl("v27", Width::W128),
    decl("v28", Width::W128),
    decl("v29", Width::W128),
    decl("v30", Width::W128),
    decl("v31", Width::W128),
    decl("fpcr", Width::W32),
    decl("fpsr", Width::W32),
    decl("tpidr_el0", Width::W64),
    decl("tpidrro_el0", Width::W64),
    decl("fp_flags", Width::W1),
];

/// The A64 register file.
pub fn regs() -> RegFile {
    RegFile::new("a64", &DECLS)
}

/// General register `x<n>` for `n` in 0 to 30.
pub fn x(n: u32) -> Reg {
    assert!(n < 31, "x{n} is not a general register");
    Reg(n as u16)
}

/// The stack pointer.
pub const SP: Reg = Reg(31);
/// The negative flag.
pub const N: Reg = Reg(32);
/// The zero flag.
pub const Z: Reg = Reg(33);
/// The carry flag.
pub const C: Reg = Reg(34);
/// The overflow flag.
pub const V: Reg = Reg(35);

/// SIMD and floating-point register `v<n>` for `n` in 0 to 31.
pub fn vreg(n: u32) -> Reg {
    assert!(n < 32, "v{n} is not a vector register");
    Reg(36 + n as u16)
}

/// The floating-point control register.
pub const FPCR: Reg = Reg(68);
/// The floating-point status register.
pub const FPSR: Reg = Reg(69);
/// The read-write thread pointer.
pub const TPIDR_EL0: Reg = Reg(70);
/// The read-only thread pointer.
pub const TPIDRRO_EL0: Reg = Reg(71);
/// Not guest state: 1 in a program that produces FPSR flags (the build's
/// FPSR census found a read that can be used), else 0. Read-only; blocks
/// whose flags the host cannot give leave for the slow path while it is 1.
pub const FP_FLAGS: Reg = Reg(72);

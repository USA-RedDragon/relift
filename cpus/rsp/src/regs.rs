//! The RSP register file, with the overlay's register names. Slots 0 to 31
//! are the scalar registers `r0` to `r31` (32 bits; `r0` reads 0 and is
//! never written), 32 to 63 the vector registers `v0` to `v31` (128 bits),
//! then the accumulator slices `ACCH`, `ACCM` and `ACCL`, the flag masks
//! `VCOL`, `VCOH`, `VCCL`, `VCCH` and `VCE`, and the divider state `DIVIN`,
//! `DIVOUT` and `DIVDP`. The PC is not a slot: a block's exit gives it.
//!
//! A vector value holds element 0 (the RSP's first, most significant
//! element) in its top 16 bits, so the RSP's big-endian element `n` is IR
//! lane `7 - n` (`relift_ir::lane`), and byte `i` of the register as the RSP
//! numbers it is IR byte `15 - i`. The accumulator slices hold one 16-bit
//! lane per element the same way. A flag mask holds all ones in an
//! element's lane when its flag is set and zeros otherwise.

use relift_ir::{Reg, RegDecl, RegFile, Width};

const fn decl(name: &'static str, width: Width) -> RegDecl {
    RegDecl { name, width }
}

static DECLS: [RegDecl; 75] = [
    decl("r0", Width::W32),
    decl("r1", Width::W32),
    decl("r2", Width::W32),
    decl("r3", Width::W32),
    decl("r4", Width::W32),
    decl("r5", Width::W32),
    decl("r6", Width::W32),
    decl("r7", Width::W32),
    decl("r8", Width::W32),
    decl("r9", Width::W32),
    decl("r10", Width::W32),
    decl("r11", Width::W32),
    decl("r12", Width::W32),
    decl("r13", Width::W32),
    decl("r14", Width::W32),
    decl("r15", Width::W32),
    decl("r16", Width::W32),
    decl("r17", Width::W32),
    decl("r18", Width::W32),
    decl("r19", Width::W32),
    decl("r20", Width::W32),
    decl("r21", Width::W32),
    decl("r22", Width::W32),
    decl("r23", Width::W32),
    decl("r24", Width::W32),
    decl("r25", Width::W32),
    decl("r26", Width::W32),
    decl("r27", Width::W32),
    decl("r28", Width::W32),
    decl("r29", Width::W32),
    decl("r30", Width::W32),
    decl("r31", Width::W32),
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
    decl("ACCH", Width::W128),
    decl("ACCM", Width::W128),
    decl("ACCL", Width::W128),
    decl("VCOL", Width::W128),
    decl("VCOH", Width::W128),
    decl("VCCL", Width::W128),
    decl("VCCH", Width::W128),
    decl("VCE", Width::W128),
    decl("DIVIN", Width::W16),
    decl("DIVOUT", Width::W16),
    decl("DIVDP", Width::W1),
];

/// The register file.
pub fn regs() -> RegFile {
    RegFile::new("rsp", &DECLS)
}

/// Scalar register `n` (0 to 31).
pub fn gpr(n: u8) -> Reg {
    assert!(n < 32, "no scalar register {n}");
    Reg(u16::from(n))
}

/// Vector register `n` (0 to 31).
pub fn vpr(n: u8) -> Reg {
    assert!(n < 32, "no vector register {n}");
    Reg(32 + u16::from(n))
}

/// The slot of the model register `name`, if the file has one.
pub fn slot(name: &str) -> Option<Reg> {
    DECLS
        .iter()
        .position(|d| d.name == name)
        .map(|i| Reg(i as u16))
}

/// `ACCH`.
pub const ACCH: Reg = Reg(64);
/// `ACCM`.
pub const ACCM: Reg = Reg(65);
/// `ACCL`.
pub const ACCL: Reg = Reg(66);
/// `VCOL`.
pub const VCOL: Reg = Reg(67);
/// `VCOH`.
pub const VCOH: Reg = Reg(68);
/// `VCCL`.
pub const VCCL: Reg = Reg(69);
/// `VCCH`.
pub const VCCH: Reg = Reg(70);
/// `VCE`.
pub const VCE: Reg = Reg(71);
/// `DIVIN`.
pub const DIVIN: Reg = Reg(72);
/// `DIVOUT`.
pub const DIVOUT: Reg = Reg(73);
/// `DIVDP`.
pub const DIVDP: Reg = Reg(74);

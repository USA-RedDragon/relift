//! The R3000A register file, with the model's register names: slots 0 to
//! 31 are the general registers `r0` to `r31` (`r0` reads 0 and is never
//! written), then `HI` and `LO`, all 32 bits. The model's registers are 64
//! bits; `St` sign-extends a read and keeps the low word of a write. The
//! PC is not a slot: a block's exit gives it.

use relift_ir::{Reg, RegDecl, RegFile, Width};

const fn decl(name: &'static str, width: Width) -> RegDecl {
    RegDecl { name, width }
}

static DECLS: [RegDecl; 34] = [
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
    decl("HI", Width::W32),
    decl("LO", Width::W32),
];

/// The register file.
pub fn regs() -> RegFile {
    RegFile::new("r3000a", &DECLS)
}

/// General register `n` (0 to 31).
pub fn gpr(n: u8) -> Reg {
    assert!(n < 32, "no general register {n}");
    Reg(u16::from(n))
}

/// The slot of the model register `name`, if the file has one.
pub fn slot(name: &str) -> Option<Reg> {
    DECLS
        .iter()
        .position(|d| d.name == name)
        .map(|i| Reg(i as u16))
}

/// `HI`.
pub const HI: Reg = Reg(32);
/// `LO`.
pub const LO: Reg = Reg(33);

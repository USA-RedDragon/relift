//! The VR4300 register file, with the model's register names. Slots 0 to
//! 31 are the general registers `r0` to `r31` (`r0` reads 0 and is never
//! written), then `HI` and `LO`, the FPU registers `f0` to `f31` (64 bits,
//! the FR=1 view; the overlay derives the FR=0 views), FCR31, and the CP0
//! registers the overlay keeps. `CP0Latch` is the last value written to any
//! CP0 register, which reads of unimplemented numbers return, and
//! `CP0LLBit` is the LL/SC link bit. The PC is not a slot: a block's exit
//! gives it.

use relift_ir::{Reg, RegDecl, RegFile, Width};

const fn decl(name: &'static str, width: Width) -> RegDecl {
    RegDecl { name, width }
}

static DECLS: [RegDecl; 90] = [
    decl("r0", Width::W64),
    decl("r1", Width::W64),
    decl("r2", Width::W64),
    decl("r3", Width::W64),
    decl("r4", Width::W64),
    decl("r5", Width::W64),
    decl("r6", Width::W64),
    decl("r7", Width::W64),
    decl("r8", Width::W64),
    decl("r9", Width::W64),
    decl("r10", Width::W64),
    decl("r11", Width::W64),
    decl("r12", Width::W64),
    decl("r13", Width::W64),
    decl("r14", Width::W64),
    decl("r15", Width::W64),
    decl("r16", Width::W64),
    decl("r17", Width::W64),
    decl("r18", Width::W64),
    decl("r19", Width::W64),
    decl("r20", Width::W64),
    decl("r21", Width::W64),
    decl("r22", Width::W64),
    decl("r23", Width::W64),
    decl("r24", Width::W64),
    decl("r25", Width::W64),
    decl("r26", Width::W64),
    decl("r27", Width::W64),
    decl("r28", Width::W64),
    decl("r29", Width::W64),
    decl("r30", Width::W64),
    decl("r31", Width::W64),
    decl("HI", Width::W64),
    decl("LO", Width::W64),
    decl("f0", Width::W64),
    decl("f1", Width::W64),
    decl("f2", Width::W64),
    decl("f3", Width::W64),
    decl("f4", Width::W64),
    decl("f5", Width::W64),
    decl("f6", Width::W64),
    decl("f7", Width::W64),
    decl("f8", Width::W64),
    decl("f9", Width::W64),
    decl("f10", Width::W64),
    decl("f11", Width::W64),
    decl("f12", Width::W64),
    decl("f13", Width::W64),
    decl("f14", Width::W64),
    decl("f15", Width::W64),
    decl("f16", Width::W64),
    decl("f17", Width::W64),
    decl("f18", Width::W64),
    decl("f19", Width::W64),
    decl("f20", Width::W64),
    decl("f21", Width::W64),
    decl("f22", Width::W64),
    decl("f23", Width::W64),
    decl("f24", Width::W64),
    decl("f25", Width::W64),
    decl("f26", Width::W64),
    decl("f27", Width::W64),
    decl("f28", Width::W64),
    decl("f29", Width::W64),
    decl("f30", Width::W64),
    decl("f31", Width::W64),
    decl("FCR31", Width::W32),
    decl("CP0Index", Width::W64),
    decl("CP0EntryLo0", Width::W64),
    decl("CP0EntryLo1", Width::W64),
    decl("CP0Context", Width::W64),
    decl("CP0PageMask", Width::W64),
    decl("CP0Wired", Width::W64),
    decl("CP0BadVAddr", Width::W64),
    decl("CP0Count", Width::W32),
    decl("CP0EntryHi", Width::W64),
    decl("CP0Compare", Width::W32),
    decl("CP0Status", Width::W32),
    decl("CP0Cause", Width::W32),
    decl("CP0EPC", Width::W64),
    decl("CP0Config", Width::W64),
    decl("CP0LLAddr", Width::W64),
    decl("CP0WatchLo", Width::W64),
    decl("CP0WatchHi", Width::W64),
    decl("CP0XContext", Width::W64),
    decl("CP0ParityErr", Width::W64),
    decl("CP0TagLo", Width::W64),
    decl("CP0ErrorEPC", Width::W64),
    decl("CP0Latch", Width::W64),
    decl("CP0LLBit", Width::W1),
];

/// The register file.
pub fn regs() -> RegFile {
    RegFile::new("vr4300", &DECLS)
}

/// General register `n` (0 to 31).
pub fn gpr(n: u8) -> Reg {
    assert!(n < 32, "no general register {n}");
    Reg(u16::from(n))
}

/// FPU register `n` (0 to 31).
pub fn fpr(n: u8) -> Reg {
    assert!(n < 32, "no FPU register {n}");
    Reg(34 + u16::from(n))
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
/// `FCR31`.
pub const FCR31: Reg = Reg(66);
/// `CP0Index`.
pub const CP0_INDEX: Reg = Reg(67);
/// `CP0EntryLo0`.
pub const CP0_ENTRY_LO0: Reg = Reg(68);
/// `CP0EntryLo1`.
pub const CP0_ENTRY_LO1: Reg = Reg(69);
/// `CP0Context`.
pub const CP0_CONTEXT: Reg = Reg(70);
/// `CP0PageMask`.
pub const CP0_PAGE_MASK: Reg = Reg(71);
/// `CP0Wired`.
pub const CP0_WIRED: Reg = Reg(72);
/// `CP0BadVAddr`.
pub const CP0_BAD_VADDR: Reg = Reg(73);
/// `CP0Count`.
pub const CP0_COUNT: Reg = Reg(74);
/// `CP0EntryHi`.
pub const CP0_ENTRY_HI: Reg = Reg(75);
/// `CP0Compare`.
pub const CP0_COMPARE: Reg = Reg(76);
/// `CP0Status`.
pub const CP0_STATUS: Reg = Reg(77);
/// `CP0Cause`.
pub const CP0_CAUSE: Reg = Reg(78);
/// `CP0EPC`.
pub const CP0_EPC: Reg = Reg(79);
/// `CP0Config`.
pub const CP0_CONFIG: Reg = Reg(80);
/// `CP0LLAddr`.
pub const CP0_LLADDR: Reg = Reg(81);
/// `CP0WatchLo`.
pub const CP0_WATCH_LO: Reg = Reg(82);
/// `CP0WatchHi`.
pub const CP0_WATCH_HI: Reg = Reg(83);
/// `CP0XContext`.
pub const CP0_XCONTEXT: Reg = Reg(84);
/// `CP0ParityErr`.
pub const CP0_PARITY_ERR: Reg = Reg(85);
/// `CP0TagLo`.
pub const CP0_TAG_LO: Reg = Reg(86);
/// `CP0ErrorEPC`.
pub const CP0_ERROR_EPC: Reg = Reg(87);
/// `CP0Latch`.
pub const CP0_LATCH: Reg = Reg(88);
/// `CP0LLBit`.
pub const CP0_LLBIT: Reg = Reg(89);

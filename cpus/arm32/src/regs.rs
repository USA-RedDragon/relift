//! The ARMv4T register file. Slots 0 to 14 are `r0` to `r14` as the
//! current mode sees them; the flags `n`, `z`, `c`, `v` and the Thumb bit
//! `t` follow, then the rest of the CPSR (`m`, the mode bits; `i` and `f`,
//! the interrupt masks; `psr_rsvd`, bits 27 to 8 as the last write left
//! them), the five SPSRs, and the banked copies of `r8` to `r14` that the
//! current mode does not see. A copy is valid exactly while its register is
//! not in the current view: a mode change saves the view into the old
//! mode's copies and loads it from the new mode's (see `lift::rt`). The PC
//! is not a slot: a block's exit gives it.

use relift_ir::{Reg, RegDecl, RegFile, Width};

const fn decl(name: &'static str, width: Width) -> RegDecl {
    RegDecl { name, width }
}

static DECLS: [RegDecl; 51] = [
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
    decl("n", Width::W1),
    decl("z", Width::W1),
    decl("c", Width::W1),
    decl("v", Width::W1),
    decl("t", Width::W1),
    decl("m", Width::W8),
    decl("i", Width::W1),
    decl("f", Width::W1),
    decl("psr_rsvd", Width::W32),
    decl("spsr_fiq", Width::W32),
    decl("spsr_irq", Width::W32),
    decl("spsr_svc", Width::W32),
    decl("spsr_abt", Width::W32),
    decl("spsr_und", Width::W32),
    decl("r8_usr", Width::W32),
    decl("r9_usr", Width::W32),
    decl("r10_usr", Width::W32),
    decl("r11_usr", Width::W32),
    decl("r12_usr", Width::W32),
    decl("r13_usr", Width::W32),
    decl("r14_usr", Width::W32),
    decl("r8_fiq", Width::W32),
    decl("r9_fiq", Width::W32),
    decl("r10_fiq", Width::W32),
    decl("r11_fiq", Width::W32),
    decl("r12_fiq", Width::W32),
    decl("r13_fiq", Width::W32),
    decl("r14_fiq", Width::W32),
    decl("r13_irq", Width::W32),
    decl("r14_irq", Width::W32),
    decl("r13_svc", Width::W32),
    decl("r14_svc", Width::W32),
    decl("r13_abt", Width::W32),
    decl("r14_abt", Width::W32),
    decl("r13_und", Width::W32),
    decl("r14_und", Width::W32),
];

/// The register file.
pub fn regs() -> RegFile {
    RegFile::new("armv4t", &DECLS)
}

/// General register `n` (0 to 14) of the current mode.
pub fn r(n: u32) -> Reg {
    assert!(n < 15, "r15 is not a slot");
    Reg(n as u16)
}

/// The negative flag.
pub const N: Reg = Reg(15);
/// The zero flag.
pub const Z: Reg = Reg(16);
/// The carry flag.
pub const C: Reg = Reg(17);
/// The overflow flag.
pub const V: Reg = Reg(18);
/// The Thumb state bit.
pub const T: Reg = Reg(19);
/// The mode bits, CPSR bits 4 to 0.
pub const M: Reg = Reg(20);
/// The IRQ mask, CPSR bit 7.
pub const I: Reg = Reg(21);
/// The FIQ mask, CPSR bit 6.
pub const F: Reg = Reg(22);
/// CPSR bits 27 to 8 (the other bits read as zero here).
pub const PSR_RSVD: Reg = Reg(23);

/// A processor mode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// User.
    Usr,
    /// FIQ.
    Fiq,
    /// IRQ.
    Irq,
    /// Supervisor.
    Svc,
    /// Abort.
    Abt,
    /// Undefined.
    Und,
    /// System.
    Sys,
}

impl Mode {
    /// Every mode.
    pub const ALL: [Mode; 7] = [
        Mode::Usr,
        Mode::Fiq,
        Mode::Irq,
        Mode::Svc,
        Mode::Abt,
        Mode::Und,
        Mode::Sys,
    ];

    /// The CPSR mode bits.
    pub const fn bits(self) -> u8 {
        match self {
            Mode::Usr => 0x10,
            Mode::Fiq => 0x11,
            Mode::Irq => 0x12,
            Mode::Svc => 0x13,
            Mode::Abt => 0x17,
            Mode::Und => 0x1b,
            Mode::Sys => 0x1f,
        }
    }

    /// The mode of CPSR mode bits `m`, if they name one.
    pub fn of(m: u32) -> Option<Mode> {
        Mode::ALL
            .into_iter()
            .find(|x| u32::from(x.bits()) == m & 0x1f)
    }

    /// The mode's SPSR slot; user and system mode have none.
    pub const fn spsr(self) -> Option<Reg> {
        match self {
            Mode::Usr | Mode::Sys => None,
            Mode::Fiq => Some(Reg(24)),
            Mode::Irq => Some(Reg(25)),
            Mode::Svc => Some(Reg(26)),
            Mode::Abt => Some(Reg(27)),
            Mode::Und => Some(Reg(28)),
        }
    }

    /// The bank register `n` (8 to 14) belongs to in this mode: the user
    /// bank, the FIQ bank, or for `r13` and `r14` the mode's own.
    pub const fn bank(self, n: u32) -> Bank {
        match (self, n) {
            (Mode::Fiq, _) => Bank::Fiq,
            (Mode::Usr | Mode::Sys, _) | (_, 8..=12) => Bank::Usr,
            (Mode::Irq, _) => Bank::Irq,
            (Mode::Svc, _) => Bank::Svc,
            (Mode::Abt, _) => Bank::Abt,
            (Mode::Und, _) => Bank::Und,
        }
    }
}

/// A register bank: where a banked register is kept while another mode
/// runs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Bank {
    /// User and system mode's `r8` to `r14`, and every other mode's `r8`
    /// to `r12` but FIQ's.
    Usr,
    /// FIQ's `r8` to `r14`.
    Fiq,
    /// IRQ's `r13` and `r14`.
    Irq,
    /// Supervisor's `r13` and `r14`.
    Svc,
    /// Abort's `r13` and `r14`.
    Abt,
    /// Undefined's `r13` and `r14`.
    Und,
}

impl Bank {
    /// Every bank.
    pub const ALL: [Bank; 6] = [
        Bank::Usr,
        Bank::Fiq,
        Bank::Irq,
        Bank::Svc,
        Bank::Abt,
        Bank::Und,
    ];

    /// The slot that keeps register `n` (8 to 14) of this bank, if the bank
    /// has that register.
    pub const fn slot(self, n: u32) -> Option<Reg> {
        let i = match (self, n) {
            (Bank::Usr, 8..=14) => 29 + n - 8,
            (Bank::Fiq, 8..=14) => 36 + n - 8,
            (Bank::Irq, 13 | 14) => 43 + n - 13,
            (Bank::Svc, 13 | 14) => 45 + n - 13,
            (Bank::Abt, 13 | 14) => 47 + n - 13,
            (Bank::Und, 13 | 14) => 49 + n - 13,
            _ => return None,
        };
        Some(Reg(i as u16))
    }
}

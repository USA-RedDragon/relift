//! The Game Boy oracle: rustyboi-core run as the reference SM83 machine.
//!
//! The interface is plain data (raw opcode bytes and a register struct) so the
//! comparison against lifted IR can plug in later without touching the
//! backend.
//!
//! * `step` runs one instruction from a given register file and memory image.
//! * `rom` runs a whole ROM on a given hardware revision.

pub mod cli;
mod lift;
mod machine;
mod rom;
mod step;
#[cfg(test)]
mod tests;

pub use machine::Hardware;
pub use rom::{RomSpec, Stop, run_rom};
pub use step::{Segment, Sequence, StepSpec, run_sequence, run_step};

/// The SM83 register file and the interrupt master enable.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Regs {
    pub a: u8,
    pub f: u8,
    pub b: u8,
    pub c: u8,
    pub d: u8,
    pub e: u8,
    pub h: u8,
    pub l: u8,
    pub sp: u16,
    pub pc: u16,
    pub ime: bool,
}

impl Regs {
    /// The flag byte as Z, N, H, C.
    pub fn flags(&self) -> [bool; 4] {
        [
            self.f & 0x80 != 0,
            self.f & 0x40 != 0,
            self.f & 0x20 != 0,
            self.f & 0x10 != 0,
        ]
    }
}

/// An oracle failure.
#[derive(Debug)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

pub(crate) fn err<T>(msg: impl Into<String>) -> Result<T, Error> {
    Err(Error(msg.into()))
}

//! Function discovery for Game Boy ROMs across mapper banks.
//!
//! A code location is a bank and an address: `$0000-$3FFF` is always bank 0
//! and `$4000-$7FFF` carries the bank mapped there. The analysis starts at
//! the entry point and the interrupt and restart vectors, follows jumps,
//! calls and restarts, and tracks constant writes to the mapper's bank
//! registers along each path to learn the bank at every transfer into the
//! switchable window. A routine that writes a bank register from an
//! argument register and then jumps to an address in `hl` is a bank-switch
//! trampoline; its call sites are resolved from the constants they load.
//! Transfers it cannot resolve are listed, never guessed. Jumps into RAM
//! are reported as code the ROM must have copied there.
//!
//! Computed transfers are followed only along a proven load or store path:
//!
//! - A jump through a pointer kept in RAM goes to every constant word stored
//!   to that cell anywhere in the program (a value-set per cell, joined over
//!   all paths), with the bank in effect at the jump, or the bank stored
//!   next to the pointer when the jump maps the bank from a cell.
//! - A jump through a word read from a ROM table at `base + index * step`
//!   takes every entry below the bound of the index (a compare, a mask, a
//!   constant) or, with no bound, the run of entries that point to code,
//!   ending at the first that does not; every target must decode to a
//!   return or known code without an unused opcode.
//! - A callee that jumps to or returns through an address its caller passes
//!   is run again inside each caller with the caller's values.
//! - A pure helper, such as a multiply, is evaluated for every value of an
//!   unknown index byte, so a table indexed through it keeps its bound.
//!
//! Discovery repeats until no new code, store or table appears.
//!
//! Only decode and control-flow facts are used; the analysis does not depend
//! on any lifted form of the instructions.

mod analysis;
mod bankmodel;
mod func;
mod fx;
mod image;
mod loc;
mod probe;
mod report;
mod state;

pub use analysis::{AnalyzeError, analyze, analyze_with};
pub use bankmodel::{BankModel, BankReg, OutOfRange};
pub use func::{AddressTaken, CallKind, CopyLoop, RamTransfer};
pub use loc::Loc;
pub use report::{
    BankCoverage, Edge, Function, JumpTable, Origin, RamCode, RamPointerJump, Report, TableBound,
    Trampoline, Unresolved, UnresolvedKind,
};

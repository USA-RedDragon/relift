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
//! Only decode and control-flow facts are used; the analysis does not depend
//! on any lifted form of the instructions.

mod analysis;
mod bankmodel;
mod func;
mod image;
mod loc;
mod report;
mod state;

pub use analysis::{AnalyzeError, analyze};
pub use bankmodel::{BankModel, BankReg, OutOfRange};
pub use func::{AddressTaken, CallKind, CopyLoop, RamTransfer};
pub use loc::Loc;
pub use report::{
    BankCoverage, Edge, Function, Origin, RamCode, Report, Trampoline, Unresolved, UnresolvedKind,
};

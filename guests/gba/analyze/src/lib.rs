//! Function discovery for Game Boy Advance ROMs across ARM and Thumb code.
//!
//! A code location ([`Loc`]) is an address, the CPU state (ARM or Thumb)
//! and, for code in work RAM, the copy whose bytes are there. The analysis
//! starts at the reset entry and at every handler stored to the IRQ vector
//! (`0x03007FFC`), lifts each instruction with `relift-cpu-arm32` and runs
//! the lifted IR over abstract values: constants, partly known bits, an
//! entry register plus an offset, a word loaded from a constant RAM
//! address, a word of the caller's frame, a table index (`base + stride *
//! i`) and a word read from a table. Every rule below follows from the
//! instruction semantics in the IR; nothing is taken from a byte pattern.
//!
//! - State: the state of an instruction is the Thumb bit the IR leaves.
//!   `BX` with a known register interworks by bit 0, `B`, `BL`, `MOV PC`,
//!   `LDR PC` and `POP {PC}` keep the state (ARMv4T), a Thumb `BL` pair is
//!   the two halves the lifter links through `LR`.
//! - Calls: `BL`, and any jump made with `LR` holding the address after it
//!   (`MOV LR,PC` then `BX` or `LDR PC`). The code after a call is reached
//!   only when the callee has a path back to its entry `LR`. A callee that
//!   leaves through a word its caller pushed (a `BL` to a shared epilogue)
//!   ends the caller's frame instead. A callee that jumps to an address in
//!   an argument register (a `BX` veneer) is resolved with the caller's
//!   value.
//! - Data: words read at constant ROM addresses (literal pools, tables)
//!   are data and never decoded; [`Report::conflicts`] lists instructions
//!   that a read also covers. Code in RAM reads its own copy as constants
//!   only in literal (PC-relative) loads: the rest of a copy may be
//!   variables the program changes.
//! - Jump tables: a computed jump after a compare with an immediate and a
//!   conditional branch on it is run for every index the check lets
//!   through. A jump through a word of a ROM table with a constant base
//!   and stride follows the entries from the base while each is a code
//!   address. A value not known times a constant is an index with that
//!   stride (a table of structures).
//! - Pointers: a constant word stored to a constant RAM address and later
//!   jumped through or called gives the target; so do the words of a copy
//!   in RAM. An odd word in the image that discovered code reads as a
//!   constant, or the entries of a ROM table of such words that code reads
//!   and stores, is a Thumb function entry once its own run lifts without
//!   an undefined instruction and overlaps no data and no other decoding
//!   ([`Origin::AddressTaken`]).
//! - Code in RAM: a `CpuSet` or `CpuFastSet` call, an immediate DMA with
//!   constant source, destination and count, or a call whose arguments
//!   are a ROM and a RAM address and which runs to its return on constants
//!   alone (a copy loop, a decompressor) makes a [`RamCopy`]; code reached
//!   in its destination is lifted from those bytes at its run address.
//!
//! Function runs are repeated until the summaries (returns, registers
//! kept, argument effects), the entries and the RAM facts stop changing.
//! A transfer whose target or state stays unknown is listed in
//! [`Report::unresolved`], never guessed. A store to an unknown address is
//! taken not to overwrite the stack frame of the function being run.

mod analysis;
mod conc;
mod eval;
mod func;
mod image;
mod loc;
mod report;
mod val;

pub use analysis::{AnalyzeError, RESET, analyze};
pub use loc::{Loc, Region};
pub use report::{
    CopyKind, Edge, EdgeKind, Function, Origin, RamCopy, RegionCoverage, Report, Swi, Table,
    Unresolved, UnresolvedKind,
};

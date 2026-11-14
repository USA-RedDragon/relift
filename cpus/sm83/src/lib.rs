//! The SM83 (Game Boy and Game Boy Color CPU) decoder and opcode table.
//!
//! Every one of the 256 base opcodes and 256 CB-prefixed opcodes has one
//! [`Opcode`] entry in [`BASE`] and [`CB`]: mnemonic, operand kinds, length in
//! bytes, cost in M-cycles (taken and not taken for conditionals), the flags
//! it reads and writes, and the bus accesses it makes with the M-cycle of
//! each. The 11 unused base opcodes are marked [`Mnemonic::Illegal`].
//!
//! Timing convention: cycle 1 of every instruction is its first opcode fetch,
//! so a NOP costs 1. On hardware the fetch of the next opcode overlaps the
//! last cycle of the current instruction; cycle counts here include the
//! fetch, so the relative order of the data accesses is unchanged. A CB entry
//! includes the `0xCB` prefix: its prefix fetch is cycle 1, its own opcode
//! fetch is cycle 2, and its length is 2. Accesses are numbered in the order
//! the CPU issues them, per Pan Docs and the Game Boy Complete Technical
//! Reference.
//!
//! The crate has no dependency on the IR or any guest. It only describes the
//! CPU.

mod decode;
mod table;
mod types;

pub use decode::{DecodeError, Immediate, Instruction, decode};
pub use table::{BASE, CB};
pub use types::{
    AccessKind, AccessTarget, Accesses, Cond, Cycles, FlagEffect, FlagSet, Flags, MemAccess,
    Mnemonic, Opcode, Operand, Prefix, Reg8, Reg16,
};

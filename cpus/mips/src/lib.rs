//! MIPS frontends: decoding of MIPS words to the encodings of the
//! sail-cheri-mips model kept on one CPU, and lifting to `relift-ir` with
//! templates generated from sail-cheri-mips plus that CPU's overlay (see
//! `gen/README.md`). Each CPU is a profile with its own module: its decode
//! table, register file, templates and lift-time state.
//!
//! - [`vr4300`]: the Nintendo 64 CPU (MIPS III, `gen/vr4300.sail`). Its
//!   modules are also at the crate root ([`decode`], [`lift`], [`regs`]).
//! - [`r3000a`]: the PlayStation CPU (MIPS I, `gen/r3000a.sail`), a stub
//!   with the `int` class only.
//!
//! A lifted instruction reads and writes its profile's register file. A
//! branch or jump is lifted together with its delay slot into one block;
//! exceptions and the slow paths leave the block through guards before any
//! effect (see [`lift::rt`]).

mod common;
pub mod r3000a;
pub mod vr4300;

pub use vr4300::{decode, lift, regs};

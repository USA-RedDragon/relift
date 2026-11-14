//! The differential check: blocks emitted to WebAssembly and run in wasmtime
//! against `relift_ir::interp`, the definition of every operation.
//!
//! - `ops`: random blocks over every IR operation and width, one block per
//!   step ([`relift_emit_wasm::Exits::Step`]), plus the mutation control;
//!   and blocks of only the atomic, cipher and binary16 operations, with a
//!   mutation control that breaks those operations and a check that every
//!   atomic update, width, cipher step and binary16 shape was generated.
//! - `hw`: the hardware-register operations (ticks, I/O, sync points) over a
//!   Game Boy-like map, in step mode, plus the mutation control.
//! - `chain`: random programs of several functions in the real chained
//!   mode, with calls, tail calls, indirect transfers, traps, a second
//!   module joining the table, and interrupts at sync points, with tail calls
//!   on and off.
//!
//! Each runs a sample sized for CI; `RELIFT_WASM_CHECK=full` runs the full
//! sizes and `RELIFT_WASM_SEED` picks another seed.

mod chain;
mod errors;
mod hw;
mod mutate;
mod ops;
mod support;

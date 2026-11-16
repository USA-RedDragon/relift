//! The Z80 checker: `relift-cpu-z80` lifts each instruction with the
//! templates generated from our Sail model, the IR interpreter runs it, and
//! the result is compared with an oracle.
//!
//! * `--oracle sst-z80 run DIR` runs SingleStepTests/z80 v1 (`sst.rs`).
//! * `--oracle zex FILE.com` runs ZEXDOC or ZEXALL with a CP/M stub
//!   (`zex.rs`).
//! * `--oracle ares-z80` compares random states, and the interrupt entries,
//!   with the Z80 core of ares, a separate process (`ares.rs`,
//!   `check/oracle/ares`).
//!
//! Every instruction is lifted when it is reached, so nothing is interpreted
//! outside the templates; memory is a flat 64 KiB behind the device
//! interface, which lets the checker log each access with its T-state.

mod ares;
pub mod cli;
mod cpu;
mod json;
mod mutate;
mod sst;
#[cfg(test)]
mod tests;
mod zex;

//! The Linux guest: a statically linked aarch64 ELF program over
//! `relift-objfile` and the ARM64 frontend, for the runtime's Linux layer
//! (`runtime/guests/linux`).
//!
//! The program's code is lifted into one unit; its image and the facts the
//! process start needs (segments, entry, program headers, the stack the
//! file asks for) go to `program.c`, which the runtime loads.

pub mod cpu;
pub mod lift;
pub mod program;

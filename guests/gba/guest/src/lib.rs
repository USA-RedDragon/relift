//! The Game Boy Advance guest: the ARMv4T frontend over
//! `relift-cpu-arm32` for the runtime's GBA layer (`runtime/guests/gba/run`),
//! and a ROM, analyzed by `relift-gba-analyze`, lifted into one unit with the
//! program files that run it.

pub mod frontend;
pub mod program;

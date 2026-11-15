//! The Game Boy Advance guest: the ARMv4T frontend over
//! `relift-cpu-arm32` for the runtime's GBA layer (`runtime/guests/gba/run`),
//! the discovery of a ROM's code and its lifting into one unit with the
//! program files that run it.

pub mod discover;
pub mod frontend;
pub mod program;

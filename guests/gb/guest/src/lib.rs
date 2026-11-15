//! The Game Boy guest: the exact-mode SM83 frontend over
//! `relift-cpu-sm83`, and the lifting of a ROM, analyzed by
//! `relift-gb-analyze`, into one unit per bank with the program files that
//! run it on the runtime's Game Boy layer (`runtime/guests/gb`).

pub mod frontend;
pub mod program;

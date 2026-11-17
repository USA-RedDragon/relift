//! The Master System and Game Gear guest: the Z80 frontend for the
//! runtime's machine (`runtime/guests/sms/run`), and a ROM's code, as a set
//! of instructions by location ([`code::Code`]), lifted into one unit per
//! ROM bank and slot and one per group of code in work RAM, with the
//! program files that run it.

pub mod code;
pub mod frontend;
pub mod program;

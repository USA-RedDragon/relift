//! The Z80 (NMOS) frontend: the decoder that picks an instruction's template
//! and the lifter to `relift-ir` over the templates generated from our own
//! Sail model `cpus/z80/sail/z80.sail` (`cpus/z80/gen`).
//!
//! The templates cover every opcode of every page (base, CB, ED, DD, FD,
//! DDCB, FDCB), the four interrupt entries and the lone prefix, with the
//! undocumented flags X and Y, MEMPTR (WZ), Q, R, and the T-state of every
//! bus access.

mod decode;
mod frontend;
pub mod lift;
pub mod regs;

pub use decode::{DecodeError, Group, decode, opcode_mask};
pub use frontend::Z80;
pub use lift::{
    ENTRY, Entry, HALF, HALT, IFF, LiftError, Lifted, Options, PORT_IN, PORT_OUT, SyncPolicy,
    lift_bytes,
};

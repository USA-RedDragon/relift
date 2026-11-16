//! The R3000A profile (the PlayStation CPU): MIPS I with 32-bit general
//! registers, load delay slots, no FPU and no TLB.
//!
//! A stub that proves the profile seam: the decode table covers MIPS I and
//! only the `int` class has templates (generated from sail-cheri-mips plus
//! `gen/r3000a.sail`). The design of the rest (load delay slots, COP0, the
//! GTE, segments, timing) is in `cpus/mips/README.md`.

pub mod decode;
pub mod lift;
pub mod regs;

//! VR4300 (the Nintendo 64 CPU) frontend: decoding of MIPS III words to the
//! encodings of the sail-cheri-mips model kept on the VR4300, and lifting to
//! `relift-ir` with templates generated from sail-cheri-mips plus the
//! VR4300 overlay (`gen/vr4300.sail`, see `gen/README.md`).
//!
//! A lifted instruction reads and writes the register file of [`regs`]. A
//! branch or jump is lifted together with its delay slot into one block;
//! exceptions and the FPU's slow path leave the block through guards before
//! any effect (see [`lift::rt`]).

pub mod decode;
pub mod lift;
pub mod regs;

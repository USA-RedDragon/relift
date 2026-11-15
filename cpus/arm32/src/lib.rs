//! ARMv4T (ARM7TDMI, the Game Boy Advance CPU) frontend: decoding of A32
//! and Thumb words to the encodings of the sail-arm AArch32 model allowed on
//! ARMv4T, and lifting to `relift-ir` with templates generated from sail-arm
//! plus the ARMv4T overlay (`gen/armv4t.sail`, see `gen/README.md`).
//!
//! A lifted instruction reads and writes the register file of [`regs`]: the
//! current mode's registers, the CPSR in parts, the SPSRs and the banked
//! registers, so a mode change is IR like any other write. Its block ends
//! with one `Tick` of the instruction's bus events at one cycle each
//! (waitstates are the guest's), and an A32 condition that fails leaves
//! through the [`lift::COND_FAIL`] guard before any effect.

pub mod decode;
pub mod lift;
pub mod regs;

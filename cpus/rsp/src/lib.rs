//! RSP (the Nintendo 64 Reality Signal Processor) frontend: decoding of its
//! words to the encodings of sail-cheri-mips's MIPS model kept on the RSP's
//! scalar unit plus the vector unit (COP2) of the RSP overlay, and lifting to
//! `relift-ir` with templates generated from them (`gen/rsp.sail`, see
//! `gen/README.md`). The vector unit is written over the IR's lane
//! operations (`relift_ir::lane`).
//!
//! A lifted instruction reads and writes the register file of [`regs`] and
//! the 4 KiB DMEM. A branch or jump is lifted together with its delay slot
//! into one block (see [`lift::rt`]). The RSP has no exceptions, and a word
//! that is no RSP instruction does nothing.

pub mod decode;
pub mod lift;
pub mod regs;

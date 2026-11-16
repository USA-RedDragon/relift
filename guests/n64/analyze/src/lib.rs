//! Static code discovery for Nintendo 64 ROMs.
//!
//! The analysis places the game's bytes in segments: a run of a source
//! (the ROM, or a Yay0, Yaz0 or MIO0 stream decoded from it) at a link
//! address ([`Segment`]). It lifts each instruction with
//! `relift-cpu-mips`, a branch together with its delay slot, and runs the
//! lifted IR over abstract values: constants, partly known bits, the value
//! a register had at the function's entry plus an offset, a word of the
//! caller's frame, a table index (`base + stride * i`) and a word read
//! from a table. Every rule below follows from the instruction semantics
//! in the IR; the decoder only picks which words a scan lifts.
//!
//! - Boot: a boot variant known by its hash ([`Boot::Standard`]) places
//!   `ROM[0x1000..]`, 1 MiB, at the entry point it computes from the
//!   header word (6103 and 6106 subtract 1 and 2 MiB). An unknown variant
//!   that finds an ELF executable on a 256-byte boundary in the first MiB
//!   places its loadable segments ([`Boot::Elf`]). The boot code of a known
//!   variant is a segment of its own, the ROM's first 4 KiB in SP DMEM,
//!   rooted where the PIF starts the CPU (`0xA4000040`,
//!   [`Origin::Boot`]); the boot code of an unknown variant is not followed.
//! - Copies: a loop whose operands are constants on the path that first
//!   reaches it, run on those values, that loads words of a segment in
//!   order and stores each at the next word of RDRAM (at least 8 words)
//!   places them there as a segment when a transfer elsewhere targets an
//!   address in the copy that no segment holds (the boot code moves its
//!   last steps to RDRAM this way).
//! - Calls: a transfer that writes the address after its delay slot to a
//!   register. The code after a call is reached when the callee has a
//!   path back to its entry return address; the registers every return
//!   leaves as they were at entry are kept. A jump to another function's
//!   entry returns where that function returns. Returns are jumps to the
//!   entry `ra`; `ERET` goes to the constants written to `EPC` and
//!   `ErrorEPC`.
//! - Exceptions: a guard whose condition is not known is taken not to
//!   fire, so the rest of the block sees its condition false. A guard
//!   that always fires ends the path, except `SYSCALL`, whose handler
//!   comes back, and a TLB miss, after which memory is not known.
//! - Jump tables: a jump or call through a word loaded from a table index
//!   follows the entries from the base while each is a code address that
//!   lifts, at most 256 and as many as the index's known bits allow.
//! - DMA: stores to the PI and SP address registers are tracked along
//!   paths; a PI DMA from the cartridge with constant operands adds a
//!   segment at its RDRAM address, and the analysis runs again until no
//!   new one appears. A DMA whose operands are entry values of the
//!   function becomes part of its summary and is resolved at each call. An
//!   SP DMA to IMEM with constant operands names an RSP image.
//! - Roots: the entry, the target of each exception trampoline (up to
//!   four instructions from a `LUI` that write only `k0` and `k1` and jump
//!   through a register to a constant), and every constant code address a
//!   run computes that no run reached, while new ones appear
//!   ([`Origin::AddressTaken`]). Each trampoline also becomes a segment at
//!   every exception vector, where programs copy it, so the vectors are
//!   lifted code the runtime binds once the copy is in RDRAM.
//! - Scan: every source is scanned for dense code regions (32-word
//!   windows where 29 words decode, fewer than half are zero and one is a
//!   branch). The parts of a region that a boot or DMA segment holds use
//!   that segment's address; the rest becomes a region segment linked at
//!   the address its own `JAL` targets vote for: the address `V` for which
//!   the most distinct targets are `V` plus the offset of a function start
//!   of the region, with 3 votes and twice the runner-up. A region without one is
//!   followed only through its own relative branches.
//! - Segment tables: three words in a row that give the start and end of
//!   a ROM range of code and a kseg0 address in RDRAM, which the range's
//!   own `JAL`s agree with, are a segment a loader moves there (the table a
//!   build tool writes for overlays). Its code is found like a DMA
//!   segment's, without taking the regions it holds from other segments.
//! - Function starts: a word that lowers the stack pointer by a constant
//!   and the word after a return and its delay slot are roots
//!   ([`Origin::Prologue`]). A speculative root (address-taken or
//!   prologue) whose run reaches a word that does not lift, or whose code
//!   overlaps a word that a run reads as data, is rejected.
//! - Sweep: in a region (or the part of a segment a region covers) where
//!   runs found at least 64 words, every word that decodes counts as code,
//!   with the region grown across the decoding words on both sides.
//!
//! The result ([`Report`]) lists the code as [`Unit`]s (runs of code words
//! of one segment, with their source offset, link address and a hash with
//! the relocation fields cleared), the function entries, the transfers
//! and DMAs left unknown, the constant PI DMAs, and the RSP images: those
//! an SP DMA loads, and runs of words that decode as RSP code with vector
//! instructions the VR4300 does not decode or the RSP's COP0 moves and
//! `BREAK`. A region linked outside RDRAM
//! is code a loader moves: its `J`/`JAL` targets and `LUI`/low-half pairs
//! that point into it are its relocation fields. Nothing here is
//! interpreted at run time; the runtime binds units to the RDRAM ranges
//! it sees them loaded at.

mod analysis;
mod boot;
mod eval;
mod func;
mod image;
mod report;
mod scan;
mod ucode;
mod val;

pub use analysis::{AnalyzeError, Options, analyze, analyze_with};
pub use image::{Segment, SegmentOrigin, Source};
pub use report::{
    Boot, Function, Origin, PiDma, Report, RspImage, RspOrigin, Stats, Unit, Unresolved,
    UnresolvedKind,
};
pub use ucode::{RspLayout, RspPart};

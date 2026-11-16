//! NVIDIA SM86 (Ampere) SASS: decoder and text form.
//!
//! Instruction field positions follow Mesa NAK's SM70+ encoder
//! (src/nouveau/compiler/nak/sm70_encode.rs, MIT). The text form matches
//! nvdisasm's spelling so the decoder can be checked against it.

mod decode;
mod inst;
pub mod program;
pub mod spirv;
pub mod translate;

pub use decode::{DecodeError, decode};
pub use inst::{CBank, Inst, Operand, RegMods, Sched, Swz, Word, f16_to_f64, fmt_f64, fmt_hex};

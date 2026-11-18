//! NVIDIA SM86 (Ampere) SASS: the decoder into `relift-gpu-nv`
//! instructions, whose text form matches nvdisasm's spelling so the decoder
//! can be checked against it, and the parse of program bytes ([`program`]).
//!
//! Instruction field positions follow Mesa NAK's SM70+ encoder
//! (src/nouveau/compiler/nak/sm70_encode.rs, MIT).

mod decode;
pub mod program;
mod word;

pub use decode::{DecodeError, decode};
pub use word::Word;

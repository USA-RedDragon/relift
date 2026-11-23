//! The parts the NVIDIA shader front ends share: decoded instructions
//! ([`Inst`], with nvdisasm's text form), their named values ([`fields`]),
//! programs and their headers ([`program`]), and the translator from those
//! instructions to SPIR-V ([`translate`]).
//!
//! The SM86 decoder (`relift-gpu-sm86`) produces these instructions from
//! Ampere words; the SM53 lowering (`relift-gpu-sm53`) produces them from
//! Maxwell instructions, plus the pseudo-instructions of
//! [`translate::pseudo`] where no SM86 form matches.

pub mod fields;
mod inst;
pub mod lower64;
pub mod program;
pub mod spirv;
pub mod translate;

pub use fields::Fields;
pub use inst::{CBank, Inst, Operand, RegMods, Sched, Swz, f16_to_f64, fmt_f64, fmt_hex};

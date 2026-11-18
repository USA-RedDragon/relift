//! NVIDIA SM53 (Maxwell GM20B, the Switch GPU) SASS as NVN programs hold
//! it: the code header and shader program header ([`program`]), 32-byte
//! bundles of a scheduling word and three instructions ([`bundle`]), the
//! instruction forms ([`form`]) and their fields ([`field`]), and decoded
//! instructions ([`Inst`]) with a text form in envydis's spelling.
//!
//! `form.rs` and `field.rs` are written by `gen.py` from Ryujinx's decoder
//! tables. The lowering to the translator of `relift-gpu-sm86` comes next
//! (see the NVN1 plan). Shared types (`Sched`, `Stage`) are that crate's.

pub mod bundle;
pub mod field;
pub mod form;
pub mod inst;
pub mod lower;
pub mod program;
mod text;

pub use form::{Encoding, Op, Src, classify};
pub use inst::Inst;
pub use program::{ParseError, Program, Sph};

//! NVIDIA SM53 (Maxwell GM20B, the Switch GPU) SASS as NVN programs hold
//! it: the code header and shader program header ([`program`]), 32-byte
//! bundles of a scheduling word and three instructions ([`bundle`]), and
//! the instruction forms ([`form`]).
//!
//! Instructions are named and walked here; operands, the text form and the
//! lowering to the translator of `relift-gpu-sm86` come next (see the NVN1
//! plan). Shared types (`Sched`, `Stage`) are that crate's.

pub mod bundle;
pub mod form;
pub mod program;

pub use form::{Encoding, Op, Src, classify};
pub use program::{ParseError, Program, Sph};

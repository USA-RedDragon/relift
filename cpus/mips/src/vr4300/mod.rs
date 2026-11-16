//! The VR4300 profile (the Nintendo 64 CPU): MIPS III with 64-bit general
//! registers, its FPU, the 32-entry TLB, likely branches and LL/SC.
//!
//! The templates are generated from sail-cheri-mips plus the VR4300 overlay
//! (`gen/vr4300.sail`). They name this profile's modules by the crate root
//! paths `crate::decode` and `crate::lift`, which the crate re-exports, so
//! the generated files did not change when the profile moved here.

pub mod decode;
pub mod lift;
pub mod regs;

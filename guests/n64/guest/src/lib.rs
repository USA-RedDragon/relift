//! The Nintendo 64 guest: the VR4300 frontend for the runtime's N64 layer
//! (`runtime/guests/n64/run`), the RSP frontend for its RSP layer
//! (`runtime/cpus/rsp`) and a ROM, discovered by
//! `relift-n64-analyze`, lifted into one unit per segment with the program
//! files that bind each unit when the machine loads its bytes.

pub mod frontend;
pub mod program;
pub mod rsp;

//! The VR4300 (Nintendo 64 CPU) check: `relift-cpu-mips` lifted blocks, run on
//! the IR interpreter, against the ares oracle on random machine states and an
//! FP special-value corpus, and the runtime's C FPU slow path against the same
//! oracle. `relift-check --oracle ares lift`.

mod cases;
pub mod cli;
mod compare;
mod cslow;
mod fprule;
mod json;
mod oracle;
mod run;

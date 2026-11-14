//! The shared Sail frontend of relift's template generators: a parser for
//! the Sail subset the Arm models use, the model tables, and the staged
//! evaluator that turns the semantics of one encoding into the Rust body
//! of a lifting template. The per-CPU layer (the cut line, register
//! configuration and the decode groups) implements [`eval::Cpu`] and
//! [`stage::Form`] in the CPU's generator crate.

pub mod ast;
mod builtins;
pub mod eval;
pub mod model;
pub mod ops;
pub mod parse;
pub mod stage;
pub mod value;

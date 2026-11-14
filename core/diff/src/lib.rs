//! The wire protocol and the comparison of `relift diff`, which runs one
//! guest function from a live process's state in a reference engine and
//! compares the engine's registers and pages with what the lifted code left.
//!
//! The crate knows no CPU: a state is a pc, a stop address and named
//! registers, an engine is a [`Engine`], and the driver in the live process
//! speaks the line protocol of [`serve`] (docs/abi.md, "Differential
//! checks").

mod pager;
mod serve;
mod wire;

pub use pager::{PAGE, Pager, Wire};
pub use serve::{DEFAULT_LIMIT, Engine, Outcome, Status, Verdict, case, serve};
pub use wire::{Reg, State, parse_after, parse_state};

//! The guest-neutral intermediate representation: values, operations, explicit flags,
//! and memory operations that carry width, endianness and ordering.
//!
//! A [`Block`] is the lifted form of one guest instruction (or a short run of
//! them): straight-line SSA over bit-vector [`Value`]s of 1 to 128 bits, ended
//! by an [`Exit`] that names the next program counter as a constant, a select
//! between two constants, or a computed value.
//!
//! The IR knows no guest CPU. A frontend declares its register file as a
//! [`RegFile`] of named slots; condition flags are ordinary 1-bit slots that
//! the frontend computes with ordinary operations. Memory operations carry
//! their size, [`Endian`] and [`Order`], and the address width comes from the
//! frontend ([`Builder::new`]), so big-endian and 32-bit guests fit without
//! changes here.
//!
//! [`interp::run`] is the reference interpreter. Its semantics are the
//! definition of every operation; the C emitter must agree with it.

mod block;
mod builder;
pub mod interp;
mod ops;
pub mod opt;
mod regs;
pub mod text;

pub use block::{Block, Exit, Flow, Inst, Next, VerifyError};
pub use builder::Builder;
pub use ops::{BinOp, CmpOp, Endian, ExtKind, FpOp, Op, Order, UnOp, Value, Width};
pub use opt::dce;
pub use regs::{Reg, RegDecl, RegFile};
pub use text::{ParseError, parse};

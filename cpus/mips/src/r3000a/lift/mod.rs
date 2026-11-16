//! Lifting with the generated templates. A stub: only the `int` class has
//! templates, and the load delay slot is not modeled yet (see
//! `cpus/mips/README.md`).

mod generated;
pub mod rt;

use std::fmt;

use relift_ir::{Block, Builder, Width};

use super::decode::{self, Encoding};
use super::regs;
use crate::common;
use crate::common::lift::Profile;
use crate::common::rt::Core;
pub use rt::{EXC_GUARD, Exc, IN_DELAY, St};

/// Why an instruction did not lift.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LiftError {
    /// No R3000A encoding matches the word: a reserved instruction.
    Reserved(u32),
    /// The encoding has no template.
    Unsupported(Encoding),
    /// The field values make the instruction undefined.
    Undefined(Encoding),
    /// The field values make it unpredictable and the overlay pins no
    /// R3000A behavior.
    Unpredictable(Encoding),
    /// The delay slot holds a branch or a jump.
    BranchInDelaySlot(Encoding),
}

impl fmt::Display for LiftError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LiftError::Reserved(w) => write!(f, "{w:#010x} is a reserved instruction"),
            LiftError::Unsupported(e) => write!(f, "{} has no template", e.name()),
            LiftError::Undefined(e) => write!(f, "{} is undefined", e.name()),
            LiftError::Unpredictable(e) => write!(f, "{} is unpredictable", e.name()),
            LiftError::BranchInDelaySlot(e) => write!(f, "{} is in a delay slot", e.name()),
        }
    }
}

/// A generated template.
pub(crate) type GenFn = common::lift::Template<St, LiftError>;

/// Whether `enc` has a template.
pub fn has_template(enc: Encoding) -> bool {
    generated::template(enc).is_some()
}

/// One lifted instruction, or a branch and its delay slot.
pub struct Lifted {
    /// The encodings, the branch's first.
    pub encs: Vec<Encoding>,
    /// The block.
    pub block: Block,
    /// The exception the block always raises, if any.
    pub exception: Option<Exc>,
}

impl Profile for St {
    type Enc = Encoding;
    type Err = LiftError;

    fn core(&mut self) -> &mut Core {
        St::core(self)
    }

    fn decode(word: u32) -> Result<Encoding, LiftError> {
        decode::decode(word).ok_or(LiftError::Reserved(word))
    }

    fn template(enc: Encoding) -> Result<GenFn, LiftError> {
        generated::template(enc).ok_or(LiftError::Unsupported(enc))
    }

    fn slot_ok(enc: Encoding) -> Result<(), LiftError> {
        if enc.has_delay_slot() {
            return Err(LiftError::BranchInDelaySlot(enc));
        }
        Ok(())
    }
}

/// Lifts the instruction `word` at `pc`; when it has a delay slot, `slot`
/// (the word at `pc + 4`) is lifted into the same block. Addresses are 32
/// bits.
pub fn lift(pc: u32, word: u32, slot: u32) -> Result<Lifted, LiftError> {
    let pc = u64::from(pc);
    let mut b = Builder::new(regs::regs(), Width::W32, pc);
    let mut st = St::new(pc);
    let body = common::lift::body(&mut b, &mut st, pc, word, slot)?;
    let exception = st.exception_static();
    let block = common::lift::finish(b, &body);
    Ok(Lifted {
        encs: body.encs,
        block,
        exception,
    })
}

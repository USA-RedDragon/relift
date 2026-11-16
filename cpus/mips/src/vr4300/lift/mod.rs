//! Lifting with the generated templates.

mod generated;
pub mod rt;

use std::fmt;

use relift_ir::{Block, Builder, Width};

use super::decode::{self, Encoding};
use super::regs;
use crate::common;
use crate::common::lift::Profile;
use crate::common::rt::Core;
pub use rt::{EXC_GUARD, Exc, FP_SLOW, IN_DELAY, Options, St, Sys};

/// Why an instruction did not lift.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LiftError {
    /// No VR4300 encoding matches the word: a reserved instruction.
    Reserved(u32),
    /// The encoding has no template.
    Unsupported(Encoding),
    /// The field values make the instruction undefined.
    Undefined(Encoding),
    /// The field values make it unpredictable and the overlay pins no
    /// VR4300 behavior.
    Unpredictable(Encoding),
    /// The delay slot holds a branch, a jump or ERET, which is undefined on
    /// the VR4300.
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
    /// The CP0 effects the runtime carries out, in order.
    pub sys: Vec<Sys>,
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
        if enc.has_delay_slot() || enc == Encoding::Eret {
            return Err(LiftError::BranchInDelaySlot(enc));
        }
        Ok(())
    }
}

/// Lifts the instruction `word` at `pc` with the default [`Options`]. When
/// it has a delay slot, `slot` (the word at `pc + 4`) is lifted into the
/// same block; otherwise `slot` is not read.
pub fn lift(pc: u64, word: u32, slot: u32) -> Result<Lifted, LiftError> {
    lift_with(pc, word, slot, Options::default())
}

/// As [`lift`], with `opts`. Each instruction is followed by its tick: one
/// issue cycle plus the overlay's `MIPS_Cycles`, none for a delay slot a
/// likely branch annuls.
pub fn lift_with(pc: u64, word: u32, slot: u32, opts: Options) -> Result<Lifted, LiftError> {
    let mut b = Builder::new(regs::regs(), Width::W64, pc);
    let mut st = St::with_options(pc, opts);
    let body = common::lift::body(&mut b, &mut st, pc, word, slot)?;
    let exception = st.exception_static();
    let sys = st.sys_effects().to_vec();
    let block = common::lift::finish(b, &body);
    Ok(Lifted {
        encs: body.encs,
        block,
        exception,
        sys,
    })
}

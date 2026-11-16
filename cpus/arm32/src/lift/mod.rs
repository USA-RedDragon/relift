//! Lifting with the generated templates.

mod generated;
pub mod rt;

use std::fmt;

use relift_ir::{Block, Builder, Width};

use crate::decode::{self, Encoding, Profile};
use crate::regs;
pub use rt::{Bus, COND_FAIL, St};

/// Why an instruction did not lift.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LiftError {
    /// No encoding of the profile matches the word.
    Unallocated(u32),
    /// The encoding has no template yet.
    Unsupported(Encoding),
    /// The field values make the instruction UNDEFINED.
    Undefined(Encoding),
    /// The field values make it UNPREDICTABLE and the overlay pins no
    /// behavior for the CPU.
    Unpredictable(Encoding),
}

impl fmt::Display for LiftError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LiftError::Unallocated(w) => write!(f, "{w:#010x} is unallocated"),
            LiftError::Unsupported(e) => write!(f, "{} has no template", e.name()),
            LiftError::Undefined(e) => write!(f, "{} is UNDEFINED", e.name()),
            LiftError::Unpredictable(e) => write!(f, "{} is UNPREDICTABLE", e.name()),
        }
    }
}

/// A generated template.
pub(crate) type GenFn = fn(u32, &mut Builder, &mut St) -> Result<(), LiftError>;

/// Whether `enc` has an ARMv4T template.
pub fn has_template(enc: Encoding) -> bool {
    has_template_for(enc, Profile::V4t)
}

/// Whether `enc` has a template on `profile`.
pub fn has_template_for(enc: Encoding, profile: Profile) -> bool {
    generated::template(enc, profile).is_some()
}

/// One lifted instruction.
pub struct Lifted {
    /// Its encoding.
    pub enc: Encoding,
    /// The block.
    pub block: Block,
    /// Its bus events, in order (the cost the block's `Tick`s add up).
    pub bus: Vec<Bus>,
}

/// Lifts the ARMv4T A32 word or Thumb halfword `word` at `pc`.
pub fn lift(word: u32, pc: u32, thumb: bool) -> Result<Lifted, LiftError> {
    lift_for(word, pc, thumb, Profile::V4t)
}

/// Lifts the A32 word or Thumb halfword `word` at `pc` on `profile`.
pub fn lift_for(word: u32, pc: u32, thumb: bool, profile: Profile) -> Result<Lifted, LiftError> {
    let enc = if thumb {
        decode::decode_t16_for(word as u16, profile)
    } else {
        decode::decode_a32_for(word, profile)
    }
    .ok_or(LiftError::Unallocated(word))?;
    let f = generated::template(enc, profile).ok_or(LiftError::Unsupported(enc))?;
    let mut b = Builder::new(regs::regs(), Width::W32, u64::from(pc));
    let mut st = St::for_profile(thumb, profile);
    f(word, &mut b, &mut st)?;
    let (exit, bus) = st.finish(&mut b);
    Ok(Lifted {
        enc,
        block: b.finish(if thumb { 2 } else { 4 }, exit),
        bus,
    })
}

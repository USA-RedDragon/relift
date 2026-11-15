//! Lifting with the generated templates.

mod generated;
pub mod rt;

use std::fmt;

use relift_ir::{Block, Builder, Width};

use crate::decode::{self, Encoding};
use crate::regs;
pub use rt::{Bus, COND_FAIL, St};

/// Why an instruction did not lift.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LiftError {
    /// No ARMv4T encoding matches the word.
    Unallocated(u32),
    /// The encoding has no template yet.
    Unsupported(Encoding),
    /// The field values make the instruction UNDEFINED.
    Undefined(Encoding),
    /// The field values make it UNPREDICTABLE and the overlay pins no
    /// ARM7TDMI behavior.
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

/// Whether `enc` has a template.
pub fn has_template(enc: Encoding) -> bool {
    generated::template(enc).is_some()
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

/// Lifts the A32 word or Thumb halfword `word` at `pc`.
pub fn lift(word: u32, pc: u32, thumb: bool) -> Result<Lifted, LiftError> {
    let enc = if thumb {
        decode::decode_t16(word as u16)
    } else {
        decode::decode_a32(word)
    }
    .ok_or(LiftError::Unallocated(word))?;
    let f = generated::template(enc).ok_or(LiftError::Unsupported(enc))?;
    let mut b = Builder::new(regs::regs(), Width::W32, u64::from(pc));
    let mut st = St::new(thumb);
    f(word, &mut b, &mut st)?;
    let (exit, bus) = st.finish(&mut b);
    Ok(Lifted {
        enc,
        block: b.finish(if thumb { 2 } else { 4 }, exit),
        bus,
    })
}

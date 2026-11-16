//! Lifting with the generated templates.

mod generated;
pub mod rt;

use std::fmt;

use relift_ir::{Block, Builder, Exit, Flow, Next, Width};

use crate::decode::{self, Encoding};
use crate::regs;
pub use rt::{EXC_GUARD, Exc, FP_SLOW, IN_DELAY, St, Sys};

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
pub(crate) type GenFn = fn(u32, &mut Builder, &mut St) -> Result<(), LiftError>;

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

fn template(word: u32) -> Result<(Encoding, GenFn), LiftError> {
    let enc = decode::decode(word).ok_or(LiftError::Reserved(word))?;
    let f = generated::template(enc).ok_or(LiftError::Unsupported(enc))?;
    Ok((enc, f))
}

/// Lifts the instruction `word` at `pc`. When it has a delay slot, `slot`
/// (the word at `pc + 4`) is lifted into the same block; otherwise `slot` is
/// not read.
pub fn lift(pc: u64, word: u32, slot: u32) -> Result<Lifted, LiftError> {
    let (enc, f) = template(word)?;
    let mut b = Builder::new(regs::regs(), Width::W64, pc);
    let mut st = St::new(pc);
    f(word, &mut b, &mut st)?;
    let mut encs = vec![enc];
    let mut len = 4;
    if st.has_delay_slot() && st.exception_static().is_none() {
        len = 8;
        if !st.annuls_always() {
            let (senc, sf) = template(slot)?;
            if senc.has_delay_slot() || senc == Encoding::Eret {
                return Err(LiftError::BranchInDelaySlot(senc));
            }
            encs.push(senc);
            st.enter_delay_slot(&mut b);
            sf(slot, &mut b, &mut st)?;
        }
    }
    st.finish(&mut b);
    let after = pc.wrapping_add(u64::from(len));
    let next = match st.branch_target() {
        Some((rt::Target::Const(t), None)) => Next::Const(t),
        Some((rt::Target::Const(t), Some(cond))) => Next::Select {
            cond,
            taken: t,
            not_taken: after,
        },
        Some((rt::Target::Dynamic(v), None)) => Next::Dynamic(v),
        Some((rt::Target::Dynamic(v), Some(c))) => {
            let k = b.konst(Width::W64, u128::from(after));
            Next::Dynamic(b.select(c, v, k))
        }
        None => match st.jump() {
            Some(j) => Next::Dynamic(j),
            None => Next::Const(after),
        },
    };
    let exception = st.exception_static();
    let sys = st.sys_effects().to_vec();
    Ok(Lifted {
        encs,
        block: b.finish(
            len,
            Exit {
                next,
                flow: Flow::Jump,
            },
        ),
        exception,
        sys,
    })
}

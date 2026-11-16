//! Lifting with the generated templates.

mod generated;
pub mod rt;

use std::fmt;

use relift_ir::{Block, Builder, Exit, Flow, Next, Width};

use crate::decode::{self, Encoding};
use crate::regs;
pub use rt::{St, Sys};

/// Why an instruction did not lift.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LiftError {
    /// The encoding has no template.
    Unsupported(Encoding),
    /// The field values make the instruction undefined.
    Undefined(Encoding),
    /// The field values make it unpredictable and the overlay pins no RSP
    /// behavior.
    Unpredictable(Encoding),
    /// The delay slot holds a branch or a jump.
    BranchInDelaySlot(Encoding),
}

impl fmt::Display for LiftError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
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
    /// The encodings, the branch's first; `None` for a word that is no RSP
    /// instruction and does nothing.
    pub encs: Vec<Option<Encoding>>,
    /// The block.
    pub block: Block,
    /// The effects the runtime carries out after the block, in order.
    pub sys: Vec<Sys>,
}

fn template(word: u32) -> Result<Option<(Encoding, GenFn)>, LiftError> {
    let Some(enc) = decode::decode(word) else {
        return Ok(None);
    };
    let f = generated::template(enc).ok_or(LiftError::Unsupported(enc))?;
    Ok(Some((enc, f)))
}

/// Lifts the instruction `word` at `pc` (an IMEM address, 12 bits). When it
/// has a delay slot, `slot` (the word at `pc + 4`, wrapping) is lifted into
/// the same block; otherwise `slot` is not read.
pub fn lift(pc: u64, word: u32, slot: u32) -> Result<Lifted, LiftError> {
    let pc = pc & rt::ADDR_MASK;
    let first = template(word)?;
    let mut b = Builder::new(regs::regs(), Width::W32, pc);
    let mut st = St::new(pc);
    if let Some((_, f)) = first {
        f(word, &mut b, &mut st)?;
    }
    let mut encs = vec![first.map(|x| x.0)];
    let mut len = 4;
    if st.has_delay_slot() {
        len = 8;
        let second = template(slot)?;
        if let Some((senc, sf)) = second {
            if senc.has_delay_slot() {
                return Err(LiftError::BranchInDelaySlot(senc));
            }
            st.enter_delay_slot();
            sf(slot, &mut b, &mut st)?;
        }
        encs.push(second.map(|x| x.0));
    }
    st.finish(&mut b);
    let after = (pc + u64::from(len)) & rt::ADDR_MASK;
    let next = match st.branch_target() {
        Some((rt::Target::Const(t), None)) => Next::Const(t),
        Some((rt::Target::Const(t), Some(cond))) => Next::Select {
            cond,
            taken: t,
            not_taken: after,
        },
        Some((rt::Target::Dynamic(v), None)) => Next::Dynamic(v),
        Some((rt::Target::Dynamic(v), Some(c))) => {
            let k = b.konst(Width::W32, u128::from(after));
            Next::Dynamic(b.select(c, v, k))
        }
        None => Next::Const(after),
    };
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
        sys,
    })
}

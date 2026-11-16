//! The block shape every MIPS profile shares: one instruction, or a branch
//! or jump with its delay slot, ending in a tick of one cycle per
//! instruction and the exit its branch state gives.

use relift_ir::{Builder, Exit, Flow, Next, Width};

use super::rt::{Core, Target};

/// A profile's generated template.
pub(crate) type Template<S, E> = fn(u32, &mut Builder, &mut S) -> Result<(), E>;

/// A profile's lift-time state, as the block driver sees it.
pub(crate) trait Profile: Sized {
    /// The profile's encoding.
    type Enc: Copy;
    /// Why an instruction did not lift.
    type Err;

    /// The shared state.
    fn core(&mut self) -> &mut Core;

    /// The encoding of `word`.
    fn decode(word: u32) -> Result<Self::Enc, Self::Err>;

    /// The template of `enc`.
    fn template(enc: Self::Enc) -> Result<Template<Self, Self::Err>, Self::Err>;

    /// Refuses an encoding that may not be in a delay slot.
    fn slot_ok(enc: Self::Enc) -> Result<(), Self::Err>;
}

/// A lifted block before `Builder::finish`: its encodings (the branch's
/// first), its length in bytes and its exit.
pub(crate) struct Body<E> {
    pub(crate) encs: Vec<E>,
    pub(crate) len: u32,
    pub(crate) exit: Exit,
}

/// Lifts `word` at `pc` into `b` with `st`; when it has a delay slot,
/// `slot` (the word at `pc + 4`) is lifted into the same block unless the
/// branch always annuls it. The block ends in a tick of one cycle per
/// instruction it holds (a branch and its delay slot are two, also when
/// the slot is annulled).
pub(crate) fn body<S: Profile>(
    b: &mut Builder,
    st: &mut S,
    pc: u64,
    word: u32,
    slot: u32,
) -> Result<Body<S::Enc>, S::Err> {
    let enc = S::decode(word)?;
    S::template(enc)?(word, b, st)?;
    let mut encs = vec![enc];
    let mut len = 4;
    let c = st.core();
    if c.has_delay_slot() && c.exception_static().is_none() {
        len = 8;
        if !c.annuls_always() {
            let senc = S::decode(slot)?;
            let sf = S::template(senc)?;
            S::slot_ok(senc)?;
            encs.push(senc);
            st.core().enter_delay_slot(b);
            sf(slot, b, st)?;
        }
    }
    b.tick(len / 4);
    let c = st.core();
    c.finish(b);
    let after = pc.wrapping_add(u64::from(len));
    let next = match c.branch_target() {
        Some((Target::Const(t), None)) => Next::Const(t),
        Some((Target::Const(t), Some(cond))) => Next::Select {
            cond,
            taken: t,
            not_taken: after,
        },
        Some((Target::Dynamic(v), None)) => Next::Dynamic(v),
        Some((Target::Dynamic(v), Some(cond))) => {
            let k = b.konst(Width::W64, u128::from(after));
            Next::Dynamic(b.select(cond, v, k))
        }
        None => match c.jump() {
            Some(j) => Next::Dynamic(j),
            None => Next::Const(after),
        },
    };
    Ok(Body {
        encs,
        len,
        exit: Exit {
            next,
            flow: Flow::Jump,
        },
    })
}

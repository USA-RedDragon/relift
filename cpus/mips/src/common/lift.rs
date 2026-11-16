//! The block shape every MIPS profile shares: one instruction, or a branch
//! or jump with its delay slot, each instruction followed by its tick, and
//! the exit its branch state gives.

use relift_ir::{Block, Builder, Exit, Flow, Inst, Next, Op, Value};

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

/// A lifted block before [`finish`]: its encodings (the branch's first),
/// its length in bytes, its exit and the branch's tick when the block also
/// holds the delay slot.
pub(crate) struct Body<E> {
    pub(crate) encs: Vec<E>,
    pub(crate) len: u32,
    pub(crate) exit: Exit,
    branch_tick: Option<usize>,
}

/// Lifts `word` at `pc` into `b` with `st`; when it has a delay slot,
/// `slot` (the word at `pc + 4`) is lifted into the same block unless the
/// branch always annuls it. Each instruction ticks its issue cycle plus the
/// cycles its template adds, and a slot a likely branch annuls takes none.
/// A branch's tick goes after its slot here; [`finish`] moves it in front
/// of the slot's first effect.
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
    let mut branch_tick = None;
    let c = st.core();
    if c.has_delay_slot() && c.exception_static().is_none() {
        len = 8;
        if !c.annuls_always() {
            let n = 1 + c.take_cycles();
            let senc = S::decode(slot)?;
            let sf = S::template(senc)?;
            S::slot_ok(senc)?;
            encs.push(senc);
            st.core().enter_delay_slot(b);
            sf(slot, b, st)?;
            branch_tick = Some(b.push(Op::Tick(n), None).index());
        }
    }
    let c = st.core();
    c.tick(b);
    c.finish(b);
    let after = match b.addr_width().bits() {
        64 => pc.wrapping_add(u64::from(len)),
        n => pc.wrapping_add(u64::from(len)) & ((1 << n) - 1),
    };
    let next = match c.branch_target() {
        Some((Target::Const(t), None)) => Next::Const(t),
        Some((Target::Const(t), Some(cond))) => Next::Select {
            cond,
            taken: t,
            not_taken: after,
        },
        Some((Target::Dynamic(v), None)) => Next::Dynamic(v),
        Some((Target::Dynamic(v), Some(cond))) => {
            let k = b.konst(b.addr_width(), u128::from(after));
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
        branch_tick,
    })
}

/// Ends the block of `body`: a branch's tick moves to just after the
/// block's last guard, so the slot's accesses and calls see the branch's
/// cycle and every guard still comes before any effect.
pub(crate) fn finish<E>(b: Builder, body: &Body<E>) -> Block {
    let mut block = b.finish(body.len, body.exit);
    let Some(from) = body.branch_tick else {
        return block;
    };
    let to = block
        .insts
        .iter()
        .rposition(|i| matches!(i.op, Op::Guard(..)))
        .map_or(0, |g| g + 1);
    if to < from {
        move_before(&mut block, from, to);
    }
    block
}

/// Moves `block.insts[from]`, which has no result, in front of
/// `block.insts[to]` (`to < from`), renumbering the values in between.
fn move_before(block: &mut Block, from: usize, to: usize) {
    let old = std::mem::take(&mut block.insts);
    let renum = |v: &Value| {
        let i = v.index();
        Value(if i >= to && i < from { i + 1 } else { i } as u32)
    };
    let mut insts: Vec<Inst> = Vec::with_capacity(old.len());
    for (i, inst) in old.iter().enumerate() {
        if i == to {
            insts.push(old[from].clone());
        }
        if i != from {
            insts.push(Inst {
                op: inst.op.map_values(&renum),
                width: inst.width,
            });
        }
    }
    block.insts = insts;
    block.exit.next = match block.exit.next {
        Next::Dynamic(v) => Next::Dynamic(renum(&v)),
        Next::Select {
            cond,
            taken,
            not_taken,
        } => Next::Select {
            cond: renum(&cond),
            taken,
            not_taken,
        },
        next => next,
    };
}

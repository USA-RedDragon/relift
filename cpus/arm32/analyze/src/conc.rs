//! Runs a call whose arguments are constants to its return, to learn the
//! bytes it writes to work RAM.
//!
//! The run uses the same evaluator as discovery with a byte-exact memory
//! for work RAM and a private stack: every value is a constant or unknown.
//! It stops without a result when a branch, a condition, an address (of a
//! store too) or the state depends on an unknown value (a register the
//! caller did not set, uninitialized RAM, an I/O register), at a SWI, or
//! after [`MAX_STEPS`] instructions. When it returns, the written RAM bytes whose
//! values are known are what the call leaves there, whatever the call is
//! (a copy loop, a decompressor).
//!
//! A word the run loads from RAM it did not write is the symbol of that
//! address; where the run stores such a word unchanged to a constant RAM
//! address, the destination holds what the source held (a RAM-to-RAM
//! copy), which the run reports as a move.

use std::collections::BTreeMap;

use relift_cpu_arm32::decode::Encoding;

use crate::eval::{self, SCRATCH_STACK, Scratch};
use crate::func::{Args, Env};
use crate::loc::Loc;
use crate::val::{Abs, Base, LR, SP, State, Sym};

/// The return address the run's caller leaves in `lr`.
const SENTINEL: u32 = 0x0F00_0000;
/// The longest run.
pub(crate) const MAX_STEPS: usize = 1 << 21;

fn fetch(env: &Env<'_>, m: &Scratch, loc: Loc) -> Option<u32> {
    if env.image.is_ram(loc.addr) {
        let mut v = 0u32;
        for i in (0..loc.size()).rev() {
            v = v << 8 | u32::from((*m.get(&(loc.addr + i))?)?);
        }
        return Some(v);
    }
    env.image.fetch(loc)
}

/// What a run with constant inputs leaves in RAM.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct Written {
    /// The bytes written with known values, as ranges.
    pub ranges: Vec<(u32, Vec<u8>)>,
    /// Words copied unchanged from RAM the run did not write: destination
    /// and source.
    pub moves: Vec<(u32, u32)>,
}

/// What a call to `callee` with `r0`-`r12` = `args` writes, or `None`
/// when the run cannot be completed with constants.
pub(crate) fn run_call(env: &Env<'_>, callee: Loc, args: Args) -> Option<Written> {
    let mut st = State::entry(callee.thumb);
    for (i, r) in st.regs.iter_mut().enumerate().take(15) {
        *r = match args.get(i) {
            Some(a @ Abs::K(_)) => *a,
            _ => Abs::Top,
        };
    }
    st.regs[usize::from(SP)] = Abs::K(u128::from(SCRATCH_STACK - 0x10));
    st.regs[usize::from(LR)] = Abs::K(u128::from(SENTINEL));
    run(env, callee, st, None)
}

/// What a function leaves in RAM when it runs from its entry, with no
/// register known, until it reaches `stop` (a jump into RAM it wrote, such
/// as a boot stub that copies code with its own loop and runs the copy), or
/// `None` when the run needs a value it does not know first.
pub(crate) fn run_to(env: &Env<'_>, entry: Loc, stop: Loc) -> Option<Written> {
    let mut st = State::entry(entry.thumb);
    for r in st.regs.iter_mut().take(15) {
        *r = Abs::Top;
    }
    st.regs[usize::from(LR)] = Abs::K(u128::from(SENTINEL));
    run(env, entry, st, Some(stop))
}

fn run(env: &Env<'_>, entry: Loc, mut st: State, stop: Option<Loc>) -> Option<Written> {
    let mut m = Scratch::new();
    let mut moves: BTreeMap<u32, u32> = BTreeMap::new();
    let mut loc = entry;
    for _ in 0..MAX_STEPS {
        let stopped = stop.is_some_and(|s| s.addr == loc.addr && s.thumb == loc.thumb);
        if loc.addr == SENTINEL || stopped {
            return Some(Written {
                ranges: ranges(env, &m),
                moves: moves.into_iter().collect(),
            });
        }
        let word = fetch(env, &m, loc)?;
        let lifted = env.lift(loc, word).ok()?;
        let (enc, block) = &*lifted;
        if matches!(enc, Encoding::SVC_A1 | Encoding::SVC_T1) {
            return None;
        }
        let out = eval::eval(
            block,
            loc,
            &st,
            env.image,
            eval::pc_relative(*enc, word, loc.thumb),
            Some(&mut m),
        );
        if out.may_skip && out.may_run || out.stores.iter().any(|s| s.0.k().is_none()) {
            return None;
        }
        for &(a, n, v) in &out.stores {
            let Some(a) = a.k32() else { continue };
            let w = a & !3;
            moves.retain(|d, _| d.wrapping_add(4) <= a || *d >= a.wrapping_add(u32::from(n)));
            if let Abs::S(Sym {
                base: Base::Mem(s),
                off: 0,
                align: 0,
            }) = v
                && n == 4
                && w == a
                && env.image.is_ram(a)
            {
                moves.insert(a, s);
            }
        }
        if !out.may_run {
            loc.addr = loc.addr.wrapping_add(loc.size());
            continue;
        }
        let [Abs::K(t)] = out.targets[..] else {
            return None;
        };
        st = out.st;
        let thumb = st.thumb()?;
        loc = Loc {
            addr: t as u32,
            thumb,
            copy: None,
        };
    }
    None
}

fn ranges(env: &Env<'_>, m: &Scratch) -> Vec<(u32, Vec<u8>)> {
    let ram: BTreeMap<u32, u8> = m
        .iter()
        .filter(|(a, _)| env.image.is_ram(**a))
        .filter_map(|(a, b)| Some((*a, (*b)?)))
        .collect();
    let mut out: Vec<(u32, Vec<u8>)> = Vec::new();
    for (a, b) in ram {
        match out.last_mut() {
            Some((s, v)) if *s + v.len() as u32 == a => v.push(b),
            _ => out.push((a, vec![b])),
        }
    }
    out
}

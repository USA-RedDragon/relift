//! Runs a call whose arguments are constants to its return, to learn the
//! bytes it writes to work RAM.
//!
//! The run uses the same evaluator as discovery with a byte-exact memory
//! for work RAM and a private stack: every value is a constant or unknown.
//! It stops without a result when a branch, a condition, an address or the
//! state depends on an unknown value (an argument the caller did not set,
//! uninitialized RAM, an I/O register), at a SWI, or after
//! [`MAX_STEPS`] instructions. When it returns, the written RAM bytes whose
//! values are known are what the call leaves there, whatever the call is
//! (a copy loop, a decompressor).

use std::collections::BTreeMap;

use relift_cpu_arm32::decode::Encoding;

use crate::eval::{self, SCRATCH_STACK, Scratch};
use crate::func::Env;
use crate::loc::{Loc, Region};
use crate::val::{Abs, LR, SP, State};

/// The return address the run's caller leaves in `lr`.
const SENTINEL: u32 = 0x0F00_0000;
/// The longest run.
pub(crate) const MAX_STEPS: usize = 1 << 21;

fn fetch(env: &Env<'_>, m: &Scratch, loc: Loc) -> Option<u32> {
    if loc.region().is_ram() {
        let mut v = 0u32;
        for i in (0..loc.size()).rev() {
            v = v << 8 | u32::from(*m.get(&(loc.addr + i))?);
        }
        return Some(v);
    }
    env.image.fetch(loc)
}

/// The RAM ranges a call to `callee` with `r0`-`r3` = `args` writes, or
/// `None` when the run cannot be completed with constants.
pub(crate) fn run_call(env: &Env<'_>, callee: Loc, args: [Abs; 4]) -> Option<Vec<(u32, Vec<u8>)>> {
    let mut st = State::entry(callee.thumb);
    for (i, r) in st.regs.iter_mut().enumerate().take(15) {
        *r = args.get(i).copied().unwrap_or(Abs::Top);
    }
    st.regs[usize::from(SP)] = Abs::K(u128::from(SCRATCH_STACK - 0x10));
    st.regs[usize::from(LR)] = Abs::K(u128::from(SENTINEL));
    let mut m = Scratch::new();
    let mut loc = callee;
    for _ in 0..MAX_STEPS {
        if loc.addr == SENTINEL {
            return Some(ranges(&m));
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
        if out.may_skip && out.may_run {
            return None;
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

fn ranges(m: &Scratch) -> Vec<(u32, Vec<u8>)> {
    let ram: BTreeMap<u32, u8> = m
        .iter()
        .filter(|(a, _)| Region::of(**a).is_ram())
        .map(|(a, b)| (*a, *b))
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

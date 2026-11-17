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
//! A run from a reset entry ([`boot`]) instead takes both ways where a
//! condition depends on an unknown value: a boot waits on the other CPU
//! before it copies its code into place and jumps there.
//!
//! A word the run loads from RAM it did not write is the symbol of that
//! address; where the run stores such a word unchanged to a constant RAM
//! address, the destination holds what the source held (a RAM-to-RAM
//! copy), which the run reports as a move.

use std::collections::{BTreeMap, HashMap};

use relift_cpu_arm32::decode::Encoding;
use relift_cpu_arm32::regs::{self, Mode};

use crate::eval::{self, SCRATCH_STACK, Scratch};
use crate::func::{Args, Env};
use crate::loc::Loc;
use crate::val::{Abs, Base, LR, SP, State, Sym};

/// The return address the run's caller leaves in `lr`.
const SENTINEL: u32 = 0x0F00_0000;
/// The longest run.
pub(crate) const MAX_STEPS: usize = 1 << 21;
/// The longest run from a reset entry, over all its paths.
const MAX_BOOT_STEPS: usize = 1 << 18;
/// The most paths a run from a reset entry follows at a time.
const MAX_PATHS: usize = 64;
/// The bytes after a call a run to its return may read as the caller's.
const RETURN_DATA: u32 = 256;
/// The most times a path forks at one address: a loop whose end depends
/// on an unknown value runs at most this many times.
const MAX_REVISITS: u32 = 64;

fn fetch(env: &Env<'_>, m: &Scratch, loc: Loc) -> Option<u32> {
    if loc.copy.is_some() && (0..loc.size()).all(|i| !m.contains_key(&(loc.addr + i))) {
        return env.image.fetch(loc);
    }
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

/// Where a call from `site` to `callee` with `r0`-`r12` = `args` goes on:
/// the target of the first computed jump the callee makes (its return,
/// maybe past data it reads after the call), or `None` when the run needs
/// a value it does not know first. The bytes after the call are the ones
/// the caller's copy holds there.
pub(crate) fn run_return(env: &Env<'_>, site: Loc, callee: Loc, args: Args) -> Option<Loc> {
    let lr = site.addr.wrapping_add(site.size()) | u32::from(site.thumb);
    let mut st = State::entry(callee.thumb);
    for (i, r) in st.regs.iter_mut().enumerate().take(15) {
        *r = match args.get(i) {
            Some(a @ Abs::K(_)) => *a,
            _ => Abs::Top,
        };
    }
    st.regs[usize::from(SP)] = Abs::K(u128::from(SCRATCH_STACK - 0x10));
    st.regs[usize::from(LR)] = Abs::K(u128::from(lr));
    let mut m = Scratch::new();
    if let Some(c) = site.copy {
        let after = lr & !1;
        for a in after..after.wrapping_add(RETURN_DATA) {
            if let Some(b) = env.image.read(Some(c), a, 1) {
                m.insert(a, Some(b as u8));
            }
        }
    }
    let mut loc = callee;
    for _ in 0..256 {
        let word = fetch(env, &m, loc)?;
        let lifted = env.lift(loc, word).ok()?;
        let (enc, block) = &*lifted;
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
        let addr = t as u32;
        let copy = loc.copy.filter(|&c| env.image.holds(c, addr));
        let next = Loc {
            addr,
            thumb: st.thumb()?,
            copy,
        };
        if matches!(block.exit.next, relift_ir::Next::Dynamic(_)) {
            return Some(next);
        }
        loc = next;
    }
    None
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

/// The code a boot copies into RAM before it jumps there: per jump from
/// the image into RAM, the bytes the RAM around its target holds when the
/// boot first makes it.
pub(crate) type Boot = HashMap<(u32, bool), (u32, Vec<u8>)>;

/// Runs the program from the reset entry `entry` in `mode`, with no
/// register known, taking both ways where a condition depends on an
/// unknown value (the other side of an I/O register could answer either
/// way): up to [`MAX_PATHS`] paths and [`MAX_BOOT_STEPS`] instructions in
/// all. A path that comes back to a fork without a store to work RAM or the
/// stack since it last forked there only waits (on the other CPU, a timer
/// or a device), and the way that left the first time stands for it; one
/// that forks [`MAX_REVISITS`] times at one address ends. The result has
/// the jumps into RAM it made, as the first path to make each saw them.
pub(crate) fn boot(env: &Env<'_>, entry: Loc, mode: Mode) -> Boot {
    let mut st = State::entry(entry.thumb);
    for r in st.regs.iter_mut().take(15) {
        *r = Abs::Top;
    }
    st.regs[regs::M.index()] = Abs::K(u128::from(mode.bits()));
    st.regs[usize::from(LR)] = Abs::K(u128::from(SENTINEL));
    let mut work = vec![Path::new(entry, st)];
    let mut steps = 0;
    let mut out = Boot::new();
    while let Some(mut p) = work.pop() {
        if let Step::Fork(mut arms) = walk(
            env,
            &mut p,
            None,
            &mut steps,
            MAX_BOOT_STEPS,
            Some(&mut out),
        ) {
            arms.truncate(MAX_PATHS.saturating_sub(work.len()).max(1));
            work.extend(arms.into_iter().rev());
        }
    }
    out
}

/// One way through a run that forks: where it is, its registers, its
/// memory, the moves it found, and per fork address the count of its
/// stores to work RAM and the stack when it last forked there and how
/// often it forked there.
#[derive(Clone)]
struct Path {
    loc: Loc,
    st: State,
    m: Scratch,
    moves: BTreeMap<u32, u32>,
    forks: HashMap<u32, (Option<u64>, u32)>,
    stores: u64,
}

impl Path {
    fn new(loc: Loc, st: State) -> Path {
        Path {
            loc,
            st,
            m: Scratch::new(),
            moves: BTreeMap::new(),
            forks: HashMap::new(),
            stores: 0,
        }
    }
}

fn run(env: &Env<'_>, entry: Loc, st: State, stop: Option<Loc>) -> Option<Written> {
    let mut p = Path::new(entry, st);
    let mut steps = 0;
    match walk(env, &mut p, stop, &mut steps, MAX_STEPS, None) {
        Step::End => Some(Written {
            ranges: ranges(env, &p.m),
            moves: p.moves.into_iter().collect(),
        }),
        Step::Fork(_) | Step::Fail => None,
    }
}

enum Step {
    /// The path reached the run's end.
    End,
    /// The path cannot go on.
    Fail,
    /// The path goes on as each of these, the most likely first.
    Fork(Vec<Path>),
}

/// Runs `p` to its end or to a condition that depends on a value the run
/// does not know, which forks it (see [`boot`]); with `jumps`, records the
/// jumps from the image into RAM.
fn walk(
    env: &Env<'_>,
    p: &mut Path,
    stop: Option<Loc>,
    steps: &mut usize,
    limit: usize,
    mut jumps: Option<&mut Boot>,
) -> Step {
    while *steps < limit {
        *steps += 1;
        let loc = p.loc;
        let stopped = stop.is_some_and(|s| s.addr == loc.addr && s.thumb == loc.thumb);
        if loc.addr == SENTINEL || stopped {
            return Step::End;
        }
        let Some(word) = fetch(env, &p.m, loc) else {
            return Step::Fail;
        };
        let Ok(lifted) = env.lift(loc, word) else {
            return Step::Fail;
        };
        let (enc, block) = &*lifted;
        if matches!(enc, Encoding::SVC_A1 | Encoding::SVC_T1) {
            return Step::Fail;
        }
        let before = p.m.clone();
        let out = eval::eval(
            block,
            loc,
            &p.st,
            env.image,
            eval::pc_relative(*enc, word, loc.thumb),
            Some(&mut p.m),
        );
        if out.stores.iter().any(|s| s.0.k().is_none()) {
            return Step::Fail;
        }
        for &(a, n, v) in &out.stores {
            let Some(a) = a.k32() else { continue };
            if eval::in_scratch(env.image, a) {
                p.stores += 1;
            }
            let w = a & !3;
            p.moves
                .retain(|d, _| d.wrapping_add(4) <= a || *d >= a.wrapping_add(u32::from(n)));
            if let Abs::S(Sym {
                base: Base::Mem(s),
                off: 0,
                align: 0,
            }) = v
                && n == 4
                && w == a
                && env.image.is_ram(a)
            {
                p.moves.insert(a, s);
            }
        }
        let next = Loc {
            addr: loc.addr.wrapping_add(loc.size()),
            ..loc
        };
        if !out.may_run {
            p.loc = next;
            continue;
        }
        let mut arms: Vec<(Loc, State)> = Vec::new();
        for t in &out.targets {
            let Abs::K(t) = *t else { return Step::Fail };
            let Some(thumb) = out.st.thumb() else {
                return Step::Fail;
            };
            let to = Loc {
                addr: t as u32,
                thumb,
                copy: None,
            };
            if let Some(j) = jumps.as_deref_mut()
                && !env.image.is_ram(loc.addr)
                && env.image.is_ram(to.addr)
                && !j.contains_key(&(loc.addr, loc.thumb))
                && let Some(held) = held(&p.m, to.addr)
            {
                j.insert((loc.addr, loc.thumb), held);
            }
            arms.push((to, out.st.clone()));
        }
        let skip = out.may_skip.then(|| (next, p.st.clone()));
        if arms.len() == 1 && skip.is_none() {
            let (to, st) = arms.swap_remove(0);
            p.loc = to;
            p.st = st;
            continue;
        }
        let seen = p.forks.entry(loc.addr).or_default();
        if seen.0 == Some(p.stores) || seen.1 >= MAX_REVISITS {
            return Step::Fail;
        }
        *seen = (Some(p.stores), seen.1 + 1);
        let mut ways: Vec<Path> = Vec::new();
        if let Some((to, st)) = skip {
            let mut q = p.clone();
            q.m = before.clone();
            q.loc = to;
            q.st = st;
            ways.push(q);
        }
        for (to, st) in arms {
            let mut q = p.clone();
            q.loc = to;
            q.st = st;
            ways.push(q);
        }
        ways.sort_by_key(|q| q.loc.addr <= loc.addr);
        return Step::Fork(ways);
    }
    Step::Fail
}

/// The bytes with known values around `addr`, and where they start.
fn held(m: &Scratch, addr: u32) -> Option<(u32, Vec<u8>)> {
    let byte = |a: u32| m.get(&a).copied().flatten();
    byte(addr)?;
    let mut lo = addr;
    while lo > 0 && byte(lo - 1).is_some() {
        lo -= 1;
    }
    let mut bytes = Vec::new();
    let mut a = lo;
    while let Some(b) = byte(a) {
        bytes.push(b);
        a = a.wrapping_add(1);
    }
    Some((lo, bytes))
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

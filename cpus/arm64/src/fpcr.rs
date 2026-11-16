//! The FPCR proof: whether every FPCR write in a program keeps the FP mode
//! the threads start with.
//!
//! Lifted FP code assumes FPCR's mode bits (RMode, FZ, DN, AHP, FZ16) are
//! zero and guards each block that relies on it (`sail::SLOW_FPCR`). When
//! no write can change the mode, the guards never leave and the build
//! drops them. A64 writes FPCR only with `MSR FPCR`; Horizon starts the
//! main thread with FPCR zero and a new thread with its creator's FPCR, and
//! the runtime starts every thread with zero, so by induction over each
//! thread's execution the mode stays zero when every write writes zero
//! guest bits whenever every earlier read gave zero.
//!
//! That is what the analysis checks. It interprets code over abstract
//! values (known bits, pointers at an offset from a stack base, the
//! function addresses loader-filled words hold) with `MRS FPCR` giving the
//! default, from a root function's entry with nothing known, inlining the
//! callees (every function that reaches a write, others a few levels below
//! the nearest such frame), and asks at each reached `MSR FPCR` whether the guest bits
//! written are known zero. A write that depends on the root's arguments
//! (`fesetround(mode)`, or `fesetenv(env)` restoring an environment saved
//! by `fegetenv` on the caller's stack) is decided at the callers instead:
//! the search climbs from the function holding the write to each of its
//! callers, which must all be known (its address must not escape), and
//! the climb stops where every root proves every write it reaches.
//!
//! Memory is tracked for the stack the root runs on (and stacks it switches
//! to), byte by byte, under the model of a program that writes an object
//! only through addresses derived from that object's own address (see
//! [`value::Mem`]); calls the analysis does not follow change only objects
//! whose address escaped and the caller-saved registers (AAPCS64). A
//! landing pad starts from what each call it serves leaves, when the
//! analysis does not follow that call. A computed jump without known targets, a resume point (`setjmp`)
//! or too much code stops a root without a result.

mod interp;
mod value;

#[cfg(test)]
mod tests;

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::sync::Arc;

use relift_ir::Block;

pub use interp::Check;

/// Whether `w` is `MSR FPCR, Xt`.
pub fn msr_fpcr(w: u32) -> bool {
    w & 0xffff_ffe0 == 0xd51b_4400
}

/// What runs at a function entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Host {
    /// The guest's code.
    Guest,
    /// A host function instead of the guest's (it writes no FPCR).
    Override,
    /// Code the analysis cannot follow: a host function that returns more
    /// than once (`setjmp`), or a mod's variant of the function.
    Unknown,
}

/// The program as the analysis reads it.
pub struct Code<'a> {
    /// The lifted instruction at an address, `None` where none lifts.
    pub block: &'a dyn Fn(u64) -> Option<Arc<Block>>,
    /// The function entries a loader-filled word at an address may hold,
    /// `None` when the address holds no such word.
    pub slot: &'a dyn Fn(u64) -> Option<Vec<u64>>,
    /// The jump-table targets of a computed jump.
    pub table: &'a dyn Fn(u64) -> Option<Vec<u64>>,
    /// Whether an address is a function entry.
    pub is_entry: &'a dyn Fn(u64) -> bool,
    /// The landing pads that serve the call at an address.
    pub pads: &'a dyn Fn(u64) -> Vec<u64>,
    /// What runs at an entry.
    pub host: &'a dyn Fn(u64) -> Host,
}

/// The callee depth the analysis inlines a callee to when no write needs
/// it, and the steps one root may take, tried in this order.
const TRIES: [(u32, usize); 3] = [(3, 300_000), (1, 300_000), (0, 300_000)];

/// The most callees one root may stop inlining.
const SKIPS: usize = 32;

/// The most callers above a write the search climbs.
const CLIMB: usize = 6;

/// Interprets the function at `root`, inlining callees up to `depth` deep
/// and the functions `must` names at any depth.
pub fn check(code: &Code<'_>, root: u64, must: &dyn Fn(u64) -> bool, depth: u32) -> Check {
    let (_, budget) = TRIES[0];
    check_skipping(code, root, must, depth, budget)
}

/// [`check`] that, when the interpretation stops in an inlined callee it
/// need not inline, tries again with that callee as a call it does not
/// follow.
fn check_skipping(
    code: &Code<'_>,
    root: u64,
    must: &dyn Fn(u64) -> bool,
    depth: u32,
    budget: usize,
) -> Check {
    let mut skip = BTreeSet::new();
    loop {
        let r = interp::Interp::new(code, must, &skip, depth, budget).run(root);
        match r.blame {
            Some(f) if skip.len() < SKIPS && skip.insert(f) => {}
            _ => return r,
        }
    }
}

fn check_tries(code: &Code<'_>, root: u64, must: &dyn Fn(u64) -> bool) -> Check {
    let mut last = Check::default();
    for (depth, budget) in TRIES {
        last = check_skipping(code, root, must, depth, budget);
        if last.proven() {
            break;
        }
    }
    last
}

/// The result of [`prove`].
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Proof {
    /// Whether every write keeps the default mode.
    pub constant: bool,
    /// The functions whose interpretation proved every write it reached.
    pub roots: BTreeSet<u64>,
    /// The functions the search climbed above, with why their own
    /// interpretation did not prove the writes.
    pub climbed: BTreeMap<u64, String>,
    /// The functions holding a write that nothing calls.
    pub unreachable: BTreeSet<u64>,
    /// Why the proof failed.
    pub reason: Option<String>,
}

/// Proves that every `MSR FPCR` (each in the functions `sites` gives for
/// it) writes the default mode, climbing through `callers`, which gives
/// the functions that may call, tail-call or fall into an entry (none when
/// nothing enters it), or `None` when code the analysis cannot see may.
pub fn prove(
    code: &Code<'_>,
    callers: &dyn Fn(u64) -> Option<Vec<u64>>,
    sites: &BTreeMap<u64, BTreeSet<u64>>,
) -> Proof {
    let mut out = Proof {
        constant: true,
        ..Proof::default()
    };
    let mut todo: VecDeque<(u64, usize)> = VecDeque::new();
    for (&site, fns) in sites {
        if fns.is_empty() {
            out.constant = false;
            out.reason = Some(format!("MSR FPCR at {site:#x} is in no function"));
            return out;
        }
        todo.extend(fns.iter().map(|&f| (f, 0)));
    }
    let reach = reaching(callers, sites);
    loop {
        while let Some((f, up)) = todo.pop_front() {
            if out.roots.contains(&f)
                || out.climbed.contains_key(&f)
                || out.unreachable.contains(&f)
            {
                continue;
            }
            let must = |t: u64| t != f && reach.contains(&t);
            let r = check_tries(code, f, &must);
            if r.proven() {
                out.roots.insert(f);
                continue;
            }
            let why = r.gave_up.clone().unwrap_or_else(|| {
                let bad: Vec<String> = r
                    .sites
                    .iter()
                    .filter(|s| !*s.1)
                    .map(|s| format!("{:#x}", s.0))
                    .collect();
                format!("writes at {} depend on the caller", bad.join(", "))
            });
            if let Err(e) = climb(&mut out, &mut todo, callers, f, up, why) {
                out.constant = false;
                out.reason = Some(e);
                return out;
            }
        }
        // A root checked before a later climb did not yet inline the
        // functions the search visited since; check every root again.
        let mut again = false;
        for f in out.roots.clone() {
            let must = |t: u64| t != f && reach.contains(&t);
            let r = check_tries(code, f, &must);
            if !r.proven() {
                out.roots.remove(&f);
                let why = r
                    .gave_up
                    .unwrap_or_else(|| "a write depends on the caller".into());
                if let Err(e) = climb(&mut out, &mut todo, callers, f, 0, why) {
                    out.constant = false;
                    out.reason = Some(e);
                    return out;
                }
                again = true;
            }
        }
        if !again {
            return out;
        }
    }
}

/// The functions that reach a write within [`CLIMB`] calls: the functions
/// holding one and their callers. A root inlines them at any depth (a
/// callee that saves the environment the root later restores must be
/// followed even when it proves on its own).
fn reaching(
    callers: &dyn Fn(u64) -> Option<Vec<u64>>,
    sites: &BTreeMap<u64, BTreeSet<u64>>,
) -> BTreeSet<u64> {
    let mut out: BTreeSet<u64> = sites.values().flatten().copied().collect();
    let mut level: Vec<u64> = out.iter().copied().collect();
    for _ in 0..CLIMB {
        let mut next = Vec::new();
        for f in level {
            for c in callers(f).unwrap_or_default() {
                if out.insert(c) {
                    next.push(c);
                }
            }
        }
        level = next;
    }
    out
}

fn climb(
    out: &mut Proof,
    todo: &mut VecDeque<(u64, usize)>,
    callers: &dyn Fn(u64) -> Option<Vec<u64>>,
    f: u64,
    up: usize,
    why: String,
) -> Result<(), String> {
    out.climbed.insert(f, why.clone());
    if up >= CLIMB {
        return Err(format!("{f:#x}: {why}, {CLIMB} callers up"));
    }
    let Some(cs) = callers(f) else {
        return Err(format!(
            "{f:#x}: {why}, and code the analysis cannot see may call it"
        ));
    };
    if cs.is_empty() {
        out.climbed.remove(&f);
        out.unreachable.insert(f);
        return Ok(());
    }
    todo.extend(cs.into_iter().map(|c| (c, up + 1)));
    Ok(())
}

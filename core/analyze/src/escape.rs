//! Return addresses that code can come back to without returning: the
//! resume points of non-local transfers (`setjmp` and `longjmp`, coroutine
//! and context switches that save a call's return address and later jump to
//! it).
//!
//! A function *escapes* its return address when it stores a value derived
//! from the link register's entry value to memory that is not a slot of its
//! own frame: `setjmp` saves the caller's return address in the buffer it is
//! given, and another function jumps there later with the stack pointer
//! `setjmp` saw. The address after the call to such a function is then a
//! resume point of the caller: the emitter gives the caller a landing-pad
//! dispatch case for it, and the dispatch table a pad entry, so the jump
//! unwinds the host stack to the frame whose stack pointer it carries
//! (`docs/abi.md` section 7).
//!
//! A function that escapes and itself calls an escaper (musl's
//! `sigsetjmp`, which saves its return address, calls `setjmp`, and
//! finishes the work after the call) has no frame of its own when the jump
//! comes: the code after the call runs again as a continuation, which
//! returns to the saved address in a frame that is still live. Such an
//! address is reported as a continuation, to be made a function entry,
//! instead of a resume point.
//!
//! An indirect call has no known target. When the address of any escaper is
//! taken anywhere in the program (a code address in data or computed in
//! code), every indirect call is treated as a call to an escaper, which is
//! sound; a program that never takes one's address gets no such pads.
//!
//! A forward dataflow over each function tracks, for every register, whether
//! it holds the stack pointer plus a constant and whether its value derives
//! from the link register. A call leaves the registers the frontend names
//! [`Frontend::preserved`] as they were; the others stop holding the stack
//! pointer plus a constant but keep what derives from the link register,
//! since a function may park its return address in a register it knows the
//! callee leaves alone (glibc's `__sigsetjmp` does around its call to
//! `__libc_arm_za_disable`).

use std::collections::{BTreeMap, BTreeSet};

use relift_guest::{Frontend, Module, RootKind};
use relift_ir::{BinOp, Block, Flow, Next, Op};

use crate::{Analysis, Function, Insn};

/// What the analysis found in a module.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Escapes {
    /// `(function entry, address)`: the return address of a call to an
    /// escaper in a function that is not one, to be added to that
    /// function's pads.
    pub resume_points: Vec<(u64, u64)>,
    /// Return addresses of calls to an escaper in an escaper, to be made
    /// function entries.
    pub continuations: BTreeSet<u64>,
}

#[derive(Clone, Copy, Default, PartialEq, Eq)]
struct V {
    sp: bool,
    link: bool,
    konst: bool,
}

impl V {
    fn join(self, o: V) -> V {
        V {
            sp: self.sp && o.sp,
            link: self.link || o.link,
            konst: false,
        }
    }
}

/// What one function does that the analysis cares about.
#[derive(Default)]
struct Summary {
    escapes: bool,
    calls: Vec<(u64, u64)>,
    indirect: Vec<u64>,
    tails: Vec<u64>,
}

fn state_after(b: &Block, entry: &[V], sum: &mut Summary) -> Vec<V> {
    let b = &relift_ir::simplify(b);
    let mut vals: Vec<V> = Vec::with_capacity(b.insts.len());
    let mut sets: Vec<(usize, V)> = Vec::new();
    for inst in &b.insts {
        let get = |v: relift_ir::Value| vals.get(v.index()).copied().unwrap_or_default();
        let v = match &inst.op {
            Op::Const(_) => V {
                konst: true,
                ..V::default()
            },
            Op::Get(r) => entry.get(usize::from(r.0)).copied().unwrap_or_default(),
            Op::Set(r, v) => {
                sets.push((usize::from(r.0), get(*v)));
                V::default()
            }
            Op::Bin(op @ (BinOp::Add | BinOp::Sub), x, y) => {
                let (x, y) = (get(*x), get(*y));
                V {
                    sp: (x.sp && y.konst) || (*op == BinOp::Add && y.sp && x.konst),
                    link: x.link || y.link,
                    konst: x.konst && y.konst,
                }
            }
            Op::Ext(_, x) | Op::Extract(x, 0) => get(*x),
            Op::Store { addr, value, .. } => {
                if get(*value).link && !get(*addr).sp {
                    sum.escapes = true;
                }
                V::default()
            }
            op => V {
                link: op.operands().iter().any(|v| get(*v).link),
                ..V::default()
            },
        };
        vals.push(v);
    }
    let mut out = entry.to_vec();
    for (r, v) in sets {
        if r < out.len() {
            out[r] = V { konst: false, ..v };
        }
    }
    out
}

fn summarize(
    fe: &dyn Frontend,
    f: &Function,
    known: &dyn Fn(u64) -> bool,
    via: &BTreeMap<u64, u64>,
) -> Summary {
    let regs = fe.regs().len();
    let sp = fe.stack_pointer().map(|r| usize::from(r.0));
    let link = fe.link_register().map(|r| usize::from(r.0));
    let preserved: BTreeSet<usize> = fe.preserved().iter().map(|r| usize::from(r.0)).collect();
    let mut start = vec![V::default(); regs];
    if let Some(s) = sp.filter(|&s| s < regs) {
        start[s].sp = true;
    }
    if let Some(l) = link.filter(|&l| l < regs) {
        start[l].link = true;
    }
    let mut sum = Summary::default();
    let mut states: BTreeMap<u64, Vec<V>> = BTreeMap::new();
    states.insert(f.entry, start);
    let mut work = vec![f.entry];
    while let Some(pc) = work.pop() {
        let Some(Insn::Lifted(b)) = f.insts.get(&pc) else {
            continue;
        };
        let entry = states[&pc].clone();
        let mut local = Summary::default();
        let mut out = state_after(b, &entry, &mut local);
        sum.escapes |= local.escapes;
        let mut next: Vec<u64> = Vec::new();
        match (b.exit.flow, b.exit.next) {
            (Flow::Call { ret }, n) => {
                match n {
                    Next::Const(t) if known(t) => sum.calls.push((ret, t)),
                    Next::Const(_) => {}
                    _ => match via.get(&pc) {
                        Some(&t) => sum.calls.push((ret, t)),
                        None => sum.indirect.push(ret),
                    },
                }
                for (r, v) in out.iter_mut().enumerate() {
                    if !preserved.contains(&r) {
                        *v = V {
                            link: v.link,
                            ..V::default()
                        };
                    }
                }
                next.push(ret);
            }
            (Flow::Return | Flow::Trap(_), _) => {}
            (Flow::Jump, Next::Const(t)) => {
                if f.insts.contains_key(&t) {
                    next.push(t);
                } else if known(t) {
                    sum.tails.push(t);
                }
            }
            (
                Flow::Jump,
                Next::Select {
                    taken, not_taken, ..
                },
            ) => {
                for t in [taken, not_taken] {
                    if f.insts.contains_key(&t) {
                        next.push(t);
                    } else if known(t) {
                        sum.tails.push(t);
                    }
                }
            }
            (Flow::Jump, Next::Dynamic(_)) => {
                if let Some(&t) = via.get(&pc) {
                    sum.tails.push(t);
                }
                next.extend(f.tables.get(&pc).into_iter().flatten().copied());
            }
        }
        for t in next {
            let merged = match states.get(&t) {
                None => out.clone(),
                Some(old) => old.iter().zip(&out).map(|(a, b)| a.join(*b)).collect(),
            };
            if states.get(&t) != Some(&merged) {
                states.insert(t, merged);
                work.push(t);
            }
        }
    }
    sum
}

/// One module of a program for [`return_escapes_linked`].
pub struct Linked<'a> {
    /// The module.
    pub module: &'a Module,
    /// What discovery found in it.
    pub analysis: &'a Analysis,
    /// Each loader-filled slot of the module (a global offset table word)
    /// and the function of the program the loader binds it to, as far as
    /// the symbol names the program defines say.
    pub slots: &'a BTreeMap<u64, u64>,
}

/// The resume points and continuations of `a`'s functions.
pub fn return_escapes(fe: &dyn Frontend, module: &Module, a: &Analysis) -> Escapes {
    let slots = BTreeMap::new();
    let linked = [Linked {
        module,
        analysis: a,
        slots: &slots,
    }];
    return_escapes_linked(fe, &linked).pop().unwrap_or_default()
}

/// The resume points and continuations of the functions of every module of
/// a program, one [`Escapes`] per module. A call to a procedure linkage
/// stub that jumps through a slot, and a call or jump through a slot, count
/// as going to the function the slot is bound to, so a `setjmp` in one
/// library makes the code after its call in another a resume point.
pub fn return_escapes_linked(fe: &dyn Frontend, mods: &[Linked<'_>]) -> Vec<Escapes> {
    if fe.link_register().is_none() {
        return vec![Escapes::default(); mods.len()];
    }
    let known: BTreeSet<u64> = mods
        .iter()
        .flat_map(|m| m.analysis.functions.keys().copied())
        .collect();
    let is_known = |a: u64| known.contains(&a);
    let mut sums: Vec<BTreeMap<u64, Summary>> = Vec::new();
    let mut escaped_targets: BTreeSet<u64> = BTreeSet::new();
    for m in mods {
        let slot_set: BTreeSet<u64> = m.slots.keys().copied().collect();
        let mut sm = BTreeMap::new();
        for (&e, f) in &m.analysis.functions {
            let uses = crate::slots::slot_uses(f, &slot_set, 8);
            escaped_targets.extend(uses.escaped.iter().filter_map(|s| m.slots.get(s).copied()));
            let via: BTreeMap<u64, u64> = uses
                .transfers
                .iter()
                .filter_map(|(pc, s)| Some((*pc, *m.slots.get(s)?)))
                .collect();
            sm.insert(e, summarize(fe, f, &is_known, &via));
        }
        sums.push(sm);
    }
    let mut escapers: BTreeSet<u64> = sums
        .iter()
        .flatten()
        .filter(|(_, s)| s.escapes)
        .map(|(&e, _)| e)
        .collect();
    loop {
        let more: Vec<u64> = sums
            .iter()
            .flatten()
            .filter(|(e, s)| !escapers.contains(e) && s.tails.iter().any(|t| escapers.contains(t)))
            .map(|(&e, _)| e)
            .collect();
        if more.is_empty() {
            break;
        }
        escapers.extend(more);
    }
    let mut taken: BTreeSet<u64> = escaped_targets;
    for m in mods {
        taken.extend(
            m.module
                .roots
                .iter()
                .filter(|r| r.kind == RootKind::Pointer)
                .map(|r| r.addr),
        );
        for f in m.analysis.functions.values() {
            taken.extend(crate::address_taken(m.module, fe.granule(), f));
        }
    }
    let escaper_taken = escapers.iter().any(|e| taken.contains(e));
    sums.iter()
        .map(|sm| {
            let mut out = Escapes::default();
            for (&g, s) in sm {
                if escaper_taken {
                    for &ret in &s.indirect {
                        if is_known(ret) {
                            continue;
                        }
                        if escapers.contains(&g) {
                            out.continuations.insert(ret);
                        } else {
                            out.resume_points.push((g, ret));
                        }
                    }
                }
                for &(ret, t) in &s.calls {
                    if !escapers.contains(&t) || is_known(ret) {
                        continue;
                    }
                    if escapers.contains(&g) {
                        out.continuations.insert(ret);
                    } else {
                        out.resume_points.push((g, ret));
                    }
                }
            }
            out
        })
        .collect()
}

/// Adds the resume points of `e` to the pads of the functions of `a`.
pub fn apply(a: &mut Analysis, e: &Escapes) {
    for &(g, ret) in &e.resume_points {
        if let Some(f) = a.functions.get_mut(&g)
            && f.insts.contains_key(&ret)
        {
            f.pads.insert(ret);
        }
    }
}

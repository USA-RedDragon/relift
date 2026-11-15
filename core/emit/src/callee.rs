//! Which callee-saved registers ([`Frontend::callee_saved`]) each lifted
//! function of a module provably returns with their entry values, so that
//! a caller keeps them in its locals across a direct call instead of
//! reloading them (`docs/abi.md` section 5.3).
//!
//! Each function is reduced to a [`Prog`]: per instruction, the registers
//! it writes as an expression over the registers it reads (a register plus
//! a constant, or unknown), the call it makes and where it goes. A forward
//! dataflow over a function's program gives, at every instruction, each
//! register as its value at the function's entry plus a constant, or
//! unknown. A register is kept when it holds its entry value at every
//! return and at every tail call to a function that keeps it. A call
//! makes every register the callee does not keep unknown; an indirect
//! call or tail call, a trap and a sync point make all of them unknown,
//! and so does a landing pad or other resume point, where the context
//! holds what the unwinder installed. Memory is never read: a register
//! restored from the stack is unknown. The whole module is solved as a
//! greatest fixpoint, every function starting as keeping everything; by
//! induction on the depth of calls, every normal return then has what its
//! function's result claims.

use std::collections::BTreeMap;

use relift_guest::Frontend;
use relift_ir::{BinOp, Flow, Next, Op, Width};

use crate::state::Regs;
use crate::{Function, Insn, Target};

#[cfg(test)]
mod tests;

/// A set of the frontend's callee-saved registers: bit `i` stands for
/// `Frontend::callee_saved()[i]`.
pub(crate) type Keeps = u64;

/// A register's value relative to the entry of the instruction or function
/// it is read in: register `.0` plus `.1`, or unknown.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Val {
    Reg(u16, i64),
    Unknown,
}

/// The call an instruction makes after its register writes.
#[derive(Clone, Copy, Debug)]
enum Call {
    No,
    /// A direct call or a call of a lifted function by name.
    To(u64),
    /// An indirect call, a trap or a sync point: everything is unknown
    /// after it.
    Any,
}

/// Where an instruction may go.
#[derive(Clone, Copy, Debug)]
enum Succ {
    /// The instruction with this index in the program.
    Edge(u32),
    /// A tail call by name to the function at this entry.
    Tail(u64),
    /// A tail call through the dispatch table.
    Any,
    Return,
}

#[derive(Clone, Copy, Debug)]
struct Node {
    /// The end of its writes in [`Prog::sets`].
    sets: u32,
    /// The end of its successors in [`Prog::succs`].
    succs: u32,
    call: Call,
}

/// One function as the analysis reads it.
#[derive(Debug, Default)]
pub(crate) struct Prog {
    nodes: Vec<Node>,
    sets: Vec<(u16, Val)>,
    succs: Vec<Succ>,
    entry: Option<u32>,
    /// Where the function resumes with the context the unwinder installed.
    seeds: Vec<u32>,
}

impl Prog {
    fn callees(&self) -> impl Iterator<Item = u64> + '_ {
        let calls = self.nodes.iter().filter_map(|n| match n.call {
            Call::To(t) => Some(t),
            _ => None,
        });
        let tails = self.succs.iter().filter_map(|s| match s {
            Succ::Tail(t) => Some(*t),
            _ => None,
        });
        calls.chain(tails)
    }
}

/// The value of one instruction of a block.
#[derive(Clone, Copy)]
enum Abs {
    Reg(u16, i64),
    Const(u128),
    Unknown,
}

/// `f` as the analysis reads it, with `targets` the module's direct-call
/// targets; `fault` says the emitter miscompiles it on purpose
/// (`Options::test_fault`), which keeps nothing.
pub(crate) fn program(
    fe: &dyn Frontend,
    f: &Function<'_>,
    targets: &BTreeMap<u64, Target>,
    fault: bool,
) -> Prog {
    let mut p = Prog::default();
    if fault {
        return p;
    }
    let index: BTreeMap<u64, u32> = f
        .insts
        .keys()
        .enumerate()
        .map(|(k, &pc)| (pc, k as u32))
        .collect();
    let clock = fe.clock().is_some();
    let jump = |t: u64| match index.get(&t) {
        Some(&k) => Succ::Edge(k),
        None if targets.get(&t).is_some_and(|t| t.lifted) => Succ::Tail(t),
        None => Succ::Any,
    };
    for (pc, i) in &f.insts {
        let Insn::Lifted { block, .. } = i else {
            // The frontend's undefined call never returns `RL_OK`.
            p.nodes.push(Node {
                sets: p.sets.len() as u32,
                succs: p.succs.len() as u32,
                call: Call::No,
            });
            continue;
        };
        let b = relift_ir::simplify(block);
        let mut vals: Vec<Abs> = Vec::with_capacity(b.insts.len());
        let first = p.sets.len();
        let mut guard = false;
        let mut sync = false;
        for inst in &b.insts {
            let get = |v: relift_ir::Value| vals.get(v.index()).copied().unwrap_or(Abs::Unknown);
            let w64 = inst.width == Some(Width::W64);
            let a = match &inst.op {
                Op::Const(k) => Abs::Const(k.get()),
                Op::Get(r) => Abs::Reg(r.0, 0),
                Op::Bin(op @ (BinOp::Add | BinOp::Sub), x, y) if w64 => {
                    match (get(*x), get(*y), op) {
                        (Abs::Reg(r, o), Abs::Const(c), BinOp::Add)
                        | (Abs::Const(c), Abs::Reg(r, o), BinOp::Add) => {
                            Abs::Reg(r, o.wrapping_add(c as u64 as i64))
                        }
                        (Abs::Reg(r, o), Abs::Const(c), _) => {
                            Abs::Reg(r, o.wrapping_sub(c as u64 as i64))
                        }
                        _ => Abs::Unknown,
                    }
                }
                Op::Set(r, v) => {
                    let val = match get(*v) {
                        Abs::Reg(s, o) => Val::Reg(s, o),
                        _ => Val::Unknown,
                    };
                    match p.sets[first..].iter_mut().find(|s| s.0 == r.0) {
                        Some(s) => s.1 = val,
                        None => p.sets.push((r.0, val)),
                    }
                    Abs::Unknown
                }
                Op::Guard(..) => {
                    guard = true;
                    Abs::Unknown
                }
                Op::Sync => {
                    sync = clock;
                    Abs::Unknown
                }
                _ => Abs::Unknown,
            };
            vals.push(a);
        }
        // A guard's slow path runs the instruction in the runtime, which
        // writes the registers the block writes, and goes on after it.
        if guard {
            for s in &mut p.sets[first..] {
                s.1 = Val::Unknown;
            }
            p.succs.push(jump(pc + u64::from(b.len)));
        }
        let mut call = if sync { Call::Any } else { Call::No };
        match (b.exit.flow, b.exit.next) {
            (Flow::Return, _) => p.succs.push(Succ::Return),
            (Flow::Call { ret }, Next::Const(t)) => {
                if !sync {
                    call = if targets.get(&t).is_some_and(|t| t.lifted) {
                        Call::To(t)
                    } else {
                        Call::Any
                    };
                }
                p.succs.push(jump(ret));
            }
            (Flow::Call { ret }, Next::Dynamic(_)) => {
                call = Call::Any;
                p.succs.push(jump(ret));
            }
            (Flow::Trap(_), Next::Const(t)) => {
                call = Call::Any;
                p.succs.push(jump(t));
            }
            (Flow::Call { .. }, Next::Select { .. }) | (Flow::Trap(_), _) => {}
            (Flow::Jump, Next::Const(t)) => p.succs.push(jump(t)),
            (
                Flow::Jump,
                Next::Select {
                    taken, not_taken, ..
                },
            ) => {
                p.succs.push(jump(taken));
                p.succs.push(jump(not_taken));
            }
            (Flow::Jump, Next::Dynamic(_)) => {
                for t in f.tables.get(pc).into_iter().flatten() {
                    if let Some(&k) = index.get(t) {
                        p.succs.push(Succ::Edge(k));
                    }
                }
                p.succs.push(Succ::Any);
            }
        }
        p.nodes.push(Node {
            sets: p.sets.len() as u32,
            succs: p.succs.len() as u32,
            call,
        });
    }
    p.entry = index.get(&f.entry).copied();
    let resumes = f
        .pads
        .iter()
        .copied()
        .chain(f.insts.values().filter_map(|i| match i {
            Insn::Lifted { block: b, .. } => match (b.exit.flow, b.exit.next) {
                (Flow::Call { ret }, Next::Const(t))
                    if targets.get(&t).is_some_and(|t| t.resume_point) =>
                {
                    Some(ret)
                }
                _ => None,
            },
            Insn::Failed(_) => None,
        }));
    p.seeds = resumes.filter_map(|r| index.get(&r).copied()).collect();
    p
}

/// What [`program`] reads of a frontend: the size of its register file and
/// its callee-saved registers, by index.
pub(crate) struct Shape {
    regs: usize,
    saved: Vec<u16>,
    /// The bit of each register in [`Keeps`], if it has one.
    bit: Vec<Option<u8>>,
}

impl Shape {
    pub(crate) fn of(fe: &dyn Frontend) -> Shape {
        let regs = fe.regs().len();
        let saved: Vec<u16> = fe
            .callee_saved()
            .iter()
            .map(|r| r.0)
            .filter(|&r| usize::from(r) < regs)
            .take(64)
            .collect();
        let mut bit = vec![None; regs];
        for (i, &r) in saved.iter().enumerate() {
            bit[usize::from(r)] = Some(i as u8);
        }
        Shape { regs, saved, bit }
    }

    /// Whether the frontend has callee-saved registers at all.
    pub(crate) fn any(&self) -> bool {
        !self.saved.is_empty()
    }

    fn all(&self) -> Keeps {
        if self.saved.len() == 64 {
            u64::MAX
        } else {
            (1u64 << self.saved.len()) - 1
        }
    }

    /// The registers of `keeps`, as a set over the register file.
    pub(crate) fn regs(&self, keeps: Keeps) -> Regs {
        let mut s = Regs::new(self.regs);
        for (i, &r) in self.saved.iter().enumerate() {
            if keeps & (1 << i) != 0 {
                s.insert(relift_ir::Reg(r));
            }
        }
        s
    }
}

/// What each function of `progs` keeps, by entry, with `targets` the
/// module's direct-call targets: a function keeps nothing that a callee
/// outside `progs` may change.
pub(crate) fn solve(shape: &Shape, progs: &BTreeMap<u64, Prog>) -> BTreeMap<u64, Keeps> {
    let all = shape.all();
    let mut keeps: BTreeMap<u64, Keeps> = progs.keys().map(|&e| (e, all)).collect();
    let mut callers: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
    for (&e, p) in progs {
        for t in p.callees() {
            if progs.contains_key(&t) {
                callers.entry(t).or_default().push(e);
            }
        }
    }
    for v in callers.values_mut() {
        v.sort_unstable();
        v.dedup();
    }
    let order = callees_first(progs);
    let mut queued: BTreeMap<u64, bool> = progs.keys().map(|&e| (e, true)).collect();
    let mut work: std::collections::VecDeque<u64> = order.into();
    let mut scratch = Scratch::default();
    while let Some(e) = work.pop_front() {
        queued.insert(e, false);
        let k = analyze(shape, &progs[&e], &keeps, &mut scratch) & keeps[&e];
        if k == keeps[&e] {
            continue;
        }
        keeps.insert(e, k);
        for &c in callers.get(&e).into_iter().flatten() {
            if !std::mem::replace(queued.get_mut(&c).expect("caller"), true) {
                work.push_back(c);
            }
        }
    }
    keeps
}

/// The entries of `progs` with every function after the functions it
/// calls, cycles aside: a depth-first post-order of the call graph.
fn callees_first(progs: &BTreeMap<u64, Prog>) -> Vec<u64> {
    let mut seen: BTreeMap<u64, bool> = BTreeMap::new();
    let mut order = Vec::with_capacity(progs.len());
    for &root in progs.keys() {
        if seen.contains_key(&root) {
            continue;
        }
        seen.insert(root, true);
        let mut stack: Vec<(u64, Vec<u64>)> = vec![(root, progs[&root].callees().collect())];
        while let Some((e, rest)) = stack.last_mut() {
            match rest.pop() {
                Some(t) if progs.contains_key(&t) && !seen.contains_key(&t) => {
                    seen.insert(t, true);
                    let next = progs[&t].callees().collect();
                    stack.push((t, next));
                }
                Some(_) => {}
                None => {
                    order.push(*e);
                    stack.pop();
                }
            }
        }
    }
    order
}

/// Buffers [`analyze`] reuses from one function to the next.
#[derive(Default)]
struct Scratch {
    state: Vec<Val>,
    reached: Vec<bool>,
    queued: Vec<bool>,
    work: Vec<u32>,
    out: Vec<Val>,
}

/// What `p` keeps, with `keeps` what every function keeps so far.
fn analyze(shape: &Shape, p: &Prog, keeps: &BTreeMap<u64, Keeps>, s: &mut Scratch) -> Keeps {
    let Some(entry) = p.entry else {
        return 0;
    };
    let n = shape.regs;
    let nodes = p.nodes.len();
    s.state.clear();
    s.state.resize(n * nodes, Val::Unknown);
    s.reached.clear();
    s.reached.resize(nodes, false);
    s.queued.clear();
    s.queued.resize(nodes, false);
    s.work.clear();
    let e = entry as usize;
    for (r, v) in s.state[e * n..(e + 1) * n].iter_mut().enumerate() {
        *v = Val::Reg(r as u16, 0);
    }
    s.reached[e] = true;
    s.queued[e] = true;
    s.work.push(entry);
    for &k in &p.seeds {
        let k = k as usize;
        s.state[k * n..(k + 1) * n].fill(Val::Unknown);
        s.reached[k] = true;
        if !s.queued[k] {
            s.queued[k] = true;
            s.work.push(k as u32);
        }
    }
    let keeps_of = |t: u64| keeps.get(&t).copied().unwrap_or(0);
    let mut result = shape.all();
    let held = |out: &[Val]| -> Keeps {
        let mut m = 0;
        for (i, &r) in shape.saved.iter().enumerate() {
            if out[usize::from(r)] == Val::Reg(r, 0) {
                m |= 1 << i;
            }
        }
        m
    };
    while let Some(k) = s.work.pop() {
        let k = k as usize;
        s.queued[k] = false;
        s.out.clear();
        s.out.extend_from_slice(&s.state[k * n..(k + 1) * n]);
        let node = p.nodes[k];
        let sets_from = if k == 0 { 0 } else { p.nodes[k - 1].sets } as usize;
        for &(r, v) in &p.sets[sets_from..node.sets as usize] {
            s.out[usize::from(r)] = match v {
                Val::Reg(q, o) => match s.state[k * n + usize::from(q)] {
                    Val::Reg(b, o2) => Val::Reg(b, o2.wrapping_add(o)),
                    Val::Unknown => Val::Unknown,
                },
                Val::Unknown => Val::Unknown,
            };
        }
        match node.call {
            Call::No => {}
            Call::To(t) => {
                let m = keeps_of(t);
                for (v, b) in s.out.iter_mut().zip(&shape.bit) {
                    if !b.is_some_and(|b| m & (1 << b) != 0) {
                        *v = Val::Unknown;
                    }
                }
            }
            Call::Any => s.out.fill(Val::Unknown),
        }
        let succs_from = if k == 0 { 0 } else { p.nodes[k - 1].succs } as usize;
        for &succ in &p.succs[succs_from..node.succs as usize] {
            match succ {
                Succ::Return => result &= held(&s.out),
                Succ::Tail(t) => result &= held(&s.out) & keeps_of(t),
                Succ::Any => result = 0,
                Succ::Edge(j) => {
                    let j = j as usize;
                    let dst = &mut s.state[j * n..(j + 1) * n];
                    let changed = if s.reached[j] {
                        let mut changed = false;
                        for (d, &o) in dst.iter_mut().zip(&s.out) {
                            if *d != o && *d != Val::Unknown {
                                *d = Val::Unknown;
                                changed = true;
                            }
                        }
                        changed
                    } else {
                        dst.copy_from_slice(&s.out);
                        s.reached[j] = true;
                        true
                    };
                    if changed && !s.queued[j] {
                        s.queued[j] = true;
                        s.work.push(j as u32);
                    }
                }
            }
            if result == 0 {
                return 0;
            }
        }
    }
    result
}

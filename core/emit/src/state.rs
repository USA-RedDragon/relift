//! Which register locals each point of a piece stores to the context and
//! reloads from it.
//!
//! A piece keeps guest registers in C locals. The context is brought up to
//! date only where something outside the piece can see it: before a call,
//! a trap, a guard's slow path, a sync point, a return and a jump out of
//! the piece. There the piece stores the registers that may be dirty, those
//! written since the locals and the context last agreed on every path to
//! that point (a forward dataflow). After a call, a trap, a slow path or a
//! sync point the context may have changed, and the piece reloads only the
//! registers that are live there: read, or stored as dirty, before every
//! path writes them (a backward dataflow over the stores just found). A
//! register outside both sets is not touched: its context field already
//! holds its value, and its local is never read before it is written.

use std::collections::HashMap;

use relift_ir::{Block, Flow, Next, Op, Reg};

use crate::func::Env;

/// A set of registers, by index, as bits.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Regs(Vec<u64>);

impl Regs {
    pub(crate) fn new(regs: usize) -> Regs {
        Regs(vec![0; regs.div_ceil(64).max(1)])
    }

    pub(crate) fn insert(&mut self, r: Reg) {
        let i = r.index();
        self.0[i / 64] |= 1 << (i % 64);
    }

    pub(crate) fn contains(&self, r: Reg) -> bool {
        let i = r.index();
        self.0.get(i / 64).is_some_and(|w| w & (1 << (i % 64)) != 0)
    }

    /// Adds `o`; whether that changed the set.
    pub(crate) fn union(&mut self, o: &Regs) -> bool {
        let mut changed = false;
        for (a, b) in self.0.iter_mut().zip(&o.0) {
            let n = *a | b;
            changed |= n != *a;
            *a = n;
        }
        changed
    }

    fn with(&self, o: &Regs) -> Regs {
        let mut s = self.clone();
        s.union(o);
        s
    }

    fn and(&self, o: &Regs) -> Regs {
        Regs(self.0.iter().zip(&o.0).map(|(a, b)| a & b).collect())
    }

    fn is_empty(&self) -> bool {
        self.0.iter().all(|w| *w == 0)
    }

    pub(crate) fn without(&self, o: &Regs) -> Regs {
        Regs(self.0.iter().zip(&o.0).map(|(a, b)| a & !b).collect())
    }
}

/// What one instruction of a piece reads and writes, and where it goes.
struct Shape {
    /// Registers its operations read, before any of its writes.
    gets: Regs,
    /// Registers it writes, at its end.
    sets: Regs,
    /// Whether it has a guard, whose slow path stores and reloads.
    guard: bool,
    /// Whether it ends in a sync point.
    sync: bool,
    /// Whether it has no block and only stores and leaves.
    failed: bool,
}

/// The stores and reloads of one piece.
pub(crate) struct Plan {
    at: HashMap<u64, usize>,
    shapes: Vec<Shape>,
    dirty_in: Vec<Regs>,
    live_in: Vec<Regs>,
    exit_live: Vec<Regs>,
    /// The registers each call keeps in its locals ([`crate::callee`]),
    /// with the return address: a direct call of a lifted function that
    /// returns them unchanged.
    kept: Vec<Option<(u64, Regs)>>,
    lp_reg: Option<Reg>,
    empty: Regs,
    /// Whether the guards share slow paths (each of at most
    /// `SHARED_GUARDS` guards), and what each stores and reloads.
    shared: Option<(Regs, Regs)>,
}

impl Plan {
    fn local(&self, t: u64) -> Option<usize> {
        self.at.get(&t).copied()
    }

    /// The registers that may be dirty where the instruction at `pc`
    /// starts: what a guard or a failed instruction stores.
    pub(crate) fn dirty_in(&self, pc: u64) -> &Regs {
        self.at.get(&pc).map_or(&self.empty, |&k| &self.dirty_in[k])
    }

    /// The registers that may be dirty where the instruction at `pc` ends,
    /// after its writes: what its exit stores.
    pub(crate) fn dirty_out(&self, pc: u64) -> Regs {
        self.at.get(&pc).map_or(self.empty.clone(), |&k| {
            self.dirty_in[k].with(&self.shapes[k].sets)
        })
    }

    /// The registers to load where the piece continues at `t` with the
    /// context up to date: none when `t` is outside the piece.
    pub(crate) fn live_at(&self, t: u64) -> &Regs {
        self.local(t).map_or(&self.empty, |k| &self.live_in[k])
    }

    /// The registers to reload after the sync point that ends `pc`.
    pub(crate) fn exit_live(&self, pc: u64) -> &Regs {
        self.at
            .get(&pc)
            .map_or(&self.empty, |&k| &self.exit_live[k])
    }

    /// The registers the call that ends `pc` keeps in its locals: they are
    /// live across it and not reloaded after it.
    pub(crate) fn kept(&self, pc: u64) -> &Regs {
        self.at
            .get(&pc)
            .and_then(|&k| self.kept[k].as_ref())
            .map_or(&self.empty, |k| &k.1)
    }

    pub(crate) fn none(&self) -> &Regs {
        &self.empty
    }

    /// The stores and reloads of the slow paths the piece's guards share,
    /// when they share them.
    pub(crate) fn shared(&self) -> Option<&(Regs, Regs)> {
        self.shared.as_ref()
    }
}

/// The plan of the piece over `pcs` (all of them in the piece, in address
/// order); `clock` says whether its sync points are emitted.
pub(crate) fn plan(env: &Env<'_>, pcs: &[u64], clock: bool, lp_reg: Option<Reg>) -> Plan {
    let fe = env.fe;
    let n_regs = fe.regs().len();
    let empty = Regs::new(n_regs);
    let at: HashMap<u64, usize> = pcs.iter().enumerate().map(|(k, &pc)| (pc, k)).collect();
    let fp_state = fe.fp_state();
    let shapes: Vec<Shape> = pcs
        .iter()
        .map(|pc| {
            let mut s = Shape {
                gets: empty.clone(),
                sets: empty.clone(),
                guard: false,
                sync: false,
                failed: false,
            };
            let Some(b) = env.blocks.get(pc) else {
                s.failed = true;
                return s;
            };
            for i in &b.insts {
                match i.op {
                    Op::Get(r) => s.gets.insert(r),
                    Op::Set(r, _) => s.sets.insert(r),
                    Op::Fp(..) => fp_state.iter().for_each(|&r| s.gets.insert(r)),
                    Op::Guard(_, code) => {
                        s.guard = true;
                        if let Some(r) = fe.guard_state(code) {
                            s.gets.insert(r);
                        }
                    }
                    _ => {}
                }
            }
            s.sync = clock && matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync));
            s
        })
        .collect();
    let mut p = Plan {
        at,
        shapes,
        dirty_in: vec![empty.clone(); pcs.len()],
        live_in: vec![empty.clone(); pcs.len()],
        exit_live: vec![empty.clone(); pcs.len()],
        kept: pcs
            .iter()
            .map(|pc| kept(env, env.blocks.get(pc)?))
            .collect(),
        lp_reg,
        empty,
        shared: None,
    };
    let blocks: Vec<Option<&Block>> = pcs.iter().map(|pc| env.blocks.get(pc)).collect();
    // The in-piece successors an instruction reaches with its locals as
    // they are; a call, trap or slow path reloads before it goes on, so
    // its continuation starts clean.
    let jumps: Vec<Vec<usize>> = blocks
        .iter()
        .map(|b| match b {
            Some(b) => targets(env, b)
                .into_iter()
                .filter_map(|t| p.local(t))
                .collect(),
            None => Vec::new(),
        })
        .collect();
    let mut work: Vec<usize> = (0..pcs.len()).rev().collect();
    let mut queued = vec![true; pcs.len()];
    while let Some(k) = work.pop() {
        queued[k] = false;
        let out = p.dirty_in[k].with(&p.shapes[k].sets);
        for &j in &jumps[k] {
            if p.dirty_in[j].union(&out) && !queued[j] {
                queued[j] = true;
                work.push(j);
            }
        }
    }
    // With more than one guard, the guards jump to one slow path that
    // stores what any of them may find dirty; each guard then reads all of
    // it, so those locals hold their values wherever a guard runs.
    let guards: usize = blocks
        .iter()
        .flatten()
        .map(|b| {
            b.insts
                .iter()
                .filter(|i| matches!(i.op, Op::Guard(..)))
                .count()
        })
        .sum();
    let shared_store = (guards > 1).then(|| {
        let mut u = p.empty.clone();
        for (k, s) in p.shapes.iter().enumerate() {
            if s.guard {
                u.union(&p.dirty_in[k]);
            }
        }
        u
    });
    let mut preds: Vec<Vec<usize>> = vec![Vec::new(); pcs.len()];
    for (k, js) in jumps.iter().enumerate() {
        for &j in js {
            preds[j].push(k);
        }
        if let Some(j) = p.kept[k].as_ref().and_then(|(ret, _)| p.local(*ret)) {
            preds[j].push(k);
        }
    }
    let mut work: Vec<usize> = (0..pcs.len()).collect();
    let mut queued = vec![true; pcs.len()];
    while let Some(k) = work.pop() {
        queued[k] = false;
        let s = &p.shapes[k];
        let (live, exit) = if s.failed {
            (p.dirty_in[k].clone(), p.empty.clone())
        } else {
            let out = p.dirty_in[k].with(&s.sets);
            let exit = exit_live(env, &p, blocks[k].expect("block"), &out);
            let end = if s.sync {
                exit.with(&out)
            } else {
                exit.clone()
            };
            let mut live = s.gets.with(&end.without(&s.sets));
            if s.guard {
                live.union(shared_store.as_ref().unwrap_or(&p.dirty_in[k]));
            }
            (live, exit)
        };
        p.exit_live[k] = exit;
        if p.live_in[k] != live {
            p.live_in[k] = live;
            for &q in &preds[k] {
                if !queued[q] {
                    queued[q] = true;
                    work.push(q);
                }
            }
        }
    }
    p.shared = shared_store.map(|u| {
        let mut r = p.empty.clone();
        for (k, b) in blocks.iter().enumerate() {
            if let Some(b) = b
                && p.shapes[k].guard
            {
                r.union(p.live_at(b.pc + u64::from(b.len)));
            }
        }
        (u, r)
    });
    p
}

/// The constant targets the exit of `b` jumps to without a call between.
fn targets(env: &Env<'_>, b: &Block) -> Vec<u64> {
    match (b.exit.flow, b.exit.next) {
        (Flow::Jump, Next::Const(t)) => vec![t],
        (Flow::Call { .. }, Next::Const(t)) if env.trampoline => vec![t],
        (
            Flow::Jump,
            Next::Select {
                taken, not_taken, ..
            },
        ) => vec![taken, not_taken],
        (Flow::Jump, Next::Dynamic(_)) => env
            .f
            .tables
            .get(&b.pc)
            .into_iter()
            .flatten()
            .copied()
            .collect(),
        _ => Vec::new(),
    }
}

/// The registers live where `b` leaves, after its writes, with `out` the
/// registers that may be dirty there.
fn exit_live(env: &Env<'_>, p: &Plan, b: &Block, out: &Regs) -> Regs {
    let side = |t: u64| match p.local(t) {
        Some(k) => p.live_in[k].clone(),
        None => out.clone(),
    };
    match (b.exit.flow, b.exit.next) {
        (Flow::Return, _) => out.clone(),
        (Flow::Call { .. }, Next::Const(t)) if env.trampoline => side(t),
        (Flow::Call { .. } | Flow::Trap(_), _) => {
            let mut s = out.clone();
            if let Some(r) = p.lp_reg {
                s.insert(r);
            }
            if let Some((ret, k)) = p.at.get(&b.pc).and_then(|&i| p.kept[i].as_ref()) {
                s.union(&side_kept(p, *ret, k));
            }
            s
        }
        (Flow::Jump, Next::Const(t)) => side(t),
        (
            Flow::Jump,
            Next::Select {
                taken, not_taken, ..
            },
        ) => side(taken).with(&side(not_taken)),
        (Flow::Jump, Next::Dynamic(_)) => {
            let mut s = out.clone();
            for t in targets(env, b) {
                s.union(&side(t));
            }
            s
        }
    }
}

/// The registers of `k` live at `ret`: those a call that keeps `k` needs in
/// its locals before it.
fn side_kept(p: &Plan, ret: u64, k: &Regs) -> Regs {
    p.live_at(ret).and(k)
}

/// The registers the call that ends `b` keeps, with its return address:
/// the callee-saved registers its callee, a lifted function called by name,
/// returns unchanged; `None` when that is none.
fn kept(env: &Env<'_>, b: &Block) -> Option<(u64, Regs)> {
    let (Flow::Call { ret }, Next::Const(t)) = (b.exit.flow, b.exit.next) else {
        return None;
    };
    let target = env.targets.get(&t)?;
    if !target.lifted || target.keeps == 0 {
        return None;
    }
    let k = env.shape.regs(target.keeps);
    (!k.is_empty()).then_some((ret, k))
}

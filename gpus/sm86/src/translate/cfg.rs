//! Control flow: basic blocks from the branches, loops from the dominator
//! tree, and structured SPIR-V from both.
//!
//! Each selection merges at its immediate post-dominator inside the
//! innermost loop; back edges become continues and loop exits become
//! breaks. EXIT and KILL are not blocks: every edge to them returns (after
//! the epilogue) or kills in place. BSSY/BSYNC only mark the reconvergence
//! that the structure already gives, so they emit nothing. A graph this
//! cannot express (irreducible, a loop with several exits, an exit out of
//! more than one loop) is rejected with the reason.

use super::{Code, Ctx, Res, bad};
use crate::Operand;
use crate::program::Stage;
use crate::spirv::op;
use std::collections::{HashMap, HashSet};

const EXIT: usize = usize::MAX;
const KILL: usize = usize::MAX - 1;

#[derive(Clone, Debug)]
enum Term {
    Jump(usize),
    Branch(Vec<Operand>, usize, usize),
}

#[derive(Clone, Debug)]
struct Block {
    first: usize,
    end: usize,
    term: Term,
}

fn terminal(b: usize) -> bool {
    b == EXIT || b == KILL
}

struct Graph {
    blocks: Vec<Block>,
    succs: Vec<Vec<usize>>,
    headers: HashMap<usize, (Vec<bool>, Option<usize>)>,
    pdoms: HashMap<Option<usize>, Vec<Option<usize>>>,
}

/// The branch condition: the guard and the predicates the instruction
/// itself names, all of which must hold.
fn conditions(i: &crate::Inst, w: crate::Word) -> Res<Vec<Operand>> {
    let mut c = Vec::new();
    if let Some(g) = &i.guard {
        c.push(g.clone());
    }
    for o in &i.ops {
        match o {
            Operand::Pred(n, not) => c.push(Operand::Pred(*n, *not)),
            Operand::UReg(_, m) if i.mnemonic == "BRA" && w.bit(91) => {
                let n = w.get(24, 3) as u8;
                c.push(Operand::UPred(n, m.not));
            }
            Operand::Target(_) => {}
            o => return bad(format!("{} operand {o}", i.mnemonic)),
        }
    }
    c.retain(|p| !matches!(p, Operand::Pred(7, false) | Operand::UPred(7, false)));
    Ok(c)
}

fn build(code: &Code) -> Res<Vec<Block>> {
    let n = code.insts.len();
    let mut end = n;
    while end > 0 && code.insts[end - 1].mnemonic == "BRA" {
        let i = &code.insts[end - 1];
        if !matches!(i.ops.as_slice(), [Operand::Target(t)] if *t == 16 * (end as i64 - 1)) {
            break;
        }
        end -= 1;
    }
    let mut leaders = vec![false; end + 1];
    leaders[0] = true;
    leaders[end] = true;
    for (k, i) in code.insts[..end].iter().enumerate() {
        match i.mnemonic {
            "BRA" => {
                if i.mods.iter().any(|m| *m != "U") {
                    return bad(format!("BRA.{}", i.mods.join(".")));
                }
                let Some(Operand::Target(t)) = i.ops.last() else {
                    return bad("BRA without a target");
                };
                let t = usize::try_from(*t / 16)
                    .ok()
                    .filter(|k| *k < end && *t % 16 == 0);
                let Some(t) = t else {
                    return bad("branch target outside the program");
                };
                leaders[t] = true;
                leaders[k + 1] = true;
            }
            "EXIT" | "KILL" => leaders[k + 1] = true,
            "BRX" | "BRXU" | "JMP" | "JMX" | "CALL" | "RET" | "BPT" | "RTT"
            | "WARPSYNC.EXCLUSIVE" => {
                return bad(format!("control {}", i.mnemonic));
            }
            _ => {}
        }
    }
    let starts: Vec<usize> = (0..end).filter(|k| leaders[*k]).collect();
    let index: HashMap<usize, usize> = starts.iter().enumerate().map(|(b, s)| (*s, b)).collect();
    let mut blocks = Vec::new();
    for (b, &s) in starts.iter().enumerate() {
        let stop = starts.get(b + 1).copied().unwrap_or(end);
        let last = &code.insts[stop - 1];
        let fall = if stop == end { None } else { Some(b + 1) };
        let term_inst = matches!(last.mnemonic, "BRA" | "EXIT" | "KILL");
        let term = if term_inst {
            let conds = conditions(last, code.words[stop - 1])?;
            let to = match last.mnemonic {
                "EXIT" => EXIT,
                "KILL" => KILL,
                _ => match last.ops.last() {
                    Some(Operand::Target(t)) => index[&((*t / 16) as usize)],
                    _ => return bad("BRA without a target"),
                },
            };
            if conds.is_empty() {
                Term::Jump(to)
            } else {
                let Some(f) = fall else {
                    return bad("conditional branch falls off the end");
                };
                Term::Branch(conds, to, f)
            }
        } else {
            match fall {
                Some(f) => Term::Jump(f),
                None => return bad("code falls off the end"),
            }
        };
        blocks.push(Block {
            first: s,
            end: if term_inst { stop - 1 } else { stop },
            term,
        });
    }
    Ok(blocks)
}

/// Immediate dominators of a graph given by successor lists, by Cooper,
/// Harvey and Kennedy's iteration; `None` for nodes `entry` cannot reach.
fn idoms(succ: &[Vec<usize>], entry: usize) -> Vec<Option<usize>> {
    let n = succ.len();
    let mut post = vec![usize::MAX; n];
    let mut order = Vec::with_capacity(n);
    let mut seen = vec![false; n];
    let mut stack = vec![(entry, 0usize)];
    seen[entry] = true;
    while let Some(&mut (u, ref mut k)) = stack.last_mut() {
        if *k < succ[u].len() {
            let v = succ[u][*k];
            *k += 1;
            if !seen[v] {
                seen[v] = true;
                stack.push((v, 0));
            }
        } else {
            post[u] = order.len();
            order.push(u);
            stack.pop();
        }
    }
    let mut preds = vec![Vec::new(); n];
    for (u, ss) in succ.iter().enumerate() {
        if seen[u] {
            for &v in ss {
                preds[v].push(u);
            }
        }
    }
    let mut idom: Vec<Option<usize>> = vec![None; n];
    idom[entry] = Some(entry);
    let mut changed = true;
    while changed {
        changed = false;
        for &b in order.iter().rev() {
            if b == entry {
                continue;
            }
            let mut new: Option<usize> = None;
            for &p in &preds[b] {
                if idom[p].is_none() {
                    continue;
                }
                new = Some(match new {
                    None => p,
                    Some(mut a) => {
                        let mut q = p;
                        while a != q {
                            while post[a] < post[q] {
                                a = idom[a].unwrap_or(entry);
                            }
                            while post[q] < post[a] {
                                q = idom[q].unwrap_or(entry);
                            }
                        }
                        a
                    }
                });
            }
            if new.is_some() && idom[b] != new {
                idom[b] = new;
                changed = true;
            }
        }
    }
    idom
}

fn dominates(idom: &[Option<usize>], a: usize, mut b: usize) -> bool {
    loop {
        if a == b {
            return true;
        }
        match idom[b] {
            Some(p) if p != b => b = p,
            _ => return false,
        }
    }
}

impl Graph {
    fn new(blocks: Vec<Block>) -> Res<Graph> {
        let n = blocks.len();
        let succs: Vec<Vec<usize>> = blocks
            .iter()
            .map(|b| {
                let mut v = match &b.term {
                    Term::Jump(t) => vec![*t],
                    Term::Branch(_, t, f) => vec![*t, *f],
                };
                v.retain(|t| !terminal(*t));
                v.dedup();
                v
            })
            .collect();
        let idom = idoms(&succs, 0);
        let mut latches: HashMap<usize, Vec<usize>> = HashMap::new();
        let mut state = vec![0u8; n];
        let mut stack = vec![(0usize, 0usize)];
        state[0] = 1;
        while let Some(&mut (u, ref mut k)) = stack.last_mut() {
            if *k < succs[u].len() {
                let v = succs[u][*k];
                *k += 1;
                match state[v] {
                    0 => {
                        state[v] = 1;
                        stack.push((v, 0));
                    }
                    1 => {
                        if !dominates(&idom, v, u) {
                            return bad("irreducible control flow");
                        }
                        latches.entry(v).or_default().push(u);
                    }
                    _ => {}
                }
            } else {
                state[u] = 2;
                stack.pop();
            }
        }
        for (u, ss) in succs.iter().enumerate() {
            for &v in ss {
                if idom[u].is_some()
                    && dominates(&idom, v, u)
                    && !latches.get(&v).is_some_and(|l| l.contains(&u))
                {
                    latches.entry(v).or_default().push(u);
                }
            }
        }
        let mut preds = vec![Vec::new(); n];
        for (u, ss) in succs.iter().enumerate() {
            for &v in ss {
                preds[v].push(u);
            }
        }
        let mut headers = HashMap::new();
        for (&h, ls) in &latches {
            let mut body = vec![false; n];
            body[h] = true;
            let mut work: Vec<usize> = ls.clone();
            while let Some(u) = work.pop() {
                if body[u] {
                    continue;
                }
                body[u] = true;
                work.extend(preds[u].iter().copied());
            }
            let mut exits: Vec<usize> = Vec::new();
            for u in (0..n).filter(|u| body[*u]) {
                for &s in &succs[u] {
                    if !body[s] && !exits.contains(&s) {
                        exits.push(s);
                    }
                }
            }
            if exits.len() > 1 {
                return bad(format!("loop with {} exits", exits.len()));
            }
            headers.insert(h, (body, exits.first().copied()));
        }
        Ok(Graph {
            blocks,
            succs,
            headers,
            pdoms: HashMap::new(),
        })
    }

    /// Immediate post-dominator of `b` among the blocks of loop `ctx` (the
    /// whole program for `None`), where leaving the loop or going round it
    /// ends every path; `None` if paths from `b` share no block.
    fn ipdom(&mut self, ctx: Option<usize>, b: usize) -> Option<usize> {
        if !self.pdoms.contains_key(&ctx) {
            let n = self.blocks.len();
            let end = n;
            let mut rev = vec![Vec::new(); n + 1];
            let body = ctx.map(|h| self.headers[&h].0.clone());
            for u in 0..n {
                if body.as_ref().is_some_and(|bd| !bd[u]) {
                    continue;
                }
                let mut any = false;
                for &s in &self.succs[u] {
                    let sink = Some(s) == ctx || body.as_ref().is_some_and(|bd| !bd[s]);
                    any = true;
                    if sink {
                        rev[end].push(u);
                    } else {
                        rev[s].push(u);
                    }
                }
                if !any {
                    rev[end].push(u);
                }
            }
            let id = idoms(&rev, end);
            let v = id
                .into_iter()
                .take(n)
                .map(|p| p.filter(|p| *p != end))
                .collect();
            self.pdoms.insert(ctx, v);
        }
        self.pdoms[&ctx][b]
    }
}

struct Lp {
    header: usize,
    exit: Option<usize>,
    cont: u32,
    brk: u32,
    body: Vec<bool>,
}

enum Dest {
    Label(u32),
    Ret,
    Kill,
    Inline(usize),
}

struct Emit<'a> {
    g: Graph,
    code: &'a Code,
    used: HashSet<u32>,
    emitted: usize,
}

impl Emit<'_> {
    fn resolve(&self, t: usize, follow: Option<(usize, u32)>, lp: Option<&Lp>) -> Res<Dest> {
        if t == EXIT {
            return Ok(Dest::Ret);
        }
        if t == KILL {
            return Ok(Dest::Kill);
        }
        if let Some(l) = lp {
            if t == l.header {
                return Ok(Dest::Label(l.cont));
            }
            if Some(t) == l.exit {
                return Ok(Dest::Label(l.brk));
            }
        }
        if let Some((f, fl)) = follow
            && f == t
        {
            return Ok(Dest::Label(fl));
        }
        if let Some(l) = lp
            && !l.body[t]
        {
            return bad("branch out of more than one loop");
        }
        Ok(Dest::Inline(t))
    }

    fn br(&mut self, c: &mut Ctx, l: u32) {
        self.used.insert(l);
        c.m.emit_void(op::BRANCH, &[l]);
    }

    fn terminal(&mut self, c: &mut Ctx, d: &Dest) -> Res<()> {
        match d {
            Dest::Ret => {
                c.epilogue();
                c.m.emit_void(op::RETURN, &[]);
            }
            Dest::Kill => {
                if c.stage != Stage::Fragment {
                    return bad("KILL outside a fragment shader");
                }
                c.m.emit_void(op::KILL, &[]);
            }
            _ => unreachable!(),
        }
        Ok(())
    }

    fn content(&mut self, c: &mut Ctx, b: usize) -> Res<()> {
        let blk = &self.g.blocks[b];
        for k in blk.first..blk.end {
            c.guarded(&self.code.insts[k], self.code.words[k], 16 * k as u64)?;
        }
        Ok(())
    }

    fn cond(&mut self, c: &mut Ctx, conds: &[Operand]) -> Res<u32> {
        let mut acc: Option<u32> = None;
        for p in conds {
            let v = c.pred_operand(p)?;
            acc = Some(match acc {
                None => v,
                Some(a) => {
                    let t = c.boolt();
                    c.m.emit(op::LOGICAL_AND, t, &[a, v])
                }
            });
        }
        Ok(acc.unwrap_or_else(|| c.m.c_bool(true)))
    }

    fn region(
        &mut self,
        c: &mut Ctx,
        mut cur: usize,
        mut label: u32,
        follow: Option<(usize, u32)>,
        lp: Option<&Lp>,
    ) -> Res<()> {
        loop {
            self.emitted += 1;
            if self.emitted > 4 * self.g.blocks.len() + 64 {
                return bad("control flow needs too much duplication");
            }
            if let Some((body, exit)) = self.g.headers.get(&cur).cloned()
                && lp.is_none_or(|l| l.header != cur)
            {
                let h = c.m.id();
                let merge = c.m.id();
                let cont = c.m.id();
                let bodyl = c.m.id();
                c.m.emit_void(op::LABEL, &[label]);
                self.br(c, h);
                c.m.emit_void(op::LABEL, &[h]);
                c.m.emit_void(op::LOOP_MERGE, &[merge, cont, 0]);
                self.br(c, bodyl);
                let inner = Lp {
                    header: cur,
                    exit,
                    cont,
                    brk: merge,
                    body,
                };
                self.region(c, cur, bodyl, None, Some(&inner))?;
                c.m.emit_void(op::LABEL, &[cont]);
                self.br(c, h);
                let Some(e) = exit else {
                    c.m.emit_void(op::LABEL, &[merge]);
                    c.m.emit_void(op::UNREACHABLE, &[]);
                    return Ok(());
                };
                match self.resolve(e, follow, lp)? {
                    Dest::Inline(e) => {
                        cur = e;
                        label = merge;
                        continue;
                    }
                    Dest::Label(x) => {
                        c.m.emit_void(op::LABEL, &[merge]);
                        self.br(c, x);
                        return Ok(());
                    }
                    d => {
                        c.m.emit_void(op::LABEL, &[merge]);
                        return self.terminal(c, &d);
                    }
                }
            }
            c.m.emit_void(op::LABEL, &[label]);
            self.content(c, cur)?;
            let term = self.g.blocks[cur].term.clone();
            let (conds, t, f) = match term {
                Term::Jump(t) => match self.resolve(t, follow, lp)? {
                    Dest::Inline(t) => {
                        let l = c.m.id();
                        self.br(c, l);
                        cur = t;
                        label = l;
                        continue;
                    }
                    Dest::Label(x) => {
                        self.br(c, x);
                        return Ok(());
                    }
                    d => return self.terminal(c, &d),
                },
                Term::Branch(conds, t, f) => (conds, t, f),
            };
            let cv = self.cond(c, &conds)?;
            let pd = self.g.ipdom(lp.map(|l| l.header), cur);
            let real = pd.filter(|&m| {
                follow.is_none_or(|(f, _)| f != m)
                    && lp.is_none_or(|l| l.header != m && l.exit != Some(m))
            });
            let merge = c.m.id();
            let arm_follow = match real {
                Some(r) => Some((r, merge)),
                None => follow.map(|(f, _)| (f, merge)),
            };
            let mut arms = Vec::new();
            let mut labels = [0u32; 2];
            for (k, &tg) in [t, f].iter().enumerate() {
                let d = if Some(tg) == real {
                    Dest::Label(merge)
                } else {
                    self.resolve(tg, arm_follow, lp)?
                };
                labels[k] = match d {
                    Dest::Label(x) => x,
                    d => {
                        let l = c.m.id();
                        arms.push((l, d));
                        l
                    }
                };
                self.used.insert(labels[k]);
            }
            c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
            c.m.emit_void(op::BRANCH_CONDITIONAL, &[cv, labels[0], labels[1]]);
            for (l, d) in arms {
                match d {
                    Dest::Inline(b) => self.region(c, b, l, arm_follow, lp)?,
                    d => {
                        c.m.emit_void(op::LABEL, &[l]);
                        self.terminal(c, &d)?;
                    }
                }
            }
            match real {
                Some(r) => {
                    cur = r;
                    label = merge;
                }
                None => {
                    c.m.emit_void(op::LABEL, &[merge]);
                    match follow {
                        Some((_, fl)) if self.used.contains(&merge) => self.br(c, fl),
                        _ => c.m.emit_void(op::UNREACHABLE, &[]),
                    }
                    return Ok(());
                }
            }
        }
    }
}

/// Emits the program's body, starting with label `first`.
pub(crate) fn emit(c: &mut Ctx, code: &Code, first: u32) -> Res<()> {
    let blocks = build(code)?;
    let g = Graph::new(blocks)?;
    let mut e = Emit {
        g,
        code,
        used: HashSet::new(),
        emitted: 0,
    };
    e.region(c, 0, first, None, None)
}

//! Control flow: basic blocks from the branches, loops from the dominator
//! tree, and structured SPIR-V from both.
//!
//! Each selection merges at its immediate post-dominator inside the
//! innermost loop; back edges become continues and loop exits become
//! breaks. A loop with several exits stores which one it took in a variable
//! and breaks; the code after the loop dispatches on it. EXIT is not a
//! block: every edge to it returns (after the epilogue) in place. KILL is
//! not control flow at all (see the io module).
//!
//! BSSY/BSYNC mark where the guest's lanes come together again. They emit
//! nothing: where a program asks for maximal reconvergence (see the lanes
//! module), lanes come together at the merges this structure picks, so
//! each BSSY's target must lie on the chain of merges that starts at its
//! block or no warp operation may run between the target and the next
//! merge on that chain, no BREAK may let lanes run warp operations before
//! their BSYNC,
//! and no BMOV may change a barrier; anything else is rejected. Elsewhere
//! which lanes run together cannot be observed.
//!
//! `CALL.REL` subroutines are inlined at every call site: a block is a pair
//! of the call stack and its first instruction, and `RET` continues after
//! the innermost call. The callee returns through the address its caller
//! put in a register with `MOV`, which with the RET's offset must name the
//! instruction after the `CALL`.
//!
//! A CALL whose callee never reaches a RET is a plain jump.
//!
//! `BRX` and `BRXU` jump to a register plus a base, through a table in a
//! constant bank the program cannot see. Their targets are the instruction
//! after the jump and the code nothing else reaches: each instruction after
//! an unconditional transfer that is unreachable without the jump belongs
//! to the nearest jump before it. The jump becomes an OpSwitch over those
//! addresses. Its default, an address outside the cases, ends the
//! invocation: a fragment is discarded, any other stage returns without
//! its outputs, so a missed case shows instead of running the wrong one.
//!
//! BAR waits for every thread of the CTA that has not exited, and a lane
//! that exited counts as arrived (measured on SM86); lanes of one warp at
//! two BAR sites hang. OpControlBarrier instead needs every invocation of
//! the workgroup at the same instruction. So each BAR must lie where the
//! whole workgroup runs: not in a block that a branch with a condition
//! that may differ between invocations can make some of them skip (the
//! blocks between the branch and its post-dominator), not under such a
//! guard, and not after a varying EXIT. Which values are uniform comes
//! from the `uniform` module; anything else is rejected with the reason.
//!
//! A graph this cannot express (irreducible, recursive calls) is rejected
//! with the reason.

mod uniform;

use super::{Code, Ctx, Res, bad};
use crate::Operand;
use crate::spirv::op;
use std::collections::{HashMap, HashSet};

const EXIT: usize = usize::MAX;
const MAX_CALL_DEPTH: usize = 8;

#[derive(Clone, Copy, Debug)]
enum Sel {
    Reg(u8),
    UReg(u8),
}

#[derive(Clone, Debug)]
enum Term {
    Jump(usize),
    Branch(Vec<Operand>, usize, usize),
    /// Jump to `selector + base`: (address, block) cases.
    Switch(Sel, u32, Vec<(u32, usize)>),
}

#[derive(Clone, Debug)]
struct Block {
    first: usize,
    end: usize,
    term: Term,
}

fn terminal(b: usize) -> bool {
    b == EXIT
}

struct Lp {
    header: usize,
    exits: Vec<usize>,
    cont: u32,
    brk: u32,
    body: Vec<bool>,
    var: u32,
}

struct Graph {
    blocks: Vec<Block>,
    succs: Vec<Vec<usize>>,
    headers: HashMap<usize, (Vec<bool>, Vec<usize>)>,
    pdoms: HashMap<Option<usize>, Vec<Option<usize>>>,
}

/// The branch condition: the guard and the predicates the instruction
/// itself names, all of which must hold.
fn conditions(i: &crate::Inst, w: crate::Word) -> Res<Vec<Operand>> {
    let mut c = Vec::new();
    if let Some(g) = &i.guard {
        c.push(g.clone());
    }
    if i.mnemonic == "BRA" || i.mnemonic == "EXIT" {
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
    }
    c.retain(|p| !matches!(p, Operand::Pred(7, false) | Operand::UPred(7, false)));
    Ok(c)
}

/// Signed 48-bit branch offset of a control word, in bytes.
fn raw_target(w: crate::Word) -> i64 {
    ((w.get64(34, 48) as i64) << 16 >> 16) * 4
}

fn is_transfer(i: &crate::Inst) -> bool {
    matches!(i.mnemonic, "BRA" | "EXIT" | "CALL" | "RET" | "BRX" | "BRXU")
}

/// Ends a block with no fall-through: an unguarded BRA, EXIT, BRX or RET.
fn no_fall(i: &crate::Inst, w: crate::Word) -> bool {
    matches!(i.mnemonic, "BRA" | "EXIT" | "BRX" | "BRXU" | "RET")
        && conditions(i, w).is_ok_and(|c| c.is_empty())
}

fn bra_target(i: &crate::Inst, end: usize) -> Res<usize> {
    let Some(Operand::Target(t)) = i.ops.last() else {
        return bad(format!("{} without a target", i.mnemonic));
    };
    usize::try_from(*t / 16)
        .ok()
        .filter(|k| *k < end && *t % 16 == 0)
        .map_or_else(|| bad("branch target outside the program"), Ok)
}

/// Instruction-level successors, with each BRX going only to the
/// instruction after it and each CALL to its target and its return point.
fn plain_succs(code: &Code, end: usize, k: usize) -> Vec<usize> {
    let i = &code.insts[k];
    let w = code.words[k];
    let mut v = Vec::new();
    if matches!(i.mnemonic, "BRA" | "CALL")
        && let Ok(t) = bra_target(i, end)
    {
        v.push(t);
    }
    if i.mnemonic == "BRX" || !no_fall(i, w) {
        v.push(k + 1);
    }
    v.retain(|t| *t < end);
    v
}

/// The cases of every BRX: the instruction after it and each instruction
/// after a transfer that nothing reaches without the BRX, given to the
/// nearest BRX before it. Heads are taken one at a time, lowest first, so
/// that a subroutine the cases call is reached through its call and not
/// taken for a case; code that reaches a RET is a subroutine (called or
/// dead), never a case.
fn brx_targets(code: &Code, end: usize) -> HashMap<usize, Vec<usize>> {
    let brxs: Vec<usize> = (0..end)
        .filter(|k| matches!(code.insts[*k].mnemonic, "BRX" | "BRXU"))
        .collect();
    let mut cases: HashMap<usize, Vec<usize>> = brxs.iter().map(|k| (*k, vec![k + 1])).collect();
    let mut seen = vec![false; end];
    let mut work = vec![0usize];
    loop {
        while let Some(k) = work.pop() {
            if k >= end || seen[k] {
                continue;
            }
            seen[k] = true;
            work.extend(plain_succs(code, end, k));
            if let Some(cs) = cases.get(&k) {
                work.extend(cs.iter().copied());
            }
        }
        let head = (1..end).find_map(|m| {
            if seen[m] || !no_fall(&code.insts[m - 1], code.words[m - 1]) || returns(code, end, m) {
                return None;
            }
            brxs.iter()
                .rev()
                .find(|k| **k < m && seen[**k])
                .map(|k| (*k, m))
        });
        let Some((k, m)) = head else {
            return cases;
        };
        if let Some(cs) = cases.get_mut(&k) {
            cs.push(m);
        }
        work.push(m);
    }
}

/// Whether code from `t` reaches a RET (not counting the subroutines it
/// calls); a CALL to code that never returns is a plain jump.
fn returns(code: &Code, end: usize, t: usize) -> bool {
    let mut seen = vec![false; end];
    let mut work = vec![t];
    while let Some(k) = work.pop() {
        if k >= end || seen[k] {
            continue;
        }
        seen[k] = true;
        let i = &code.insts[k];
        if i.mnemonic == "RET" {
            return true;
        }
        if i.mnemonic == "CALL" {
            work.push(k + 1);
            continue;
        }
        work.extend(plain_succs(code, end, k));
    }
    false
}

/// The register a CALL's return address goes through is set by a `MOV`
/// shortly before it. `RET.REL` at `ret` jumps to the register it reads
/// plus its own offset, so the address moved is the return point minus the
/// offset, into that register or one the callee copies into it with
/// unguarded register MOVs.
fn return_address_set(code: &Code, k: usize, ret: usize) -> bool {
    let rw = code.words[ret];
    if rw.bit(91) {
        return false;
    }
    let mut regs = vec![rw.get(24, 8)];
    if let Ok(callee) = bra_target(&code.insts[k], code.insts.len()) {
        for j in (callee..ret).rev() {
            let w = code.words[j];
            if w.opcode() == 0x202
                && w.get(72, 4) == 0xf
                && code.insts[j].guard.is_none()
                && regs.contains(&w.get(16, 8))
            {
                let src = w.get(32, 8);
                regs.retain(|r| *r != w.get(16, 8));
                regs.push(src);
            }
        }
    }
    let off = 16 * (ret as i64 + 1) + raw_target(rw);
    let want = (16 * (k as i64 + 1) - off) as u32;
    (k.saturating_sub(16)..k).rev().any(|j| {
        let w = code.words[j];
        w.opcode() == 0x802
            && regs.contains(&w.get(16, 8))
            && w.get(32, 32) == want
            && code.insts[j].guard.is_none()
    })
}

/// Block numbers of (call stack, first instruction) pairs, and the pairs
/// still to build.
#[derive(Default)]
struct Ids {
    ids: HashMap<(Vec<usize>, usize), usize>,
    work: Vec<(Vec<usize>, usize, usize)>,
}

impl Ids {
    fn get(&mut self, key: (Vec<usize>, usize), blocks: &mut Vec<Option<Block>>) -> usize {
        if let Some(&b) = self.ids.get(&key) {
            return b;
        }
        let b = blocks.len();
        blocks.push(None);
        self.work.push((key.0.clone(), key.1, b));
        self.ids.insert(key, b);
        b
    }

    fn next(&mut self) -> Option<(Vec<usize>, usize, usize)> {
        self.work.pop()
    }
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
                let t = bra_target(i, end)?;
                leaders[t] = true;
                leaders[k + 1] = true;
            }
            "CALL" => {
                if !i.has("REL") || !matches!(i.ops.last(), Some(Operand::Target(_))) {
                    return bad(format!("CALL form {i}"));
                }
                let t = bra_target(i, end)?;
                leaders[t] = true;
                leaders[k + 1] = true;
            }
            "RET" => {
                if !i.has("REL") {
                    return bad(format!("RET form {i}"));
                }
                leaders[k + 1] = true;
            }
            "EXIT" | "BRX" | "BRXU" => leaders[k + 1] = true,
            "JMP" | "JMX" | "BPT" | "RTT" => {
                return bad(format!("control {}", i.mnemonic));
            }
            "WARPSYNC" if i.has("EXCLUSIVE") => return bad("WARPSYNC.EXCLUSIVE"),
            _ => {}
        }
    }
    for i in &code.insts[..end] {
        if matches!(i.mnemonic, "BRX" | "BRXU") && (i.guard.is_some() || !i.mods.is_empty()) {
            return bad(format!("{} form {i}", i.mnemonic));
        }
    }
    let brx = brx_targets(code, end);
    for t in brx.values().flatten() {
        leaders[*t] = true;
    }
    let mut calls: HashMap<usize, bool> = HashMap::new();
    let mut next = vec![end; end + 1];
    for k in (0..end).rev() {
        next[k] = if leaders[k + 1] { k + 1 } else { next[k + 1] };
    }
    let mut ids = Ids::default();
    let mut blocks: Vec<Option<Block>> = Vec::new();
    ids.get((Vec::new(), 0), &mut blocks);
    while let Some((stack, s, me)) = ids.next() {
        let stop = next[s];
        let last = &code.insts[stop - 1];
        let w = code.words[stop - 1];
        let fall = (stop < end).then(|| (stack.clone(), stop));
        let term_inst = is_transfer(last);
        let to = |stack: &[usize], t: usize| (stack.to_vec(), t);
        let target: Option<(Vec<usize>, usize)>;
        let mut term = None;
        match last.mnemonic {
            "BRA" => target = Some(to(&stack, bra_target(last, end)?)),
            "EXIT" => target = None,
            "CALL"
                if !*calls.entry(stop - 1).or_insert_with(|| {
                    bra_target(last, end).is_ok_and(|t| returns(code, end, t))
                }) =>
            {
                target = Some(to(&stack, bra_target(last, end)?));
            }
            "CALL" => {
                let t = bra_target(last, end)?;
                if stack.len() >= MAX_CALL_DEPTH
                    || stack
                        .iter()
                        .any(|&c| matches!(code.insts[c].ops.last(), Some(Operand::Target(x)) if *x / 16 == t as i64))
                {
                    return bad("recursive CALL");
                }
                let mut st = stack.clone();
                st.push(stop - 1);
                target = Some((st, t));
            }
            "RET" => {
                let Some((&site, rest)) = stack.split_last() else {
                    return bad("RET outside a call");
                };
                if !return_address_set(code, site, stop - 1) {
                    return bad("CALL without its return address");
                }
                target = Some(to(rest, site + 1));
            }
            "BRX" | "BRXU" => {
                let sel = if w.bit(91) || last.mnemonic == "BRXU" {
                    Sel::UReg(w.get(24, 6) as u8)
                } else {
                    Sel::Reg(w.get(24, 8) as u8)
                };
                let base = (16 * stop as i64 + raw_target(w)) as u32;
                let mut cases = Vec::new();
                for &t in &brx[&(stop - 1)] {
                    let b = ids.get(to(&stack, t), &mut blocks);
                    cases.push((16 * t as u32, b));
                }
                term = Some(Term::Switch(sel, base, cases));
                target = None;
            }
            _ => target = None,
        }
        let term = match term {
            Some(t) => t,
            None if term_inst => {
                let conds = conditions(last, w)?;
                let to = match last.mnemonic {
                    "EXIT" => EXIT,
                    _ => match target {
                        Some(k) => ids.get(k, &mut blocks),
                        None => return bad(format!("{} without a target", last.mnemonic)),
                    },
                };
                if conds.is_empty() {
                    Term::Jump(to)
                } else {
                    let Some(f) = fall else {
                        return bad("conditional branch falls off the end");
                    };
                    Term::Branch(conds, to, ids.get(f, &mut blocks))
                }
            }
            None => match fall {
                Some(f) => Term::Jump(ids.get(f, &mut blocks)),
                None => return bad("code falls off the end"),
            },
        };
        blocks[me] = Some(Block {
            first: s,
            end: if term_inst { stop - 1 } else { stop },
            term,
        });
    }
    blocks
        .into_iter()
        .map(|b| b.map_or_else(|| bad("block never built"), Ok))
        .collect()
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
                    Term::Switch(_, _, cases) => cases.iter().map(|c| c.1).collect(),
                };
                v.retain(|t| !terminal(*t));
                let mut seen = HashSet::new();
                v.retain(|t| seen.insert(*t));
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
            headers.insert(h, (body, exits));
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

enum Dest {
    Label(u32),
    Ret,
    /// Leave the loop through its exit with this index.
    Break(u32, u32, u32),
    Inline(usize),
}

/// How a block with several successors picks one.
enum Head {
    Cond(u32),
    /// Selector and one literal per target; the first target is the default.
    Switch(u32, Vec<u32>),
    /// Selector and one literal per target; any other value ends the
    /// invocation.
    Cases(u32, Vec<u32>),
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
        if let Some(l) = lp {
            if t == l.header {
                return Ok(Dest::Label(l.cont));
            }
            if let Some(k) = l.exits.iter().position(|e| *e == t) {
                if l.exits.len() == 1 {
                    return Ok(Dest::Label(l.brk));
                }
                return Ok(Dest::Break(l.var, k as u32, l.brk));
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

    /// Ends the current block at a destination that is not code to inline.
    fn leave(&mut self, c: &mut Ctx, d: &Dest) -> Res<()> {
        match *d {
            Dest::Ret => {
                c.epilogue();
                c.m.emit_void(op::RETURN, &[]);
            }
            Dest::Break(var, k, brk) => {
                let v = c.m.c_u32(k);
                c.m.emit_void(op::STORE, &[var, v]);
                self.br(c, brk);
            }
            Dest::Label(l) => self.br(c, l),
            Dest::Inline(_) => unreachable!(),
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

    /// The first post-dominator of `b` in loop context `ctx` that is not
    /// in `skip` (the body of a loop headed by `b`).
    fn merge_after(
        &mut self,
        ctx: Option<usize>,
        b: usize,
        skip: Option<&[bool]>,
    ) -> Option<usize> {
        let mut p = self.g.ipdom(ctx, b);
        let mut steps = 0;
        while let Some(x) = p
            && skip.is_some_and(|s| s[x])
        {
            steps += 1;
            if steps > self.g.blocks.len() {
                return None;
            }
            p = self.g.ipdom(ctx, x);
        }
        p
    }

    /// Emits a selection that picks one of `targets` and merges at `pd`;
    /// returns where the region goes on, or `None` if it ended.
    fn fork(
        &mut self,
        c: &mut Ctx,
        head: Head,
        targets: &[usize],
        pd: Option<usize>,
        follow: Option<(usize, u32)>,
        lp: Option<&Lp>,
    ) -> Res<Option<(usize, u32)>> {
        let real = pd.filter(|&m| {
            follow.is_none_or(|(f, _)| f != m)
                && lp.is_none_or(|l| l.header != m && !l.exits.contains(&m))
        });
        let merge = c.m.id();
        let arm_follow = match real {
            Some(r) => Some((r, merge)),
            None => follow.map(|(f, _)| (f, merge)),
        };
        let switch = matches!(head, Head::Switch(..) | Head::Cases(..));
        let mut arms: Vec<(u32, Dest)> = Vec::new();
        let mut by_target: HashMap<usize, u32> = HashMap::new();
        let mut labels = Vec::new();
        for &tg in targets {
            if let Some(&l) = by_target.get(&tg) {
                labels.push(l);
                continue;
            }
            let d = if Some(tg) == real {
                Dest::Label(merge)
            } else {
                self.resolve(tg, arm_follow, lp)?
            };
            let l = match d {
                Dest::Label(x) if !switch || x == merge => x,
                d => {
                    let l = c.m.id();
                    arms.push((l, d));
                    l
                }
            };
            self.used.insert(l);
            by_target.insert(tg, l);
            labels.push(l);
        }
        c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        match head {
            Head::Cond(cv) => {
                c.m.emit_void(op::BRANCH_CONDITIONAL, &[cv, labels[0], labels[1]]);
            }
            Head::Switch(sel, lits) => {
                let mut a = vec![sel, labels[0]];
                for (k, &l) in labels.iter().enumerate().skip(1) {
                    a.push(lits[k]);
                    a.push(l);
                }
                c.m.emit_void(op::SWITCH, &a);
            }
            Head::Cases(sel, lits) => {
                let trap = c.m.id();
                let mut a = vec![sel, trap];
                for (k, &l) in labels.iter().enumerate() {
                    a.push(lits[k]);
                    a.push(l);
                }
                c.m.emit_void(op::SWITCH, &a);
                c.m.emit_void(op::LABEL, &[trap]);
                if c.stage == crate::program::Stage::Fragment {
                    c.m.emit_void(op::KILL, &[]);
                } else {
                    c.m.emit_void(op::RETURN, &[]);
                }
            }
        }
        for (l, d) in arms {
            match d {
                Dest::Inline(b) => self.region(c, b, l, arm_follow, lp)?,
                d => {
                    c.m.emit_void(op::LABEL, &[l]);
                    self.leave(c, &d)?;
                }
            }
        }
        match real {
            Some(r) => Ok(Some((r, merge))),
            None => {
                c.m.emit_void(op::LABEL, &[merge]);
                match follow {
                    Some((_, fl)) if self.used.contains(&merge) => self.br(c, fl),
                    _ => c.m.emit_void(op::UNREACHABLE, &[]),
                }
                Ok(None)
            }
        }
    }

    fn selector(&mut self, c: &mut Ctx, sel: Sel, base: u32) -> u32 {
        let r = match sel {
            Sel::Reg(n) => c.load_reg(n),
            Sel::UReg(n) => c.load_ureg(n),
        };
        let u = c.u32t();
        let b = c.m.c_u32(base);
        c.m.emit(op::I_ADD, u, &[r, b])
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
            if let Some((body, exits)) = self.g.headers.get(&cur).cloned()
                && lp.is_none_or(|l| l.header != cur)
            {
                let h = c.m.id();
                let merge = c.m.id();
                let cont = c.m.id();
                let bodyl = c.m.id();
                let var = if exits.len() > 1 {
                    let v = c.m.id();
                    c.locals.push(v);
                    v
                } else {
                    0
                };
                c.m.emit_void(op::LABEL, &[label]);
                self.br(c, h);
                c.m.emit_void(op::LABEL, &[h]);
                c.m.emit_void(op::LOOP_MERGE, &[merge, cont, 0]);
                self.br(c, bodyl);
                let inner = Lp {
                    header: cur,
                    exits: exits.clone(),
                    cont,
                    brk: merge,
                    body,
                    var,
                };
                self.region(c, cur, bodyl, None, Some(&inner))?;
                c.m.emit_void(op::LABEL, &[cont]);
                self.br(c, h);
                match exits.as_slice() {
                    [] => {
                        c.m.emit_void(op::LABEL, &[merge]);
                        c.m.emit_void(op::UNREACHABLE, &[]);
                        return Ok(());
                    }
                    [e] => match self.resolve(*e, follow, lp)? {
                        Dest::Inline(e) => {
                            cur = e;
                            label = merge;
                            continue;
                        }
                        d => {
                            c.m.emit_void(op::LABEL, &[merge]);
                            return self.leave(c, &d);
                        }
                    },
                    _ => {
                        c.m.emit_void(op::LABEL, &[merge]);
                        let u = c.u32t();
                        let sel = c.m.emit(op::LOAD, u, &[var]);
                        let lits = (0..exits.len() as u32).collect();
                        let pd = self.merge_after(lp.map(|l| l.header), cur, Some(&inner.body));
                        match self.fork(c, Head::Switch(sel, lits), &exits, pd, follow, lp)? {
                            Some((r, l)) => {
                                cur = r;
                                label = l;
                                continue;
                            }
                            None => return Ok(()),
                        }
                    }
                }
            }
            c.m.emit_void(op::LABEL, &[label]);
            self.content(c, cur)?;
            let term = self.g.blocks[cur].term.clone();
            let (head, targets) = match term {
                Term::Jump(t) => match self.resolve(t, follow, lp)? {
                    Dest::Inline(t) => {
                        let l = c.m.id();
                        self.br(c, l);
                        cur = t;
                        label = l;
                        continue;
                    }
                    d => return self.leave(c, &d),
                },
                Term::Branch(conds, t, f) => (Head::Cond(self.cond(c, &conds)?), vec![t, f]),
                Term::Switch(sel, base, cases) => {
                    let s = self.selector(c, sel, base);
                    let lits = cases.iter().map(|x| x.0).collect();
                    (Head::Cases(s, lits), cases.iter().map(|x| x.1).collect())
                }
            };
            let pd = self.g.ipdom(lp.map(|l| l.header), cur);
            match self.fork(c, head, &targets, pd, follow, lp)? {
                Some((r, l)) => {
                    cur = r;
                    label = l;
                }
                None => return Ok(()),
            }
        }
    }
}

impl Graph {
    /// The innermost loop that holds `b`.
    fn innermost(&self, b: usize) -> Option<usize> {
        self.headers
            .iter()
            .filter(|(_, (body, _))| body[b])
            .min_by_key(|(_, (body, _))| body.iter().filter(|x| **x).count())
            .map(|(h, _)| *h)
    }

    /// Where the lanes that run loop `h` come together after it.
    fn after_loop(&mut self, h: usize) -> Option<usize> {
        let (body, exits) = self.headers[&h].clone();
        match exits.as_slice() {
            [] => None,
            [e] => Some(*e),
            _ => {
                let outer = self
                    .headers
                    .iter()
                    .filter(|(o, (b, _))| **o != h && b[h])
                    .min_by_key(|(_, (b, _))| b.iter().filter(|x| **x).count())
                    .map(|(o, _)| *o);
                let mut p = self.ipdom(outer, h);
                for _ in 0..self.blocks.len() {
                    match p {
                        Some(x) if body[x] => p = self.ipdom(outer, x),
                        _ => break,
                    }
                }
                // Exits that meet nowhere inside the enclosing loop: the
                // lanes that stay in it are together again at its header.
                p.or(outer)
            }
        }
    }

    /// The next block where the lanes that run `b` are together again.
    fn next_merge(&mut self, b: usize, entering: bool) -> Option<usize> {
        if entering && self.headers.contains_key(&b) {
            return self.after_loop(b);
        }
        match &self.blocks[b].term {
            Term::Jump(t) => (!terminal(*t)).then_some(*t),
            _ => {
                let ctx = self.innermost(b);
                match self.ipdom(ctx, b) {
                    Some(p) => Some(p),
                    None => ctx,
                }
            }
        }
    }

    fn holds(&self, b: usize, k: usize) -> bool {
        self.blocks[b].first <= k && k <= self.blocks[b].end
    }

    /// The instructions run after block `b` and before instruction `t`:
    /// the blocks reached from the successors of `b` without passing `t`,
    /// as ranges of instructions.
    fn reach_before(&self, b: usize, t: usize) -> Vec<std::ops::Range<usize>> {
        self.reach_before_any(b, &[t])
    }

    /// As `reach_before`, stopping at any of `stops`.
    fn reach_before_any(&self, b: usize, stops: &[usize]) -> Vec<std::ops::Range<usize>> {
        let mut seen = vec![false; self.blocks.len()];
        let mut work = self.succs[b].clone();
        let mut out = Vec::new();
        while let Some(u) = work.pop() {
            if seen[u] {
                continue;
            }
            seen[u] = true;
            let blk = &self.blocks[u];
            if let Some(t) = stops.iter().copied().filter(|t| self.holds(u, *t)).min() {
                out.push(blk.first..t);
                continue;
            }
            out.push(blk.first..blk.end);
            work.extend(self.succs[u].iter().copied());
        }
        out
    }

    /// Checks that the structure reconverges the guest's lanes where its
    /// BSSY/BSYNC regions do (see the module documentation).
    fn check_reconvergence(&mut self, code: &Code) -> Res<()> {
        let insts = &code.insts;
        for b in 0..self.blocks.len() {
            let blk = self.blocks[b].clone();
            for k in blk.first..blk.end {
                let i = &insts[k];
                if super::mem::lanes::writes_barrier(i) {
                    return bad(format!(
                        "BMOV changes a convergence barrier at {:#x}",
                        16 * k
                    ));
                }
                match i.mnemonic {
                    "BSSY" => {
                        let Some(Operand::Target(t)) = i.ops.last() else {
                            return bad("BSSY without a target");
                        };
                        let t = (*t / 16) as usize;
                        if !self.merges_at(b, t)
                            && let Some(j) = self.unmerged_warp_op(b, t, insts)
                        {
                            return bad(format!(
                                "BSSY at {:#x} reconverges at {:#x}, not at a merge, before the warp operation at {:#x}",
                                16 * k,
                                16 * t,
                                16 * j
                            ));
                        }
                    }
                    "BREAK" => {
                        let Some(n) = i.ops.last().and_then(super::mem::lanes::barrier_name) else {
                            return bad("BREAK without a barrier");
                        };
                        let bssy = (0..k).rev().find(|&j| {
                            insts[j].mnemonic == "BSSY"
                                && insts[j]
                                    .ops
                                    .first()
                                    .and_then(super::mem::lanes::barrier_name)
                                    == Some(n)
                        });
                        let Some(Operand::Target(t)) = bssy.and_then(|j| insts[j].ops.last())
                        else {
                            return bad("BREAK without its BSSY");
                        };
                        let t = (*t / 16) as usize;
                        let mut stops = vec![t];
                        stops.extend(enclosing_bsyncs(insts, bssy.unwrap_or(0), k));
                        let after = (k + 1..blk.end)
                            .chain(self.reach_before_any(b, &stops).into_iter().flatten());
                        let warp = after
                            .into_iter()
                            .find(|&j| super::mem::lanes::reads_lanes(&insts[j]));
                        if let Some(j) = warp {
                            return bad(format!(
                                "warp operation at {:#x} between BREAK and its BSYNC",
                                16 * j
                            ));
                        }
                    }
                    _ => {}
                }
            }
        }
        Ok(())
    }

    /// Checks that every BAR runs where all the workgroup's invocations
    /// run it (see the module documentation).
    fn check_barriers(&mut self, code: &Code) -> Res<()> {
        let insts = &code.insts;
        let n = self.blocks.len();
        let mut partial = vec![false; n];
        let mut exited = vec![false; n];
        loop {
            let entry = self.varying(code, &partial);
            let mut p = partial.clone();
            let mut e = exited.clone();
            for b in 0..n {
                let Some(mut v) = entry[b] else { continue };
                let blk = &self.blocks[b];
                for i in &insts[blk.first..blk.end] {
                    uniform::step(i, partial[b], &mut v);
                }
                let (uniform, exits) = match &blk.term {
                    Term::Jump(_) => (true, false),
                    Term::Branch(conds, t, f) => {
                        (v.uniform_all(conds), terminal(*t) || terminal(*f))
                    }
                    Term::Switch(Sel::Reg(r), ..) => (v.uniform_reg(*r, false), false),
                    Term::Switch(Sel::UReg(r), ..) => (v.uniform_reg(*r, true), false),
                };
                if uniform {
                    continue;
                }
                let stop = if exits { None } else { self.ipdom(None, b) };
                let mut work = self.succs[b].clone();
                let mut seen = vec![false; n];
                while let Some(u) = work.pop() {
                    if seen[u] || Some(u) == stop {
                        continue;
                    }
                    seen[u] = true;
                    let marks = if exits { &mut e } else { &mut p };
                    marks[u] = true;
                    work.extend(self.succs[u].iter().copied());
                }
            }
            if p == partial && e == exited {
                for b in 0..n {
                    let Some(mut v) = entry[b] else { continue };
                    let blk = &self.blocks[b];
                    for (k, i) in insts.iter().enumerate().take(blk.end).skip(blk.first) {
                        if i.mnemonic == "BAR" {
                            let guard = i.guard.iter().cloned().collect::<Vec<_>>();
                            if partial[b] || !v.uniform_all(&guard) {
                                return bad(format!(
                                    "BAR at {:#x} in non-uniform control flow",
                                    16 * k
                                ));
                            }
                            if exited[b] {
                                return bad(format!(
                                    "BAR at {:#x} after an EXIT only some invocations take",
                                    16 * k
                                ));
                            }
                        }
                        uniform::step(i, partial[b], &mut v);
                    }
                }
                return Ok(());
            }
            partial = p;
            exited = e;
        }
    }

    /// What may vary between invocations at the start of each block
    /// (`None` where nothing reaches it), with the blocks in `partial` run
    /// by only some invocations.
    fn varying(&self, code: &Code, partial: &[bool]) -> Vec<Option<uniform::Varying>> {
        let n = self.blocks.len();
        let mut entry: Vec<Option<uniform::Varying>> = vec![None; n];
        entry[0] = Some(uniform::Varying::default());
        let mut work = vec![0];
        while let Some(b) = work.pop() {
            let Some(mut v) = entry[b] else { continue };
            let blk = &self.blocks[b];
            for k in blk.first..blk.end {
                uniform::step(&code.insts[k], partial[b], &mut v);
            }
            for &s in &self.succs[b] {
                let changed = match &mut entry[s] {
                    Some(old) => old.join(&v),
                    slot @ None => {
                        *slot = Some(v);
                        true
                    }
                };
                if changed {
                    work.push(s);
                }
            }
        }
        entry
    }

    /// The first instruction that sees which lanes run together (a warp
    /// operation that reads other lanes or the active mask, or WARPSYNC)
    /// from `t` on and before the lanes of `b` reach a merge of the
    /// structure: where the guest's lanes are back together and the host's
    /// may not be yet.
    fn unmerged_warp_op(&mut self, b: usize, t: usize, insts: &[crate::Inst]) -> Option<usize> {
        let mut chain = vec![false; self.blocks.len()];
        let mut cur = Some(b);
        let mut entering = false;
        for _ in 0..2 * self.blocks.len() + 2 {
            let Some(x) = cur else { break };
            if x != b {
                chain[x] = true;
            }
            cur = self.next_merge(x, entering);
            entering = true;
        }
        let sees = |i: &crate::Inst| {
            super::mem::lanes::reads_lanes(i)
                || super::mem::lanes::reads_active(i)
                || i.mnemonic == "WARPSYNC"
        };
        let mut seen = vec![false; self.blocks.len()];
        let mut work: Vec<(usize, usize)> = (0..self.blocks.len())
            .filter(|&u| self.holds(u, t))
            .map(|u| (u, t))
            .collect();
        while let Some((u, from)) = work.pop() {
            if seen[u] || chain[u] {
                continue;
            }
            seen[u] = true;
            let blk = &self.blocks[u];
            if let Some(j) = (from.max(blk.first)..blk.end).find(|&j| sees(&insts[j])) {
                return Some(j);
            }
            work.extend(self.succs[u].iter().map(|&s| (s, 0)));
        }
        None
    }

    /// Instruction `t` lies on the chain of merges that starts at `b`, or
    /// the lanes of `b` never reach it.
    fn merges_at(&mut self, b: usize, t: usize) -> bool {
        let mut cur = Some(b);
        let mut entering = false;
        for _ in 0..2 * self.blocks.len() + 2 {
            let Some(x) = cur else { break };
            if self.holds(x, t) {
                return true;
            }
            cur = self.next_merge(x, entering);
            entering = true;
        }
        !self
            .reach_before(b, usize::MAX)
            .iter()
            .any(|r| r.contains(&t) || r.end == t)
    }
}

/// The BSYNCs of the regions around the BSSY at `inner` that hold the
/// BREAK at `k` and are never left by a BREAK of their own: lanes that
/// break out of the inner region still wait there for the rest.
fn enclosing_bsyncs(insts: &[crate::Inst], inner: usize, k: usize) -> Vec<usize> {
    use super::mem::lanes::barrier_name;
    let mut out = Vec::new();
    for (j, i) in insts.iter().enumerate().take(inner) {
        if i.mnemonic != "BSSY" {
            continue;
        }
        let (Some(m), Some(Operand::Target(t))) =
            (i.ops.first().and_then(barrier_name), i.ops.last())
        else {
            continue;
        };
        let t = (*t / 16) as usize;
        let Some(sync) = t.checked_sub(1).filter(|&s| s > k) else {
            continue;
        };
        let is_sync = |x: &crate::Inst, name: &str| {
            x.mnemonic == name && x.ops.last().and_then(barrier_name) == Some(m)
        };
        if !insts.get(sync).is_some_and(|x| is_sync(x, "BSYNC")) {
            continue;
        }
        if insts[j + 1..sync]
            .iter()
            .any(|x| is_sync(x, "BREAK") || is_sync(x, "BSSY"))
        {
            continue;
        }
        out.push(sync);
    }
    out
}

/// Emits the program's body, starting with label `first`.
pub(crate) fn emit(c: &mut Ctx, code: &Code, first: u32) -> Res<()> {
    super::io::prescan(c, &code.insts);
    let blocks = build(code)?;
    let mut g = Graph::new(blocks)?;
    if c.iface.host.max_reconvergence {
        g.check_reconvergence(code)?;
    }
    if code.insts.iter().any(|i| i.mnemonic == "BAR") {
        g.check_barriers(code)?;
    }
    let mut e = Emit {
        g,
        code,
        used: HashSet::new(),
        emitted: 0,
    };
    e.region(c, 0, first, None, None)
}

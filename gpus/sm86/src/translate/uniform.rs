//! Draw-uniform values: registers and predicates that hold the same value
//! in every invocation of a draw or dispatch, whichever lanes share a warp.
//!
//! A value is uniform when it is computed by pure arithmetic from
//! immediates, constant buffer words and other uniform values, and every
//! branch that decides which definition reaches it is uniform too. The
//! analysis is a forward dataflow over the instruction graph; a branch on
//! a predicate that is not uniform makes every register written between it
//! and its immediate post-dominator non-uniform at that post-dominator
//! (the lanes rejoin there with values from different paths).
//!
//! A vote on a uniform predicate gives that predicate (ANY, ALL) or true
//! (EQ) in every warp the guest could form, so it is the same on any host
//! with any subgroup size: [`invariant_votes`] lists them.

use super::Code;
use super::cfg::FlowBlock;
use crate::{Inst, Operand};
use std::collections::BTreeSet;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Loc {
    R(u8),
    Ur(u8),
    P(u8),
    Up(u8),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct St {
    r: [u64; 4],
    ur: u64,
    p: u8,
    up: u8,
}

impl St {
    const NONE: St = St {
        r: [0; 4],
        ur: 0,
        p: 0,
        up: 0,
    };

    fn get(&self, l: Loc) -> bool {
        match l {
            Loc::R(255) | Loc::Ur(63) | Loc::P(7) | Loc::Up(7) => true,
            Loc::R(n) => self.r[n as usize / 64] >> (n % 64) & 1 == 1,
            Loc::Ur(n) => self.ur >> (n % 64) & 1 == 1,
            Loc::P(n) => self.p >> (n % 8) & 1 == 1,
            Loc::Up(n) => self.up >> (n % 8) & 1 == 1,
        }
    }

    fn set(&mut self, l: Loc, v: bool) {
        let (w, b): (&mut u64, u32) = match l {
            Loc::R(n) => (&mut self.r[n as usize / 64], u32::from(n % 64)),
            Loc::Ur(n) => (&mut self.ur, u32::from(n % 64)),
            Loc::P(n) => {
                self.p = (self.p & !(1 << (n % 8))) | (u8::from(v) << (n % 8));
                return;
            }
            Loc::Up(n) => {
                self.up = (self.up & !(1 << (n % 8))) | (u8::from(v) << (n % 8));
                return;
            }
        };
        *w = (*w & !(1 << b)) | (u64::from(v) << b);
    }

    fn and(&mut self, o: &St) {
        for k in 0..4 {
            self.r[k] &= o.r[k];
        }
        self.ur &= o.ur;
        self.p &= o.p;
        self.up &= o.up;
    }

    fn or(&mut self, o: &St) {
        for k in 0..4 {
            self.r[k] |= o.r[k];
        }
        self.ur |= o.ur;
        self.p |= o.p;
        self.up |= o.up;
    }

    fn clear(&mut self, o: &St) {
        for k in 0..4 {
            self.r[k] &= !o.r[k];
        }
        self.ur &= !o.ur;
        self.p &= !o.p;
        self.up &= !o.up;
    }

    fn mark(&mut self, l: Loc) {
        match l {
            Loc::R(n) => self.r[n as usize / 64] |= 1 << (n % 64),
            Loc::Ur(n) => self.ur |= 1 << (n % 64),
            Loc::P(n) => self.p |= 1 << (n % 8),
            Loc::Up(n) => self.up |= 1 << (n % 8),
        }
    }
}

fn loc(o: &Operand) -> Option<Loc> {
    Some(match o {
        Operand::Reg(n, _) => Loc::R(*n),
        Operand::UReg(n, _) => Loc::Ur(*n),
        Operand::Pred(n, _) => Loc::P(*n),
        Operand::UPred(n, _) => Loc::Up(*n),
        _ => return None,
    })
}

/// The next registers of a pair or quad starting at `l`.
fn wide(l: Loc) -> impl Iterator<Item = Loc> {
    (1..4u8).filter_map(move |k| match l {
        Loc::R(n) if n < 255 => n.checked_add(k).filter(|&m| m < 255).map(Loc::R),
        Loc::Ur(n) if n < 63 => n.checked_add(k).filter(|&m| m < 63).map(Loc::Ur),
        _ => None,
    })
}

/// Pure operations: the result depends only on the operands.
fn pure(m: &str) -> bool {
    matches!(
        m,
        "MOV"
            | "MOV32I"
            | "IMAD"
            | "IMUL"
            | "IADD"
            | "IADD3"
            | "IADD32I"
            | "ISCADD"
            | "LEA"
            | "LOP"
            | "LOP3"
            | "LOP32I"
            | "SHF"
            | "SHL"
            | "SHR"
            | "ISETP"
            | "FSETP"
            | "DSETP"
            | "HSETP2"
            | "FSET"
            | "ISET"
            | "FADD"
            | "FADD32I"
            | "FMUL"
            | "FMUL32I"
            | "FFMA"
            | "FFMA32I"
            | "FMNMX"
            | "IMNMX"
            | "IABS"
            | "SEL"
            | "FSEL"
            | "PLOP3"
            | "PRMT"
            | "I2F"
            | "I2FP"
            | "I2I"
            | "F2I"
            | "F2F"
            | "F2FP"
            | "FRND"
            | "MUFU"
            | "FLO"
            | "POPC"
            | "BREV"
            | "BMSK"
            | "SGXT"
            | "IDP"
            | "HADD2"
            | "HMUL2"
            | "HFMA2"
            | "HMNMX2"
            | "HSET2"
            | "DADD"
            | "DMUL"
            | "DFMA"
            | "LDC"
            | "ULDC"
            | "UMOV"
            | "UIADD3"
            | "UIMAD"
            | "ULEA"
            | "ULOP"
            | "ULOP3"
            | "USHF"
            | "USHL"
            | "USHR"
            | "UISETP"
            | "USEL"
            | "UPLOP3"
            | "UPRMT"
            | "UFLO"
            | "UPOPC"
            | "UBREV"
            | "UBMSK"
            | "USGXT"
            | "R2UR"
            | "P2R"
    )
}

/// Texture fetches: two destination operands of a register pair each
/// (Rd, Rd+1, Rd2, Rd2+1).
fn texture(m: &str) -> bool {
    matches!(m, "TEX" | "TLD" | "TLD4" | "TXD" | "TXQ" | "TMML")
}

/// Other loads and matrix operations that may write up to four registers
/// from their first operand.
fn quad_dest(m: &str) -> bool {
    matches!(
        m,
        "SULD" | "ALD" | "LDSM" | "HMMA" | "IMMA" | "BMMA" | "DMMA"
    )
}

fn pair(l: Loc) -> impl Iterator<Item = Loc> {
    wide(l).take(1)
}

/// Wider than one register: a pair or quad.
fn is_wide(i: &Inst) -> bool {
    i.mnemonic.starts_with('D')
        || (i.mnemonic == "CS2R" && !i.has("32"))
        || i.mods
            .iter()
            .any(|m| m.contains("64") || m.contains("128") || *m == "WIDE")
}

/// Loads from memory every invocation may read alike.
fn global_load(m: &str) -> bool {
    matches!(m, "LDG" | "LD")
}

/// Registers written by a load or wide multiply whose width the
/// modifiers give exactly.
fn exact_width(i: &Inst) -> Option<usize> {
    let w = if i.has("128") {
        4
    } else if i.has("64") || i.has("WIDE") {
        2
    } else {
        1
    };
    match i.mnemonic {
        "LDG" | "LD" | "LDC" | "ULDC" => Some(w),
        "IMAD" if i.has("WIDE") => Some(2),
        _ => None,
    }
}

/// Instructions that may write global memory, which other invocations
/// of the draw could then read.
fn writes_memory(m: &str) -> bool {
    matches!(
        m,
        "STG" | "ST" | "ATOM" | "ATOMG" | "RED" | "SUST" | "SUATOM" | "SURED" | "CCTL"
    )
}

/// Operands an instruction writes: `(certain, possible)`. The first
/// operand is written when it is a register or predicate; predicates right
/// after it and the rest of a pair or quad may be.
fn defs(i: &Inst) -> (Vec<Loc>, Vec<Loc>) {
    let mut maybe = Vec::new();
    if i.mnemonic == "R2P" {
        maybe.extend((0..7).map(Loc::P));
        return (Vec::new(), maybe);
    }
    if matches!(i.mnemonic, "VOTE" | "VOTEU") {
        let n = i.ops.len().saturating_sub(1);
        return (i.ops.iter().take(n).filter_map(loc).collect(), maybe);
    }
    let Some(first) = i.ops.first().and_then(loc) else {
        return (Vec::new(), maybe);
    };
    if let Some(n) = exact_width(i) {
        let mut exact = vec![first];
        exact.extend(wide(first).take(n - 1));
        return (exact, maybe);
    }
    let tex = texture(i.mnemonic);
    if quad_dest(i.mnemonic) || is_wide(i) {
        maybe.extend(wide(first));
    } else if tex {
        maybe.extend(pair(first));
    }
    for (k, o) in i.ops.iter().enumerate().skip(1) {
        match (o, loc(o)) {
            (Operand::Pred(_, false) | Operand::UPred(_, false), Some(l)) => maybe.push(l),
            (Operand::Reg(..) | Operand::UReg(..), Some(l))
                if k == 1 && (tex || matches!(first, Loc::P(_) | Loc::Up(_))) =>
            {
                maybe.push(l);
                if tex {
                    maybe.extend(pair(l));
                } else {
                    maybe.extend(wide(l));
                }
                break;
            }
            _ => break,
        }
    }
    (vec![first], maybe)
}

fn operand_uniform(s: &St, o: &Operand) -> bool {
    match o {
        Operand::Reg(..) | Operand::UReg(..) | Operand::Pred(..) | Operand::UPred(..) => {
            loc(o).is_some_and(|l| s.get(l))
        }
        Operand::Int(_)
        | Operand::Hex(_)
        | Operand::F32(..)
        | Operand::F16x2(..)
        | Operand::F64Hi(_)
        | Operand::Target(_) => true,
        Operand::CBuf { bank, index, .. } => {
            let b = match bank {
                crate::CBank::Imm(_) => true,
                crate::CBank::UReg(n) => s.get(Loc::Ur(*n)),
            };
            b && index.as_deref().is_none_or(|x| operand_uniform(s, x))
        }
        Operand::Sym(n) if n.starts_with("c[") || n.starts_with('[') => sym_uniform(s, n),
        Operand::Sym(n) => n == "SRZ",
        Operand::Mem { base, ubase, .. } => base
            .iter()
            .chain(ubase.iter())
            .all(|x| operand_uniform(s, x)),
        _ => false,
    }
}

/// A constant buffer or address operand left as text (`c[0x0][0x540]`,
/// `[R28.U32+UR8]`): uniform when every register named in it holds a
/// uniform value.
fn sym_uniform(s: &St, text: &str) -> bool {
    text.split(|c: char| !c.is_ascii_alphanumeric()).all(|t| {
        let num = |p: &str| t.strip_prefix(p).and_then(|n| n.parse::<u8>().ok());
        if let Some(n) = num("UR") {
            s.get(Loc::Ur(n))
        } else if let Some(n) = num("R") {
            s.get(Loc::R(n))
        } else {
            true
        }
    })
}

fn guard_uniform(s: &St, i: &Inst) -> bool {
    i.guard.as_ref().is_none_or(|g| operand_uniform(s, g))
}

/// The state after instruction `i`. Every operand but the first counts
/// as a source, which is safe for the predicates that may be outputs too.
fn step(s: &St, i: &Inst, readonly: bool) -> St {
    let mut out = *s;
    let (exact, maybe) = defs(i);
    let g = guard_uniform(s, i);
    let vote = matches!(i.mnemonic, "VOTE" | "VOTEU");
    let value = |l: Loc| -> bool {
        if vote {
            matches!(l, Loc::P(_) | Loc::Up(_))
                && i.ops.last().is_some_and(|o| operand_uniform(s, o))
        } else if i.mnemonic == "CS2R" {
            matches!(i.ops.get(1), Some(Operand::Sym(n)) if n == "SRZ")
        } else if global_load(i.mnemonic) {
            (readonly || i.has("CONSTANT")) && i.ops.iter().skip(1).all(|o| operand_uniform(s, o))
        } else {
            pure(i.mnemonic) && i.ops.iter().skip(1).all(|o| operand_uniform(s, o))
        }
    };
    for &l in &exact {
        let v = value(l) && g && (i.guard.is_none() || s.get(l));
        out.set(l, v);
    }
    for &l in &maybe {
        let v = value(l) && g && s.get(l);
        out.set(l, v);
    }
    out
}

fn all_defs(i: &Inst) -> St {
    let mut d = St::NONE;
    let (exact, maybe) = defs(i);
    for l in exact.into_iter().chain(maybe) {
        d.mark(l);
    }
    d
}

/// Immediate post-dominators of a graph whose node `n` (the length of
/// `succ`) is the exit; `None` where a node has no path to the exit.
fn ipdoms(succ: &[Vec<usize>]) -> Vec<Option<usize>> {
    let n = succ.len();
    let exit = n;
    let mut preds: Vec<Vec<usize>> = vec![Vec::new(); n + 1];
    for (k, s) in succ.iter().enumerate() {
        for &t in s {
            preds[t].push(k);
        }
    }
    let mut order = Vec::with_capacity(n + 1);
    let mut seen = vec![false; n + 1];
    let mut stack = vec![(exit, 0usize)];
    seen[exit] = true;
    while let Some(&mut (v, ref mut k)) = stack.last_mut() {
        if let Some(&p) = preds[v].get(*k) {
            *k += 1;
            if !seen[p] {
                seen[p] = true;
                stack.push((p, 0));
            }
        } else {
            order.push(v);
            stack.pop();
        }
    }
    let mut rank = vec![usize::MAX; n + 1];
    for (r, &v) in order.iter().enumerate() {
        rank[v] = r;
    }
    let mut ip: Vec<Option<usize>> = vec![None; n + 1];
    ip[exit] = Some(exit);
    let intersect = |ip: &[Option<usize>], mut a: usize, mut b: usize| -> usize {
        while a != b {
            while rank[a] < rank[b] {
                a = ip[a].unwrap_or(exit);
            }
            while rank[b] < rank[a] {
                b = ip[b].unwrap_or(exit);
            }
        }
        a
    };
    let mut changed = true;
    while changed {
        changed = false;
        for &v in order.iter().rev() {
            if v == exit {
                continue;
            }
            let mut new: Option<usize> = None;
            for &t in &succ[v] {
                if ip[t].is_none() {
                    continue;
                }
                new = Some(match new {
                    None => t,
                    Some(c) => intersect(&ip, c, t),
                });
            }
            if new.is_some() && ip[v] != new {
                ip[v] = new;
                changed = true;
            }
        }
    }
    ip.truncate(n);
    ip
}

/// Whether the block's choice of successor is the same in every
/// invocation, given the state at its end.
fn uniform_choice(s: &St, b: &FlowBlock) -> bool {
    b.conds.iter().all(|c| operand_uniform(s, c))
        && b.selector.as_ref().is_none_or(|o| operand_uniform(s, o))
}

/// The uniform state before each instruction of each block (`None`:
/// unreachable).
fn analyze(insts: &[Inst], blocks: &[FlowBlock]) -> Option<Vec<Option<St>>> {
    let n = blocks.len();
    if n == 0 {
        return Some(Vec::new());
    }
    let succ: Vec<Vec<usize>> = blocks
        .iter()
        .map(|b| b.succs.iter().map(|t| t.unwrap_or(n)).collect())
        .collect();
    let ip = ipdoms(&succ);
    let readonly = !insts.iter().any(|i| writes_memory(i.mnemonic));
    let defs: Vec<St> = blocks
        .iter()
        .map(|b| {
            let mut d = St::NONE;
            for k in b.insts.clone() {
                d.or(&all_defs(&insts[k]));
            }
            d
        })
        .collect();
    let mut divergent = vec![false; n];
    loop {
        let mut kill = vec![St::NONE; n];
        for b in (0..n).filter(|&b| divergent[b]) {
            let j = ip[b]?;
            if j >= n {
                continue;
            }
            let mut region = St::NONE;
            let mut seen = vec![false; n];
            let mut work: Vec<usize> = succ[b].clone();
            while let Some(v) = work.pop() {
                if v == j || v >= n || seen[v] {
                    continue;
                }
                seen[v] = true;
                region.or(&defs[v]);
                work.extend(succ[v].iter().copied());
            }
            kill[j].or(&region);
        }
        let mut entry = St::NONE;
        entry.set(Loc::R(255), true);
        let mut state: Vec<Option<St>> = vec![None; n];
        state[0] = Some(entry);
        let mut out: Vec<Option<St>> = vec![None; n];
        let mut work = vec![0usize];
        let mut queued = vec![false; n];
        queued[0] = true;
        while let Some(v) = work.pop() {
            queued[v] = false;
            let Some(mut s) = state[v] else { continue };
            for k in blocks[v].insts.clone() {
                s = step(&s, &insts[k], readonly);
            }
            out[v] = Some(s);
            for &t in succ[v].iter().filter(|&&t| t < n) {
                let mut inc = s;
                inc.clear(&kill[t]);
                let new = match state[t] {
                    None => inc,
                    Some(mut old) => {
                        old.and(&inc);
                        old
                    }
                };
                if state[t] != Some(new) {
                    state[t] = Some(new);
                    if !queued[t] {
                        queued[t] = true;
                        work.push(t);
                    }
                }
            }
        }
        let mut changed = false;
        for b in 0..n {
            if let Some(s) = out[b]
                && succ[b].len() > 1
                && !divergent[b]
                && !uniform_choice(&s, &blocks[b])
            {
                divergent[b] = true;
                changed = true;
            }
        }
        if !changed {
            return Some(state);
        }
    }
}

/// Indices of the VOTE and VOTEU instructions whose source predicate is
/// draw-uniform wherever they run and that write no ballot: their result
/// is the source predicate (ANY, ALL) or true (EQ) in any warp.
pub(crate) fn invariant_votes(code: &Code) -> BTreeSet<usize> {
    let insts = &code.insts;
    let is_vote = |i: &Inst| matches!(i.mnemonic, "VOTE" | "VOTEU");
    if !insts.iter().any(is_vote) {
        return BTreeSet::new();
    }
    let Ok(blocks) = super::cfg::flow(code) else {
        return BTreeSet::new();
    };
    let Some(state) = analyze(insts, &blocks) else {
        return BTreeSet::new();
    };
    let readonly = !insts.iter().any(|i| writes_memory(i.mnemonic));
    let mut yes = BTreeSet::new();
    let mut no = BTreeSet::new();
    for (b, blk) in blocks.iter().enumerate() {
        let Some(mut s) = state[b] else { continue };
        for k in blk.insts.clone() {
            let i = &insts[k];
            if is_vote(i) {
                let ballot = match i.ops.as_slice() {
                    [d, _, _] => !matches!(d, Operand::Reg(255, _) | Operand::UReg(63, _)),
                    [_, _] => false,
                    _ => true,
                };
                if !ballot && i.ops.last().is_some_and(|o| operand_uniform(&s, o)) {
                    yes.insert(k);
                } else {
                    no.insert(k);
                }
            }
            s = step(&s, i, readonly);
        }
    }
    yes.difference(&no).copied().collect()
}

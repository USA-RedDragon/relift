//! Warp operations whose results do not depend on which quads share a
//! warp.
//!
//! The guest packs fragment quads into warps by rasterization order, so
//! the lanes a warp-wide operation sees vary with the draw. Many uses do
//! not depend on it, and those are exact on any host with any subgroup
//! size. A forward dataflow over the instruction graph gives each register
//! and predicate a class:
//!
//! - uniform: the same in every invocation of the draw: pure arithmetic on
//!   immediates, constant buffer words, loads from uniform addresses of
//!   memory the draw does not write, and other uniform values;
//! - quad-position: a function of uniform values and the lane's place in
//!   its 2x2 quad (`SR_LANEID & 3`, the same on the host);
//! - varying: anything else that does not depend on the warp's other quads;
//! - lane-level values the idioms below follow (lane ids, quad masks, the
//!   active mask, an elected lane, warp reductions). Any other use of one
//!   of these makes the program depend on the warp's composition.
//!
//! A branch on a value that is not uniform makes every register written
//! between it and its immediate post-dominator quad-position or varying
//! there (the lanes rejoin with values from different paths).
//!
//! Predicates that `PLOP3` copies from another one (or its negation) are
//! followed back to that one, so a guard and a branch that test two copies
//! of a predicate agree on which lanes they name.
//!
//! The idioms:
//!
//! - a vote on a uniform predicate gives the predicate (ANY, ALL) or true
//!   (EQ) in any warp: it is translated per lane;
//! - the lane id masked to its quad position;
//! - a shuffle whose source lane is computed in the reader's own quad
//!   (from the lane id, its quad's mask of active lanes and `FLO`): a quad
//!   shuffle on the host;
//! - a quad reduction that takes a fast path when the whole warp is active
//!   (`VOTE.ALL R, PT, PT` compared with all ones) and a slow path over the
//!   quad's active lanes otherwise, with only exact, order-free operations
//!   (minimum, maximum, bitwise, moves) between: both paths give the same
//!   result on every quad, so the host may take either;
//! - warp-aggregated atomics: one lane elected by `FLO` of the active mask
//!   adds the `POPC` of that mask to a uniform address (or applies a
//!   uniform value, or the `REDUX` of a value, with MIN, MAX, AND or OR
//!   there). The memory result is the same however lanes are grouped.

use super::Code;
use super::cfg::FlowBlock;
use super::mem::lanes::{shfl_is_quad, warp_op};
use crate::{Inst, Operand};
use std::collections::BTreeSet;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Loc {
    R(u8),
    Ur(u8),
    P(u8),
    Up(u8),
}

/// The class of a value; instruction indices name the ballot (or REDUX)
/// a lane-level value comes from.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Cls {
    K(u32),
    U,
    Q,
    V,
    /// The lane id.
    Lane,
    /// A lane in the reader's own quad, at or below the reader's lane.
    QLow,
    /// A lane in the reader's own quad.
    QLane,
    /// A lane in the reader's own quad, or all ones.
    QLaneOrNone,
    /// The lane id with its quad position cleared.
    QBase,
    /// A mask within the reader's quad that holds the reader's bit.
    QMaskSelf,
    /// A mask within the reader's quad.
    QMask,
    /// A lane mask (`SR_LTMASK` and the like).
    Mask,
    /// The ballot of every active lane, taken at the instruction.
    Active(usize),
    /// The highest lane of that ballot.
    Leader(usize),
    /// The number of lanes in that ballot.
    Count(usize),
    /// Whether this lane is the leader of that ballot.
    Elect(usize),
    /// A REDUX with the operation (one of [`IDEMPOTENT`]) at the
    /// instruction.
    Reduced(usize, u8),
    /// Whether that ballot holds the whole warp.
    Full(usize),
    /// A vote of the predicate at the location (negated when the flag is
    /// set) over the warp: ANY when the second flag is set, else ALL.
    Vote(Loc, bool, bool),
    /// A value that the warp's composition decides except in the lanes
    /// where the predicate at the location has the given value.
    Cond(Loc, bool),
    Bad,
}

impl Cls {
    fn plain(self) -> bool {
        matches!(self, Cls::K(_) | Cls::U | Cls::Q | Cls::V)
    }

    /// Plain, or plain where a predicate holds.
    fn ok(self) -> bool {
        self.plain() || matches!(self, Cls::Cond(..))
    }

    fn uniform(self) -> bool {
        matches!(self, Cls::K(_) | Cls::U)
    }

    /// The rank of a lane value: each includes the ones below it.
    fn lane_rank(self) -> Option<u8> {
        match self {
            Cls::Lane => Some(0),
            Cls::QLow => Some(1),
            Cls::QLane => Some(2),
            Cls::QLaneOrNone => Some(3),
            _ => None,
        }
    }

    fn lane_of(rank: u8) -> Cls {
        [Cls::Lane, Cls::QLow, Cls::QLane, Cls::QLaneOrNone][rank.min(3) as usize]
    }

    fn join(self, o: Cls) -> Cls {
        use Cls::*;
        if self == o {
            return self;
        }
        if let (Some(a), Some(b)) = (self.lane_rank(), o.lane_rank()) {
            return Cls::lane_of(a.max(b).max(1));
        }
        match (self, o) {
            (K(_) | U, K(_) | U) => U,
            (K(_) | U | Q, K(_) | U | Q) => Q,
            (K(_) | U | Q | V, K(_) | U | Q | V) => V,
            (QMaskSelf | QMask, QMaskSelf | QMask) => QMask,
            (Cond(p, a), K(_) | U | Q | V) | (K(_) | U | Q | V, Cond(p, a)) => Cond(p, a),
            _ => Bad,
        }
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
struct St {
    r: Vec<Cls>,
    ur: Vec<Cls>,
    p: [Cls; 8],
    up: [Cls; 8],
    /// Predicates that hold another one's value (or its negation when the
    /// flag is clear), from `PLOP3` copies: (copy, original, same).
    alias: Vec<(Loc, Loc, bool)>,
}

impl St {
    fn new(c: Cls) -> St {
        St {
            r: vec![c; 256],
            ur: vec![c; 64],
            p: [c; 8],
            up: [c; 8],
            alias: Vec::new(),
        }
    }

    /// The predicate `l` names and the polarity it reads it with, through
    /// the copies `PLOP3` made.
    fn root(&self, l: Loc, pol: bool) -> (Loc, bool) {
        match self.alias.iter().find(|a| a.0 == l) {
            Some(&(_, o, same)) => (o, pol == same),
            None => (l, pol),
        }
    }

    /// Forgets the copies of and from predicate `l`, which is written.
    fn unalias(&mut self, l: Loc) {
        self.alias.retain(|a| a.0 != l && a.1 != l);
    }

    fn get(&self, l: Loc) -> Cls {
        match l {
            Loc::R(255) | Loc::Ur(63) => Cls::K(0),
            Loc::P(7) | Loc::Up(7) => Cls::U,
            Loc::R(n) => self.r[n as usize],
            Loc::Ur(n) => self.ur[n as usize % 64],
            Loc::P(n) => self.p[n as usize % 8],
            Loc::Up(n) => self.up[n as usize % 8],
        }
    }

    fn set(&mut self, l: Loc, c: Cls) {
        match l {
            Loc::R(n) => self.r[n as usize] = c,
            Loc::Ur(n) => self.ur[n as usize % 64] = c,
            Loc::P(n) => self.p[n as usize % 8] = c,
            Loc::Up(n) => self.up[n as usize % 8] = c,
        }
    }

    /// Calls `f` on every location and its class.
    fn each(&mut self, mut f: impl FnMut(Loc, &mut Cls)) {
        for (n, c) in self.r.iter_mut().enumerate() {
            f(Loc::R(n as u8), c);
        }
        for (n, c) in self.ur.iter_mut().enumerate() {
            f(Loc::Ur(n as u8), c);
        }
        for (n, c) in self.p.iter_mut().enumerate() {
            f(Loc::P(n as u8), c);
        }
        for (n, c) in self.up.iter_mut().enumerate() {
            f(Loc::Up(n as u8), c);
        }
    }

    fn join(&mut self, o: &St) {
        for (a, b) in self.r.iter_mut().zip(&o.r) {
            *a = a.join(*b);
        }
        for (a, b) in self.ur.iter_mut().zip(&o.ur) {
            *a = a.join(*b);
        }
        for (a, b) in self.p.iter_mut().zip(&o.p) {
            *a = a.join(*b);
        }
        for (a, b) in self.up.iter_mut().zip(&o.up) {
            *a = a.join(*b);
        }
        self.alias.retain(|a| o.alias.contains(a));
    }
}

/// A set of locations.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
struct Set {
    r: [u64; 4],
    ur: u64,
    p: u8,
    up: u8,
}

impl Set {
    fn add(&mut self, l: Loc) {
        match l {
            Loc::R(n) => self.r[n as usize / 64] |= 1 << (n % 64),
            Loc::Ur(n) => self.ur |= 1 << (n % 64),
            Loc::P(n) => self.p |= 1 << (n % 8),
            Loc::Up(n) => self.up |= 1 << (n % 8),
        }
    }

    fn or(&mut self, o: &Set) {
        for k in 0..4 {
            self.r[k] |= o.r[k];
        }
        self.ur |= o.ur;
        self.p |= o.p;
        self.up |= o.up;
    }

    /// Joins every location of the set in `s` with `c`.
    fn lift(&self, s: &mut St, c: Cls) {
        for n in 0..256 {
            if self.r[n / 64] >> (n % 64) & 1 == 1 {
                s.r[n] = s.r[n].join(c);
            }
        }
        for n in 0..64 {
            if self.ur >> n & 1 == 1 {
                s.ur[n] = s.ur[n].join(c);
            }
        }
        for n in 0..8 {
            if self.p >> n & 1 == 1 {
                s.p[n] = s.p[n].join(c);
            }
            if self.up >> n & 1 == 1 {
                s.up[n] = s.up[n].join(c);
            }
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

/// The registers a texture fetch writes, as the translation stores them:
/// the components the mask selects (all four without one) go in order to
/// Rd, Rd+1, Rd2, Rd2+1, or as half pairs to Rd and Rd2.
/// The registers (and the sparse fault predicate) a texture instruction
/// writes. The text names the destination at bits 64 (the third and fourth
/// components) before the one at bits 16 (the first two).
fn texture_dests(i: &Inst) -> Option<Vec<Loc>> {
    if !matches!(i.mnemonic, "TEX" | "TLD" | "TLD4" | "TXD") {
        return None;
    }
    let pred = match i.ops.first() {
        Some(Operand::Pred(n, _)) => Some(*n),
        _ => None,
    };
    let at = usize::from(pred.is_some());
    let reg = |k: usize| match i.ops.get(at + k) {
        Some(Operand::Reg(n, _)) => Some(*n),
        _ => None,
    };
    let (rd2, rd) = (reg(0)?, reg(1)?);
    let mask = match i.ops.last() {
        Some(Operand::Int(v)) if i.mnemonic != "TLD4" => *v as u32 & 0xf,
        Some(Operand::Hex(v)) if i.mnemonic != "TLD4" => *v as u32 & 0xf,
        _ => 0xf,
    };
    let n = mask.count_ones() as usize;
    let next = |r: u8| if r == 255 { 255 } else { r.saturating_add(1) };
    let regs: Vec<u8> = if i.mods.iter().any(|m| m.contains("F16")) {
        [rd, rd2].into_iter().take(n.div_ceil(2)).collect()
    } else {
        [rd, next(rd), rd2, next(rd2)].into_iter().take(n).collect()
    };
    let mut out: Vec<Loc> = regs.into_iter().filter(|&r| r != 255).map(Loc::R).collect();
    out.extend(pred.filter(|&n| n != 7).map(Loc::P));
    Some(out)
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

/// The registers an instruction writes or may write (see [`defs`]).
pub(super) fn written_regs(i: &Inst) -> Vec<u8> {
    let (certain, maybe) = defs(i);
    certain
        .into_iter()
        .chain(maybe)
        .filter_map(|l| match l {
            Loc::R(n) if n != 255 => Some(n),
            _ => None,
        })
        .collect()
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
    if matches!(i.mnemonic, "PLOP3" | "UPLOP3") {
        return (i.ops.iter().take(2).filter_map(loc).collect(), maybe);
    }
    let Some(first) = i.ops.first().and_then(loc) else {
        return (Vec::new(), maybe);
    };
    if let Some(d) = texture_dests(i) {
        return (d, maybe);
    }
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
    let mut exact = vec![first];
    for (k, o) in i.ops.iter().enumerate().skip(1) {
        match (o, loc(o)) {
            (Operand::Pred(_, false) | Operand::UPred(_, false), Some(l)) if k <= out_preds(i) => {
                exact.push(l)
            }
            (Operand::Pred(_, false) | Operand::UPred(_, false), Some(l)) => maybe.push(l),
            (Operand::Reg(..) | Operand::UReg(..), Some(l))
                if k == 1 && (tex || matches!(first, Loc::P(_) | Loc::Up(_))) =>
            {
                maybe.push(l);
                if tex {
                    maybe.extend(pair(l));
                }
                break;
            }
            _ => break,
        }
    }
    (exact, maybe)
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

/// The number of outputs right after the first operand: the carries of
/// an integer add, or the second result of a compare.
fn out_preds(i: &Inst) -> usize {
    match i.ops.first() {
        _ if matches!(i.mnemonic, "VOTE" | "VOTEU") => 0,
        Some(Operand::Pred(..) | Operand::UPred(..)) => usize::from(i.ops.len() > 2),
        Some(Operand::Reg(..) | Operand::UReg(..)) => i
            .ops
            .iter()
            .skip(1)
            .take(2)
            .take_while(|o| matches!(o, Operand::Pred(_, false) | Operand::UPred(_, false)))
            .count(),
        _ => 0,
    }
}

/// The class of a source operand.
fn cls(s: &St, o: &Operand) -> Cls {
    match o {
        Operand::Reg(..) | Operand::UReg(..) | Operand::Pred(..) | Operand::UPred(..) => {
            loc(o).map_or(Cls::Bad, |l| s.get(l))
        }
        Operand::Int(v) => Cls::K(*v as u32),
        Operand::Hex(v) => Cls::K(*v as u32),
        Operand::F32(v, _) => Cls::K(*v),
        Operand::F16x2(..)
        | Operand::F64Hi(_)
        | Operand::Target(_)
        | Operand::Barrier(_)
        | Operand::ScoreBoard(_) => Cls::U,
        Operand::CBuf { bank, index, .. } => {
            let b = match bank {
                crate::CBank::Imm(_) => Cls::U,
                crate::CBank::UReg(n) => s.get(Loc::Ur(*n)),
            };
            let x = index.as_deref().map_or(Cls::U, |x| cls(s, x));
            address(b.join(x))
        }
        Operand::Sym(n) if n.starts_with("c[") || n.starts_with('[') => sym(s, n),
        Operand::Sym(n) if n == "SRZ" => Cls::K(0),
        Operand::Mem { base, ubase, .. } => {
            let b = base.as_deref().map_or(Cls::U, |x| match x {
                Operand::Reg(n, _) => s.get(Loc::R(*n)).join(s.get(Loc::R(n.saturating_add(1)))),
                x => cls(s, x),
            });
            let u = ubase.as_deref().map_or(Cls::U, |x| cls(s, x));
            address(b.join(u))
        }
        _ => Cls::V,
    }
}

/// The class of a value read through an address of class `c`.
fn address(c: Cls) -> Cls {
    match c {
        Cls::K(_) => Cls::U,
        c if c.plain() => c,
        _ => Cls::Bad,
    }
}

/// A constant buffer or address operand left as text (`c[0x0][0x540]`,
/// `[R28.U32+UR8]`): the join of the registers it names.
fn sym(s: &St, text: &str) -> Cls {
    let mut c = Cls::U;
    let toks: Vec<&str> = text.split(|c: char| !c.is_ascii_alphanumeric()).collect();
    for (k, t) in toks.iter().enumerate() {
        let num = |p: &str| t.strip_prefix(p).and_then(|n| n.parse::<u8>().ok());
        let wide = toks.get(k + 1) == Some(&"64");
        if let Some(n) = num("UR") {
            c = c.join(s.get(Loc::Ur(n)));
            if wide {
                c = c.join(s.get(Loc::Ur(n.saturating_add(1))));
            }
        } else if let Some(n) = num("R") {
            c = c.join(s.get(Loc::R(n)));
            if wide {
                c = c.join(s.get(Loc::R(n.saturating_add(1))));
            }
        }
    }
    address(c)
}

/// Bitwise LOP3 of three words.
fn lop3(a: u32, b: u32, c: u32, lut: u32) -> u32 {
    let mut r = 0;
    for idx in 0..8 {
        if lut >> idx & 1 == 1 {
            let pick = |bit: u32, v: u32| if idx & bit != 0 { v } else { !v };
            r |= pick(4, a) & pick(2, b) & pick(1, c);
        }
    }
    r
}

/// The reductions that give the same memory whether a group of lanes
/// applies them once or each lane applies its own value.
const IDEMPOTENT: [&str; 4] = ["MIN", "MAX", "AND", "OR"];

/// The index in [`IDEMPOTENT`] of an integer reduction's operation.
fn idempotent(i: &Inst) -> Option<u8> {
    if i.mods
        .iter()
        .any(|m| m.contains("64") || m.starts_with('F'))
    {
        return None;
    }
    IDEMPOTENT.iter().position(|m| i.has(m)).map(|k| k as u8)
}

/// An integer RED: `Some(true)` for ADD, `Some(false)` for the
/// operations that give the same memory whether one lane or many apply a
/// value (MIN, MAX, AND, OR).
fn red_op(i: &Inst) -> Option<bool> {
    if i.mods
        .iter()
        .any(|m| m.starts_with('F') || m.starts_with("BF") || m.contains("64") || m.contains("16"))
    {
        return None;
    }
    if i.has("ADD") {
        Some(true)
    } else if i.has("MIN") || i.has("MAX") || i.has("AND") || i.has("OR") {
        Some(false)
    } else {
        None
    }
}

/// Instructions a whole-warp fast path and its slow path may hold for the
/// two to agree on every quad: exact and free of operand order.
fn order_free(i: &Inst) -> bool {
    matches!(
        i.mnemonic,
        "SHFL"
            | "FMNMX"
            | "IMNMX"
            | "MOV"
            | "UMOV"
            | "R2UR"
            | "S2R"
            | "S2UR"
            | "CS2R"
            | "VOTE"
            | "VOTEU"
            | "LOP3"
            | "ULOP3"
            | "PLOP3"
            | "SHF"
            | "USHF"
            | "FLO"
            | "UFLO"
            | "POPC"
            | "ISETP"
            | "UISETP"
            | "SEL"
            | "FSEL"
            | "BRA"
            | "NOP"
            | "BSSY"
            | "BSYNC"
            | "DEPBAR"
            | "WARPSYNC"
    ) || (i.mnemonic == "IMAD" && i.has("MOV"))
}

/// The translation choices for one program.
#[derive(Default)]
pub(crate) struct Plan {
    /// Votes on a uniform predicate, translated per lane.
    pub per_lane: BTreeSet<usize>,
    /// Warp-wide shuffles whose source lane is in the reader's own quad.
    pub quad_shfl: BTreeSet<usize>,
    /// Every warp-wide operation is one of the idioms.
    pub invariant: bool,
}

struct Walk<'a> {
    insts: &'a [Inst],
    readonly: bool,
    /// The instructions of the block being stepped, up to the current one.
    block: std::ops::Range<usize>,
    failed: bool,
    votes_yes: BTreeSet<usize>,
    votes_no: BTreeSet<usize>,
    quad_shfl: BTreeSet<usize>,
}

impl Walk<'_> {
    fn here(&self, k: usize) -> bool {
        self.block.contains(&k)
    }

    /// The classes an instruction gives its outputs, when it is one of the
    /// idioms or reads no lane-level value; `None` when it uses a
    /// lane-level value any other way.
    fn rule(&mut self, s: &St, i: &Inst, k: usize) -> Option<Vec<(Loc, Cls)>> {
        let c = |n: usize| i.ops.get(n).map_or(Cls::U, |o| cls(s, o));
        let at = |n: usize| i.ops.get(n).and_then(loc);
        let out = |n: usize, v: Cls| at(n).map(|l| vec![(l, v)]).unwrap_or_default();
        let m = i.mnemonic;
        let copy = match m {
            "MOV" | "UMOV" | "R2UR" if i.ops.len() == 2 => Some(1),
            "IMAD"
                if i.ops.len() == 4
                    && !i.has("WIDE")
                    && !i.has("HI")
                    && !i.has("X")
                    && c(1) == Cls::K(0)
                    && c(2) == Cls::K(0) =>
            {
                Some(3)
            }
            _ => None,
        };
        if let Some(src) = copy {
            return Some(out(0, c(src)));
        }
        match m {
            "S2R" | "S2UR" | "CS2R" => {
                let Some(Operand::Sym(n)) = i.ops.get(1) else {
                    return Some(out(0, Cls::V));
                };
                return Some(out(
                    0,
                    match n.as_str() {
                        "SRZ" => Cls::K(0),
                        "SR_LANEID" => Cls::Lane,
                        "SR_EQMASK" | "SR_LTMASK" | "SR_LEMASK" | "SR_GTMASK" | "SR_GEMASK" => {
                            Cls::Mask
                        }
                        _ => Cls::V,
                    },
                ));
            }
            "VOTE" | "VOTEU" => {
                let src = c(i.ops.len() - 1);
                let truth = matches!(i.ops.last(), Some(Operand::Pred(7, false)));
                let mut v = Vec::new();
                if i.ops.len() == 3 {
                    let ballot = if truth && !i.has("EQ") {
                        Cls::Active(k)
                    } else {
                        Cls::Bad
                    };
                    v.extend(out(0, ballot));
                }
                let pd = match i.ops.last() {
                    _ if src.uniform() => Cls::U,
                    Some(o @ (Operand::Pred(_, neg) | Operand::UPred(_, neg)))
                        if src.plain() && !i.has("EQ") =>
                    {
                        loc(o).map_or(Cls::Bad, |l| Cls::Vote(l, *neg, i.has("ANY")))
                    }
                    _ => Cls::Bad,
                };
                v.extend(out(i.ops.len() - 2, pd));
                let ballot = i.ops.len() == 3
                    && !matches!(i.ops[0], Operand::Reg(255, _) | Operand::UReg(63, _));
                if src.uniform() && !ballot {
                    self.votes_yes.insert(k);
                } else {
                    self.votes_no.insert(k);
                }
                return Some(v);
            }
            "LOP3" | "ULOP3" => return self.lop3(s, i),
            "SHF" | "USHF" => {
                let (lo, sh, hi) = (c(1), c(2), c(3));
                if sh == Cls::QBase && i.has("L") {
                    let mask = if i.has("HI") { (lo, hi) } else { (hi, lo) };
                    if let (Cls::K(0), Cls::K(v)) = mask
                        && v & !0xf == 0
                    {
                        let m = if v == 0xf { Cls::QMaskSelf } else { Cls::QMask };
                        return Some(out(0, m));
                    }
                }
            }
            "FLO" | "UFLO" if i.has("U32") && !i.has("SH") => match c(1) {
                Cls::QMaskSelf => return Some(out(0, Cls::QLane)),
                Cls::QMask => return Some(out(0, Cls::QLaneOrNone)),
                Cls::Active(a) => return Some(out(0, Cls::Leader(a))),
                _ => {}
            },
            "POPC" | "UPOPC" => match c(1) {
                Cls::QMask | Cls::QMaskSelf => return Some(out(0, Cls::V)),
                Cls::Active(a) => return Some(out(0, Cls::Count(a))),
                _ => {}
            },
            "IMNMX" | "UIMNMX"
                if i.has("U32") && matches!(i.ops.get(3), Some(Operand::Pred(7, false))) =>
            {
                if let (Some(a), Some(b)) = (c(1).lane_rank(), c(2).lane_rank()) {
                    return Some(out(0, Cls::lane_of(a.min(b).max(1))));
                }
            }
            "SHFL" => return self.shfl(s, i, k),
            "REDUX" => {
                if let (Some(op), true) = (idempotent(i), c(1).plain()) {
                    return Some(out(0, Cls::Reduced(k, op)));
                }
            }
            "ISETP" | "UISETP" if (i.has("EQ") || i.has("NE")) && i.ops.len() == 5 => {
                let (a, b) = (c(2), c(3));
                let pair = |x: Cls, y: Cls| -> Option<Cls> {
                    match (x, y) {
                        (Cls::Active(o), Cls::K(0xffff_ffff)) => Some(Cls::Full(o)),
                        (Cls::Lane, Cls::Leader(o)) if i.has("EQ") => Some(Cls::Elect(o)),
                        _ => None,
                    }
                };
                if let Some(r) = pair(a, b).or_else(|| pair(b, a))
                    && c(1) == Cls::U
                    && c(4) == Cls::U
                {
                    return Some(out(0, r));
                }
            }
            "RED" if matches!(i.guard.as_ref().map(|g| cls(s, g)), Some(Cls::Elect(_))) => {
                let Some(Cls::Elect(a)) = i.guard.as_ref().map(|g| cls(s, g)) else {
                    return None;
                };
                let add = red_op(i)?;
                let fine = self.here(a)
                    && cls(s, &i.ops[0]).uniform()
                    && match c(1) {
                        Cls::Count(o) => o == a && add,
                        Cls::Reduced(o, op) => self.here(o) && idempotent(i) == Some(op),
                        Cls::K(0) => true,
                        x => x.uniform() && !add,
                    };
                if fine {
                    return Some(Vec::new());
                }
                return None;
            }
            _ => {}
        }
        if warp_op(i).is_some() {
            return None;
        }
        let srcs_plain = i.ops.iter().skip(1 + out_preds(i)).all(|o| {
            let c = cls(s, o);
            c.plain() || (c.ok() && pure(i.mnemonic))
        }) && (i
            .ops
            .first()
            .is_none_or(|o| loc(o).is_some() || cls(s, o).plain()));
        srcs_plain.then(Vec::new)
    }

    fn lop3(&mut self, s: &St, i: &Inst) -> Option<Vec<(Loc, Cls)>> {
        let pred_out = matches!(i.ops.first(), Some(Operand::Pred(..) | Operand::UPred(..)));
        let base = usize::from(pred_out);
        let d = i.ops.get(base).and_then(loc);
        let p = if pred_out {
            i.ops.first().and_then(loc)
        } else {
            None
        };
        let srcs: Vec<Cls> = (0..3)
            .map(|n| i.ops.get(base + 1 + n).map_or(Cls::K(0), |o| cls(s, o)))
            .collect();
        let lut = match i.ops.get(base + 4) {
            Some(Operand::Int(v)) => *v as u32,
            Some(Operand::Hex(v)) => *v as u32,
            _ => return None,
        };
        let pin = i.ops.get(base + 5).map_or(Cls::U, |o| cls(s, o));
        if srcs.iter().all(|c| c.plain()) || !pin.plain() {
            return pin.plain().then(Vec::new);
        }
        let mut v = Vec::new();
        let lane_only = srcs.iter().filter(|c| **c == Cls::Lane).count() == 1
            && srcs.iter().all(|c| matches!(c, Cls::Lane | Cls::K(_)));
        if lane_only {
            let f = |l: u32| {
                let w: Vec<u32> = srcs
                    .iter()
                    .map(|c| if let Cls::K(x) = c { *x } else { l })
                    .collect();
                lop3(w[0], w[1], w[2], lut & 0xff)
            };
            let (r, pr) = if (0..32).all(|l| f(l) == f(l & 3)) {
                (Cls::Q, Cls::Q)
            } else if (0..32).all(|l| f(l) == l & !3) {
                (Cls::QBase, Cls::Bad)
            } else {
                return None;
            };
            v.extend(d.map(|l| (l, r)));
            v.extend(p.map(|l| (l, pr)));
            return Some(v);
        }
        let keeps = [0xf0u32, 0xcc, 0xaa];
        let in_quad = srcs
            .iter()
            .zip(keeps)
            .any(|(c, keep)| matches!(c, Cls::QMask | Cls::QMaskSelf) && lut & !keep & 0xff == 0);
        let masks = srcs.iter().all(|c| {
            matches!(
                c,
                Cls::QMask | Cls::QMaskSelf | Cls::Active(_) | Cls::Mask | Cls::K(_)
            )
        });
        if !in_quad || !masks {
            return None;
        }
        let bits: Vec<Option<u32>> = srcs
            .iter()
            .map(|c| match c {
                Cls::QMaskSelf | Cls::Active(_) => Some(1),
                Cls::K(0) => Some(0),
                Cls::K(0xffff_ffff) => Some(1),
                _ => None,
            })
            .collect();
        let has_self = match bits.as_slice() {
            [Some(a), Some(b), Some(c)] => lop3(*a, *b, *c, lut) & 1 == 1,
            _ => false,
        };
        let m = if has_self { Cls::QMaskSelf } else { Cls::QMask };
        v.extend(d.map(|l| (l, m)));
        v.extend(p.map(|l| (l, Cls::V)));
        Some(v)
    }

    fn shfl(&mut self, s: &St, i: &Inst, k: usize) -> Option<Vec<(Loc, Cls)>> {
        let c = |n: usize| i.ops.get(n).map_or(Cls::U, |o| cls(s, o));
        let at = |n: usize| i.ops.get(n).and_then(loc);
        let (v, lane, ctl) = (c(2), c(3), c(4));
        let mut out = Vec::new();
        if shfl_is_quad(i) {
            let value = match v {
                x if x.lane_rank().is_some() => Cls::QLane,
                x if x.uniform() => Cls::U,
                x if x.plain() => x,
                _ => return None,
            };
            if !lane.plain() && lane.lane_rank().is_none() {
                return None;
            }
            out.extend(at(1).map(|l| (l, value)));
            out.extend(at(0).map(|l| (l, Cls::Q)));
            return Some(out);
        }
        let (Cls::K(ctl), Some(src)) = (ctl, lane.lane_rank()) else {
            return None;
        };
        let seg = (ctl >> 8) & 0x1f;
        let clamp = ctl & 0x1f;
        let low = src <= 1;
        if !i.has("IDX") || seg != 0 || src > 2 || !(clamp == 0x1f || (clamp >= 0x1e && low)) {
            return None;
        }
        let value = match v.lane_rank() {
            Some(0) => Cls::lane_of(src.max(1)),
            Some(r) if r <= 2 => Cls::lane_of(r.max(src)),
            Some(_) => Cls::QLaneOrNone,
            None if v.uniform() => Cls::U,
            None if v.plain() => Cls::V,
            None => return None,
        };
        self.quad_shfl.insert(k);
        out.extend(at(1).map(|l| (l, value)));
        let valid = if clamp == 0x1f { Cls::U } else { Cls::Bad };
        out.extend(at(0).map(|l| (l, valid)));
        Some(out)
    }

    /// Steps `s` over instruction `k`.
    fn step(&mut self, s: &mut St, k: usize) {
        let i = &self.insts[k];
        let mut writes: Vec<(Loc, Cls)> = Vec::new();
        let (exact, maybe) = defs(i);
        let g = i.guard.as_ref().map_or(Cls::U, |g| cls(s, g));
        let under = match &i.guard {
            Some(o @ (Operand::Pred(_, neg) | Operand::UPred(_, neg))) => {
                loc(o).map(|l| s.root(l, !*neg))
            }
            _ => None,
        };
        let mut lifted = Vec::new();
        if let Some((p, pol)) = under {
            s.each(|l, c| {
                if *c == Cls::Cond(p, pol) {
                    *c = Cls::V;
                    lifted.push(l);
                }
            });
        }
        let gneg = matches!(
            i.guard,
            Some(Operand::Pred(_, true) | Operand::UPred(_, true))
        );
        let decided = match g {
            Cls::Vote(p, neg, any) => Some((Cls::Cond(p, any ^ neg), any ^ gneg)),
            _ => None,
        };
        let level = match g {
            Cls::Full(_) => Cls::Q,
            Cls::Elect(_) if i.mnemonic == "RED" => Cls::U,
            Cls::Vote(..) => Cls::U,
            g if g.plain() => g,
            _ => {
                self.failed = true;
                Cls::Bad
            }
        };
        let ruled = self.rule(&*s, i, k);
        let ruled = match ruled {
            Some(v) => v,
            None => {
                self.failed = true;
                exact.iter().chain(&maybe).map(|&l| (l, Cls::Bad)).collect()
            }
        };
        let readonly = self.readonly;
        let generic = |l: Loc| -> Cls {
            if let Some(&(_, c)) = ruled.iter().find(|(m, _)| *m == l) {
                return c;
            }
            let srcs = i
                .ops
                .iter()
                .skip(1 + out_preds(i))
                .fold(Cls::U, |a, o| a.join(cls(s, o)));
            let srcs = match srcs {
                Cls::K(_) => Cls::U,
                c if c.ok() => c,
                _ => Cls::Bad,
            };
            if pure(i.mnemonic) || (global_load(i.mnemonic) && (readonly || i.has("CONSTANT"))) {
                srcs
            } else if srcs == Cls::Bad {
                Cls::Bad
            } else {
                Cls::V
            }
        };
        let guarded = i.guard.is_some() && !matches!(i.guard, Some(Operand::Pred(7, false)));
        let ruled_at = |l: Loc| ruled.iter().any(|(m, _)| *m == l);
        let merge = |l: Loc, new: Cls| -> Cls {
            let old = s.get(l);
            let v = new.join(old).join(level);
            match under {
                Some((p, pol)) if new.ok() && level.plain() && old == Cls::Cond(p, !pol) => {
                    new.join(level)
                }
                Some((p, pol)) if new.ok() && level.plain() && !old.plain() => Cls::Cond(p, pol),
                _ => v,
            }
        };
        for &l in exact.iter().filter(|&&l| !ruled_at(l)) {
            let v = generic(l);
            writes.push((l, if guarded { merge(l, v) } else { v }));
        }
        for &l in maybe.iter().filter(|&&l| !ruled_at(l)) {
            writes.push((l, merge(l, generic(l))));
        }
        for &(l, c) in &ruled {
            writes.push((l, if guarded { merge(l, c) } else { c }));
        }
        if let Some((d, runs)) = decided {
            let fresh: Vec<Cls> = writes
                .iter()
                .map(|&(l, _)| {
                    if !runs {
                        s.get(l)
                    } else if let Some(&(_, c)) = ruled.iter().find(|(m, _)| *m == l) {
                        c
                    } else {
                        generic(l)
                    }
                })
                .collect();
            for ((_, c), f) in writes.iter_mut().zip(fresh) {
                *c = if f.ok() { d } else { Cls::Bad };
            }
        }
        for &l in &lifted {
            if let Some((p, pol)) = under {
                s.set(l, Cls::Cond(p, pol));
            }
        }
        let copy = if guarded { None } else { plop3_copy(i) };
        let copy = copy.map(|(d, o, same)| {
            let (o, same) = s.root(o, same);
            (d, o, same)
        });
        for (l, c) in writes {
            if matches!(l, Loc::P(_) | Loc::Up(_)) && !matches!(l, Loc::P(7) | Loc::Up(7)) {
                s.each(|_, x| {
                    if matches!(*x, Cls::Cond(p, _) | Cls::Vote(p, ..) if p == l) {
                        *x = Cls::Bad;
                    }
                });
                s.unalias(l);
            }
            s.set(l, c);
        }
        if let Some(a) = copy.filter(|a| a.0 != a.1) {
            s.alias.push(a);
        }
    }
}

/// `PLOP3.LUT Pd, PT, a, b, c` whose result is one source predicate or its
/// negation (the others being constant): (Pd, that source, whether Pd
/// equals it).
fn plop3_copy(i: &Inst) -> Option<(Loc, Loc, bool)> {
    if !matches!(i.mnemonic, "PLOP3" | "UPLOP3") {
        return None;
    }
    let [d, d2, a, b, c, lut, ..] = i.ops.as_slice() else {
        return None;
    };
    if !matches!(d2, Operand::Pred(7, false) | Operand::UPred(7, false)) {
        return None;
    }
    let lut = match lut {
        Operand::Hex(v) => *v as u32,
        Operand::Int(v) => u32::try_from(*v).ok()?,
        _ => return None,
    };
    let mut fixed = [None; 3];
    let mut var = None;
    for (k, o) in [a, b, c].into_iter().enumerate() {
        match o {
            Operand::Pred(7, neg) | Operand::UPred(7, neg) => fixed[k] = Some(!*neg),
            Operand::Pred(_, neg) | Operand::UPred(_, neg) if var.is_none() => {
                var = Some((k, loc(o)?, *neg));
            }
            _ => return None,
        }
    }
    let (k, src, neg) = var?;
    let eval = |x: bool| {
        let bit = |j: usize| u32::from(if j == k { x } else { fixed[j].unwrap_or(false) });
        lut >> (bit(0) << 2 | bit(1) << 1 | bit(2)) & 1 == 1
    };
    let (f0, f1) = (eval(false), eval(true));
    let d = loc(d)?;
    (f0 != f1).then_some((d, src, f1 != neg))
}

/// What every lane that takes successor `e` of the block knows: the
/// value of the predicate a one-predicate branch tests.
fn edge_fact(b: &FlowBlock, e: usize) -> Option<(Loc, bool)> {
    let [o @ (Operand::Pred(_, neg) | Operand::UPred(_, neg))] = b.conds.as_slice() else {
        return None;
    };
    if b.succs.len() != 2 {
        return None;
    }
    loc(o).map(|l| (l, (e == 0) != *neg))
}

/// How the block's choice of successor varies, given the state at its
/// end: `None` when the same in every invocation.
fn choice(s: &St, b: &FlowBlock) -> Option<Cls> {
    let mut c = Cls::U;
    for o in b.conds.iter().chain(b.selector.as_ref()) {
        c = c.join(match cls(s, o) {
            Cls::Full(_) => Cls::Q,
            x => x,
        });
    }
    match c {
        Cls::K(_) | Cls::U => None,
        Cls::Q => Some(Cls::Q),
        Cls::V => Some(Cls::V),
        _ => Some(Cls::Bad),
    }
}

/// Whether every instruction between block `b` and its immediate
/// post-dominator is [`order_free`].
fn region_order_free(
    insts: &[Inst],
    blocks: &[FlowBlock],
    succ: &[Vec<usize>],
    ip: &[Option<usize>],
    b: usize,
) -> bool {
    let n = blocks.len();
    let Some(j) = ip[b] else { return false };
    let mut seen = vec![false; n];
    let mut work: Vec<usize> = succ[b].clone();
    while let Some(v) = work.pop() {
        if v == j || v >= n || seen[v] {
            continue;
        }
        seen[v] = true;
        if !blocks[v].insts.clone().all(|k| order_free(&insts[k])) {
            return false;
        }
        work.extend(succ[v].iter().copied());
    }
    true
}

/// Picks the translation of the program's warp operations; `outputs` are
/// the registers the program's epilogue reads.
pub(crate) fn plan(code: &Code, outputs: &[u8]) -> Plan {
    let insts = &code.insts;
    if !insts.iter().any(|i| warp_op(i).is_some()) {
        return Plan::default();
    }
    let Ok(blocks) = super::cfg::flow(code) else {
        return Plan::default();
    };
    let n = blocks.len();
    if n == 0 {
        return Plan::default();
    }
    let succ: Vec<Vec<usize>> = blocks
        .iter()
        .map(|b| b.succs.iter().map(|t| t.unwrap_or(n)).collect())
        .collect();
    let ip = ipdoms(&succ);
    let defs: Vec<Set> = blocks
        .iter()
        .map(|b| {
            let mut d = Set::default();
            for k in b.insts.clone() {
                let (e, m) = defs(&insts[k]);
                for l in e.into_iter().chain(m) {
                    d.add(l);
                }
            }
            d
        })
        .collect();
    let mut divergent: Vec<Option<Cls>> = vec![None; n];
    loop {
        let mut kill: Vec<Vec<(Set, Cls)>> = vec![Vec::new(); n];
        let mut bail = false;
        for b in 0..n {
            let Some(level) = divergent[b] else { continue };
            let Some(j) = ip[b] else {
                bail = true;
                continue;
            };
            if j >= n {
                continue;
            }
            let mut region = Set::default();
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
            kill[j].push((region, level));
        }
        let mut w = Walk {
            insts,
            readonly: !insts.iter().any(|i| writes_memory(i.mnemonic)),
            block: 0..0,
            failed: bail,
            votes_yes: BTreeSet::new(),
            votes_no: BTreeSet::new(),
            quad_shfl: BTreeSet::new(),
        };
        let mut entry = St::new(Cls::V);
        entry.set(Loc::R(255), Cls::K(0));
        let mut state: Vec<Option<St>> = vec![None; n];
        state[0] = Some(entry);
        let mut out: Vec<Option<St>> = vec![None; n];
        let mut work = vec![0usize];
        let mut queued = vec![false; n];
        queued[0] = true;
        while let Some(v) = work.pop() {
            queued[v] = false;
            let Some(mut s) = state[v].clone() else {
                continue;
            };
            w.block = blocks[v].insts.clone();
            for k in blocks[v].insts.clone() {
                w.step(&mut s, k);
            }
            for (e, t) in succ[v].iter().enumerate().filter(|&(_, &t)| t < n) {
                let mut inc = s.clone();
                if let Some((p, holds)) = edge_fact(&blocks[v], e).map(|(p, h)| s.root(p, h))
                    && succ[v].iter().filter(|&x| x == t).count() == 1
                {
                    inc.each(|_, c| {
                        if let Cls::Cond(q, pol) = *c
                            && q == p
                        {
                            *c = if pol == holds { Cls::V } else { Cls::Bad };
                        }
                    });
                }
                for (set, level) in &kill[*t] {
                    set.lift(&mut inc, *level);
                }
                let new = match &state[*t] {
                    None => inc,
                    Some(old) => {
                        let mut o = old.clone();
                        o.join(&inc);
                        o
                    }
                };
                if state[*t].as_ref() != Some(&new) {
                    state[*t] = Some(new);
                    if !queued[*t] {
                        queued[*t] = true;
                        work.push(*t);
                    }
                }
            }
            out[v] = Some(s);
        }
        w.failed = bail;
        w.votes_yes.clear();
        w.votes_no.clear();
        w.quad_shfl.clear();
        for v in 0..n {
            let Some(mut s) = state[v].clone() else {
                continue;
            };
            w.block = blocks[v].insts.clone();
            for k in blocks[v].insts.clone() {
                w.step(&mut s, k);
            }
        }
        let mut changed = false;
        for b in 0..n {
            let Some(s) = &out[b] else { continue };
            if succ[b].contains(&n) && outputs.iter().any(|&r| !s.get(Loc::R(r)).plain()) {
                w.failed = true;
            }
            if succ[b].len() > 1 {
                let c = choice(s, &blocks[b]);
                let c = match (divergent[b], c) {
                    (Some(a), Some(b)) => Some(a.join(b)),
                    (a, b) => a.or(b),
                };
                if c == Some(Cls::Bad) {
                    w.failed = true;
                }
                let full = blocks[b]
                    .conds
                    .iter()
                    .any(|o| matches!(cls(s, o), Cls::Full(_)));
                if full && !region_order_free(insts, &blocks, &succ, &ip, b) {
                    w.failed = true;
                }
                if c != divergent[b] {
                    divergent[b] = c;
                    changed = true;
                }
            }
        }
        if changed {
            continue;
        }
        let per_lane: BTreeSet<usize> = w.votes_yes.difference(&w.votes_no).copied().collect();
        let invariant = !w.failed;
        return Plan {
            per_lane,
            quad_shfl: if invariant {
                w.quad_shfl
            } else {
                BTreeSet::new()
            },
            invariant,
        };
    }
}

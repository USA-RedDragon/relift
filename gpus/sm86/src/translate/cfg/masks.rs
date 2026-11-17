//! Which registers hold the mask of the lanes running an instruction: the
//! warp masks a WARPSYNC may wait for.
//!
//! The driver keeps the active lanes in a register as it goes: `VOTE.ALL
//! R, PT` (or `VOTEU`) gives the lanes running it, `VOTE.ANY R, p` the
//! ones among them where `p` holds, and after a branch on `p` the lanes
//! that stayed are that ballot (ANDed with the earlier mask, which changes
//! nothing). This follows those facts through each block and into a
//! successor that has no other predecessor; a branch on a condition that
//! is the same in every lane of the warp leaves them all as they were.
//! Any other write to a register or predicate forgets what it held.

use super::super::uniform::{Loc, written_locs};
use super::{Code, Graph, Term};
use crate::{Inst, Operand};
use std::collections::{HashMap, HashSet};

/// What a register holds: the ballot of a predicate (`None` for the
/// active mask) over the lanes running the current instruction.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Fact {
    Ballot(Option<(u8, bool)>),
    /// The same value in every lane of the warp.
    Same,
}

#[derive(Clone, Default, Debug)]
struct State {
    regs: HashMap<Loc, Fact>,
    /// Predicates that hold the same value in every lane of the warp.
    same: HashSet<u8>,
}

impl State {
    fn get(&self, o: &Operand) -> Option<Fact> {
        match o {
            Operand::Reg(255, _) | Operand::UReg(63, _) => Some(Fact::Same),
            Operand::UReg(n, _) => Some(self.regs.get(&Loc::Ur(*n)).copied().unwrap_or(Fact::Same)),
            Operand::Reg(n, _) => self.regs.get(&Loc::R(*n)).copied(),
            Operand::Int(_) | Operand::Hex(_) | Operand::CBuf { index: None, .. } => {
                Some(Fact::Same)
            }
            _ => None,
        }
    }

    fn same_pred(&self, o: &Operand) -> bool {
        match o {
            Operand::Pred(7, _) | Operand::UPred(..) => true,
            Operand::Pred(p, _) => self.same.contains(p),
            _ => false,
        }
    }

    /// Forgets what instruction `i` overwrites, and the ballots of a
    /// predicate it overwrites.
    fn kill(&mut self, i: &Inst) {
        if i.mnemonic == "WARPSYNC" {
            return;
        }
        for l in written_locs(i) {
            match l {
                Loc::P(p) => {
                    self.same.remove(&p);
                    self.regs
                        .retain(|_, f| !matches!(f, Fact::Ballot(Some((q, _))) if *q == p));
                }
                l => {
                    self.regs.remove(&l);
                }
            }
        }
    }

    fn step(&mut self, i: &Inst) {
        let guarded = i
            .guard
            .as_ref()
            .is_some_and(|g| !matches!(g, Operand::Pred(7, false)));
        let dest = i.ops.first().and_then(|o| match o {
            Operand::Reg(n, _) if *n != 255 => Some(Loc::R(*n)),
            Operand::UReg(n, _) if *n != 63 => Some(Loc::Ur(*n)),
            _ => None,
        });
        let fact = if guarded {
            None
        } else {
            match (i.mnemonic, i.ops.as_slice()) {
                ("VOTE" | "VOTEU", [_, _, src]) => match src {
                    Operand::Pred(7, false) | Operand::UPred(7, false) => Some(Fact::Ballot(None)),
                    Operand::Pred(p, n) if *p != 7 => Some(Fact::Ballot(Some((*p, *n)))),
                    _ => None,
                },
                ("MOV", [_, s]) | ("IMAD", [_, Operand::Reg(255, _), Operand::Reg(255, _), s])
                    if i.mnemonic == "MOV" || i.has("U32") || i.has("MOV") =>
                {
                    self.get(s)
                }
                ("LOP3", [_, a, b, c, Operand::Hex(lut), Operand::Pred(7, true)])
                    if *lut == 0xc0 || *lut == 0x80 =>
                {
                    let mut ops = vec![self.get(a), self.get(b)];
                    if *lut == 0x80 {
                        ops.push(self.get(c));
                    }
                    and(&ops)
                }
                _ => None,
            }
        };
        let same_pred = !guarded
            && i.mnemonic == "ISETP"
            && matches!(i.ops.first(), Some(Operand::Pred(..)))
            && matches!(i.ops.get(1), Some(Operand::Pred(7, false)))
            && i.ops.get(2).and_then(|o| self.get(o)).is_some()
            && i.ops.get(3).and_then(|o| self.get(o)).is_some()
            && i.ops.get(4).is_some_and(|o| self.same_pred(o));
        self.kill(i);
        if let (Some(d), Some(f)) = (dest, fact) {
            self.regs.insert(d, f);
        }
        if same_pred && let Some(Operand::Pred(p, _)) = i.ops.first() {
            self.same.insert(*p);
        }
    }

    /// The facts on the edge where the lanes for which `cond` is `taken`
    /// go on.
    fn edge(&self, cond: &[Operand], taken: bool) -> State {
        if cond.iter().all(|c| self.same_pred(c)) {
            return self.clone();
        }
        let mut s = State {
            regs: HashMap::new(),
            same: self.same.clone(),
        };
        s.regs.extend(
            self.regs
                .iter()
                .filter(|(_, f)| **f == Fact::Same)
                .map(|(l, f)| (*l, *f)),
        );
        if let [Operand::Pred(p, n)] = cond {
            let stay = (*p, *n != !taken);
            for (l, f) in &self.regs {
                if *f == Fact::Ballot(Some(stay)) {
                    s.regs.insert(*l, Fact::Ballot(None));
                }
            }
        }
        s
    }
}

/// The AND of masks: a ballot ANDed with the active mask (or with itself)
/// is that ballot.
fn and(ops: &[Option<Fact>]) -> Option<Fact> {
    let mut out = Fact::Ballot(None);
    for f in ops {
        match ((*f)?, out) {
            (Fact::Ballot(None), _) => {}
            (b, Fact::Ballot(None)) => out = b,
            (b, o) if b == o => {}
            _ => return None,
        }
    }
    Some(out)
}

impl Graph {
    /// The WARPSYNCs whose mask register holds the lanes running them and
    /// whose guard is the same in every lane of the warp.
    pub(super) fn active_mask_warpsyncs(&self, code: &Code) -> HashSet<usize> {
        let n = self.blocks.len();
        let mut preds = vec![0usize; n];
        for ss in &self.succs {
            for &s in ss {
                preds[s] += 1;
            }
        }
        let mut entry: Vec<Option<State>> = vec![None; n];
        entry[0] = Some(State::default());
        let mut out = HashSet::new();
        let mut done = vec![false; n];
        let mut work = vec![0];
        let mut order = Vec::new();
        while let Some(b) = work.pop() {
            if std::mem::replace(&mut done[b], true) {
                continue;
            }
            order.push(b);
            for &s in &self.succs[b] {
                work.push(s);
            }
        }
        for b in order {
            let mut st = entry[b].take().unwrap_or_default();
            let blk = &self.blocks[b];
            for k in blk.first..blk.end {
                let i = &code.insts[k];
                if i.mnemonic == "WARPSYNC"
                    && i.guard.as_ref().is_none_or(|g| st.same_pred(g))
                    && let Some(m @ Operand::Reg(..)) = i.ops.first()
                    && st.get(m) == Some(Fact::Ballot(None))
                {
                    out.insert(k);
                }
                st.step(i);
            }
            let edges: Vec<(usize, State)> = match &blk.term {
                Term::Jump(t) => vec![(*t, st.clone())],
                Term::Branch(conds, t, f) => {
                    vec![(*t, st.edge(conds, true)), (*f, st.edge(conds, false))]
                }
                _ => Vec::new(),
            };
            for (t, s) in edges {
                if t < n && preds[t] == 1 && t != 0 {
                    entry[t] = Some(s);
                }
            }
        }
        out
    }
}

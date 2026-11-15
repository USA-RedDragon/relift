//! Exhaustive evaluation of pure helpers, such as a multiply routine, over
//! every value of one unknown byte.
//!
//! A helper that stores nothing outside its own stack frame is run along
//! its single path with all other inputs as the caller has them. A register
//! pair whose result is `base + step * i` for every input value `i` comes
//! back as an indexed value; that is exact, not a guess, because every
//! input value was run.

use relift_cpu_sm83::{Cond, Immediate, Mnemonic, Operand, Reg16, decode};

use std::collections::BTreeSet;

use crate::func::Summary;
use crate::fx::FxMap;
use crate::loc::Loc;
use crate::state::{self, Base, Bv, Ctx, State, Sym, Val, WindowBank, Word, Write};

const STEPS: usize = 1024;
const EXHAUST: usize = 4096;
const SENTINEL: u16 = 0xFFFF;

/// Run `entry` as a call from `st`; the state at its return, or `None` when
/// the path depends on a value that is not known or the code has an effect
/// outside its stack.
pub(crate) fn run(cx: &Ctx<'_>, entry: Loc, st: State) -> Option<State> {
    run_to(cx, entry, st, SENTINEL, STEPS, None).and_then(|(s, a)| (a == SENTINEL).then_some(s))
}

/// The computed transfers inside `entry` for every value of entry register
/// `r`, with all other registers unknown: for each site, the addresses
/// control went to. `None` unless every run decides its path, decodes every
/// instruction it reaches and returns to its caller.
pub(crate) fn transfers(
    cx: &Ctx<'_>,
    entry: Loc,
    st: &State,
    r: usize,
    effects: &FxMap<Loc, Summary>,
    budget: &mut usize,
) -> Option<FxMap<Loc, BTreeSet<u16>>> {
    let mut out: FxMap<Loc, BTreeSet<u16>> = FxMap::default();
    for i in 0..=255u8 {
        let mut s = *st;
        for x in 0..7 {
            s.set(x, if x == r { Val::Const(i) } else { Val::Top });
        }
        let mut log = Trail::default();
        let steps = EXHAUST.min(*budget);
        let res = walk(cx, entry, s, SENTINEL, steps, Some(effects), Some(&mut log));
        *budget = budget.saturating_sub(log.used);
        let (_, a) = res?;
        if a != SENTINEL {
            return None;
        }
        for (site, t) in log.sites {
            out.entry(site).or_default().insert(t);
        }
    }
    Some(out)
}

/// Run `entry` as a call from `st` that returns to `ret`, along its single
/// path, until control leaves the callee's frame: a return, or a jump with
/// the return address popped. The state and the address control goes to,
/// or `None` when a branch, a target or the stack is not known. With
/// `effects`, stores are allowed: RAM bytes are tracked as the path writes
/// them, and a store to an unknown address forgets RAM and the bank.
pub(crate) fn run_to(
    cx: &Ctx<'_>,
    entry: Loc,
    st: State,
    ret: u16,
    steps: usize,
    effects: Option<&FxMap<Loc, Summary>>,
) -> Option<(State, u16)> {
    walk(cx, entry, st, ret, steps, effects, None)
}

/// The computed transfers one walk made and the steps it took.
#[derive(Default)]
struct Trail {
    sites: Vec<(Loc, u16)>,
    used: usize,
}

fn walk(
    cx: &Ctx<'_>,
    entry: Loc,
    mut st: State,
    ret: u16,
    steps: usize,
    effects: Option<&FxMap<Loc, Summary>>,
    mut log: Option<&mut Trail>,
) -> Option<(State, u16)> {
    let [rh, rl] = ret.to_be_bytes();
    st.push(Val::Const(rh), Val::Const(rl));
    let base = st.sp?;
    let mut pc = entry;
    let mut frames: Vec<i16> = Vec::new();
    for _ in 0..steps {
        if let Some(l) = log.as_deref_mut() {
            l.used += 1;
        }
        let bytes = cx.image.fetch(pc)?;
        let ins = decode(bytes).ok()?;
        let op = ins.opcode;
        if op.is_illegal() {
            return None;
        }
        let next = pc.addr.checked_add(u16::from(ins.length()))?;
        let taken = match op.dst {
            Operand::Cond(c) => {
                let v = match c {
                    Cond::Z => st.z,
                    Cond::Nz => st.z.map(|z| !z),
                    Cond::C => st.c,
                    Cond::Nc => st.c.map(|c| !c),
                };
                Some(v?)
            }
            _ => None,
        };
        let to = |st: &State, addr: u16| -> Option<Loc> {
            if addr < 0x4000 {
                Some(Loc::fixed(addr))
            } else if addr < 0x8000 {
                match st.window_bank(cx.model) {
                    WindowBank::Known(bank) => Some(Loc { bank, addr }),
                    _ => None,
                }
            } else {
                None
            }
        };
        match op.mnemonic {
            Mnemonic::Jr | Mnemonic::Jp if op.src == Operand::R16(Reg16::Hl) => {
                let Word::Const(a) = st.word(Reg16::Hl) else {
                    return None;
                };
                if frames.is_empty() && st.sp == Some(base + 2) {
                    return Some((st, a));
                }
                if let Some(l) = log.as_deref_mut() {
                    l.sites.push((pc, a));
                }
                pc = to(&st, a)?;
            }
            Mnemonic::Jr | Mnemonic::Jp => {
                if taken == Some(false) {
                    pc = Loc { addr: next, ..pc };
                } else {
                    pc = to(&st, ins.branch_target(pc.addr)?)?;
                }
            }
            Mnemonic::Call | Mnemonic::Rst => {
                if taken == Some(false) {
                    pc = Loc { addr: next, ..pc };
                } else {
                    let target = to(&st, ins.branch_target(pc.addr)?)?;
                    if let Some(sum) = effects.and_then(|s| s.get(&target))
                        && !sum.ctx
                        && !sum.pure
                        && sum.exits.len() == 1
                        && sum.exits.contains(&0)
                        && !sum.assumed
                    {
                        let before = st;
                        st.kill_all();
                        for (r, v) in sum.regs.iter().enumerate() {
                            if let Val::Entry(x) = v {
                                st.regs[r] = before.regs[usize::from(*x)];
                            } else {
                                st.set(r, *v);
                            }
                        }
                        st.lo = match sum.lo {
                            Bv::Entry => before.lo,
                            Bv::Param => Bv::Unknown,
                            v => v,
                        };
                        st.hi = match sum.hi {
                            Bv::Entry => before.hi,
                            Bv::Param => Bv::Unknown,
                            v => v,
                        };
                        pc = Loc { addr: next, ..pc };
                        continue;
                    }
                    let [h, l] = next.to_be_bytes();
                    st.push(Val::Const(h), Val::Const(l));
                    frames.push(st.sp?);
                    pc = target;
                }
            }
            Mnemonic::Ret | Mnemonic::Reti => {
                if taken == Some(false) {
                    pc = Loc { addr: next, ..pc };
                    continue;
                }
                let at = st.sp?;
                let (h, l) = st.pop()?;
                let Word::Const(a) = state::word_of(h, l) else {
                    return None;
                };
                if frames.last() == Some(&at) {
                    frames.pop();
                } else if frames.is_empty() && at == base {
                    return Some((st, a));
                } else if at > base {
                    return None;
                } else {
                    if frames.is_empty() && log.is_none() {
                        return None;
                    }
                    frames.retain(|f| *f > at);
                    if let Some(l) = log.as_deref_mut() {
                        l.sites.push((pc, a));
                    }
                }
                pc = to(&st, a)?;
            }
            Mnemonic::Halt | Mnemonic::Stop => return None,
            _ => {
                if matches!(op.dst, Operand::R16(Reg16::Sp))
                    && !matches!(ins.immediate, Immediate::I8(_))
                    && op.mnemonic != Mnemonic::Inc
                    && op.mnemonic != Mnemonic::Dec
                {
                    return None;
                }
                let mut events = Vec::new();
                state::step(cx, &mut st, &ins, &mut events);
                if !events.is_empty() {
                    effects?;
                    if events
                        .iter()
                        .any(|e| matches!(e, Write::Unknown | Write::Param))
                    {
                        st.lo = Bv::Unknown;
                        st.hi = Bv::Unknown;
                    }
                }
                if st.sp.is_none_or(|s| s > base + 2) {
                    return None;
                }
                pc = Loc { addr: next, ..pc };
            }
        }
        if pc.addr >= 0x8000 {
            return None;
        }
    }
    None
}

/// The results of calling `entry` from `st` for every value of register
/// `r`, as an indexed value per register pair, when every run succeeds and
/// some pair is linear in the index with a nonzero step.
pub(crate) fn fit(cx: &Ctx<'_>, entry: Loc, st: &State, r: usize) -> Option<[Val; 7]> {
    let n = st.bound[r].map_or(256, |b| usize::from(b).min(256));
    let same: Vec<usize> = (0..7).filter(|x| st.regs[*x] == st.regs[r]).collect();
    let mut top = *st;
    for x in &same {
        top.regs[*x] = Val::Top;
    }
    if let Some(end) = run(cx, entry, top)
        && end.regs.iter().all(|v| matches!(v, Val::Const(_)))
    {
        return None;
    }
    let mut outs: Vec<[Val; 7]> = Vec::with_capacity(n);
    for i in 0..n {
        let mut s = *st;
        for x in &same {
            s.set(*x, Val::Const(i as u8));
        }
        s.lo = st.lo;
        s.hi = st.hi;
        let end = run(cx, entry, s)?;
        outs.push(end.regs);
    }
    let first = outs[0];
    let id = (r as u16) << 12 | (st.ver[r] & 0x0FFF);
    let count = st.bound[r];
    let mut res = [Val::Top; 7];
    for x in 0..7 {
        if outs.iter().all(|o| o[x] == first[x]) && matches!(first[x], Val::Const(_)) {
            res[x] = first[x];
        }
    }
    let mut linear = false;
    for (h, l) in [
        (state::B, state::C),
        (state::D, state::E),
        (state::H, state::L),
    ] {
        let word = |o: &[Val; 7]| match (o[h], o[l]) {
            (Val::Const(a), Val::Const(b)) => Some(u16::from_be_bytes([a, b])),
            _ => None,
        };
        let Some(w0) = word(&first) else { continue };
        let Some(w1) = outs.get(1).and_then(word) else {
            continue;
        };
        let step = w1.wrapping_sub(w0);
        if step == 0 || step > 0xFF {
            continue;
        }
        let ok = outs
            .iter()
            .enumerate()
            .all(|(i, o)| word(o) == Some(w0.wrapping_add(step.wrapping_mul(i as u16))));
        if ok {
            let s = Sym {
                base: Base::Abs,
                off: w0,
                step: step as u8,
                count,
                ix: id,
            };
            let (vh, vl) = state::split(Word::Sym(s));
            res[h] = vh;
            res[l] = vl;
            linear = true;
        }
    }
    linear.then_some(res)
}

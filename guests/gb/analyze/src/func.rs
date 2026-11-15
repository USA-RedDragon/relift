//! Analysis of one function: the code reachable from an entry without
//! following calls, what it does to the bank registers and the stack, and
//! where it returns.
//!
//! A function is run once on its own with symbolic entry values. A callee
//! whose transfers, stores or results depend on its caller (it reads the
//! bytes after its call, jumps to a register argument, returns a pointer
//! derived from one) is run again inside each caller with the caller's
//! values: that is how inline arguments, bank-switch helpers, jump-table
//! dispatchers and computed returns are resolved.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet};
use std::rc::Rc;

use relift_cpu_sm83::{Cond, Immediate, Instruction, Mnemonic, Operand, Reg16, decode};

use crate::bankmodel::{BankModel, BankReg};
use crate::fx::{FxMap, FxSet};
use crate::image::Image;
use crate::loc::Loc;
use crate::report::{
    JumpTable as TableUse, RamPointerJump as CellUse, TableBound, Unresolved, UnresolvedKind,
};
use crate::state::{
    self, A, Base, Bv, Ctx, H, L, State, Sym, Val, WindowBank, Word, Write, word_of,
};

/// How a callee acts, found by running it with symbolic entry values.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Summary {
    /// The offsets past the return address it returns to.
    pub exits: BTreeSet<u16>,
    /// The registers at return: `Entry(r)` for the caller's register `r`.
    pub regs: [Val; 7],
    pub lo: Bv,
    pub hi: Bv,
    /// It has to be run inside each caller.
    pub ctx: bool,
    /// It writes a bank register, itself or through a callee.
    pub touches: bool,
    /// The entry register it writes to the low bank register.
    pub bank_reg: Option<usize>,
    /// The entry registers of the address it jumps to.
    pub target_regs: Option<(usize, usize)>,
    /// It stores nothing outside its stack and transfers nowhere unknown.
    pub pure: bool,
    /// No return was found and the exits are assumed: a return to the
    /// address after the call.
    pub assumed: bool,
    /// It is run inside its callers only to resolve the addresses it
    /// stores to; its transfers do not depend on them.
    pub store_only: bool,
    /// Its transfers do not depend on its callers: what depends on them is
    /// only the addresses it stores to and exit values that are an entry
    /// register pair plus a constant.
    pub relative: bool,
    /// Its computed transfers were found by running it for every value of
    /// the entry register they depend on.
    pub enumerated: bool,
}

/// How control reaches a callee.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum CallKind {
    /// `call`.
    Call,
    /// `rst`.
    Rst,
    /// A call through a bank-switch trampoline.
    Trampoline,
    /// A computed transfer: a jump through a register, a pushed address, a
    /// table or a pointer kept in RAM.
    Indirect,
}

/// A transfer to another function.
#[derive(Clone, Copy, Debug)]
pub(crate) struct CallSite {
    pub site: Loc,
    pub target: Loc,
    pub kind: CallKind,
}

/// A transfer into RAM, where code the ROM copied there would run.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct RamTransfer {
    /// The instruction.
    pub site: Loc,
    /// The target address.
    pub target: u16,
}

/// A loop that reads and writes memory, with the constants near it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CopyLoop {
    /// The backward branch closing the loop.
    pub at: Loc,
    /// Constant 16-bit loads in the function that name ROM addresses.
    pub sources: Vec<u16>,
    /// Constant destinations: RAM addresses loaded into a register pair and
    /// high-page addresses named by `ld c,n`.
    pub dests: Vec<u16>,
    /// Constant counts loaded into `b`.
    pub counts: Vec<u8>,
}

/// A constant loaded into a register pair that may be a code address.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct AddressTaken {
    /// The instruction.
    pub site: Loc,
    /// The constant.
    pub value: u16,
    /// Where the constant points, when the bank is known.
    pub target: Option<Loc>,
    /// The RAM address of the low byte when two constant bytes were stored
    /// there, which makes the constant a stored pointer.
    pub stored_at: Option<u16>,
}

/// A value stored as a word to a RAM address.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) enum Elem {
    /// A constant, with the window bank at the store and the constant bytes
    /// stored on the same path before it.
    Const {
        value: u16,
        bank: Option<u16>,
        with: Vec<(u16, u8)>,
    },
    /// The return address of a function plus an offset.
    Ret { func: Loc, k: u16 },
    /// A word read from a table in ROM, with the bytes of the same table
    /// row stored on the same path at other addresses.
    Table { sym: Sym, with: Vec<(u16, Sym)> },
}

/// A store to RAM seen on some path.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum Store {
    Word {
        addr: u16,
        elem: Elem,
        site: Loc,
    },
    /// A byte that is not part of a known word.
    Byte {
        addr: u16,
        value: Option<u8>,
    },
}

/// What one run found.
#[derive(Clone, Debug, Default)]
pub(crate) struct Outcome {
    pub body: BTreeMap<Loc, u8>,
    pub calls: Vec<CallSite>,
    pub exits: Vec<(u16, State)>,
    pub escapes: Vec<(Val, Val, State)>,
    pub unresolved: Vec<(Unresolved, Dep)>,
    pub ram: Vec<RamTransfer>,
    pub copies: Vec<CopyLoop>,
    pub taken: Vec<AddressTaken>,
    pub stores: Vec<Store>,
    pub dyn_writes: Vec<Loc>,
    pub tables: Vec<TableUse>,
    pub cells: Vec<CellUse>,
    pub deps: BTreeSet<Loc>,
    pub cell_deps: BTreeSet<u16>,
    pub inlined: BTreeSet<Loc>,
    pub entry_dep: bool,
    pub computed_dep: bool,
    pub store_dep: bool,
    pub impure: bool,
    pub popped_ret: bool,
    /// It halts or enables interrupts, so an interrupt handler may leave
    /// a loop that has no exit of its own.
    pub waits: bool,
    pub enumerated: bool,
    pub touches: bool,
    pub bank_reg: Option<usize>,
    pub target_regs: Option<(usize, usize)>,
}

/// How an unresolved transfer depends on the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Dep {
    No,
    /// It is a symbolic entry value: a caller's constants resolve it.
    Symbolic,
    /// It is computed from entry values: a caller's constants may resolve
    /// it, but a caller that passes on its own entry values does not
    /// become context-dependent itself.
    Computed,
}

/// The cells seen so far: their word values and whether some byte stored
/// there is not part of a known word.
#[derive(Clone, Debug, Default)]
pub(crate) struct CellInfo {
    pub elems: BTreeSet<Elem>,
    pub loose: bool,
}

/// Results of evaluating pure helpers, by callee and caller state.
pub(crate) type Probes = FxMap<(Loc, State), Option<[Val; 7]>>;

pub(crate) struct Env<'a> {
    pub model: &'a BankModel,
    pub image: &'a Image<'a>,
    pub summaries: &'a FxMap<Loc, Summary>,
    pub cells: &'a FxMap<u16, CellInfo>,
    pub known: &'a FxSet<Loc>,
    pub limits: &'a FxMap<Loc, usize>,
    pub cache: RefCell<FxMap<(Loc, State), Rc<Outcome>>>,
    pub probes: &'a RefCell<Probes>,
    pub budget: RefCell<usize>,
    pub inline_body: RefCell<FxMap<Loc, FxMap<Loc, u8>>>,
    pub transfers: RefCell<TransferCache>,
}

/// The computed transfers of a function for every value of one entry
/// register, by site.
pub(crate) type Transfers = FxMap<Loc, BTreeSet<u16>>;

/// [`Transfers`] by function and entry register; `None` when the runs do
/// not all end in a return.
pub(crate) type TransferCache = FxMap<(Loc, usize), Option<Rc<Transfers>>>;

const MAX_DEPTH: u8 = 4;
const MAX_CHAIN: u8 = 6;
const MAX_TABLE: usize = 256;
const MAX_SPS: u8 = 4;
const EXIT_STEPS: usize = 20_000;
const ENUM_STEPS: usize = 65_536;

/// How a run started.
#[derive(Clone, Copy, Debug)]
enum Mode {
    /// The function on its own: returns are pops of the symbolic return
    /// address at stack offset 0.
    Alone,
    /// Inside a caller: the return address is the constant `ret`, pushed at
    /// stack offset `e`.
    Inline { ret: u16, e: i16, depth: u8 },
}

struct Run<'a> {
    env: &'a Env<'a>,
    func: Loc,
    root: Loc,
    mode: Mode,
    record_body: bool,
    out: Outcome,
    consts16: Vec<(u16, Reg16)>,
    consts_c: Vec<u8>,
    consts_b: Vec<u8>,
    branches: Vec<(Loc, Loc)>,
    work: Vec<Key>,
    states: FxMap<Key, usize>,
    store: Vec<State>,
    body: FxMap<Loc, u8>,
    sps: FxMap<Loc, u8>,
    tails: BTreeSet<Loc>,
}

type Key = (Loc, Option<i16>, u32);

fn key_of(loc: Loc, st: &State) -> Key {
    let cell = match st.lo {
        Bv::Cell(x) => 0x1_0000 | u32::from(x),
        _ => 0,
    };
    (loc, st.sp, cell)
}

fn is_ram(addr: u16) -> bool {
    addr >= 0x8000
}

fn pin(model: &BankModel, loc: Loc, st: &mut State) {
    if loc.is_window() {
        let (lo, hi) = model.split(loc.bank);
        if model.effective(lo, hi).is_ok_and(|b| b == loc.bank) {
            st.lo = Bv::Known(lo);
            st.hi = Bv::Known(u16::from(hi));
        }
    }
}

/// Run `entry` on its own from `st0` (a symbolic state, or a root's state).
pub(crate) fn run_alone(env: &Env<'_>, entry: Loc, st0: State) -> Outcome {
    let mut r = Run::new(env, entry, entry, Mode::Alone, true);
    r.explore(st0);
    r.out.body = r.body.drain().collect();
    r.find_copies();
    r.out
}

impl<'a> Run<'a> {
    fn new(env: &'a Env<'a>, func: Loc, root: Loc, mode: Mode, record_body: bool) -> Run<'a> {
        Run {
            env,
            func,
            root,
            mode,
            record_body,
            out: Outcome::default(),
            consts16: Vec::new(),
            consts_c: Vec::new(),
            consts_b: Vec::new(),
            branches: Vec::new(),
            work: Vec::new(),
            states: FxMap::default(),
            store: Vec::new(),
            body: FxMap::default(),
            sps: FxMap::default(),
            tails: BTreeSet::new(),
        }
    }

    fn cx(&self) -> Ctx<'a> {
        Ctx {
            model: self.env.model,
            image: self.env.image,
        }
    }

    fn unresolved(&mut self, at: Loc, kind: UnresolvedKind, detail: String, entry_dep: bool) {
        let dep = if entry_dep { Dep::Symbolic } else { Dep::No };
        self.unresolved_dep(at, kind, detail, dep);
    }

    fn unresolved_dep(&mut self, at: Loc, kind: UnresolvedKind, detail: String, dep: Dep) {
        match dep {
            Dep::Symbolic => self.out.entry_dep = true,
            Dep::Computed => self.out.computed_dep = true,
            Dep::No => {}
        }
        self.out
            .unresolved
            .push((Unresolved { at, kind, detail }, dep));
    }

    fn depth(&self) -> u8 {
        match self.mode {
            Mode::Alone => 0,
            Mode::Inline { depth, .. } => depth,
        }
    }

    fn entry_sp(&self) -> i16 {
        match self.mode {
            Mode::Alone => 0,
            Mode::Inline { e, .. } => e,
        }
    }

    fn enter(&mut self, loc: Loc, mut st: State) {
        pin(self.env.model, loc, &mut st);
        if loc != self.func
            && st.sp == Some(self.entry_sp())
            && self.env.summaries.get(&loc).is_some_and(|s| s.enumerated)
        {
            self.tails.insert(loc);
        }
        let mut key = key_of(loc, &st);
        if !self.states.contains_key(&key) {
            let n = self.sps.entry(loc).or_insert(0);
            if *n >= MAX_SPS {
                st.sp = None;
                if matches!(st.lo, Bv::Cell(_)) {
                    st.lo = Bv::Unknown;
                }
                key = key_of(loc, &st);
            } else {
                *n += 1;
            }
        }
        match self.states.get(&key) {
            Some(i) => {
                if self.store[*i].merge(&st) {
                    self.work.push(key);
                }
            }
            None => {
                self.states.insert(key, self.store.len());
                self.store.push(st);
                self.work.push(key);
            }
        }
    }

    fn flow(&self, st: &State, addr: u16) -> Result<Loc, (UnresolvedKind, bool)> {
        if addr < 0x4000 {
            return Ok(Loc::fixed(addr));
        }
        match st.window_bank(self.env.model) {
            WindowBank::Known(bank) => Ok(Loc { bank, addr }),
            WindowBank::OutOfRange(_) => Err((UnresolvedKind::BankOutOfRange, false)),
            WindowBank::Entry | WindowBank::Param => Err((UnresolvedKind::UnknownBank, true)),
            WindowBank::Cell(_) | WindowBank::Unknown => Err((UnresolvedKind::UnknownBank, false)),
        }
    }

    fn explore(&mut self, st0: State) {
        self.enter(self.func, st0);
        while let Some(key) = self.work.pop() {
            let loc = key.0;
            {
                let mut b = self.env.budget.borrow_mut();
                if *b == 0 {
                    break;
                }
                *b -= 1;
            }
            let Some(st) = self.states.get(&key).map(|i| self.store[*i]) else {
                continue;
            };
            self.step_at(loc, st);
        }
    }

    fn step_at(&mut self, loc: Loc, mut st: State) {
        let Some(bytes) = self.env.image.fetch(loc) else {
            self.unresolved(loc, UnresolvedKind::OutsideRom, String::new(), false);
            return;
        };
        let Ok(ins) = decode(bytes) else {
            self.unresolved(loc, UnresolvedKind::RunsOffWindow, String::new(), false);
            return;
        };
        if ins.opcode.is_illegal() {
            self.unresolved(
                loc,
                UnresolvedKind::Illegal,
                format!("${:02X}", bytes[0]),
                false,
            );
            return;
        }
        let len = ins.length();
        if self.record_body {
            self.body.insert(loc, len);
        } else {
            self.env
                .inline_body
                .borrow_mut()
                .entry(self.func)
                .or_default()
                .insert(loc, len);
        }
        let next_addr = loc.addr.wrapping_add(u16::from(len));
        let next = if next_addr < loc.addr {
            None
        } else {
            Some(next_addr)
        };
        self.record_consts(loc, &st, &ins);
        let mn = ins.opcode.mnemonic;
        let cond = match ins.opcode.dst {
            Operand::Cond(c) => Some(c),
            _ => None,
        };
        match mn {
            Mnemonic::Jr
            | Mnemonic::Jp
            | Mnemonic::Call
            | Mnemonic::Rst
            | Mnemonic::Ret
            | Mnemonic::Reti => {
                let (taken, mut fall) = self.branch_states(&st, cond);
                if fall.is_none()
                    && cond.is_some()
                    && ins.branch_target(loc.addr) == Some(loc.addr)
                    && let Some(t) = taken
                {
                    let mut s = t;
                    s.kill_memory();
                    fall = Some(s);
                }
                if let Some(t) = taken {
                    self.control(loc, &ins, t, next);
                }
                if let Some(f) = fall {
                    self.fall(loc, next, f);
                }
            }
            _ => {
                let mut events = Vec::new();
                state::step(&self.cx(), &mut st, &ins, &mut events);
                if mn == Mnemonic::Pop && !st.switched && st.sp.is_some_and(|s| s > self.entry_sp())
                {
                    self.out.popped_ret = true;
                }
                for w in events {
                    self.write_event(loc, &st, w);
                }
                if mn == Mnemonic::Stop {
                    st.kill_all();
                }
                if mn == Mnemonic::Halt {
                    st.kill_memory();
                    self.halt_bug(loc, next, st);
                }
                if matches!(mn, Mnemonic::Halt | Mnemonic::Ei) {
                    self.out.waits = true;
                }
                self.fall(loc, next, st);
            }
        }
    }

    /// After a halt with interrupts disabled and one pending, the next
    /// instruction runs twice.
    fn halt_bug(&mut self, loc: Loc, next: Option<u16>, st: State) {
        let Some(n) = next else {
            return;
        };
        let at = Loc { addr: n, ..loc };
        let Some(ins) = self.env.image.fetch(at).and_then(|b| decode(b).ok()) else {
            return;
        };
        let mn = ins.opcode.mnemonic;
        if ins.opcode.is_illegal()
            || matches!(
                mn,
                Mnemonic::Jr
                    | Mnemonic::Jp
                    | Mnemonic::Call
                    | Mnemonic::Rst
                    | Mnemonic::Ret
                    | Mnemonic::Reti
                    | Mnemonic::Halt
                    | Mnemonic::Stop
            )
        {
            return;
        }
        let mut s = st;
        let mut events = Vec::new();
        state::step(&self.cx(), &mut s, &ins, &mut events);
        for w in events {
            self.write_event(at, &s, w);
        }
        self.fall(loc, next, s);
    }

    fn branch_states(&self, st: &State, cond: Option<Cond>) -> (Option<State>, Option<State>) {
        let Some(c) = cond else {
            return (Some(*st), None);
        };
        let known = match c {
            Cond::Z => st.z,
            Cond::Nz => st.z.map(|z| !z),
            Cond::C => st.c,
            Cond::Nc => st.c.map(|c| !c),
        };
        let mut t = *st;
        let mut f = *st;
        if let Some((ver, n)) = st.cp
            && ver == st.ver[A]
        {
            let lt = |s: &mut State| {
                s.bound[A] = Some(s.bound[A].map_or(u16::from(n), |b| b.min(u16::from(n))));
            };
            match c {
                Cond::C => lt(&mut t),
                Cond::Nc => lt(&mut f),
                _ => {}
            }
        }
        if let Some((ver, x)) = st.zword
            && ver == st.ver[A]
        {
            match c {
                Cond::Nz => t.nonzero = Some(x),
                Cond::Z => f.nonzero = Some(x),
                _ => {}
            }
        }
        if let Some((ver, x)) = st.zcell
            && ver == st.ver[A]
        {
            match c {
                Cond::Z => t.know_cell(x, Val::Const(0)),
                Cond::Nz => f.know_cell(x, Val::Const(0)),
                _ => {}
            }
        }
        if let Some((sum, hi)) = st.carry
            && matches!(c, Cond::C | Cond::Nc)
        {
            for r in [state::B, state::D, H] {
                if st.regs[r] == hi {
                    let (with, without) = if c == Cond::C {
                        (&mut t, &mut f)
                    } else {
                        (&mut f, &mut t)
                    };
                    with.regs[r] = Val::HiNoCarry(sum);
                    without.regs[r] = Val::Hi(sum);
                    with.carry = None;
                    without.carry = None;
                }
            }
        }
        match known {
            Some(true) => (Some(t), None),
            Some(false) => (None, Some(f)),
            None => (Some(t), Some(f)),
        }
    }

    fn fall(&mut self, loc: Loc, next: Option<u16>, st: State) {
        let Some(n) = next else {
            self.unresolved(loc, UnresolvedKind::RunsOffWindow, String::new(), false);
            return;
        };
        if is_ram(n) {
            self.out.ram.push(RamTransfer {
                site: loc,
                target: n,
            });
            return;
        }
        if !loc.is_window() && n >= 0x4000 {
            match self.flow(&st, n) {
                Ok(t) => self.enter(t, st),
                Err((k, dep)) => self.unresolved(loc, k, format!("falls into ${n:04X}"), dep),
            }
            return;
        }
        if loc.is_window() != (n >= 0x4000) {
            self.unresolved(
                loc,
                UnresolvedKind::RunsOffWindow,
                format!("falls into ${n:04X}"),
                false,
            );
            return;
        }
        self.enter(Loc { addr: n, ..loc }, st);
    }

    fn control(&mut self, loc: Loc, ins: &Instruction, mut st: State, next: Option<u16>) {
        match ins.opcode.mnemonic {
            Mnemonic::Ret | Mnemonic::Reti => {
                let Some((h, l)) = st.pop() else {
                    self.unresolved(
                        loc,
                        UnresolvedKind::IndirectJump,
                        "return with the stack pointer not known".to_owned(),
                        false,
                    );
                    return;
                };
                self.transfer(loc, h, l, st, false, 0);
            }
            Mnemonic::Jp if ins.opcode.src == Operand::R16(Reg16::Hl) => {
                let (h, l) = (st.regs[H], st.regs[L]);
                let single =
                    matches!(self.mode, Mode::Alone) && matches!(word_of(h, l), Word::Const(_));
                self.transfer(loc, h, l, st, single, 0);
            }
            Mnemonic::Jr | Mnemonic::Jp => {
                let Some(addr) = ins.branch_target(loc.addr) else {
                    return;
                };
                if is_ram(addr) {
                    self.out.ram.push(RamTransfer {
                        site: loc,
                        target: addr,
                    });
                    return;
                }
                match self.flow(&st, addr) {
                    Ok(t) => {
                        self.branches.push((loc, t));
                        self.enter(t, st);
                    }
                    Err((k, dep)) => {
                        self.unresolved(loc, k, format!("target ${addr:04X}"), dep);
                    }
                }
            }
            Mnemonic::Call | Mnemonic::Rst => {
                let Some(addr) = ins.branch_target(loc.addr) else {
                    return;
                };
                let kind = if ins.opcode.mnemonic == Mnemonic::Rst {
                    CallKind::Rst
                } else {
                    CallKind::Call
                };
                let Some(ret) = next else {
                    self.unresolved(loc, UnresolvedKind::RunsOffWindow, String::new(), false);
                    return;
                };
                if is_ram(addr) {
                    self.out.ram.push(RamTransfer {
                        site: loc,
                        target: addr,
                    });
                    let mut s = st;
                    s.kill_all();
                    self.fall(loc, Some(ret), s);
                    return;
                }
                match self.flow(&st, addr) {
                    Ok(t) => self.call(loc, t, kind, ret, st),
                    Err((k, dep)) => {
                        self.unresolved(loc, k, format!("target ${addr:04X}"), dep);
                    }
                }
            }
            _ => {}
        }
        let _ = &mut st;
    }

    /// After a plain callee returns to the address in `slot + k`.
    fn apply(&self, sum: &Summary, st: &State) -> State {
        let mut s = *st;
        for r in 0..7 {
            let v = match sum.regs[r] {
                Val::Entry(x) => {
                    let x = usize::from(x);
                    let (v, b, ver) = (st.regs[x], st.bound[x], st.ver[x]);
                    s.regs[r] = v;
                    s.bound[r] = b;
                    s.ver[r] = ver;
                    continue;
                }
                v if v.depends_on_entry() => Val::Top,
                v => v,
            };
            s.set(r, v);
        }
        for (h, l) in [(state::B, state::C), (state::D, state::E), (H, L)] {
            if let (Val::Hi(a), Val::Lo(b)) = (sum.regs[h], sum.regs[l])
                && a == b
                && a.step == 0
                && let Base::Pair(eh, el) = a.base
            {
                let w = word_of(st.regs[usize::from(eh)], st.regs[usize::from(el)]);
                let w = match w {
                    Word::Const(_) | Word::Sym(_) => state::add_word(w, Word::Const(a.off)),
                    _ => Word::Other,
                };
                let (vh, vl) = state::split(w);
                s.set(h, vh);
                s.set(l, vl);
            }
        }
        s.lo = match sum.lo {
            Bv::Entry => st.lo,
            Bv::Param => Bv::Unknown,
            v => v,
        };
        s.hi = match sum.hi {
            Bv::Entry => st.hi,
            Bv::Param => Bv::Unknown,
            v => v,
        };
        s.kill_memory();
        s
    }

    fn summary(&mut self, t: Loc) -> Option<Summary> {
        self.out.deps.insert(t);
        self.env.summaries.get(&t).cloned()
    }

    fn call(&mut self, site: Loc, target: Loc, kind: CallKind, ret: u16, st: State) {
        self.out.calls.push(CallSite { site, target, kind });
        let Some(sum) = self.summary(target) else {
            return;
        };
        if sum.touches {
            self.out.touches = true;
        }
        if !sum.pure {
            self.out.impure = true;
        }
        if !sum.ctx
            && sum.assumed
            && let Some(exit) = self.run_exit(target, &st, ret)
        {
            let mut s = st;
            s.kill_all();
            self.fall(site, Some(exit), s);
            return;
        }
        if !sum.ctx {
            let probed = if sum.pure && sum.regs.contains(&Val::Top) {
                self.probe(target, &st)
            } else {
                None
            };
            for k in &sum.exits {
                let mut s = self.apply(&sum, &st);
                if let Some(regs) = probed {
                    for (r, v) in regs.iter().enumerate() {
                        if *v != Val::Top {
                            s.set(r, *v);
                        }
                    }
                }
                self.fall(site, Some(ret.wrapping_add(*k)), s);
            }
            return;
        }
        if self.depth() >= MAX_DEPTH && (sum.store_only || sum.relative) {
            for k in &sum.exits {
                let s = self.apply(&sum, &st);
                self.fall(site, Some(ret.wrapping_add(*k)), s);
            }
            return;
        }
        if self.depth() >= MAX_DEPTH {
            self.unresolved(
                site,
                UnresolvedKind::ContextDepth,
                format!("call to {target} runs too deep in callers to resolve"),
                false,
            );
            let mut s = st;
            s.kill_all();
            for k in &sum.exits {
                self.fall(site, Some(ret.wrapping_add(*k)), s);
            }
            return;
        }
        let mut cs = st;
        cs.switched = false;
        let [rh, rl] = ret.to_be_bytes();
        cs.push(Val::Const(rh), Val::Const(rl));
        let Some(e) = cs.sp else {
            self.unresolved(
                site,
                UnresolvedKind::IndirectCall,
                "stack pointer not known at the call".to_owned(),
                false,
            );
            return;
        };
        let o = self.inline(target, cs, ret, e);
        if o.exits.is_empty()
            && let Some(exit) = self.run_exit(target, &st, ret)
        {
            let mut known = (*o).clone();
            known.escapes.clear();
            known
                .unresolved
                .retain(|(u, _)| u.kind != UnresolvedKind::IndirectJump);
            self.absorb(site, target, &sum, &known, ret);
            let mut s = st;
            s.kill_all();
            self.fall(site, Some(exit), s);
            return;
        }
        if o.exits.is_empty() && o.waits {
            let mut known = (*o).clone();
            known.escapes.clear();
            self.absorb(site, target, &sum, &known, ret);
            let mut s = st;
            s.kill_all();
            self.fall(site, Some(ret), s);
            return;
        }
        self.absorb(site, target, &sum, &o, ret);
        if o.exits.is_empty()
            && o.escapes.is_empty()
            && !o.popped_ret
            && !(o.unresolved.is_empty() && o.ram.is_empty())
        {
            let mut s = st;
            s.kill_all();
            self.fall(site, Some(ret), s);
        }
    }

    /// Where a callee with no exit found returns to, when its single path
    /// from this call site is known to the end: a routine that reads data
    /// placed after the call and returns past it.
    fn run_exit(&self, target: Loc, st: &State, ret: u16) -> Option<u16> {
        {
            let mut b = self.env.budget.borrow_mut();
            if *b < EXIT_STEPS {
                return None;
            }
            *b -= EXIT_STEPS / 8;
        }
        let mut s = *st;
        s.kill_memory();
        let (_, exit) = crate::probe::run_to(
            &self.cx(),
            target,
            s,
            ret,
            EXIT_STEPS,
            Some(self.env.summaries),
        )?;
        (exit.wrapping_sub(ret) < 0x400 && exit >= ret).then_some(exit)
    }

    /// Evaluate a pure callee for every value of one unknown register.
    fn probe(&self, target: Loc, st: &State) -> Option<[Val; 7]> {
        let mut norm = State::symbolic();
        norm.kill_memory();
        norm.clear_stack();
        norm.lo = st.lo;
        norm.hi = st.hi;
        let cx = self.cx();
        for r in 0..7 {
            norm.set(
                r,
                if let Val::Const(c) = st.regs[r] {
                    Val::Const(c)
                } else {
                    Val::Top
                },
            );
            norm.ver[r] = 0;
        }
        for r in 0..7 {
            if matches!(st.regs[r], Val::Const(_)) || (0..r).any(|x| st.regs[x] == st.regs[r]) {
                continue;
            }
            let mut s = norm;
            for x in 0..7 {
                if st.regs[x] == st.regs[r] {
                    s.regs[x] = Val::Entry(r as u8);
                }
            }
            s.bound[r] = st.bound[r];
            let key = (target, s);
            let cached = self.env.probes.borrow().get(&key).copied();
            let res = match cached {
                Some(v) => v,
                None => {
                    {
                        let mut b = self.env.budget.borrow_mut();
                        if *b < 4096 {
                            return None;
                        }
                        *b -= 4096;
                    }
                    let v = crate::probe::fit(&cx, target, &s, r);
                    self.env.probes.borrow_mut().insert(key, v);
                    v
                }
            };
            if let Some(mut regs) = res {
                let id = (r as u16) << 12 | (st.ver[r] & 0x0FFF);
                for v in &mut regs {
                    if let Val::Hi(s) | Val::Lo(s) = v {
                        s.ix = id;
                    }
                }
                return Some(regs);
            }
        }
        None
    }

    fn inline(&mut self, target: Loc, cs: State, ret: u16, e: i16) -> Rc<Outcome> {
        let key = (target, cs);
        if let Some(o) = self.env.cache.borrow().get(&key) {
            return o.clone();
        }
        let mode = Mode::Inline {
            ret,
            e,
            depth: self.depth() + 1,
        };
        let mut r = Run::new(self.env, target, self.root, mode, false);
        r.explore(cs);
        let o = Rc::new(r.out);
        self.env.cache.borrow_mut().insert(key, o.clone());
        o
    }

    fn absorb(&mut self, site: Loc, target: Loc, sum: &Summary, o: &Outcome, ret: u16) {
        self.out.inlined.insert(target);
        let kind = if sum.bank_reg.is_some() {
            CallKind::Trampoline
        } else {
            CallKind::Indirect
        };
        for c in &o.calls {
            let k = if c.kind == CallKind::Indirect || c.site.addr == target.addr {
                kind
            } else {
                c.kind
            };
            self.out.calls.push(CallSite {
                site,
                target: c.target,
                kind: if c.kind == CallKind::Indirect {
                    k
                } else {
                    c.kind
                },
            });
        }
        self.out.deps.extend(o.deps.iter().copied());
        self.out.cell_deps.extend(o.cell_deps.iter().copied());
        self.out.inlined.extend(o.inlined.iter().copied());
        self.out.stores.extend(o.stores.iter().cloned());
        self.out.ram.extend(o.ram.iter().copied());
        self.out.dyn_writes.extend(o.dyn_writes.iter().copied());
        self.out.tables.extend(o.tables.iter().copied());
        self.out.cells.extend(o.cells.iter().copied());
        if o.touches {
            self.out.touches = true;
        }
        if o.entry_dep {
            self.out.entry_dep = true;
        }
        for (u, dep) in &o.unresolved {
            if *dep == Dep::Computed
                || (u.kind == UnresolvedKind::IndirectJump && sum.target_regs.is_none())
            {
                let detail = format!("{}, called from {site}", u.detail);
                self.out.unresolved.push((
                    Unresolved {
                        at: u.at,
                        kind: u.kind,
                        detail,
                    },
                    Dep::No,
                ));
                continue;
            }
            let kind = match u.kind {
                UnresolvedKind::IndirectJump => UnresolvedKind::IndirectCall,
                UnresolvedKind::UnknownBank if sum.bank_reg.is_some() => {
                    UnresolvedKind::TrampolineBank
                }
                k => k,
            };
            let detail = format!("in the call to {target}, at {}: {}", u.at, u.detail);
            self.out.unresolved.push((
                Unresolved {
                    at: site,
                    kind,
                    detail,
                },
                *dep,
            ));
        }
        for (k, s) in &o.exits {
            self.fall(site, Some(ret.wrapping_add(*k)), *s);
        }
        for (h, l, s) in &o.escapes {
            self.transfer(site, *h, *l, *s, false, 0);
        }
    }

    /// Whether `(h, l)` popped or jumped to at stack offset `sp` is the
    /// function's own return address plus `k`.
    fn own_return(&self, h: Val, l: Val, sp: Option<i16>) -> Option<u16> {
        if sp != Some(self.entry_sp() + 2) {
            return None;
        }
        match (self.mode, word_of(h, l)) {
            (Mode::Alone, Word::Sym(s)) if s.base == Base::Ret && s.step == 0 && s.off < 64 => {
                Some(s.off)
            }
            (Mode::Inline { ret, .. }, Word::Const(v)) if v.wrapping_sub(ret) < 64 => {
                Some(v.wrapping_sub(ret))
            }
            _ => None,
        }
    }

    /// Control goes to the address `(h, l)` with the stack in `st`: a
    /// return, a jump within the function, or a computed transfer.
    fn transfer(&mut self, at: Loc, h: Val, l: Val, st: State, single: bool, chain: u8) {
        if let Some(k) = self.own_return(h, l, st.sp) {
            self.out.exits.push((k, st));
            return;
        }
        if let (Mode::Inline { e, .. }, Some(sp)) = (self.mode, st.sp)
            && sp >= e + 2
        {
            self.out.escapes.push((h, l, st));
            return;
        }
        if chain > MAX_CHAIN {
            self.unresolved(
                at,
                UnresolvedKind::IndirectJump,
                "chain of computed returns too long".to_owned(),
                false,
            );
            return;
        }
        let w = word_of(h, l);
        if !matches!(w, Word::Const(_) | Word::Load(_))
            && let Some(t) = self.enumerated(at)
        {
            self.out.enumerated = true;
            for addr in t {
                match self.flow(&st, addr) {
                    Ok(loc) if !is_ram(addr) => {
                        self.branches.push((at, loc));
                        self.enter(loc, st);
                    }
                    _ => self.unresolved(
                        at,
                        UnresolvedKind::IndirectJump,
                        format!("computed target ${addr:04X}"),
                        false,
                    ),
                }
            }
            return;
        }
        let from_ram = matches!(w, Word::Load(_));
        let targets = match self.targets(at, w, h, l, &st) {
            Some(t) => t,
            None => return,
        };
        for (addr, bank) in targets {
            if is_ram(addr) {
                self.out.ram.push(RamTransfer {
                    site: at,
                    target: addr,
                });
                continue;
            }
            let loc = match bank {
                Some(b) if addr >= 0x4000 => Loc { bank: b, addr },
                _ => match self.flow(&st, addr) {
                    Ok(l) => l,
                    Err((k, dep)) => {
                        self.unresolved(at, k, format!("computed target ${addr:04X}"), dep);
                        continue;
                    }
                },
            };
            if single {
                self.branches.push((at, loc));
                self.enter(loc, st);
                continue;
            }
            self.out.calls.push(CallSite {
                site: at,
                target: loc,
                kind: CallKind::Indirect,
            });
            let Some(sum) = self.summary(loc) else {
                continue;
            };
            if sum.touches {
                self.out.touches = true;
            }
            if sum.ctx {
                if from_ram {
                    continue;
                }
                self.out.inlined.insert(loc);
                let mut s = st;
                pin(self.env.model, loc, &mut s);
                self.enter(loc, s);
                continue;
            }
            for k in &sum.exits {
                let mut s = self.apply(&sum, &st);
                let Some((h2, l2)) = s.pop() else {
                    continue;
                };
                let (h2, l2) = state::split(state::add_word(word_of(h2, l2), Word::Const(*k)));
                self.transfer(at, h2, l2, s, false, chain + 1);
            }
        }
    }

    /// The constant targets of a computed transfer, with a bank when the
    /// pointer's store names one; `None` when it is listed as unresolved.
    fn targets(
        &mut self,
        at: Loc,
        w: Word,
        h: Val,
        l: Val,
        st: &State,
    ) -> Option<Vec<(u16, Option<u16>)>> {
        match w {
            Word::Const(v) => Some(vec![(v, None)]),
            Word::Load(s) if s.base == Base::Abs && s.step == 0 && is_ram(s.off) => {
                self.cell_targets(at, s.off, st)
            }
            Word::Load(s) if matches!(s.base, Base::Rom(b) if b != state::ENTRY_BANK) => {
                Some(self.table_targets(at, s, st))
            }
            Word::EntryPair(h, l)
            | Word::Sym(Sym {
                base: Base::Pair(h, l),
                off: 0,
                step: 0,
                ..
            }) => {
                self.out
                    .target_regs
                    .get_or_insert((usize::from(h), usize::from(l)));
                self.unresolved(
                    at,
                    UnresolvedKind::IndirectJump,
                    "jump to an address the caller passes".to_owned(),
                    true,
                );
                None
            }
            w if w.depends_on_entry() => {
                self.unresolved(
                    at,
                    UnresolvedKind::IndirectJump,
                    "jump to an address that depends on the caller".to_owned(),
                    true,
                );
                None
            }
            _ => {
                let _ = st;
                let dep = h.depends_on_entry() || l.depends_on_entry();
                let (text, dep) = if dep {
                    (
                        "jump to an address computed from the caller's values",
                        Dep::Computed,
                    )
                } else {
                    ("jump to an address that is not known", Dep::No)
                };
                self.unresolved_dep(at, UnresolvedKind::IndirectJump, text.to_owned(), dep);
                None
            }
        }
    }

    /// The targets of the computed transfer at `at` when running the
    /// function for every value of one entry register, with every other
    /// register unknown, reaches it with a constant address on every run.
    fn enumerated(&self, at: Loc) -> Option<BTreeSet<u16>> {
        let froms: Vec<Loc> = std::iter::once(self.func)
            .chain(self.tails.iter().copied())
            .collect();
        for f in froms {
            let mut st = State::symbolic();
            pin(self.env.model, f, &mut st);
            for r in 0..7 {
                let key = (f, r);
                let cached = self.env.transfers.borrow().get(&key).cloned();
                let t = match cached {
                    Some(t) => t,
                    None => {
                        let mut b = self.env.budget.borrow_mut();
                        if *b < ENUM_STEPS {
                            return None;
                        }
                        let t = crate::probe::transfers(
                            &self.cx(),
                            f,
                            &st,
                            r,
                            self.env.summaries,
                            &mut b,
                        )
                        .map(Rc::new);
                        drop(b);
                        self.env.transfers.borrow_mut().insert(key, t.clone());
                        t
                    }
                };
                if let Some(set) = t.as_ref().and_then(|t| t.get(&at)) {
                    return Some(set.clone());
                }
            }
        }
        None
    }

    fn cell_targets(&mut self, at: Loc, cell: u16, st: &State) -> Option<Vec<(u16, Option<u16>)>> {
        self.out.cell_deps.insert(cell);
        let info = self.env.cells.get(&cell).cloned().unwrap_or_default();
        let loose = info.loose
            || self
                .env
                .cells
                .get(&cell.wrapping_add(1))
                .is_some_and(|c| c.loose);
        let wb = st.window_bank(self.env.model);
        let mut out = Vec::new();
        let mut bank_missing = Vec::new();
        for e in &info.elems {
            match e {
                Elem::Const { value, with, .. } => {
                    if *value == 0 && st.nonzero == Some(cell) {
                        continue;
                    }
                    let clash = with
                        .iter()
                        .any(|(a, v)| matches!(st.cell(*a), Some(Val::Const(k)) if k != *v));
                    if clash {
                        continue;
                    }
                    if *value < 0x4000 || is_ram(*value) {
                        out.push((*value, None));
                        continue;
                    }
                    match wb {
                        WindowBank::Known(_) => out.push((*value, None)),
                        WindowBank::Cell(b) => match with.iter().find(|(a, _)| *a == b) {
                            Some((_, v)) => {
                                if let Some(bank) = self.bank_of_byte(*v) {
                                    out.push((*value, Some(bank)));
                                }
                            }
                            None => bank_missing.push(*value),
                        },
                        _ => bank_missing.push(*value),
                    }
                }
                Elem::Ret { func, k } => {
                    if *func == self.func && st.sp == Some(self.entry_sp() + 2) {
                        self.out.exits.push((*k, *st));
                    } else {
                        self.unresolved(
                            at,
                            UnresolvedKind::RamPointer,
                            format!("${cell:04X} holds a return address of {func}"),
                            false,
                        );
                    }
                }
                Elem::Table { sym, with } => {
                    let col = with.iter().map(|(a, s)| (*a, *s)).collect::<Vec<_>>();
                    out.extend(self.table_rows(at, *sym, &col, st));
                }
            }
        }
        if !bank_missing.is_empty() {
            bank_missing.sort_unstable();
            bank_missing.dedup();
            let list: Vec<String> = bank_missing.iter().map(|v| format!("${v:04X}")).collect();
            let dep = matches!(wb, WindowBank::Entry | WindowBank::Param);
            self.unresolved(
                at,
                UnresolvedKind::CellBank,
                format!(
                    "pointer in ${cell:04X} to the window, bank not known: {}",
                    list.join(" ")
                ),
                dep,
            );
        }
        if info.elems.is_empty() {
            self.unresolved(
                at,
                UnresolvedKind::RamPointer,
                format!("no constant pointer is stored at ${cell:04X}"),
                false,
            );
        } else if loose {
            self.unresolved(
                at,
                UnresolvedKind::RamPointer,
                format!("${cell:04X} also gets bytes that are not a known pointer"),
                false,
            );
        }
        self.out.cells.push(CellUse {
            site: at,
            cell,
            targets: out.len(),
            partial: loose || !bank_missing.is_empty(),
        });
        let mut bad = Vec::new();
        out.retain(|(v, b)| {
            let keep = is_ram(*v) || self.plausible(*v, *b, st);
            if !keep {
                bad.push(*v);
            }
            keep
        });
        if !bad.is_empty() {
            bad.sort_unstable();
            bad.dedup();
            let list: Vec<String> = bad.iter().map(|v| format!("${v:04X}")).collect();
            self.unresolved(
                at,
                UnresolvedKind::RamPointer,
                format!(
                    "${cell:04X} also holds values that are not code: {}",
                    list.join(" ")
                ),
                false,
            );
        }
        Some(out)
    }

    fn bank_of_byte(&self, v: u8) -> Option<u16> {
        let lo = self.env.model.register_value(BankReg::Lo, v);
        self.env.model.effective(lo, 0).ok()
    }

    fn rom_word(&self, bank: u16, addr: u16) -> Option<u16> {
        let lo = self.rom_byte(bank, addr)?;
        let hi = self.rom_byte(bank, addr.wrapping_add(1))?;
        Some(u16::from_le_bytes([lo, hi]))
    }

    fn rom_byte(&self, bank: u16, addr: u16) -> Option<u8> {
        let loc = if addr < 0x4000 {
            Loc::fixed(addr)
        } else {
            Loc { bank, addr }
        };
        self.env.image.fetch(loc)?.first().copied()
    }

    /// The entries of the table of words at `s`.
    fn table_targets(&mut self, at: Loc, s: Sym, st: &State) -> Vec<(u16, Option<u16>)> {
        self.table_rows(at, s, &[], st)
    }

    /// The entries of the table of words at `s`: all `count` entries when
    /// the index is bounded, otherwise the run of entries that point to
    /// code, ending at the first that does not. `cols` are cells holding
    /// another byte of the same row: one that is the bank register's value
    /// gives each entry its bank, and one whose value the path knows keeps
    /// only the rows that agree.
    fn table_rows(
        &mut self,
        at: Loc,
        s: Sym,
        cols: &[(u16, Sym)],
        st: &State,
    ) -> Vec<(u16, Option<u16>)> {
        let Base::Rom(bank) = s.base else {
            return Vec::new();
        };
        let tloc = if s.off < 0x4000 {
            Loc::fixed(s.off)
        } else {
            Loc { bank, addr: s.off }
        };
        let step = u16::from(s.step.max(1));
        let (limit, bound) = match s.count {
            Some(n) => (usize::from(n).min(256), TableBound::Index),
            None => (MAX_TABLE, TableBound::Run),
        };
        let limit = match (bound, self.env.limits.get(&tloc)) {
            (TableBound::Run, Some(n)) => limit.min(*n),
            _ => limit,
        };
        let mut rows = 0;
        let wb = st.window_bank(self.env.model);
        let bank_col = match wb {
            WindowBank::Cell(b) => cols.iter().find(|(a, _)| *a == b).map(|(_, s)| *s),
            _ => None,
        };
        let window_end: u32 = if s.off < 0x4000 { 0x4000 } else { 0x8000 };
        let mut out = Vec::new();
        for i in 0..limit {
            let a = u32::from(s.off) + u32::from(step) * i as u32;
            if a + 1 >= window_end {
                break;
            }
            let a = a as u16;
            if bound == TableBound::Run && i > 0 && self.overlaps_code(bank, a) {
                break;
            }
            let Some(v) = self.rom_word(bank, a) else {
                break;
            };
            rows = i + 1;
            let row = |c: &Sym| {
                let Base::Rom(cb) = c.base else { return None };
                let off = c.off.wrapping_add(step.wrapping_mul(i as u16));
                self.rom_byte(cb, off)
            };
            let agrees = cols.iter().all(|(cell, c)| match st.cell(*cell) {
                Some(Val::Const(k)) => row(c) == Some(k),
                _ => true,
            });
            if !agrees {
                continue;
            }
            if v == 0 && !cols.is_empty() {
                continue;
            }
            let row_bank = match bank_col {
                Some(c) => match row(&c).and_then(|b| self.bank_of_byte(b)) {
                    Some(b) => Some(b),
                    None if bound == TableBound::Run => break,
                    None => continue,
                },
                None => None,
            };
            if !self.plausible(v, row_bank, st) {
                if bound == TableBound::Run && !self.inside_code(v, row_bank, st) {
                    break;
                }
                continue;
            }
            out.push((v, row_bank));
        }
        if out.is_empty() {
            self.unresolved(
                at,
                if s.off == self.inline_ret() || Some(s.off) == self.after(at) {
                    UnresolvedKind::InlineTable
                } else {
                    UnresolvedKind::JumpTable
                },
                format!("jump table at ${:04X} has no entry that is code", s.off),
                false,
            );
        } else {
            self.out.tables.push(TableUse {
                site: at,
                table: tloc,
                step: step as u8,
                entries: out.len(),
                rows,
                bound,
            });
        }
        out
    }

    fn after(&self, at: Loc) -> Option<u16> {
        let ins = decode(self.env.image.fetch(at)?).ok()?;
        matches!(ins.opcode.mnemonic, Mnemonic::Call | Mnemonic::Rst)
            .then(|| at.addr.wrapping_add(u16::from(ins.length())))
    }

    fn inline_ret(&self) -> u16 {
        match self.mode {
            Mode::Inline { ret, .. } => ret,
            Mode::Alone => u16::MAX,
        }
    }

    fn overlaps_code(&self, bank: u16, a: u16) -> bool {
        let l = |addr: u16| {
            if addr < 0x4000 {
                Loc::fixed(addr)
            } else {
                Loc { bank, addr }
            }
        };
        self.env.known.contains(&l(a)) || self.env.known.contains(&l(a.wrapping_add(1)))
    }

    /// Whether a table entry `v` points to code: an address in ROM outside
    /// the header whose code reaches a return or a known exit without an
    /// unused opcode.
    fn plausible(&self, v: u16, bank: Option<u16>, st: &State) -> bool {
        if v < 0x0008 || (0x0100..0x0150).contains(&v) || is_ram(v) {
            return false;
        }
        let loc = match bank {
            Some(b) if v >= 0x4000 => Loc { bank: b, addr: v },
            _ => match self.flow(st, v) {
                Ok(l) => l,
                Err(_) => return false,
            },
        };
        if self.inside_known(loc) {
            return false;
        }
        verify(self.env.image, self.env.known, loc)
    }

    /// Whether the entry `v` points inside an instruction already found: an
    /// address in code, so a table of code addresses goes on past it, but
    /// not one that is followed.
    fn inside_code(&self, v: u16, bank: Option<u16>, st: &State) -> bool {
        let loc = match bank {
            Some(b) if v >= 0x4000 => Loc { bank: b, addr: v },
            _ => match self.flow(st, v) {
                Ok(l) => l,
                Err(_) => return false,
            },
        };
        self.inside_known(loc)
    }

    /// Whether `loc` is a byte inside an instruction already found, other
    /// than its first.
    fn inside_known(&self, loc: Loc) -> bool {
        (1..=2u16).any(|k| {
            let Some(addr) = loc.addr.checked_sub(k) else {
                return false;
            };
            let start = Loc { addr, ..loc };
            self.env.known.contains(&start)
                && self
                    .env
                    .image
                    .fetch(start)
                    .and_then(|b| decode(b).ok())
                    .is_some_and(|i| u16::from(i.length()) > k)
        })
    }

    fn write_event(&mut self, at: Loc, st: &State, w: Write) {
        self.out.impure = true;
        match w {
            Write::Bank(b) => {
                self.out.touches = true;
                if !matches!(b.val, Val::Const(_)) {
                    self.out.dyn_writes.push(at);
                }
                if let (BankReg::Lo, Val::Entry(r)) = (b.reg, b.val) {
                    self.out.bank_reg.get_or_insert(usize::from(r));
                }
            }
            Write::Ram { addr, val } => self.ram_store(at, st, addr, val),
            Write::Param => self.out.store_dep = true,
            Write::Unknown => {}
        }
    }

    fn ram_store(&mut self, at: Loc, st: &State, addr: u16, val: Val) {
        let lo_cell = addr.wrapping_sub(1);
        let mut word = None;
        if let Some(l) = st.cell(lo_cell) {
            word = Some((lo_cell, word_of(val, l)));
        }
        if let Some(h) = st.cell(addr.wrapping_add(1))
            && !matches!(word, Some((_, w)) if w != Word::Other)
        {
            word = Some((addr, word_of(h, val)));
        }
        let elem = word.and_then(|(cell, w)| self.elem(st, cell, w).map(|e| (cell, e)));
        match elem {
            Some((cell, e)) => {
                if let Elem::Const { value, .. } = e
                    && (0x150..0x8000).contains(&value)
                {
                    let target = if value < 0x4000 {
                        Some(Loc::fixed(value))
                    } else {
                        match st.window_bank(self.env.model) {
                            WindowBank::Known(bank) => Some(Loc { bank, addr: value }),
                            _ => None,
                        }
                    };
                    self.out.taken.push(AddressTaken {
                        site: at,
                        value,
                        target,
                        stored_at: Some(cell),
                    });
                }
                let partner = if cell == addr {
                    (addr.wrapping_add(1), st.cell(addr.wrapping_add(1)))
                } else {
                    (cell, st.cell(cell))
                };
                let first = Store::Byte {
                    addr: partner.0,
                    value: partner.1.and_then(|v| v.konst()),
                };
                if let Some(i) = self.out.stores.iter().rposition(|s| *s == first) {
                    self.out.stores.remove(i);
                }
                self.out.stores.push(Store::Word {
                    addr: cell,
                    elem: e,
                    site: at,
                });
            }
            None => {
                if word.is_some_and(|(_, w)| w.depends_on_entry()) || val.depends_on_entry() {
                    if word.is_some_and(|(_, w)| w.depends_on_entry()) {
                        self.out.entry_dep = true;
                    }
                    return;
                }
                self.out.stores.push(Store::Byte {
                    addr,
                    value: val.konst(),
                });
            }
        }
    }

    fn elem(&self, st: &State, cell: u16, w: Word) -> Option<Elem> {
        match w {
            Word::Const(value) => {
                let bank = match st.window_bank(self.env.model) {
                    WindowBank::Known(b) => Some(b),
                    _ => None,
                };
                let with = st
                    .const_cells()
                    .into_iter()
                    .filter(|(a, _)| *a != cell && *a != cell.wrapping_add(1))
                    .collect();
                Some(Elem::Const { value, bank, with })
            }
            Word::Sym(s)
                if s.base == Base::Ret && s.step == 0 && matches!(self.mode, Mode::Alone) =>
            {
                Some(Elem::Ret {
                    func: self.root,
                    k: s.off,
                })
            }
            Word::Load(s)
                if matches!(s.base, Base::Rom(b) if b != state::ENTRY_BANK) && s.step > 0 =>
            {
                let with = st
                    .cells
                    .get()
                    .iter()
                    .filter(|(a, _)| *a != cell && *a != cell.wrapping_add(1))
                    .filter_map(|(a, v)| match v {
                        Val::At(t) if t.base == s.base && t.step == s.step && t.ix == s.ix => {
                            Some((*a, *t))
                        }
                        _ => None,
                    })
                    .collect();
                Some(Elem::Table { sym: s, with })
            }
            _ => None,
        }
    }

    fn record_consts(&mut self, loc: Loc, st: &State, ins: &Instruction) {
        match (
            ins.opcode.mnemonic,
            ins.opcode.dst,
            ins.opcode.src,
            ins.immediate,
        ) {
            (Mnemonic::Ld, Operand::R16(p), Operand::Imm16, Immediate::U16(v)) => {
                self.consts16.push((v, p));
                if (0x150..0x8000).contains(&v) && p != Reg16::Sp {
                    self.out.taken.push(AddressTaken {
                        site: loc,
                        value: v,
                        target: self.flow(st, v).ok(),
                        stored_at: None,
                    });
                }
            }
            (
                Mnemonic::Ld,
                Operand::R8(relift_cpu_sm83::Reg8::C),
                Operand::Imm8,
                Immediate::U8(v),
            ) => {
                self.consts_c.push(v);
            }
            (
                Mnemonic::Ld,
                Operand::R8(relift_cpu_sm83::Reg8::B),
                Operand::Imm8,
                Immediate::U8(v),
            ) => {
                self.consts_b.push(v);
            }
            _ => {}
        }
    }

    fn find_copies(&mut self) {
        self.branches.sort_unstable();
        self.branches.dedup();
        let body = &self.out.body;
        for &(from, to) in &self.branches {
            if to.bank != from.bank && from.is_window() || to.addr > from.addr {
                continue;
            }
            let mut reads = false;
            let mut writes = false;
            for (l, _) in body.range(to..=from) {
                let Some(bytes) = self.env.image.fetch(*l) else {
                    continue;
                };
                let Ok(ins) = decode(bytes) else { continue };
                if ins.opcode.mnemonic != Mnemonic::Ld {
                    continue;
                }
                let mem = |o: Operand| {
                    matches!(
                        o,
                        Operand::Indirect(_) | Operand::HlInc | Operand::HlDec | Operand::IndirectC
                    )
                };
                if mem(ins.opcode.src) {
                    reads = true;
                }
                if mem(ins.opcode.dst) {
                    writes = true;
                }
            }
            if reads && writes {
                let mut sources: Vec<u16> = self
                    .consts16
                    .iter()
                    .filter(|(v, p)| *v >= 0x150 && *v < 0x8000 && *p != Reg16::Sp)
                    .map(|(v, _)| *v)
                    .collect();
                let mut dests: Vec<u16> = self
                    .consts16
                    .iter()
                    .filter(|(v, p)| *v >= 0x8000 && *p != Reg16::Sp)
                    .map(|(v, _)| *v)
                    .collect();
                dests.extend(self.consts_c.iter().map(|c| 0xFF00 | u16::from(*c)));
                sources.sort_unstable();
                sources.dedup();
                dests.sort_unstable();
                dests.dedup();
                let mut counts = self.consts_b.clone();
                counts.sort_unstable();
                counts.dedup();
                self.out.copies.push(CopyLoop {
                    at: from,
                    sources,
                    dests,
                    counts,
                });
            }
        }
    }
}

/// Whether the code at `loc` decodes to a path that reaches a return, a
/// jump through a register or known code, with no unused opcode and
/// without running off its window, within a small budget.
pub(crate) fn verify(image: &Image<'_>, known: &FxSet<Loc>, loc: Loc) -> bool {
    let mut seen = FxSet::default();
    let mut work = vec![loc];
    let mut exit = false;
    while let Some(l) = work.pop() {
        if !seen.insert(l) {
            continue;
        }
        if seen.len() > 96 {
            break;
        }
        if l != loc && known.contains(&l) {
            exit = true;
            continue;
        }
        let Some(bytes) = image.fetch(l) else {
            return false;
        };
        let Ok(ins) = decode(bytes) else {
            return false;
        };
        if ins.opcode.is_illegal() {
            return false;
        }
        let next = l.addr.checked_add(u16::from(ins.length()));
        let in_window = |a: u16| (a >= 0x4000) == l.is_window() && a < 0x8000;
        let cond = matches!(ins.opcode.dst, Operand::Cond(_));
        match ins.opcode.mnemonic {
            Mnemonic::Ret | Mnemonic::Reti => {
                exit = true;
                if !cond {
                    continue;
                }
            }
            Mnemonic::Jp if ins.opcode.src == Operand::R16(Reg16::Hl) => {
                exit = true;
                continue;
            }
            Mnemonic::Jp | Mnemonic::Jr => {
                if let Some(t) = ins.branch_target(l.addr) {
                    if t < 0x4000 && !l.is_window() || in_window(t) {
                        work.push(Loc { addr: t, ..l });
                    } else {
                        exit = true;
                    }
                }
                if !cond {
                    continue;
                }
            }
            Mnemonic::Call | Mnemonic::Rst => {
                if let Some(t) = ins.branch_target(l.addr)
                    && t == l.addr
                {
                    return false;
                }
            }
            _ => {}
        }
        match next {
            Some(n) if in_window(n) => work.push(Loc { addr: n, ..l }),
            _ => return false,
        }
    }
    exit
}

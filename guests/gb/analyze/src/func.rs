//! Analysis of one function: the code reachable from an entry without
//! following calls, and what it does to the bank registers.

use std::collections::{BTreeMap, HashMap};

use relift_cpu_sm83::{Immediate, Instruction, Mnemonic, Operand, Reg16, decode};

use crate::bankmodel::{BankModel, BankReg};
use crate::image::Image;
use crate::loc::Loc;
use crate::report::{Unresolved, UnresolvedKind};
use crate::state::{self, BankWrite, H, L, State, Val, Write};

/// How a callee acts on the bank registers and where it goes, found by
/// running it with symbolic registers.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Summary {
    /// The entry register whose value the callee writes to the low bank
    /// register.
    pub bank: Option<usize>,
    /// The entry registers (high, low) of the address the callee jumps to
    /// with `jp hl`.
    pub target: Option<(usize, usize)>,
    /// The callee pops its return address, reads a code address from a
    /// table that follows the call and jumps to it.
    pub inline_table: bool,
}

impl Summary {
    pub(crate) fn is_empty(&self) -> bool {
        self.bank.is_none() && self.target.is_none() && !self.inline_table
    }
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
    /// A call or jump through a `jp hl` helper.
    Indirect,
}

/// A transfer to another function.
#[derive(Clone, Copy, Debug)]
pub(crate) struct CallSite {
    pub site: Loc,
    pub target: Loc,
    pub kind: CallKind,
    pub state: State,
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
    /// Where the constant points, when the bank is known. A pointer stored
    /// to RAM gets the bank of the code that stores it when that code is in
    /// the switchable window, which is a guess about where it is used.
    pub target: Option<Loc>,
    /// The RAM address of the low byte when two constant bytes were stored
    /// there, which makes the constant a stored pointer.
    pub stored_at: Option<u16>,
}

pub(crate) struct FnRun {
    pub taken: Vec<AddressTaken>,
    pub body: BTreeMap<Loc, u8>,
    pub calls: Vec<CallSite>,
    pub unresolved: Vec<Unresolved>,
    pub ram: Vec<RamTransfer>,
    pub copies: Vec<CopyLoop>,
    pub summary: Summary,
    pub touches: bool,
    pub parametric: bool,
}

pub(crate) struct Env<'a> {
    pub model: &'a BankModel,
    pub image: &'a Image<'a>,
    pub summaries: &'a HashMap<Loc, Summary>,
    pub touching: &'a std::collections::HashSet<Loc>,
}

struct Run<'a> {
    env: &'a Env<'a>,
    entry: Loc,
    own_bank: Option<u16>,
    out: FnRun,
    consts16: Vec<(u16, Reg16)>,
    consts_c: Vec<u8>,
    consts_b: Vec<u8>,
    branches: Vec<(Loc, Loc)>,
    no_fall: bool,
    cur_len: u8,
    stores: Vec<(Loc, u16, u8)>,
}

fn is_ram(addr: u16) -> bool {
    addr >= 0x8000
}

fn flow(env: &Env<'_>, st: &State, addr: u16) -> Result<Loc, UnresolvedKind> {
    if addr < 0x4000 {
        return Ok(Loc::fixed(addr));
    }
    match st.window_bank(env.model) {
        Some(Ok(bank)) => Ok(Loc { bank, addr }),
        Some(Err(_)) => Err(UnresolvedKind::BankOutOfRange),
        None => Err(UnresolvedKind::UnknownBank),
    }
}

pub(crate) fn run(env: &Env<'_>, entry: Loc, entry_state: State) -> FnRun {
    let own_bank = entry.is_window().then_some(entry.bank);
    let mut r = Run {
        env,
        entry,
        own_bank,
        out: FnRun {
            taken: Vec::new(),
            body: BTreeMap::new(),
            calls: Vec::new(),
            unresolved: Vec::new(),
            ram: Vec::new(),
            copies: Vec::new(),
            summary: Summary::default(),
            touches: false,
            parametric: false,
        },
        consts16: Vec::new(),
        consts_c: Vec::new(),
        consts_b: Vec::new(),
        branches: Vec::new(),
        no_fall: false,
        cur_len: 0,
        stores: Vec::new(),
    };
    let mut st0 = entry_state;
    r.pin_window(&mut st0);
    r.explore(st0);
    r.find_copies();
    r.out
}

impl Run<'_> {
    fn pin_window(&self, st: &mut State) {
        if let Some(n) = self.own_bank {
            let (lo, hi) = self.env.model.split(n);
            if self.env.model.effective(lo, hi).is_ok_and(|b| b == n) {
                st.lo = Some(lo);
                st.hi = Some(hi);
            } else {
                st.lo = None;
                st.hi = None;
            }
        }
    }

    fn unresolved(&mut self, at: Loc, kind: UnresolvedKind, detail: String) {
        self.out.unresolved.push(Unresolved { at, kind, detail });
    }

    fn explore(&mut self, st0: State) {
        let mut states: HashMap<Loc, State> = HashMap::new();
        let mut work = vec![self.entry];
        states.insert(self.entry, st0);
        while let Some(loc) = work.pop() {
            let Some(mut st) = states.get(&loc).copied() else {
                continue;
            };
            let Some(bytes) = self.env.image.fetch(loc) else {
                self.unresolved(loc, UnresolvedKind::OutsideRom, String::new());
                continue;
            };
            let Ok(ins) = decode(bytes) else {
                self.unresolved(loc, UnresolvedKind::RunsOffWindow, String::new());
                continue;
            };
            if ins.opcode.is_illegal() {
                self.unresolved(loc, UnresolvedKind::Illegal, format!("${:02X}", bytes[0]));
                continue;
            }
            let len = ins.length();
            self.out.body.insert(loc, len);
            self.cur_len = len;
            let next_addr = loc.addr.wrapping_add(u16::from(len));
            let mut succ: Vec<(Loc, State)> = Vec::new();
            let mut fall = true;
            self.record_consts(loc, &st, &ins);
            let mut events = Vec::new();
            match ins.opcode.mnemonic {
                Mnemonic::Jr | Mnemonic::Jp | Mnemonic::Call | Mnemonic::Rst => {
                    let conditional = matches!(ins.opcode.dst, Operand::Cond(_));
                    fall = conditional
                        || matches!(ins.opcode.mnemonic, Mnemonic::Call | Mnemonic::Rst);
                    self.branch(loc, &ins, &mut st, &mut succ);
                    if std::mem::take(&mut self.no_fall) {
                        fall = false;
                    }
                }
                Mnemonic::Ret | Mnemonic::Reti => {
                    fall = matches!(ins.opcode.dst, Operand::Cond(_));
                }
                _ => {
                    state::step(self.env.model, &mut st, &ins, &mut events);
                    for w in events {
                        match w {
                            Write::Bank(b) => self.bank_write(loc, b),
                            Write::Ram { addr, val } => self.ram_store(loc, &st, addr, val),
                        }
                    }
                }
            }
            if matches!(ins.opcode.mnemonic, Mnemonic::Call | Mnemonic::Rst) {
                st = self.after_call(st);
            }
            if fall {
                if next_addr < loc.addr {
                    self.unresolved(loc, UnresolvedKind::RunsOffWindow, String::new());
                } else {
                    match flow(self.env, &st, next_addr) {
                        Ok(n) if !is_ram(next_addr) => succ.push((n, st)),
                        Ok(_) => self.out.ram.push(RamTransfer {
                            site: loc,
                            target: next_addr,
                        }),
                        Err(k) if next_addr >= 0x8000 => {
                            let _ = k;
                            self.unresolved(loc, UnresolvedKind::RunsOffWindow, String::new());
                        }
                        Err(k) => self.unresolved(loc, k, format!("falls into ${next_addr:04X}")),
                    }
                }
            }
            for (n, s) in succ {
                match states.get_mut(&n) {
                    Some(old) => {
                        if old.merge(&s) {
                            work.push(n);
                        }
                    }
                    None => {
                        states.insert(n, s);
                        work.push(n);
                    }
                }
            }
        }
    }

    fn after_call(&self, mut st: State) -> State {
        st.kill_all();
        if self.own_bank.is_some() {
            self.pin_window(&mut st);
        }
        st
    }

    fn ram_store(&mut self, at: Loc, st: &State, addr: u16, val: Val) {
        let Val::Const(v) = val else { return };
        let near = |s: &(Loc, u16, u8)| {
            s.0.bank == at.bank && s.0.addr < at.addr && at.addr - s.0.addr <= 16
        };
        let lo = self
            .stores
            .iter()
            .rev()
            .find(|s| near(s) && s.1.wrapping_add(1) == addr)
            .copied();
        self.stores.push((at, addr, v));
        if let Some((_, _, l)) = lo {
            let value = u16::from_le_bytes([l, v]);
            if (0x150..0x8000).contains(&value) {
                let target = if value < 0x4000 {
                    Some(Loc::fixed(value))
                } else {
                    self.own_bank
                        .or_else(|| st.window_bank(self.env.model).and_then(Result::ok))
                        .map(|bank| Loc { bank, addr: value })
                };
                self.out.taken.push(AddressTaken {
                    site: at,
                    value,
                    target,
                    stored_at: Some(addr.wrapping_sub(1)),
                });
            }
        }
    }

    fn bank_write(&mut self, at: Loc, w: BankWrite) {
        self.out.touches = true;
        self.out.parametric = true;
        match (w.reg, w.val) {
            (BankReg::Lo, Val::Entry(r)) => {
                self.out.summary.bank.get_or_insert(r);
                self.out.parametric = true;
            }
            (_, Val::Top) => {
                self.out.parametric = true;
                self.unresolved(
                    at,
                    UnresolvedKind::DynamicBankWrite,
                    "value not constant".to_owned(),
                );
            }
            _ => {}
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
                        target: flow(self.env, st, v).ok(),
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

    fn target_of(&mut self, loc: Loc, ins: &Instruction, st: &State) -> Option<Loc> {
        let mn = ins.opcode.mnemonic;
        if mn == Mnemonic::Jp && ins.opcode.src == Operand::R16(Reg16::Hl) {
            return None;
        }
        let addr = ins.branch_target(loc.addr)?;
        self.resolve(loc, st, addr)
    }

    fn resolve(&mut self, site: Loc, st: &State, addr: u16) -> Option<Loc> {
        if is_ram(addr) {
            self.out.ram.push(RamTransfer { site, target: addr });
            return None;
        }
        match flow(self.env, st, addr) {
            Ok(l) => Some(l),
            Err(k) => {
                self.unresolved(site, k, format!("target ${addr:04X}"));
                None
            }
        }
    }

    fn branch(
        &mut self,
        loc: Loc,
        ins: &Instruction,
        st: &mut State,
        succ: &mut Vec<(Loc, State)>,
    ) {
        let mn = ins.opcode.mnemonic;
        let is_call = matches!(mn, Mnemonic::Call | Mnemonic::Rst);
        if mn == Mnemonic::Jp && ins.opcode.src == Operand::R16(Reg16::Hl) {
            self.jump_hl(loc, st, succ);
            return;
        }
        let Some(target) = self.target_of(loc, ins, st) else {
            return;
        };
        if !is_call {
            self.branches.push((loc, target));
            succ.push((target, *st));
            return;
        }
        let kind = if mn == Mnemonic::Rst {
            CallKind::Rst
        } else {
            CallKind::Call
        };
        self.call(loc, target, kind, st);
    }

    fn jump_hl(&mut self, loc: Loc, st: &State, succ: &mut Vec<(Loc, State)>) {
        self.out.parametric = true;
        match (st.pair(Reg16::Hl), st.regs[H], st.regs[L]) {
            (Some(a), _, _) => {
                if let Some(t) = self.resolve(loc, st, a) {
                    self.branches.push((loc, t));
                    succ.push((t, *st));
                }
            }
            (None, Val::Entry(h), Val::Entry(l)) => {
                self.out.summary.target.get_or_insert((h, l));
            }
            (None, Val::Popped, Val::Popped) => self.unresolved(
                loc,
                UnresolvedKind::IndirectJump,
                "jp hl to an address the caller pushed".to_owned(),
            ),
            (None, _, _) if st.popped => self.out.summary.inline_table = true,
            _ => self.unresolved(
                loc,
                UnresolvedKind::IndirectJump,
                "jp hl, hl not constant".to_owned(),
            ),
        }
    }

    fn call(&mut self, site: Loc, target: Loc, kind: CallKind, st: &mut State) {
        let sum = self.env.summaries.get(&target).copied().unwrap_or_default();
        if self.env.touching.contains(&target) {
            self.out.touches = true;
        }
        if sum.is_empty() {
            self.out.calls.push(CallSite {
                site,
                target,
                kind,
                state: *st,
            });
            if !self.env.touching.contains(&target) {
                return;
            }
            if self.own_bank.is_none() {
                st.lo = None;
                st.hi = None;
            }
            return;
        }
        if sum.inline_table {
            self.unresolved(
                site,
                UnresolvedKind::InlineTable,
                format!(
                    "jump table at ${:04X}, reached through {target}",
                    site.addr.wrapping_add(u16::from(self.cur_len))
                ),
            );
            self.out.calls.push(CallSite {
                site,
                target,
                kind,
                state: *st,
            });
            self.no_fall = true;
            return;
        }
        self.out.touches = true;
        let mut sub = *st;
        if let Some(r) = sum.bank {
            match st.regs[r] {
                Val::Const(v) => {
                    sub.lo = Some(self.env.model.register_value(BankReg::Lo, v));
                }
                Val::Entry(k) => {
                    self.out.summary.bank.get_or_insert(k);
                    self.out.parametric = true;
                    sub.lo = None;
                }
                Val::Top | Val::Popped => {
                    self.out.parametric = true;
                    self.unresolved(
                        site,
                        UnresolvedKind::TrampolineBank,
                        format!("bank register of the call to {target} not constant"),
                    );
                    sub.lo = None;
                }
            }
        }
        let Some((th, tl)) = sum.target else {
            if self.own_bank.is_none() {
                st.lo = sub.lo;
            }
            self.out.calls.push(CallSite {
                site,
                target,
                kind,
                state: *st,
            });
            return;
        };
        let addr = match (st.regs[th], st.regs[tl]) {
            (Val::Const(h), Val::Const(l)) => Some(u16::from_be_bytes([h, l])),
            (Val::Entry(h), Val::Entry(l)) => {
                self.out.summary.target.get_or_insert((h, l));
                self.out.parametric = true;
                None
            }
            _ => {
                self.out.parametric = true;
                self.unresolved(
                    site,
                    UnresolvedKind::IndirectCall,
                    format!("target address of the call to {target} not constant"),
                );
                None
            }
        };
        if let Some(addr) = addr
            && sum.bank.is_none_or(|_| sub.lo.is_some())
            && let Some(t) = self.resolve(site, &sub, addr)
        {
            self.out.calls.push(CallSite {
                site,
                target: t,
                kind: if sum.bank.is_some() {
                    CallKind::Trampoline
                } else {
                    CallKind::Indirect
                },
                state: sub,
            });
        }
        self.out.calls.push(CallSite {
            site,
            target,
            kind,
            state: *st,
        });
        if self.own_bank.is_none() && sum.bank.is_some() {
            st.lo = None;
            st.hi = None;
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

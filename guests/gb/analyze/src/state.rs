//! The abstract machine state and the effect of one instruction on it.

use relift_cpu_sm83::{Immediate, Instruction, Mnemonic, Operand, Reg8, Reg16};

use crate::bankmodel::{BankModel, BankReg};

pub(crate) const A: usize = 0;
pub(crate) const B: usize = 1;
pub(crate) const C: usize = 2;
pub(crate) const D: usize = 3;
pub(crate) const E: usize = 4;
pub(crate) const H: usize = 5;
pub(crate) const L: usize = 6;

/// What is known about one 8-bit register.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Val {
    Top,
    Const(u8),
    Entry(usize),
    Popped,
}

/// What is known on one path.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct State {
    pub regs: [Val; 7],
    pub lo: Option<u16>,
    pub hi: Option<u8>,
    pub depth: Option<i16>,
    pub popped: bool,
}

impl State {
    pub(crate) fn reset(model: &BankModel) -> State {
        let (lo, hi) = model.reset();
        State {
            regs: [Val::Top; 7],
            lo: Some(lo),
            hi: Some(hi),
            depth: Some(0),
            popped: false,
        }
    }

    pub(crate) fn symbolic() -> State {
        let mut regs = [Val::Top; 7];
        for (i, r) in regs.iter_mut().enumerate() {
            *r = Val::Entry(i);
        }
        State {
            regs,
            lo: None,
            hi: None,
            depth: Some(0),
            popped: false,
        }
    }

    pub(crate) fn merge(&mut self, other: &State) -> bool {
        let before = *self;
        for (a, b) in self.regs.iter_mut().zip(other.regs.iter()) {
            if a != b {
                *a = Val::Top;
            }
        }
        if self.lo != other.lo {
            self.lo = None;
        }
        if self.hi != other.hi {
            self.hi = None;
        }
        if self.depth != other.depth {
            self.depth = None;
        }
        self.popped |= other.popped;
        *self != before
    }

    pub(crate) fn kill_all(&mut self) {
        self.regs = [Val::Top; 7];
    }

    pub(crate) fn pair(&self, p: Reg16) -> Option<u16> {
        let (h, l) = pair_regs(p)?;
        match (self.regs[h], self.regs[l]) {
            (Val::Const(h), Val::Const(l)) => Some(u16::from_be_bytes([h, l])),
            _ => None,
        }
    }

    fn set_pair(&mut self, p: Reg16, v: Option<u16>) {
        if let Some((h, l)) = pair_regs(p) {
            match v {
                Some(v) => {
                    let [vh, vl] = v.to_be_bytes();
                    self.regs[h] = Val::Const(vh);
                    self.regs[l] = Val::Const(vl);
                }
                None => {
                    self.regs[h] = Val::Top;
                    self.regs[l] = Val::Top;
                }
            }
        }
    }

    pub(crate) fn window_bank(&self, model: &BankModel) -> Option<Result<u16, u16>> {
        model.resolve(self.lo, self.hi).map(|r| r.map_err(|e| e.0))
    }
}

pub(crate) fn pair_regs(p: Reg16) -> Option<(usize, usize)> {
    match p {
        Reg16::Bc => Some((B, C)),
        Reg16::De => Some((D, E)),
        Reg16::Hl => Some((H, L)),
        _ => None,
    }
}

pub(crate) fn reg_index(r: Reg8) -> usize {
    match r {
        Reg8::A => A,
        Reg8::B => B,
        Reg8::C => C,
        Reg8::D => D,
        Reg8::E => E,
        Reg8::H => H,
        Reg8::L => L,
    }
}

/// A bank register write an instruction makes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct BankWrite {
    pub reg: BankReg,
    pub val: Val,
}

/// A store the analysis records.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Write {
    Bank(BankWrite),
    Ram { addr: u16, val: Val },
}

fn address(st: &State, op: Operand, ins: &Instruction) -> Option<u16> {
    match op {
        Operand::Indirect(p) => st.pair(p),
        Operand::HlInc | Operand::HlDec => st.pair(Reg16::Hl),
        Operand::IndirectImm16 => match ins.immediate {
            Immediate::U16(a) => Some(a),
            _ => None,
        },
        Operand::IndirectC => match st.regs[C] {
            Val::Const(c) => Some(0xFF00 | u16::from(c)),
            _ => None,
        },
        Operand::HighImm8 => match ins.immediate {
            Immediate::U8(a) => Some(0xFF00 | u16::from(a)),
            _ => None,
        },
        _ => None,
    }
}

fn is_memory(op: Operand) -> bool {
    matches!(
        op,
        Operand::Indirect(_)
            | Operand::HlInc
            | Operand::HlDec
            | Operand::IndirectC
            | Operand::IndirectImm16
            | Operand::HighImm8
    )
}

fn post_adjust(st: &mut State, op: Operand) {
    let delta = match op {
        Operand::HlInc => 1u16,
        Operand::HlDec => 0xFFFF,
        _ => return,
    };
    let next = st.pair(Reg16::Hl).map(|v| v.wrapping_add(delta));
    st.set_pair(Reg16::Hl, next);
}

fn kill_operand(st: &mut State, op: Operand) {
    match op {
        Operand::R8(r) => st.regs[reg_index(r)] = Val::Top,
        Operand::R16(Reg16::Af) => st.regs[A] = Val::Top,
        Operand::R16(p) => st.set_pair(p, None),
        _ => {}
    }
}

/// Apply `ins` (not a branch) to `st`, pushing events.
pub(crate) fn step(model: &BankModel, st: &mut State, ins: &Instruction, events: &mut Vec<Write>) {
    let op = ins.opcode;
    match op.mnemonic {
        Mnemonic::Ld => step_ld(model, st, ins, events),
        Mnemonic::Inc | Mnemonic::Dec => kill_operand(st, op.dst),
        Mnemonic::Add => {
            if op.dst == Operand::R16(Reg16::Sp) {
                st.depth = None;
            }
            kill_operand(st, op.dst);
        }
        Mnemonic::Xor | Mnemonic::Sub if op.src == Operand::R8(Reg8::A) => {
            st.regs[A] = Val::Const(0);
        }
        Mnemonic::And | Mnemonic::Or if op.src == Operand::R8(Reg8::A) => {}
        Mnemonic::Adc
        | Mnemonic::Sub
        | Mnemonic::Sbc
        | Mnemonic::And
        | Mnemonic::Xor
        | Mnemonic::Or
        | Mnemonic::Rlca
        | Mnemonic::Rrca
        | Mnemonic::Rla
        | Mnemonic::Rra
        | Mnemonic::Daa
        | Mnemonic::Cpl => st.regs[A] = Val::Top,
        Mnemonic::Pop => step_pop(st, op.dst),
        Mnemonic::Push => st.depth = st.depth.map(|d| d + 1),
        Mnemonic::Rlc
        | Mnemonic::Rrc
        | Mnemonic::Rl
        | Mnemonic::Rr
        | Mnemonic::Sla
        | Mnemonic::Sra
        | Mnemonic::Swap
        | Mnemonic::Srl => kill_operand(st, op.dst),
        Mnemonic::Res | Mnemonic::Set => kill_operand(st, op.src),
        _ => {}
    }
}

fn step_pop(st: &mut State, dst: Operand) {
    let own = st.depth == Some(0);
    st.depth = st.depth.map(|d| d - 1);
    kill_operand(st, dst);
    if own {
        st.popped = true;
        if let Operand::R16(p) = dst
            && let Some((h, l)) = pair_regs(p)
        {
            st.regs[h] = Val::Popped;
            st.regs[l] = Val::Popped;
        }
    }
}

fn step_ld(model: &BankModel, st: &mut State, ins: &Instruction, events: &mut Vec<Write>) {
    let op = ins.opcode;
    let (dst, src) = (op.dst, op.src);
    if is_memory(dst) {
        let value = match (src, ins.immediate) {
            (Operand::R8(r), _) => st.regs[reg_index(r)],
            (Operand::Imm8, Immediate::U8(v)) => Val::Const(v),
            _ => Val::Top,
        };
        if let Some(addr) = address(st, dst, ins)
            && addr < 0x8000
            && let Some(reg) = model.classify(addr)
        {
            match (reg, value) {
                (BankReg::Lo, Val::Const(v)) => st.lo = Some(model.register_value(reg, v)),
                (BankReg::Hi, Val::Const(v)) => {
                    st.hi = Some(u8::try_from(model.register_value(reg, v)).unwrap_or(0));
                }
                (BankReg::Lo, _) => st.lo = None,
                (BankReg::Hi, _) => st.hi = None,
            }
            events.push(Write::Bank(BankWrite { reg, val: value }));
        } else if let Some(addr) = address(st, dst, ins)
            && addr >= 0x8000
        {
            events.push(Write::Ram { addr, val: value });
        }
        post_adjust(st, dst);
        return;
    }
    if is_memory(src) {
        kill_operand(st, dst);
        post_adjust(st, src);
        return;
    }
    match (dst, src, ins.immediate) {
        (Operand::R8(d), Operand::Imm8, Immediate::U8(v)) => st.regs[reg_index(d)] = Val::Const(v),
        (Operand::R8(d), Operand::R8(s), _) => st.regs[reg_index(d)] = st.regs[reg_index(s)],
        (Operand::R16(p), Operand::Imm16, Immediate::U16(v)) => st.set_pair(p, Some(v)),
        (Operand::R16(Reg16::Sp), _, _) => st.depth = None,
        (Operand::R16(p), _, _) => st.set_pair(p, None),
        _ => {}
    }
}

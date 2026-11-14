//! Lifting SM83 instructions to `relift-ir`.
//!
//! One instruction is one block. Each block ends in a [`relift_ir::Next::Const`]
//! exit at the next instruction. The data accesses of an instruction are
//! `IoLoad` and `IoStore` operations of kind `Any`, so the guest's [`IoMap`]
//! decides at run time (or, for a constant address, at lift time through
//! [`classify_io`]) whether one reaches a device. The clock is charged in
//! T-cycles: the cycles before each data access come right before it, the
//! rest at the end, with the access cycles taken from the opcode table.

mod alu;
mod cb;
mod load;

use core::fmt;

use relift_ir::{Block, Builder, Endian, IoKind, IoMap, Reg, Value, Width, classify_io};

use crate::regs::{self, FC, FH, FN, FZ};
use crate::{AccessKind, Immediate, Instruction, MemAccess, Mnemonic, Opcode, Operand, Reg16};

/// Why an instruction was not lifted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LiftError {
    /// The instruction is valid but control flow and machine state
    /// instructions are not lifted yet.
    NotYetLifted(Mnemonic),
    /// One of the 11 opcodes the SM83 does not define.
    Illegal(u8),
    /// The opcode table and the lifter disagree about an instruction.
    Table(&'static str),
}

impl fmt::Display for LiftError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LiftError::NotYetLifted(m) => write!(f, "{m:?} is not lifted yet"),
            LiftError::Illegal(code) => write!(f, "opcode {code:#04x} is not defined"),
            LiftError::Table(what) => write!(f, "opcode table mismatch: {what}"),
        }
    }
}

impl core::error::Error for LiftError {}

pub(crate) type Lifted<T> = Result<T, LiftError>;

pub(crate) struct Cx<'a> {
    pub(crate) b: Builder,
    pub(crate) imm: Immediate,
    op: &'a Opcode,
    accesses: Vec<MemAccess>,
    next_access: usize,
    elapsed: u32,
}

impl<'a> Cx<'a> {
    fn new(ins: &'a Instruction, pc: u16) -> Cx<'a> {
        Cx {
            b: Builder::new(regs::regs(), Width::W16, u64::from(pc)),
            imm: ins.immediate,
            op: ins.opcode,
            accesses: ins
                .opcode
                .accesses(false)
                .iter()
                .copied()
                .filter(|a| a.kind != AccessKind::Fetch)
                .collect(),
            next_access: 0,
            elapsed: 0,
        }
    }

    fn finish(mut self) -> Lifted<Block> {
        if self.next_access != self.accesses.len() {
            return Err(LiftError::Table("an access of the table was not made"));
        }
        let total = 4 * u32::from(self.op.cycles.not_taken);
        if total > self.elapsed {
            self.b.tick(total - self.elapsed);
        }
        let len = u32::from(self.op.length);
        let next = (self.b.pc() + u64::from(len)) & 0xFFFF;
        Ok(self.b.jump(len, next))
    }

    fn access(&mut self, kind: AccessKind) -> Lifted<()> {
        let a = *self
            .accesses
            .get(self.next_access)
            .ok_or(LiftError::Table("more accesses than the table lists"))?;
        if a.kind != kind {
            return Err(LiftError::Table("access kind differs from the table"));
        }
        self.next_access += 1;
        let at = 4 * (u32::from(a.cycle) - 1);
        if at > self.elapsed {
            self.b.tick(at - self.elapsed);
            self.elapsed = at;
        }
        Ok(())
    }

    pub(crate) fn load(&mut self, addr: Value) -> Lifted<Value> {
        self.access(AccessKind::Read)?;
        Ok(self.b.io_load(addr, Width::W8, Endian::Little, IoKind::Any))
    }

    pub(crate) fn store(&mut self, addr: Value, value: Value) -> Lifted<()> {
        self.access(AccessKind::Write)?;
        self.b.io_store(addr, value, Endian::Little, IoKind::Any);
        Ok(())
    }

    pub(crate) fn k8(&mut self, v: u8) -> Value {
        self.b.konst(Width::W8, u128::from(v))
    }

    pub(crate) fn k16(&mut self, v: u16) -> Value {
        self.b.konst(Width::W16, u128::from(v))
    }

    pub(crate) fn get(&mut self, r: Reg) -> Value {
        self.b.get(r)
    }

    pub(crate) fn imm8(&mut self) -> Lifted<Value> {
        match self.imm {
            Immediate::U8(v) => Ok(self.k8(v)),
            Immediate::I8(v) => Ok(self.k8(v as u8)),
            _ => Err(LiftError::Table("an 8-bit immediate was expected")),
        }
    }

    pub(crate) fn imm16(&self) -> Lifted<u16> {
        match self.imm {
            Immediate::U16(v) => Ok(v),
            _ => Err(LiftError::Table("a 16-bit immediate was expected")),
        }
    }

    pub(crate) fn pair(&mut self, r: Reg16) -> Value {
        match r {
            Reg16::Bc => self.regpair(regs::B, regs::C),
            Reg16::De => self.regpair(regs::D, regs::E),
            Reg16::Hl => self.regpair(regs::H, regs::L),
            Reg16::Sp => self.b.get(regs::SP),
            Reg16::Af => {
                let a = self.b.get(regs::A);
                let f = self.flag_byte();
                self.b.concat(a, f)
            }
        }
    }

    fn regpair(&mut self, hi: Reg, lo: Reg) -> Value {
        let h = self.b.get(hi);
        let l = self.b.get(lo);
        self.b.concat(h, l)
    }

    pub(crate) fn flag_byte(&mut self) -> Value {
        let z = self.b.get(FZ);
        let n = self.b.get(FN);
        let h = self.b.get(FH);
        let c = self.b.get(FC);
        let zn = self.b.concat(z, n);
        let hc = self.b.concat(h, c);
        let high = self.b.concat(zn, hc);
        let low = self.b.konst(Width::new(4), 0);
        self.b.concat(high, low)
    }

    pub(crate) fn set_pair(&mut self, r: Reg16, v: Value) {
        let (hi, lo) = match r {
            Reg16::Bc => (regs::B, regs::C),
            Reg16::De => (regs::D, regs::E),
            Reg16::Hl => (regs::H, regs::L),
            Reg16::Sp => {
                self.b.set(regs::SP, v);
                return;
            }
            Reg16::Af => {
                let a = self.b.extract(v, 8, Width::W8);
                self.b.set(regs::A, a);
                let flags = [(FZ, 7), (FN, 6), (FH, 5), (FC, 4)];
                for (flag, bit) in flags {
                    let x = self.b.bit(v, bit);
                    self.b.set(flag, x);
                }
                return;
            }
        };
        let h = self.b.extract(v, 8, Width::W8);
        let l = self.b.trunc(v, Width::W8);
        self.b.set(hi, h);
        self.b.set(lo, l);
    }

    pub(crate) fn is_zero(&mut self, v: Value) -> Value {
        let w = self.b.width(v);
        let z = self.b.konst(w, 0);
        self.b.cmp(relift_ir::CmpOp::Eq, v, z)
    }

    pub(crate) fn flags(
        &mut self,
        z: Option<Value>,
        n: Option<Value>,
        h: Option<Value>,
        c: Option<Value>,
    ) {
        for (reg, v) in [(FZ, z), (FN, n), (FH, h), (FC, c)] {
            if let Some(v) = v {
                self.b.set(reg, v);
            }
        }
    }

    pub(crate) fn bit_const(&mut self, v: bool) -> Value {
        self.b.konst(Width::W1, u128::from(v))
    }

    /// The address a memory operand names. `HlInc` and `HlDec` name `hl`;
    /// [`Cx::adjust_hl`] applies their side effect.
    pub(crate) fn address(&mut self, op: Operand) -> Lifted<Option<Value>> {
        Ok(Some(match op {
            Operand::Indirect(r) => self.pair(r),
            Operand::HlInc | Operand::HlDec => self.pair(Reg16::Hl),
            Operand::IndirectC => {
                let c = self.b.get(regs::C);
                let high = self.k8(0xFF);
                self.b.concat(high, c)
            }
            Operand::HighImm8 => match self.imm {
                Immediate::U8(n) => self.k16(0xFF00 | u16::from(n)),
                _ => return Err(LiftError::Table("HighImm8 without an 8-bit immediate")),
            },
            Operand::IndirectImm16 => {
                let a = self.imm16()?;
                self.k16(a)
            }
            _ => return Ok(None),
        }))
    }

    pub(crate) fn adjust_hl(&mut self, op: Operand) {
        let delta: u16 = match op {
            Operand::HlInc => 1,
            Operand::HlDec => 0xFFFF,
            _ => return,
        };
        let hl = self.pair(Reg16::Hl);
        let d = self.k16(delta);
        let sum = self.b.add(hl, d);
        self.set_pair(Reg16::Hl, sum);
    }

    pub(crate) fn read8(&mut self, op: Operand) -> Lifted<Value> {
        match op {
            Operand::R8(r) => Ok(self.b.get(regs::reg8(r))),
            Operand::Imm8 => self.imm8(),
            _ => {
                let addr = self
                    .address(op)?
                    .ok_or(LiftError::Table("unexpected 8-bit source"))?;
                let v = self.load(addr)?;
                self.adjust_hl(op);
                Ok(v)
            }
        }
    }

    pub(crate) fn write8(&mut self, op: Operand, v: Value) -> Lifted<()> {
        if let Operand::R8(r) = op {
            self.b.set(regs::reg8(r), v);
            return Ok(());
        }
        let addr = self
            .address(op)?
            .ok_or(LiftError::Table("unexpected 8-bit destination"))?;
        self.store(addr, v)?;
        self.adjust_hl(op);
        Ok(())
    }
}

/// Lifts one decoded instruction at `pc` into a block. `map` is the guest's
/// address classification: accesses at constant addresses that it calls
/// memory become plain loads and stores, those it calls devices become
/// device accesses, and the rest stay `IoKind::Any`.
pub fn lift_instruction(ins: &Instruction, pc: u16, map: &IoMap) -> Result<Block, LiftError> {
    let mut cx = Cx::new(ins, pc);
    let op = ins.opcode;
    match op.mnemonic {
        Mnemonic::Ld | Mnemonic::Push | Mnemonic::Pop => load::lift(&mut cx, op)?,
        Mnemonic::Inc
        | Mnemonic::Dec
        | Mnemonic::Add
        | Mnemonic::Adc
        | Mnemonic::Sub
        | Mnemonic::Sbc
        | Mnemonic::And
        | Mnemonic::Xor
        | Mnemonic::Or
        | Mnemonic::Cp
        | Mnemonic::Cpl
        | Mnemonic::Scf
        | Mnemonic::Ccf => alu::lift(&mut cx, op)?,
        Mnemonic::Rlca
        | Mnemonic::Rrca
        | Mnemonic::Rla
        | Mnemonic::Rra
        | Mnemonic::Rlc
        | Mnemonic::Rrc
        | Mnemonic::Rl
        | Mnemonic::Rr
        | Mnemonic::Sla
        | Mnemonic::Sra
        | Mnemonic::Swap
        | Mnemonic::Srl
        | Mnemonic::Bit
        | Mnemonic::Res
        | Mnemonic::Set => cb::lift(&mut cx, op)?,
        Mnemonic::Illegal => return Err(LiftError::Illegal(op.code)),
        m => return Err(LiftError::NotYetLifted(m)),
    }
    let block = cx.finish()?;
    Ok(classify_io(&block, map))
}

#[cfg(test)]
mod tests;

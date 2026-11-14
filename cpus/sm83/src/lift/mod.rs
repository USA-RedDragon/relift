//! Lifting SM83 instructions to `relift-ir`.
//!
//! One instruction is one block, with these exceptions:
//!
//! * `EI` is lifted with the instruction after it, because the interrupt
//!   master enable takes hold only once that instruction has run.
//! * A conditional `CALL` or `RET` is two blocks: the part up to the
//!   decision, and a continuation for the taken case at a synthetic address
//!   ([`HALF`]), because the IR has no conditional store or pop.
//! * A `HALT` that does not halt (an interrupt is already pending) runs the
//!   instruction after it with the HALT bug; that is a block at a synthetic
//!   address ([`BUG`]).
//!
//! The data accesses of an instruction are `IoLoad` and `IoStore` operations
//! of kind `Any`, so the guest's [`IoMap`] decides at run time (or, for a
//! constant address, at lift time through [`classify_io`]) whether one reaches
//! a device. The clock is charged in T-cycles: the cycles before each data
//! access come right before it, the rest at the end, with the access cycles
//! taken from the opcode table.
//!
//! Control transfers end in a sync point, and so do `EI`, `HALT` and `STOP`;
//! other instructions follow the [`SyncPolicy`].

mod alu;
mod cb;
mod flow;
mod load;

use core::fmt;

use relift_ir::{
    Block, Builder, Endian, Exit, Flow, Inst, IoKind, IoMap, Next, Op, Reg, Value, Width,
    classify_io,
};

use crate::regs::{self, FC, FH, FN, FZ};
use crate::{
    AccessKind, DecodeError, Immediate, Instruction, MemAccess, Mnemonic, Opcode, Operand, Reg16,
    decode,
};

/// Added to the address of a conditional `CALL` or `RET` to name the block of
/// its taken case. The low 16 bits are the address of the instruction.
pub const HALF: u64 = 0x1_0000;

/// Added to the address of a `HALT` to name the block of the instruction
/// after it, run with the HALT bug. The block is lifted from the bytes at the
/// `HALT`: the first byte after it is read twice. Combined with [`HALF`]
/// (`BUG | HALF`) it is the taken case of a conditional `CALL` or `RET` run
/// that way.
pub const BUG: u64 = 0x2_0000;

/// Where a block's sync point goes, besides the control transfers.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SyncPolicy {
    /// Only control transfers, `EI`, `HALT` and `STOP`: interrupts are taken
    /// at the end of each basic block.
    Blocks,
    /// Also after each instruction that stores to something that may be a
    /// device (interrupts follow a register write at the next instruction).
    #[default]
    IoStores,
    /// After every instruction: exact interrupt timing, slowest.
    Every,
}

/// What lifting needs to know besides the instruction.
#[derive(Clone, Debug)]
pub struct Options {
    /// Which addresses are memory and which reach a device.
    pub map: IoMap,
    /// Where sync points go.
    pub sync: SyncPolicy,
}

impl Options {
    /// The default policy over `map`.
    pub fn new(map: IoMap) -> Options {
        Options {
            map,
            sync: SyncPolicy::default(),
        }
    }
}

/// Why an instruction was not lifted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LiftError {
    /// One of the 11 opcodes the SM83 does not define.
    Illegal(u8),
    /// The opcode table and the lifter disagree about an instruction.
    Table(&'static str),
    /// The bytes end inside an instruction.
    Decode(DecodeError),
    /// `EI` was lifted without the instruction after it.
    NeedsNext,
    /// A combination that is not lifted.
    Unsupported(&'static str),
    /// A synthetic address that names no block.
    Address(u64),
}

impl fmt::Display for LiftError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LiftError::Illegal(code) => write!(f, "opcode {code:#04x} is not defined"),
            LiftError::Table(what) => write!(f, "opcode table mismatch: {what}"),
            LiftError::Decode(e) => e.fmt(f),
            LiftError::NeedsNext => f.write_str("EI is lifted with the instruction after it"),
            LiftError::Unsupported(what) => write!(f, "{what} is not lifted"),
            LiftError::Address(a) => write!(f, "{a:#x} names no block"),
        }
    }
}

impl core::error::Error for LiftError {}

impl From<DecodeError> for LiftError {
    fn from(e: DecodeError) -> LiftError {
        LiftError::Decode(e)
    }
}

pub(crate) type Lifted<T> = Result<T, LiftError>;

/// What kind of exit an instruction has, for the sync point.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Kind {
    /// Falls through to the next instruction.
    Plain,
    /// A control transfer, or an instruction the scheduler must see.
    Control,
    /// The part of a conditional `CALL` or `RET` up to the decision: the
    /// taken exit is a synthetic address, so no interrupt may be taken.
    Half,
}

/// How an instruction left off.
pub(crate) struct Done {
    pub(crate) next: Next,
    pub(crate) flow: Flow,
    pub(crate) kind: Kind,
}

pub(crate) struct Cx {
    pub(crate) b: Builder,
    pub(crate) imm: Immediate,
    op: &'static Opcode,
    accesses: Vec<MemAccess>,
    next_access: usize,
    elapsed: u32,
    taken: bool,
    /// The address of the instruction being lifted.
    pub(crate) pc: u16,
    /// [`BUG`] while lifting the bug view, else 0; added to the addresses of
    /// continuations.
    pub(crate) view: u64,
}

impl Cx {
    fn new(ins: &Instruction, pc: u16, view: u64, taken: bool) -> Cx {
        let mut cx = Cx {
            b: Builder::new(regs::regs(), Width::W16, u64::from(pc)),
            imm: ins.immediate,
            op: ins.opcode,
            accesses: Vec::new(),
            next_access: 0,
            elapsed: 0,
            taken,
            pc,
            view,
        };
        cx.begin(ins, taken);
        cx
    }

    /// Starts the next instruction of the block.
    fn begin(&mut self, ins: &Instruction, taken: bool) {
        self.imm = ins.immediate;
        self.op = ins.opcode;
        self.taken = taken;
        self.accesses = ins
            .opcode
            .accesses(taken)
            .iter()
            .copied()
            .filter(|a| a.kind != AccessKind::Fetch)
            .collect();
        self.next_access = 0;
        self.elapsed = if taken {
            4 * u32::from(ins.opcode.cycles.not_taken)
        } else {
            0
        };
    }

    /// Charges the rest of the instruction's cycles. A taken continuation
    /// charges the difference to the taken cost; anything else the cost
    /// without a taken branch.
    pub(crate) fn finish_insn(&mut self) -> Lifted<()> {
        if self.next_access != self.accesses.len() {
            return Err(LiftError::Table("an access of the table was not made"));
        }
        let m = if self.taken {
            self.op.cycles.taken
        } else {
            self.op.cycles.not_taken
        };
        let total = 4 * u32::from(m);
        if total > self.elapsed {
            self.b.tick(total - self.elapsed);
            self.elapsed = total;
        }
        Ok(())
    }

    /// The length of the instruction in bytes.
    pub(crate) fn len(&self) -> u16 {
        u16::from(self.op.length)
    }

    /// The address after the instruction.
    pub(crate) fn after(&self) -> u64 {
        u64::from(self.pc.wrapping_add(self.len()))
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

    /// Calls the runtime intrinsic `name` (no operands, no result).
    pub(crate) fn effect(&mut self, name: &'static str) {
        self.b.intrinsic(name, &[], None);
    }
}

/// The part of [`lift_one`] that is not control flow: the instruction falls
/// through to the next one.
fn plain(cx: &mut Cx) -> Lifted<Done> {
    cx.finish_insn()?;
    Ok(Done {
        next: Next::Const(cx.after()),
        flow: Flow::Jump,
        kind: Kind::Plain,
    })
}

/// Lifts one instruction into `cx`, which has begun it. `taken` lifts the
/// continuation of a conditional `CALL` or `RET`.
fn lift_one(cx: &mut Cx, ins: &Instruction) -> Lifted<Done> {
    let op = ins.opcode;
    match op.mnemonic {
        Mnemonic::Ld | Mnemonic::Push | Mnemonic::Pop => {
            load::lift(cx, op)?;
            plain(cx)
        }
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
        | Mnemonic::Ccf => {
            alu::lift(cx, op)?;
            plain(cx)
        }
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
        | Mnemonic::Set => {
            cb::lift(cx, op)?;
            plain(cx)
        }
        Mnemonic::Illegal => Err(LiftError::Illegal(op.code)),
        Mnemonic::Prefix => Err(LiftError::Table("a prefix byte is not an instruction")),
        _ => flow::lift(cx, op),
    }
}

/// How `ins` is lifted: at `pc`, with `next` the instruction after it (for
/// `EI`), as the continuation of a taken conditional (`half`), and as the
/// bug view (`view`), which sets only the addresses of continuations.
struct Plan<'a> {
    ins: &'a Instruction,
    next: Option<&'a Instruction>,
    pc: u16,
    half: bool,
    view: u64,
    /// The address the block is known by, when it is not `pc`.
    label: Option<u64>,
}

fn lift_plan(plan: &Plan, opts: &Options) -> Lifted<Block> {
    let ins = plan.ins;
    let mut cx = Cx::new(ins, plan.pc, plan.view, plan.half);
    let mut done = lift_one(&mut cx, ins)?;
    let mut len = u32::from(ins.length());
    if ins.opcode.mnemonic == Mnemonic::Ei && !plan.half {
        let nx = plan.next.ok_or(LiftError::NeedsNext)?;
        cx.pc = plan.pc.wrapping_add(1);
        cx.begin(nx, false);
        done = lift_one(&mut cx, nx)?;
        if done.kind == Kind::Half {
            return Err(LiftError::Unsupported(
                "EI before a conditional call or return",
            ));
        }
        if nx.opcode.mnemonic != Mnemonic::Di {
            cx.effect(crate::irq::EI);
        }
        done.kind = Kind::Control;
        len += u32::from(nx.length());
    }
    let b = cx.b;
    let len = if plan.half { 0 } else { len };
    let block = b.finish(
        len,
        Exit {
            next: done.next,
            flow: done.flow,
        },
    );
    let mut block = classify_io(&block, &opts.map);
    let sync = match done.kind {
        Kind::Control => true,
        Kind::Half => false,
        Kind::Plain => match opts.sync {
            SyncPolicy::Blocks => false,
            SyncPolicy::Every => true,
            SyncPolicy::IoStores => block
                .insts
                .iter()
                .any(|i| matches!(i.op, Op::IoStore { .. })),
        },
    };
    if sync {
        block.insts.push(Inst {
            op: Op::Sync,
            width: None,
        });
    }
    if let Some(l) = plan.label {
        block.pc = l;
    }
    Ok(block)
}

/// Lifts one decoded instruction at `pc` into a block. `map` is the guest's
/// address classification: accesses at constant addresses that it calls
/// memory become plain loads and stores, those it calls devices become
/// device accesses, and the rest stay `IoKind::Any`. `EI` needs the
/// instruction after it: use [`lift_with`].
///
/// # Errors
///
/// The instruction is `EI`, is not defined, or does not agree with the table.
pub fn lift_instruction(ins: &Instruction, pc: u16, map: &IoMap) -> Result<Block, LiftError> {
    lift_with(ins, None, pc, &Options::new(map.clone()))
}

/// Lifts the instruction `ins` at `pc`; `next` is the instruction after it,
/// needed for `EI`.
///
/// # Errors
///
/// As [`lift_instruction`].
pub fn lift_with(
    ins: &Instruction,
    next: Option<&Instruction>,
    pc: u16,
    opts: &Options,
) -> Result<Block, LiftError> {
    lift_plan(
        &Plan {
            ins,
            next,
            pc,
            half: false,
            view: 0,
            label: None,
        },
        opts,
    )
}

/// Lifts the bytes at `pc` as the frontend does: a plain address is the
/// instruction there (with the one after it for `EI`); an address with
/// [`HALF`] and/or [`BUG`] added to the address of the instruction is the
/// continuation block it names. `bytes` start at the low 16 bits of `pc`.
///
/// # Errors
///
/// The bytes end inside an instruction, the instruction is not defined, or
/// the address names no block.
pub fn lift_bytes(bytes: &[u8], pc: u64, opts: &Options) -> Result<Block, LiftError> {
    let flags = pc & (HALF | BUG);
    let real = u16::try_from(pc & 0xFFFF).map_err(|_| LiftError::Address(pc))?;
    if pc > 0x3_FFFF {
        return Err(LiftError::Address(pc));
    }
    let stream: Vec<u8>;
    let bytes = if flags & BUG != 0 {
        if bytes.first() != Some(&0x76) || bytes.len() < 2 {
            return Err(LiftError::Address(pc));
        }
        stream = core::iter::once(bytes[1])
            .chain(bytes[1..].iter().copied())
            .collect();
        &stream[..]
    } else {
        bytes
    };
    let ins = decode(bytes)?;
    let after = bytes.get(usize::from(ins.length())..).unwrap_or(&[]);
    let next = if ins.opcode.mnemonic == Mnemonic::Ei && flags & HALF == 0 {
        Some(decode(after)?)
    } else {
        None
    };
    let half = flags & HALF != 0;
    if half && !flow::is_conditional_transfer(ins.opcode) {
        return Err(LiftError::Address(pc));
    }
    lift_plan(
        &Plan {
            ins: &ins,
            next: next.as_ref(),
            pc: real,
            half,
            view: flags & BUG,
            label: (flags != 0).then_some(pc),
        },
        opts,
    )
}

#[cfg(test)]
mod tests;

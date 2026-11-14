//! The abstract machine state and the effect of one instruction on it.
//!
//! A register holds a [`Val`]: a constant, the value it had at the entry of
//! the function being analyzed, a byte of a symbolic 16-bit value (the
//! return address, the entry stack pointer, an entry register pair), or a
//! byte loaded from a symbolic address. The stack is modelled relative to
//! the stack pointer at entry, so pushed values come back on a pop and code
//! can address its own frame through `ld hl,sp+e`. Reads of ROM at a known
//! address and bank give the constant byte.

use relift_cpu_sm83::{FlagEffect, Immediate, Instruction, Mnemonic, Operand, Reg8, Reg16};

use crate::bankmodel::{BankModel, BankReg};
use crate::image::Image;
use crate::loc::Loc;

pub(crate) const A: usize = 0;
pub(crate) const B: usize = 1;
pub(crate) const C: usize = 2;
pub(crate) const D: usize = 3;
pub(crate) const E: usize = 4;
pub(crate) const H: usize = 5;
pub(crate) const L: usize = 6;

/// The lowest stack offset modelled, relative to the entry stack pointer.
pub(crate) const STACK_LO: i16 = -64;
/// One past the highest stack offset modelled.
pub(crate) const STACK_HI: i16 = 32;
pub(crate) const SLOTS: usize = (STACK_HI - STACK_LO) as usize;
const CELLS: usize = 16;
const MERGED: u16 = 0x0FFF;

/// The bank of a ROM read in the window as the caller left it.
pub(crate) const ENTRY_BANK: u16 = u16::MAX;

/// What a symbolic 16-bit value is relative to.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) enum Base {
    /// The return address of the function analyzed on its own.
    Ret,
    /// The stack pointer at entry.
    Sp,
    /// The entry value of a register pair: high and low register.
    Pair(u8, u8),
    /// An address in ROM bank `n` ([`ENTRY_BANK`] for the bank mapped at
    /// entry); only the address of a loaded byte has it.
    Rom(u16),
    /// A plain address.
    Abs,
}

/// A 16-bit value `base + off + step * i` for an unknown `i` in `0..count`
/// (`step` 0 for no index).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct Sym {
    pub base: Base,
    pub off: u16,
    pub step: u8,
    pub count: Option<u16>,
    pub ix: u16,
}

impl Sym {
    pub(crate) const fn at(base: Base, off: u16) -> Sym {
        Sym {
            base,
            off,
            step: 0,
            count: None,
            ix: 0,
        }
    }

    pub(crate) fn plus(self, d: u16) -> Sym {
        Sym {
            off: self.off.wrapping_add(d),
            ..self
        }
    }

    /// Whether the value depends on the caller: the return address, the
    /// entry registers, the caller's stack or bank.
    pub(crate) fn depends_on_entry(&self) -> bool {
        matches!(
            self.base,
            Base::Ret | Base::Sp | Base::Pair(..) | Base::Rom(ENTRY_BANK)
        )
    }
}

/// What is known about one 8-bit value.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) enum Val {
    Top,
    Const(u8),
    /// The value of a register at entry.
    Entry(u8),
    /// The low byte of a 16-bit value.
    Lo(Sym),
    /// The high byte of a 16-bit value.
    Hi(Sym),
    /// The byte read from an address.
    At(Sym),
    /// `i * mul` for an unknown `i` below `count`.
    Ix(u8, Option<u16>),
    /// Not known, but computed from values that depend on the caller.
    Dep,
    /// The high byte of a 16-bit value less one: the high byte of the base
    /// before the carry of an indexed low byte is added.
    HiNoCarry(Sym),
}

impl Val {
    pub(crate) fn depends_on_entry(&self) -> bool {
        match self {
            Val::Entry(_) | Val::Dep => true,
            Val::Lo(s) | Val::Hi(s) | Val::At(s) => s.depends_on_entry(),
            _ => false,
        }
    }

    pub(crate) fn konst(&self) -> Option<u8> {
        match self {
            Val::Const(v) => Some(*v),
            _ => None,
        }
    }
}

/// The value of a ROM bank register.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum Bv {
    Known(u16),
    /// As at entry.
    Entry,
    Unknown,
    /// The byte last stored at a RAM address.
    Cell(u16),
    /// A value that depends on the caller.
    Param,
}

/// A 16-bit view of a register pair.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Word {
    Const(u16),
    /// Both bytes of one symbolic value.
    Sym(Sym),
    /// The word read from a symbolic address.
    Load(Sym),
    /// The entry value of a register pair.
    EntryPair(u8, u8),
    Other,
}

impl Word {
    pub(crate) fn depends_on_entry(&self) -> bool {
        match self {
            Word::Sym(s) | Word::Load(s) => s.depends_on_entry(),
            Word::EntryPair(..) => true,
            _ => false,
        }
    }
}

pub(crate) fn word_of(h: Val, l: Val) -> Word {
    match (h, l) {
        (Val::Const(h), Val::Const(l)) => Word::Const(u16::from_be_bytes([h, l])),
        (Val::Hi(a), Val::Lo(b)) if a == b => Word::Sym(a),
        (Val::At(a), Val::At(b)) if a == b.plus(1) => Word::Load(b),
        (Val::Entry(h), Val::Entry(l)) => Word::EntryPair(h, l),
        _ => Word::Other,
    }
}

pub(crate) fn split(w: Word) -> (Val, Val) {
    match w {
        Word::Const(v) => {
            let [h, l] = v.to_be_bytes();
            (Val::Const(h), Val::Const(l))
        }
        Word::Sym(s) => (Val::Hi(s), Val::Lo(s)),
        Word::Load(s) => (Val::At(s.plus(1)), Val::At(s)),
        Word::EntryPair(h, l) => (Val::Entry(h), Val::Entry(l)),
        Word::Other => (Val::Top, Val::Top),
    }
}

/// What is known on one path.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct State {
    pub regs: [Val; 7],
    pub bound: [Option<u16>; 7],
    pub ver: [u16; 7],
    pub lo: Bv,
    pub hi: Bv,
    pub sp: Option<i16>,
    pub stack: [Val; SLOTS],
    pub cells: [(u16, Val); CELLS],
    pub z: Option<bool>,
    pub c: Option<bool>,
    pub cp: Option<(u16, u8)>,
    /// The zero flag tests the byte loaded from this RAM address into A.
    pub zcell: Option<(u16, u16)>,
    /// The carry flag is the carry out of the low byte of this indexed
    /// value, whose high byte without the carry is the second field.
    pub carry: Option<(Sym, Val)>,
    /// The zero flag tells whether the word at this RAM address is zero.
    pub zword: Option<(u16, u16)>,
    /// The word at this RAM address is known not to be zero.
    pub nonzero: Option<u16>,
}

impl State {
    fn blank() -> State {
        State {
            regs: [Val::Top; 7],
            bound: [None; 7],
            ver: [0; 7],
            lo: Bv::Unknown,
            hi: Bv::Unknown,
            sp: Some(0),
            stack: [Val::Top; SLOTS],
            cells: [(0, Val::Top); CELLS],
            z: None,
            c: None,
            cp: None,
            zcell: None,
            carry: None,
            zword: None,
            nonzero: None,
        }
    }

    /// The state after reset: bank one mapped, nothing else known.
    pub(crate) fn reset(model: &BankModel) -> State {
        let (lo, hi) = model.reset();
        State {
            lo: Bv::Known(lo),
            hi: Bv::Known(u16::from(hi)),
            ..State::blank()
        }
    }

    /// The state at the entry of a function analyzed on its own: entry
    /// registers, a symbolic return address and the caller's stack above it.
    pub(crate) fn symbolic() -> State {
        let mut st = State::blank();
        for (i, r) in st.regs.iter_mut().enumerate() {
            *r = Val::Entry(i as u8);
        }
        st.lo = Bv::Entry;
        st.hi = Bv::Entry;
        let ret = Sym::at(Base::Ret, 0);
        st.set_slot(0, Val::Lo(ret));
        st.set_slot(1, Val::Hi(ret));
        for off in 2..STACK_HI {
            st.set_slot(off, Val::At(Sym::at(Base::Sp, off as u16)));
        }
        st
    }

    /// Join `other` into `self`; whether `self` changed.
    pub(crate) fn merge(&mut self, other: &State) -> bool {
        let before = *self;
        for i in 0..7 {
            self.regs[i] = join_val(self.regs[i], other.regs[i]);
            self.bound[i] = match (self.bound[i], other.bound[i]) {
                (Some(a), Some(b)) => Some(a.max(b)),
                _ => None,
            };
            if self.ver[i] != other.ver[i] {
                self.ver[i] = MERGED;
            }
        }
        if self.lo != other.lo {
            self.lo = Bv::Unknown;
        }
        if self.hi != other.hi {
            self.hi = Bv::Unknown;
        }
        if self.sp != other.sp {
            self.sp = None;
        }
        for (a, b) in self.stack.iter_mut().zip(other.stack.iter()) {
            *a = join_val(*a, *b);
        }
        for c in &mut self.cells {
            if c.1 != Val::Top && !other.cells.contains(c) {
                *c = (0, Val::Top);
            }
        }
        if self.z != other.z {
            self.z = None;
        }
        if self.c != other.c {
            self.c = None;
        }
        if self.cp != other.cp {
            self.cp = None;
        }
        if self.zcell != other.zcell {
            self.zcell = None;
        }
        if self.carry != other.carry {
            self.carry = None;
        }
        if self.zword != other.zword {
            self.zword = None;
        }
        if self.nonzero != other.nonzero {
            self.nonzero = None;
        }
        *self != before
    }

    pub(crate) fn slot(&self, off: i16) -> Val {
        if (STACK_LO..STACK_HI).contains(&off) {
            self.stack[(off - STACK_LO) as usize]
        } else {
            Val::Top
        }
    }

    pub(crate) fn set_slot(&mut self, off: i16, v: Val) {
        if (STACK_LO..STACK_HI).contains(&off) {
            self.stack[(off - STACK_LO) as usize] = v;
        }
    }

    /// Pop a word: the high and low byte, or `None` when the stack pointer
    /// is not known.
    pub(crate) fn pop(&mut self) -> Option<(Val, Val)> {
        let sp = self.sp?;
        let l = self.slot(sp);
        let h = self.slot(sp + 1);
        self.sp = Some(sp + 2);
        Some((h, l))
    }

    pub(crate) fn push(&mut self, h: Val, l: Val) {
        if let Some(sp) = self.sp {
            let sp = sp - 2;
            self.set_slot(sp, l);
            self.set_slot(sp + 1, h);
            self.sp = (sp > STACK_LO).then_some(sp);
        }
    }

    pub(crate) fn set(&mut self, r: usize, v: Val) {
        self.regs[r] = v;
        self.bound[r] = match v {
            Val::Const(c) => Some(u16::from(c) + 1),
            Val::Ix(m, Some(n)) => Some(u16::from(m) * n.saturating_sub(1) + 1),
            _ => None,
        };
        self.ver[r] = (self.ver[r].wrapping_add(1)) & 0x07FF;
    }

    pub(crate) fn word(&self, p: Reg16) -> Word {
        match pair_regs(p) {
            Some((h, l)) => word_of(self.regs[h], self.regs[l]),
            None => Word::Other,
        }
    }

    pub(crate) fn set_word(&mut self, p: Reg16, w: Word) {
        if let Some((h, l)) = pair_regs(p) {
            let (vh, vl) = split(w);
            self.set(h, vh);
            self.set(l, vl);
        }
    }

    pub(crate) fn kill_all(&mut self) {
        for r in 0..7 {
            self.set(r, Val::Top);
        }
        self.kill_memory();
    }

    pub(crate) fn kill_memory(&mut self) {
        self.cells = [(0, Val::Top); CELLS];
        self.z = None;
        self.c = None;
        self.cp = None;
        self.zcell = None;
        self.carry = None;
        self.zword = None;
        self.nonzero = None;
    }

    /// Record that the byte at `addr` holds `v` on this path.
    pub(crate) fn know_cell(&mut self, addr: u16, v: Val) {
        self.set_cell(addr, v);
    }

    pub(crate) fn cell(&self, addr: u16) -> Option<Val> {
        self.cells
            .iter()
            .find(|c| c.0 == addr && c.1 != Val::Top)
            .map(|c| c.1)
    }

    fn set_cell(&mut self, addr: u16, v: Val) {
        if self
            .nonzero
            .is_some_and(|w| addr == w || addr == w.wrapping_add(1))
        {
            self.nonzero = None;
        }
        if !plain_ram(addr) {
            return;
        }
        if let Some(c) = self.cells.iter_mut().find(|c| c.0 == addr) {
            c.1 = v;
            return;
        }
        self.cells.rotate_right(1);
        self.cells[0] = (addr, v);
    }

    /// Constant bytes stored on this path, with their addresses.
    pub(crate) fn const_cells(&self) -> Vec<(u16, u8)> {
        let mut v: Vec<(u16, u8)> = self
            .cells
            .iter()
            .filter_map(|c| c.1.konst().map(|k| (c.0, k)))
            .collect();
        v.sort_unstable();
        v
    }

    pub(crate) fn window_bank(&self, model: &BankModel) -> WindowBank {
        let known = |b: Bv| match b {
            Bv::Known(v) => Some(v),
            _ => None,
        };
        if model.kind() == relift_gb_formats::MapperKind::None {
            return WindowBank::Known(1);
        }
        match (self.lo, self.hi) {
            (Bv::Entry, Bv::Entry) => WindowBank::Entry,
            (Bv::Entry, _) if !model.hi_matters() => WindowBank::Entry,
            (Bv::Param, _) | (_, Bv::Param) => WindowBank::Param,
            (Bv::Cell(x), h) if !model.hi_matters() || known(h) == Some(0) => WindowBank::Cell(x),
            (lo, hi) => {
                let hv = known(hi).map(|h| u8::try_from(h).unwrap_or(0));
                match model.resolve(known(lo), hv) {
                    Some(Ok(b)) => WindowBank::Known(b),
                    Some(Err(e)) => WindowBank::OutOfRange(e.0),
                    None => WindowBank::Unknown,
                }
            }
        }
    }
}

fn join_sym(a: Sym, b: Sym) -> Option<Sym> {
    if a == b {
        return Some(a);
    }
    let same = Sym { ix: 0, ..a } == Sym { ix: 0, ..b };
    same.then(|| Sym {
        ix: (a.ix ^ b.ix.rotate_left(5)) | 0x8000,
        ..a
    })
}

/// The join of two values on merging paths: equal values, or symbolic
/// values that differ only in which index variable they use, which become
/// one merged index.
pub(crate) fn join_val(a: Val, b: Val) -> Val {
    match (a, b) {
        _ if a == b => a,
        (Val::Lo(x), Val::Lo(y)) => join_sym(x, y).map_or(Val::Top, Val::Lo),
        (Val::Hi(x), Val::Hi(y)) => join_sym(x, y).map_or(Val::Top, Val::Hi),
        (Val::At(x), Val::At(y)) => join_sym(x, y).map_or(Val::Top, Val::At),
        (Val::Dep, _) | (_, Val::Dep) => Val::Dep,
        _ if a.depends_on_entry() || b.depends_on_entry() => Val::Dep,
        _ => Val::Top,
    }
}

/// Whether a byte stored at `addr` reads back unchanged: work RAM and high
/// RAM. Video RAM, OAM, cartridge RAM and I/O registers may not, depending
/// on the PPU mode, the RAM enable or the hardware.
pub(crate) fn plain_ram(addr: u16) -> bool {
    (0xC000..0xE000).contains(&addr) || (0xFF80..0xFFFF).contains(&addr)
}

/// The bank mapped in the window on a path.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum WindowBank {
    Known(u16),
    OutOfRange(u16),
    Entry,
    Cell(u16),
    Param,
    Unknown,
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
    /// A store to a RAM address, with the value and the value stored on
    /// this path just before at the other byte of the word at `addr - 1`
    /// and `addr + 1`.
    Ram {
        addr: u16,
        val: Val,
    },
    /// A store whose address is not known.
    Unknown,
    /// A store to an address that depends on the caller.
    Param,
}

/// An address an instruction names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Addr {
    Abs(u16),
    Stack(i16),
    Sym(Sym),
    Unknown,
}

fn address(st: &State, op: Operand, ins: &Instruction) -> Addr {
    let w = match op {
        Operand::Indirect(p) => st.word(p),
        Operand::HlInc | Operand::HlDec => st.word(Reg16::Hl),
        Operand::IndirectImm16 => match ins.immediate {
            Immediate::U16(a) => Word::Const(a),
            _ => Word::Other,
        },
        Operand::IndirectC => match st.regs[C] {
            Val::Const(c) => Word::Const(0xFF00 | u16::from(c)),
            _ => Word::Other,
        },
        Operand::HighImm8 => match ins.immediate {
            Immediate::U8(a) => Word::Const(0xFF00 | u16::from(a)),
            _ => Word::Other,
        },
        _ => Word::Other,
    };
    match as_sym(w) {
        Word::Const(a) => Addr::Abs(a),
        Word::Sym(s) if s.base == Base::Sp && s.step == 0 => Addr::Stack(s.off as i16),
        Word::Sym(s) => Addr::Sym(s),
        _ => Addr::Unknown,
    }
}

pub(crate) fn is_memory(op: Operand) -> bool {
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
    let w = add_word(st.word(Reg16::Hl), Word::Const(delta));
    st.set_word(Reg16::Hl, w);
}

/// The environment one step reads: the mapper and the image.
pub(crate) struct Ctx<'a> {
    pub model: &'a BankModel,
    pub image: &'a Image<'a>,
}

impl Ctx<'_> {
    fn rom_byte(&self, bank: u16, addr: u16) -> Option<u8> {
        let loc = if addr < 0x4000 {
            Loc::fixed(addr)
        } else {
            Loc { bank, addr }
        };
        self.image.fetch(loc).and_then(|b| b.first().copied())
    }

    /// The byte at `addr` in the current mapping.
    fn load(&self, st: &State, a: Addr) -> Val {
        match a {
            Addr::Stack(off) => st.slot(off),
            Addr::Abs(x) if x < 0x8000 => self.load_rom(st, Sym::at(Base::Abs, x)),
            Addr::Abs(x) => st.cell(x).unwrap_or(Val::At(Sym::at(Base::Abs, x))),
            Addr::Sym(s) if s.base == Base::Abs && s.off < 0x8000 => self.load_rom(st, s),
            Addr::Sym(s) if s.base == Base::Abs => Val::Top,
            Addr::Sym(s) => Val::At(s),
            Addr::Unknown => Val::Top,
        }
    }

    fn load_rom(&self, st: &State, s: Sym) -> Val {
        let bank = if s.off < 0x4000 {
            Some(0)
        } else {
            match st.window_bank(self.model) {
                WindowBank::Known(b) => Some(b),
                WindowBank::Entry => Some(ENTRY_BANK),
                _ => None,
            }
        };
        let Some(bank) = bank else { return Val::Top };
        if s.step == 0 && bank != ENTRY_BANK {
            return self.rom_byte(bank, s.off).map_or(Val::Top, Val::Const);
        }
        Val::At(Sym {
            base: Base::Rom(bank),
            ..s
        })
    }
}

fn operand_regs(op: Operand, out: &mut Vec<usize>) {
    match op {
        Operand::R8(r) => out.push(reg_index(r)),
        Operand::R16(p) | Operand::Indirect(p) => {
            if let Some((h, l)) = pair_regs(p) {
                out.extend([h, l]);
            } else if p == Reg16::Af {
                out.push(A);
            }
        }
        Operand::HlInc | Operand::HlDec => out.extend([H, L]),
        Operand::IndirectC => out.push(C),
        _ => {}
    }
}

/// Apply `ins` (not a branch) to `st`, pushing events. A register that
/// becomes unknown from inputs that depend on the caller is marked so.
pub(crate) fn step(cx: &Ctx<'_>, st: &mut State, ins: &Instruction, events: &mut Vec<Write>) {
    let before = *st;
    step_inner(cx, st, ins, events);
    let mut inputs = Vec::new();
    if matches!(
        ins.opcode.mnemonic,
        Mnemonic::Add
            | Mnemonic::Adc
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
            | Mnemonic::Cpl
    ) {
        inputs.push(A);
    }
    operand_regs(ins.opcode.dst, &mut inputs);
    operand_regs(ins.opcode.src, &mut inputs);
    if inputs.iter().any(|r| before.regs[*r].depends_on_entry()) {
        for r in 0..7 {
            if st.regs[r] == Val::Top && st.ver[r] != before.ver[r] {
                st.regs[r] = Val::Dep;
            }
        }
    }
}

fn step_inner(cx: &Ctx<'_>, st: &mut State, ins: &Instruction, events: &mut Vec<Write>) {
    let op = ins.opcode;
    let c_before = st.c;
    if !matches!(op.flags.c, FlagEffect::Keep) && op.mnemonic != Mnemonic::Cp {
        st.cp = None;
    }
    if !matches!(op.flags.z, FlagEffect::Keep) {
        st.zcell = None;
        st.zword = None;
    }
    let carry = st.carry;
    if !matches!(op.flags.c, FlagEffect::Keep) {
        st.carry = None;
    }
    match op.mnemonic {
        Mnemonic::Ld => step_ld(cx, st, ins, events),
        Mnemonic::Push => {
            if let Operand::R16(p) = op.src {
                let (h, l) = match pair_regs(p) {
                    Some((h, l)) => (st.regs[h], st.regs[l]),
                    None => (st.regs[A], Val::Top),
                };
                st.push(h, l);
            }
        }
        Mnemonic::Pop => {
            let v = st.pop();
            if let Operand::R16(p) = op.dst {
                let (h, l) = v.unwrap_or((Val::Top, Val::Top));
                match p {
                    Reg16::Af => {
                        st.set(A, h);
                        st.z = None;
                        st.c = None;
                    }
                    _ => {
                        if let Some((rh, rl)) = pair_regs(p) {
                            st.set(rh, h);
                            st.set(rl, l);
                        }
                    }
                }
            }
        }
        Mnemonic::Inc | Mnemonic::Dec => step_incdec(cx, st, ins, events),
        Mnemonic::Add if op.dst == Operand::R16(Reg16::Hl) => {
            let Operand::R16(p) = op.src else { return };
            let rhs = match p {
                Reg16::Sp => match st.sp {
                    Some(sp) => Word::Sym(Sym::at(Base::Sp, sp as u16)),
                    None => Word::Other,
                },
                _ => index_word(st, p),
            };
            let lhs = if p == Reg16::Hl {
                index_word(st, Reg16::Hl)
            } else {
                hl_word(st)
            };
            let w = add_word(lhs, rhs);
            st.set_word(Reg16::Hl, w);
            st.c = None;
        }
        Mnemonic::Add if op.dst == Operand::R16(Reg16::Sp) => {
            if let Immediate::I8(e) = ins.immediate {
                st.sp = st.sp.map(|s| s + i16::from(e));
            } else {
                st.sp = None;
            }
            st.z = Some(false);
            st.c = None;
        }
        Mnemonic::Add
        | Mnemonic::Adc
        | Mnemonic::Sub
        | Mnemonic::Sbc
        | Mnemonic::And
        | Mnemonic::Xor
        | Mnemonic::Or
        | Mnemonic::Cp => step_alu(cx, st, ins, c_before, carry),
        Mnemonic::Rlca | Mnemonic::Rrca | Mnemonic::Rla | Mnemonic::Rra => {
            let a = st.regs[A];
            let r = rotate(op.mnemonic, a, c_before);
            st.set(A, r.0);
            st.c = r.1;
            st.z = Some(false);
        }
        Mnemonic::Cpl => {
            let v = st.regs[A].konst().map_or(Val::Top, |a| Val::Const(!a));
            st.set(A, v);
        }
        Mnemonic::Daa => {
            st.set(A, Val::Top);
            st.z = None;
            st.c = None;
        }
        Mnemonic::Scf => st.c = Some(true),
        Mnemonic::Ccf => st.c = st.c.map(|c| !c),
        Mnemonic::Rlc
        | Mnemonic::Rrc
        | Mnemonic::Rl
        | Mnemonic::Rr
        | Mnemonic::Sla
        | Mnemonic::Sra
        | Mnemonic::Swap
        | Mnemonic::Srl => step_cb_shift(cx, st, ins, events, c_before),
        Mnemonic::Bit => {
            let v = cb_operand(cx, st, op.src, ins);
            st.z = match (op.dst, v) {
                (Operand::Bit(n), Val::Const(x)) => Some(x & (1 << n) == 0),
                _ => None,
            };
        }
        Mnemonic::Res | Mnemonic::Set => {
            let v = cb_operand(cx, st, op.src, ins);
            let r = match (op.dst, v) {
                (Operand::Bit(n), Val::Const(x)) if op.mnemonic == Mnemonic::Res => {
                    Val::Const(x & !(1 << n))
                }
                (Operand::Bit(n), Val::Const(x)) => Val::Const(x | (1 << n)),
                _ => Val::Top,
            };
            write_operand(cx, st, op.src, ins, r, events);
        }
        _ => {}
    }
}

fn hl_word(st: &State) -> Word {
    st.word(Reg16::Hl)
}

/// A pair as an addend: a constant, a symbolic value, or an index when the
/// high byte is zero and the low byte is not known.
fn index_word(st: &State, p: Reg16) -> Word {
    let Some((h, l)) = pair_regs(p) else {
        return Word::Other;
    };
    let w = st.word(p);
    if !matches!(w, Word::Other | Word::EntryPair(..) | Word::Load(_)) {
        return w;
    }
    if st.regs[h] != Val::Const(0) {
        return Word::Other;
    }
    let (mul, count) = match st.regs[l] {
        Val::Ix(m, n) => (m, n),
        Val::Const(_) => return w,
        _ => (1, st.bound[l]),
    };
    Word::Sym(Sym {
        base: Base::Abs,
        off: 0,
        step: mul,
        count,
        ix: (l as u16) << 12 | (st.ver[l] & 0x0FFF),
    })
}

fn as_sym(w: Word) -> Word {
    match w {
        Word::EntryPair(h, l) => Word::Sym(Sym::at(Base::Pair(h, l), 0)),
        w => w,
    }
}

pub(crate) fn add_word(a: Word, b: Word) -> Word {
    match (as_sym(a), as_sym(b)) {
        (Word::Const(x), Word::Const(y)) => Word::Const(x.wrapping_add(y)),
        (Word::Sym(s), Word::Const(y)) | (Word::Const(y), Word::Sym(s)) => Word::Sym(s.plus(y)),
        (Word::Sym(s), Word::Sym(t)) => {
            let (base, plain, ix) = match (s.step, t.step) {
                (0, _) if t.base == Base::Abs => (s.base, s, t),
                (_, 0) if s.base == Base::Abs => (t.base, t, s),
                _ if s.ix == t.ix && s.count == t.count && s.base == Base::Abs => {
                    let step = s.step.checked_add(t.step);
                    return step.map_or(Word::Other, |step| {
                        Word::Sym(Sym {
                            base: t.base,
                            off: s.off.wrapping_add(t.off),
                            step,
                            ..t
                        })
                    });
                }
                _ if s.ix == t.ix && s.count == t.count && t.base == Base::Abs => {
                    let step = s.step.checked_add(t.step);
                    return step.map_or(Word::Other, |step| {
                        Word::Sym(Sym {
                            base: s.base,
                            off: s.off.wrapping_add(t.off),
                            step,
                            ..s
                        })
                    });
                }
                _ => return Word::Other,
            };
            if plain.step != 0 {
                return Word::Other;
            }
            Word::Sym(Sym {
                base,
                off: plain.off.wrapping_add(ix.off),
                step: ix.step,
                count: ix.count,
                ix: ix.ix,
            })
        }
        _ => Word::Other,
    }
}

fn step_incdec(cx: &Ctx<'_>, st: &mut State, ins: &Instruction, events: &mut Vec<Write>) {
    let op = ins.opcode;
    let inc = op.mnemonic == Mnemonic::Inc;
    match op.dst {
        Operand::R16(Reg16::Sp) => {
            st.sp = st.sp.map(|s| if inc { s + 1 } else { s - 1 });
        }
        Operand::R16(p) => {
            let d = if inc { 1 } else { 0xFFFF };
            let w = add_word(st.word(p), Word::Const(d));
            st.set_word(p, w);
        }
        dst => {
            let v = cb_operand(cx, st, dst, ins);
            if let (true, Val::HiNoCarry(s)) = (inc, v) {
                st.z = None;
                write_operand(cx, st, dst, ins, Val::Hi(s), events);
                return;
            }
            let r = match v {
                Val::Const(x) => {
                    let r = if inc {
                        x.wrapping_add(1)
                    } else {
                        x.wrapping_sub(1)
                    };
                    st.z = Some(r == 0);
                    Val::Const(r)
                }
                _ => {
                    st.z = None;
                    Val::Top
                }
            };
            write_operand(cx, st, dst, ins, r, events);
        }
    }
}

fn cb_operand(cx: &Ctx<'_>, st: &State, op: Operand, ins: &Instruction) -> Val {
    match op {
        Operand::R8(r) => st.regs[reg_index(r)],
        o if is_memory(o) => cx.load(st, address(st, o, ins)),
        _ => Val::Top,
    }
}

fn write_operand(
    cx: &Ctx<'_>,
    st: &mut State,
    op: Operand,
    ins: &Instruction,
    v: Val,
    events: &mut Vec<Write>,
) {
    match op {
        Operand::R8(r) => st.set(reg_index(r), v),
        o if is_memory(o) => store(cx, st, address(st, o, ins), v, events),
        _ => {}
    }
}

fn rotate(m: Mnemonic, v: Val, c: Option<bool>) -> (Val, Option<bool>) {
    let Val::Const(x) = v else {
        return (Val::Top, None);
    };
    match m {
        Mnemonic::Rlca | Mnemonic::Rlc => (Val::Const(x.rotate_left(1)), Some(x & 0x80 != 0)),
        Mnemonic::Rrca | Mnemonic::Rrc => (Val::Const(x.rotate_right(1)), Some(x & 1 != 0)),
        Mnemonic::Rla | Mnemonic::Rl => match c {
            Some(c) => (Val::Const(x << 1 | u8::from(c)), Some(x & 0x80 != 0)),
            None => (Val::Top, Some(x & 0x80 != 0)),
        },
        Mnemonic::Rra | Mnemonic::Rr => match c {
            Some(c) => (Val::Const(x >> 1 | u8::from(c) << 7), Some(x & 1 != 0)),
            None => (Val::Top, Some(x & 1 != 0)),
        },
        Mnemonic::Sla => (Val::Const(x << 1), Some(x & 0x80 != 0)),
        Mnemonic::Sra => (Val::Const(x >> 1 | (x & 0x80)), Some(x & 1 != 0)),
        Mnemonic::Srl => (Val::Const(x >> 1), Some(x & 1 != 0)),
        Mnemonic::Swap => (Val::Const(x.rotate_left(4)), Some(false)),
        _ => (Val::Top, None),
    }
}

fn step_cb_shift(
    cx: &Ctx<'_>,
    st: &mut State,
    ins: &Instruction,
    events: &mut Vec<Write>,
    c: Option<bool>,
) {
    let op = ins.opcode;
    let v = cb_operand(cx, st, op.dst, ins);
    let (r, carry) = match (op.mnemonic, v) {
        (Mnemonic::Sla, Val::Ix(m, n)) if m < 128 => (Val::Ix(m * 2, n), Some(false)),
        (Mnemonic::Sla, Val::Ix(..)) => (Val::Top, None),
        (Mnemonic::Sla, v) if !matches!(v, Val::Const(_)) => {
            let n = match op.dst {
                Operand::R8(r) => st.bound[reg_index(r)],
                _ => None,
            };
            (Val::Ix(2, n.filter(|n| *n <= 128)), None)
        }
        _ => rotate(op.mnemonic, v, c),
    };
    st.c = carry;
    st.z = r.konst().map(|x| x == 0);
    write_operand(cx, st, op.dst, ins, r, events);
}

/// `a + src` as the low byte of an indexed value: one side is an index
/// `i * m`, the other the low byte of a base whose high byte is in the
/// partner register. Returns the sum and the high byte before the carry.
fn indexed_low(st: &State, a: Val, src: Val, src_op: Operand) -> Option<(Sym, Val)> {
    let Operand::R8(r) = src_op else { return None };
    let r = reg_index(r);
    let (ix, ix_id, base_lo, base_reg) = match (a, src) {
        (Val::Ix(m, n), _) => ((m, n), (A as u16) << 12 | (st.ver[A] & 0x0FFF), src, r),
        _ => return None,
    };
    let hreg = match base_reg {
        L => H,
        E => D,
        C => B,
        _ => return None,
    };
    let hi = st.regs[hreg];
    let base = match (hi, base_lo) {
        (Val::Const(h), Val::Const(l)) => Sym::at(Base::Abs, u16::from_be_bytes([h, l])),
        (Val::Hi(s), Val::Lo(t)) if s == t && s.step == 0 => s,
        _ => return None,
    };
    let sum = Sym {
        step: ix.0,
        count: ix.1,
        ix: ix_id,
        ..base
    };
    Some((sum, hi))
}

fn step_alu(
    cx: &Ctx<'_>,
    st: &mut State,
    ins: &Instruction,
    c_before: Option<bool>,
    carry: Option<(Sym, Val)>,
) {
    let op = ins.opcode;
    let m = op.mnemonic;
    let src = match (op.src, ins.immediate) {
        (Operand::Imm8, Immediate::U8(v)) => Val::Const(v),
        (o, _) => cb_operand(cx, st, o, ins),
    };
    let a = st.regs[A];
    let same = op.src == Operand::R8(Reg8::A);
    if same && matches!(m, Mnemonic::Xor | Mnemonic::Sub) {
        st.set(A, Val::Const(0));
        st.z = Some(true);
        st.c = Some(false);
        return;
    }
    if same && matches!(m, Mnemonic::And | Mnemonic::Or) {
        st.z = a.konst().map(|x| x == 0);
        st.c = Some(false);
        if let Val::At(s) = a
            && s.base == Base::Abs
            && s.step == 0
            && s.off >= 0x8000
        {
            st.zcell = Some((st.ver[A], s.off));
        }
        return;
    }
    if same && m == Mnemonic::Cp {
        st.z = Some(true);
        st.c = Some(false);
        return;
    }
    if same && m == Mnemonic::Add {
        let bound = st.bound[A];
        let r = match a {
            Val::Const(x) => Val::Const(x.wrapping_add(x)),
            Val::Ix(mul, n) if mul < 128 => Val::Ix(mul * 2, n),
            Val::Ix(..) => Val::Top,
            _ => Val::Ix(2, bound.filter(|n| *n <= 128)),
        };
        st.c = a.konst().map(|x| x >= 0x80);
        st.z = r.konst().map(|x| x == 0);
        st.set(A, r);
        return;
    }
    if m == Mnemonic::Add
        && let Some((sum, hi)) = indexed_low(st, a, src, op.src)
    {
        st.set(A, Val::Lo(sum));
        st.z = None;
        st.c = None;
        st.carry = Some((sum, hi));
        return;
    }
    if m == Mnemonic::Adc
        && src == Val::Const(0)
        && let Some((sum, hi)) = carry
        && a == hi
    {
        st.set(A, Val::Hi(sum));
        st.z = None;
        st.c = None;
        return;
    }
    if m == Mnemonic::Or
        && let (Val::At(p), Val::At(q)) = (a, src)
        && p.base == Base::Abs
        && q.base == Base::Abs
        && p.step == 0
        && q.step == 0
        && p.off >= 0x8000
        && (p.off == q.off.wrapping_add(1) || q.off == p.off.wrapping_add(1))
    {
        st.set(A, Val::Top);
        st.z = None;
        st.c = Some(false);
        st.zword = Some((st.ver[A], p.off.min(q.off)));
        return;
    }
    let (Val::Const(x), Val::Const(y)) = (a, src) else {
        match m {
            Mnemonic::Cp => {
                st.z = None;
                st.c = None;
                st.cp = src.konst().map(|n| (st.ver[A], n));
            }
            Mnemonic::And => {
                let n = src.konst().map(|n| u16::from(n) + 1);
                let bound = match (st.bound[A], n) {
                    (Some(a), Some(b)) => Some(a.min(b)),
                    (a, b) => a.or(b),
                };
                st.set(A, Val::Top);
                st.bound[A] = bound;
                st.z = None;
                st.c = Some(false);
            }
            _ => {
                st.set(A, Val::Top);
                st.z = None;
                st.c = None;
            }
        }
        return;
    };
    let cin = c_before.map(u8::from);
    let (r, c) = match m {
        Mnemonic::Add => (x.wrapping_add(y), Some(u16::from(x) + u16::from(y) > 0xFF)),
        Mnemonic::Adc => match cin {
            Some(ci) => (
                x.wrapping_add(y).wrapping_add(ci),
                Some(u16::from(x) + u16::from(y) + u16::from(ci) > 0xFF),
            ),
            None => {
                st.set(A, Val::Top);
                st.z = None;
                st.c = None;
                return;
            }
        },
        Mnemonic::Sub | Mnemonic::Cp => (x.wrapping_sub(y), Some(x < y)),
        Mnemonic::Sbc => match cin {
            Some(ci) => (
                x.wrapping_sub(y).wrapping_sub(ci),
                Some(u16::from(x) < u16::from(y) + u16::from(ci)),
            ),
            None => {
                st.set(A, Val::Top);
                st.z = None;
                st.c = None;
                return;
            }
        },
        Mnemonic::And => (x & y, Some(false)),
        Mnemonic::Xor => (x ^ y, Some(false)),
        _ => (x | y, Some(false)),
    };
    st.z = Some(r == 0);
    st.c = c;
    if m != Mnemonic::Cp {
        st.set(A, Val::Const(r));
    }
}

fn store(cx: &Ctx<'_>, st: &mut State, a: Addr, v: Val, events: &mut Vec<Write>) {
    match a {
        Addr::Stack(off) => st.set_slot(off, v),
        Addr::Abs(x) if x < 0x8000 => {
            if let Some(reg) = cx.model.classify(x) {
                let b = match v {
                    Val::Const(c) => Bv::Known(cx.model.register_value(reg, c)),
                    Val::At(s) if s.base == Base::Abs && s.step == 0 && s.off >= 0x8000 => {
                        Bv::Cell(s.off)
                    }
                    v if v.depends_on_entry() => Bv::Param,
                    _ => Bv::Unknown,
                };
                match reg {
                    BankReg::Lo => st.lo = b,
                    BankReg::Hi => st.hi = b,
                }
                events.push(Write::Bank(BankWrite { reg, val: v }));
            }
        }
        Addr::Abs(x) => {
            events.push(Write::Ram { addr: x, val: v });
            st.set_cell(x, v);
        }
        Addr::Sym(s) if s.depends_on_entry() => {
            events.push(Write::Param);
            st.cells = [(0, Val::Top); CELLS];
            st.nonzero = None;
        }
        Addr::Sym(_) | Addr::Unknown => {
            events.push(Write::Unknown);
            st.cells = [(0, Val::Top); CELLS];
            st.nonzero = None;
        }
    }
}

fn step_ld(cx: &Ctx<'_>, st: &mut State, ins: &Instruction, events: &mut Vec<Write>) {
    let op = ins.opcode;
    let (dst, src) = (op.dst, op.src);
    if is_memory(dst) {
        let a = address(st, dst, ins);
        match (src, ins.immediate) {
            (Operand::R16(Reg16::Sp), _) => {
                if let Addr::Abs(x) = a
                    && x >= 0x8000
                {
                    events.push(Write::Ram {
                        addr: x,
                        val: Val::Top,
                    });
                    events.push(Write::Ram {
                        addr: x.wrapping_add(1),
                        val: Val::Top,
                    });
                } else {
                    events.push(Write::Unknown);
                }
            }
            (Operand::R8(r), _) => store(cx, st, a, st.regs[reg_index(r)], events),
            (Operand::Imm8, Immediate::U8(v)) => store(cx, st, a, Val::Const(v), events),
            _ => store(cx, st, a, Val::Top, events),
        }
        post_adjust(st, dst);
        return;
    }
    if is_memory(src) {
        let v = cx.load(st, address(st, src, ins));
        if let Operand::R8(r) = dst {
            st.set(reg_index(r), v);
        }
        post_adjust(st, src);
        return;
    }
    match (dst, src, ins.immediate) {
        (Operand::R8(d), Operand::Imm8, Immediate::U8(v)) => st.set(reg_index(d), Val::Const(v)),
        (Operand::R8(d), Operand::R8(s), _) => {
            let (d, s) = (reg_index(d), reg_index(s));
            let (v, b) = (st.regs[s], st.bound[s]);
            st.set(d, v);
            st.bound[d] = b;
            if !matches!(v, Val::Const(_)) {
                st.ver[d] = st.ver[s] | 0x0800;
            }
        }
        (Operand::R16(p), Operand::Imm16, Immediate::U16(v)) if p != Reg16::Sp => {
            st.set_word(p, Word::Const(v));
        }
        (Operand::R16(Reg16::Hl), Operand::SpPlusSigned8, Immediate::I8(e)) => {
            let w = match st.sp {
                Some(sp) => Word::Sym(Sym::at(Base::Sp, (sp + i16::from(e)) as u16)),
                None => Word::Other,
            };
            st.set_word(Reg16::Hl, w);
            st.z = Some(false);
            st.c = None;
        }
        (Operand::R16(Reg16::Sp), Operand::R16(Reg16::Hl), _) => {
            st.sp = match st.word(Reg16::Hl) {
                Word::Sym(s) if s.base == Base::Sp && s.step == 0 => Some(s.off as i16),
                _ => None,
            };
        }
        (Operand::R16(Reg16::Sp), Operand::Imm16, _) => {
            st.sp = Some(STACK_HI - 2);
            st.stack = [Val::Top; SLOTS];
        }
        (Operand::R16(Reg16::Sp), _, _) => st.sp = None,
        (Operand::R16(p), _, _) => st.set_word(p, Word::Other),
        _ => {}
    }
}

//! The lift-time state every MIPS profile shares: pending register writes,
//! the delay-slot and branch state, guards and the static exception, and
//! the memory I/O path. A profile's `St` holds a [`Core`] and adds its own
//! registers, address translation, coprocessors and effects.
//!
//! - Register writes are pending until [`Core::finish`]; a write under a
//!   guard becomes a select of the new and the old value.
//! - A run-time exception is a guard with code [`EXC_GUARD`] plus the
//!   Cause.ExcCode, plus [`IN_DELAY`] when the delay slot raised it; one
//!   whose condition is known at lift time is also kept as the static
//!   exception.
//! - Loads and stores are I/O accesses of kind `Any` in the profile's byte
//!   order: the guest's I/O map decides at run time which physical
//!   addresses are plain memory. An access the instruction makes only
//!   under a condition reads and writes back plain memory unconditionally,
//!   selecting the old value; with a `plain_below` limit set, such an
//!   access to an address at or above it leaves first through the guard
//!   the profile names, whose slow path makes it exactly.
//! - A condition that an earlier guard of the block already proved (the
//!   address error check before a load) is not a condition.

use relift_ir::{BinOp, Builder, CmpOp, Endian, IoKind, Op, Reg, UnOp, Value, Width};

/// Where a branch goes when it is taken.
#[derive(Clone, Copy, Debug)]
pub enum Target {
    /// A known address.
    Const(u64),
    /// An address computed in the block.
    Dynamic(Value),
}

/// The kind of a memory access, for the TLB exception it raises.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Access {
    /// A load.
    LoadData,
    /// A store.
    StoreData,
}

/// The model's exceptions (the overlays' `Exception` enum).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Exc {
    /// Interrupt.
    Interrupt,
    /// TLB modification.
    TLBMod,
    /// TLB miss or invalid on a load or fetch.
    TLBL,
    /// TLB miss or invalid on a store.
    TLBS,
    /// Address error on a load or fetch.
    AdEL,
    /// Address error on a store.
    AdES,
    /// System call.
    Sys,
    /// Breakpoint.
    Bp,
    /// Reserved instruction.
    ResI,
    /// Coprocessor unusable.
    CpU,
    /// Integer overflow.
    Ov,
    /// Trap.
    Tr,
    /// The model's CP2 exception, not raised on the VR4300.
    C2E,
    /// The model's CP2 trap, not raised on the VR4300.
    C2Trap,
    /// 64-bit TLB refill on a load.
    XTLBRefillL,
    /// 64-bit TLB refill on a store.
    XTLBRefillS,
    /// 64-bit TLB invalid on a load.
    XTLBInvL,
    /// 64-bit TLB invalid on a store.
    XTLBInvS,
    /// Machine check.
    MCheck,
    /// Floating-point exception.
    FPE,
}

impl Exc {
    /// The Cause.ExcCode number.
    pub fn code(self) -> u32 {
        match self {
            Exc::Interrupt => 0,
            Exc::TLBMod => 1,
            Exc::TLBL | Exc::XTLBRefillL | Exc::XTLBInvL => 2,
            Exc::TLBS | Exc::XTLBRefillS | Exc::XTLBInvS => 3,
            Exc::AdEL => 4,
            Exc::AdES => 5,
            Exc::Sys => 8,
            Exc::Bp => 9,
            Exc::ResI => 10,
            Exc::CpU => 11,
            Exc::Ov => 12,
            Exc::Tr => 13,
            Exc::FPE => 15,
            Exc::C2E | Exc::C2Trap => 18,
            Exc::MCheck => 24,
        }
    }
}

/// The guard code of an exception is this plus its ExcCode.
pub const EXC_GUARD: u32 = 0x1000;
/// Added to an exception's or a slow path's guard code when the delay slot
/// left.
pub const IN_DELAY: u32 = 0x100;

/// `x` of width `from` sign-extended to `to` bits.
pub fn sext(x: u128, from: u32, to: u32) -> u128 {
    let s = 128 - from;
    let v = (((x << s) as i128) >> s) as u128;
    if to >= 128 {
        v
    } else {
        v & ((1u128 << to) - 1)
    }
}

/// `x` of width `w` as a signed integer.
pub fn sint(x: u128, w: u32) -> i128 {
    let s = 128 - w;
    ((x << s) as i128) >> s
}

/// The shared lift-time state of one block.
pub(crate) struct Core {
    plain_below: Option<u64>,
    endian: Endian,
    proved: Vec<Value>,
    plain: Vec<Value>,
    pc: u64,
    in_delay: bool,
    pending: Vec<(Reg, Value)>,
    slot_guard: Option<Value>,
    branch: Option<(Target, Option<Value>)>,
    annul: Option<Option<Value>>,
    has_slot: bool,
    jump: Option<Value>,
    exception: Option<Exc>,
    cycles: u32,
}

impl Core {
    /// The state for the instruction at `pc`, with memory in `endian` order
    /// and plain below `plain_below` (`None`: everywhere).
    pub(crate) fn new(pc: u64, plain_below: Option<u64>, endian: Endian) -> Core {
        Core {
            plain_below,
            endian,
            proved: Vec::new(),
            plain: Vec::new(),
            pc,
            in_delay: false,
            pending: Vec::new(),
            slot_guard: None,
            branch: None,
            annul: None,
            has_slot: false,
            jump: None,
            exception: None,
            cycles: 0,
        }
    }

    /// The instruction takes `n` cycles beyond its issue cycle under `g`:
    /// a tick under a condition of the template at once, or cycles the
    /// block driver adds to the instruction's own tick.
    pub(crate) fn cycles(&mut self, b: &mut Builder, n: u32, g: Option<Value>) {
        match self.guard(b, g) {
            Some(c) if Some(c) != self.slot_guard => b.tick_if(c, n),
            _ => self.cycles += n,
        }
    }

    /// The cycles [`Core::cycles`] left for the instruction lifted last.
    pub(crate) fn take_cycles(&mut self) -> u32 {
        std::mem::take(&mut self.cycles)
    }

    /// Ticks the instruction lifted last: its issue cycle and the cycles
    /// [`Core::cycles`] left. An annulled delay slot takes none.
    pub(crate) fn tick(&mut self, b: &mut Builder) {
        let n = 1 + self.take_cycles();
        match self.slot_guard {
            Some(c) => b.tick_if(c, n),
            None => b.tick(n),
        }
    }

    /// Moves on to the delay slot at `pc + 4` of the branch lifted so far:
    /// its effects are guarded by "not annulled".
    pub(crate) fn enter_delay_slot(&mut self, b: &mut Builder) {
        self.in_delay = true;
        self.pc += 4;
        self.slot_guard = match self.annul {
            Some(Some(g)) => Some(b.not(g)),
            _ => None,
        };
    }

    /// Whether the instruction lifted so far has a delay slot.
    pub(crate) fn has_delay_slot(&self) -> bool {
        self.has_slot
    }

    /// The branch target and the condition under which it is taken (`None`:
    /// always).
    pub(crate) fn branch_target(&self) -> Option<(Target, Option<Value>)> {
        self.branch
    }

    /// Whether a likely branch always annuls its slot.
    pub(crate) fn annuls_always(&self) -> bool {
        matches!(self.annul, Some(None))
    }

    /// The target of a jump without a delay slot.
    pub(crate) fn jump(&self) -> Option<Value> {
        self.jump
    }

    /// The exception raised whenever the block runs, if any.
    pub(crate) fn exception_static(&self) -> Option<Exc> {
        self.exception
    }

    /// Whether `v` is 1 whenever the block gets past its earlier guards.
    fn holds(&self, b: &Builder, v: Value) -> bool {
        match b.op(v) {
            Some(Op::Un(UnOp::Not, x)) => self.proved.contains(x),
            Some(Op::Bin(BinOp::And, x, y)) => {
                let (x, y) = (*x, *y);
                self.holds(b, x) && self.holds(b, y)
            }
            _ => false,
        }
    }

    /// `g` without the parts the earlier guards proved.
    fn simplify(&self, b: &Builder, g: Option<Value>) -> Option<Value> {
        let v = g?;
        if self.holds(b, v) {
            return None;
        }
        match b.op(v) {
            Some(Op::Bin(BinOp::And, x, y)) if self.holds(b, *x) => Some(*y),
            Some(Op::Bin(BinOp::And, x, y)) if self.holds(b, *y) => Some(*x),
            _ => Some(v),
        }
    }

    /// The template guard `g` and the slot guard together.
    pub(crate) fn guard(&self, b: &mut Builder, g: Option<Value>) -> Option<Value> {
        let g = self.simplify(b, g);
        match (g, self.slot_guard) {
            (Some(x), Some(y)) => Some(b.and(x, y)),
            (x, None) => x,
            (None, y) => y,
        }
    }

    /// A guard exit with `code` when `bad` holds under the guard `g`.
    pub(crate) fn exit_if(&mut self, b: &mut Builder, bad: Value, g: Option<Value>, code: u32) {
        let c = match self.guard(b, g) {
            Some(g) => b.and(g, bad),
            None => bad,
        };
        b.guard(c, code);
        if self.simplify(b, g).is_none() {
            self.proved.push(bad);
        }
    }

    /// The guard code of `e` raised by this instruction.
    pub(crate) fn exc_code(&self, e: Exc) -> u32 {
        EXC_GUARD + e.code() + self.delay_flag()
    }

    /// [`IN_DELAY`] in a delay slot, else 0.
    pub(crate) fn delay_flag(&self) -> u32 {
        if self.in_delay { IN_DELAY } else { 0 }
    }

    /// Register `r`, with this block's pending write.
    pub(crate) fn get(&mut self, b: &mut Builder, r: Reg) -> Value {
        match self.pending.iter().find(|(p, _)| *p == r) {
            Some((_, v)) => *v,
            None => b.get(r),
        }
    }

    /// Writes register `r` under `g`.
    pub(crate) fn set(&mut self, b: &mut Builder, r: Reg, v: Value, g: Option<Value>) {
        let v = match self.guard(b, g) {
            Some(g) => {
                let old = self.get(b, r);
                b.select(g, v, old)
            }
            None => v,
        };
        match self.pending.iter_mut().find(|(p, _)| *p == r) {
            Some(p) => p.1 = v,
            None => self.pending.push((r, v)),
        }
    }

    /// The instruction's address.
    pub(crate) fn pc(&self) -> u128 {
        u128::from(self.pc)
    }

    /// Whether the instruction is a delay slot.
    pub(crate) fn in_delay(&self) -> bool {
        self.in_delay
    }

    /// Raises `e` under `g`.
    pub(crate) fn exception(&mut self, b: &mut Builder, e: Exc, g: Option<Value>) {
        let code = self.exc_code(e);
        match self.guard(b, g) {
            None => {
                if self.exception.is_none() {
                    self.exception = Some(e);
                }
                let t = b.konst(Width::W1, 1);
                b.guard(t, code);
            }
            Some(c) => {
                b.guard(c, code);
                if let Some(g) = g
                    && self.simplify(b, Some(g)) == Some(g)
                {
                    self.proved.push(g);
                }
            }
        }
    }

    /// A branch to `t` under `g`, after the delay slot.
    pub(crate) fn branch(&mut self, b: &mut Builder, t: Target, g: Option<Value>) {
        let g = self.guard(b, g);
        self.branch = Some((t, g));
        self.has_slot = true;
    }

    /// A likely branch not taken under `g`: its slot is annulled.
    pub(crate) fn annul(&mut self, b: &mut Builder, g: Option<Value>) {
        let g = self.guard(b, g);
        self.annul = Some(g);
        self.has_slot = true;
    }

    /// A branch not taken: the slot runs, then the next instruction.
    pub(crate) fn delay_slot(&mut self) {
        self.has_slot = true;
    }

    /// A jump to `v` without a delay slot under `g`.
    pub(crate) fn jump_now(&mut self, b: &mut Builder, v: Value, g: Option<Value>) {
        let v = match (self.guard(b, g), self.jump) {
            (Some(c), Some(old)) => b.select(c, v, old),
            _ => v,
        };
        self.jump = Some(v);
    }

    /// Leaves through the guard of `code` when the access at `pa` is
    /// conditional (`g`) and may not be plain memory; once per address, so
    /// the guard of an instruction's first access covers the rest.
    fn device_exit(&mut self, b: &mut Builder, pa: Value, g: Option<Value>, code: Exc) {
        let (Some(_), Some(limit)) = (g, self.plain_below) else {
            return;
        };
        if self.plain.contains(&pa) {
            return;
        }
        self.plain.push(pa);
        let k = b.konst(Width::W64, u128::from(limit));
        let dev = b.cmp(CmpOp::Ule, k, pa);
        let code = self.exc_code(code);
        b.guard(dev, code);
    }

    /// A load of `n` bytes at `pa` under `g`; `dev` is the exception whose
    /// guard takes a conditional device access. A load of plain memory has
    /// no effect the block must order, so one under a guard runs
    /// unconditionally.
    pub(crate) fn load(
        &mut self,
        b: &mut Builder,
        pa: Value,
        n: u32,
        dev: Exc,
        g: Option<Value>,
    ) -> Value {
        let g = self.guard(b, g);
        self.device_exit(b, pa, g, dev);
        b.io_load(pa, Width::new(8 * n), self.endian, IoKind::Any)
    }

    /// A store of `n` bytes of `v` at `pa` under `g`; `dev` as for
    /// [`Core::load`].
    pub(crate) fn store(
        &mut self,
        b: &mut Builder,
        pa: Value,
        n: u32,
        v: Value,
        dev: Exc,
        g: Option<Value>,
    ) {
        let g = self.guard(b, g);
        self.device_exit(b, pa, g, dev);
        let v = match g {
            Some(c) => {
                let old = b.io_load(pa, Width::new(8 * n), self.endian, IoKind::Any);
                b.select(c, v, old)
            }
            None => v,
        };
        b.io_store(pa, v, self.endian, IoKind::Any);
    }

    /// The value 1 when the instruction is not annulled under `g`.
    pub(crate) fn runs(&mut self, b: &mut Builder, g: Option<Value>) -> Value {
        match self.guard(b, g) {
            Some(c) => c,
            None => b.konst(Width::W1, 1),
        }
    }

    /// Writes the pending registers, in slot order.
    pub(crate) fn finish(&mut self, b: &mut Builder) {
        let mut p = std::mem::take(&mut self.pending);
        p.sort_by_key(|(r, _)| r.index());
        for (r, v) in p {
            b.set(r, v);
        }
    }
}

use crate::block::check_op;
use crate::{
    BinOp, Block, CmpOp, Endian, Exit, ExtKind, Flow, FpOp, Inst, IoKind, Next, Op, Order, Reg,
    RegFile, UnOp, Value, Width,
};

/// Builds a [`Block`] one operation at a time, checking widths as it goes.
///
/// Every method panics on a width error: a frontend that builds an ill-typed
/// block has a bug, and the panic names the operation.
#[derive(Clone, Debug)]
pub struct Builder {
    regs: RegFile,
    addr_width: Width,
    pc: u64,
    insts: Vec<Inst>,
    widths: Vec<Option<Width>>,
    bank: Option<u64>,
}

impl Builder {
    /// A builder for a block at `pc` over `regs`, with `addr_width`-bit addresses.
    pub fn new(regs: RegFile, addr_width: Width, pc: u64) -> Builder {
        Builder {
            regs,
            addr_width,
            pc,
            insts: Vec::new(),
            widths: Vec::new(),
            bank: None,
        }
    }

    /// Marks the block as lifted from bank `bank` of its address; see
    /// [`Block::bank`].
    pub fn set_bank(&mut self, bank: u64) {
        self.bank = Some(bank);
    }

    /// The bank, `None` unless [`Builder::set_bank`] set one.
    pub fn bank(&self) -> Option<u64> {
        self.bank
    }

    /// The register file.
    pub fn regs(&self) -> RegFile {
        self.regs
    }

    /// The guest address of the block.
    pub fn pc(&self) -> u64 {
        self.pc
    }

    /// The address width.
    pub fn addr_width(&self) -> Width {
        self.addr_width
    }

    /// The width of `v`.
    ///
    /// # Panics
    ///
    /// Panics if `v` is not a value of this block or has no result.
    pub fn width(&self, v: Value) -> Width {
        match self.widths.get(v.index()) {
            Some(Some(w)) => *w,
            _ => panic!("value {} has no width", v.0),
        }
    }

    /// Appends `op` with result width `width` and returns its value.
    ///
    /// # Panics
    ///
    /// Panics if the operation is ill-typed.
    pub fn push(&mut self, op: Op, width: Option<Width>) -> Value {
        if let Err(e) = check_op(&op, width, &self.widths, &self.regs, self.addr_width) {
            panic!("ill-typed {op:?}: {e}");
        }
        let v = Value(self.insts.len() as u32);
        self.insts.push(Inst { op, width });
        self.widths.push(width);
        v
    }

    fn val(&mut self, op: Op, width: Width) -> Value {
        self.push(op, Some(width))
    }

    /// A constant, truncated to `width`.
    pub fn konst(&mut self, width: Width, value: u128) -> Value {
        self.val(Op::Const(value & width.mask()), width)
    }

    /// Reads a slot's entry value.
    pub fn get(&mut self, reg: Reg) -> Value {
        let w = self.regs.width(reg);
        self.val(Op::Get(reg), w)
    }

    /// Writes a slot when the block exits.
    pub fn set(&mut self, reg: Reg, value: Value) {
        self.push(Op::Set(reg, value), None);
    }

    /// A one-operand operation.
    pub fn un(&mut self, op: UnOp, a: Value) -> Value {
        let w = self.width(a);
        self.val(Op::Un(op, a), w)
    }

    /// A two-operand operation.
    pub fn bin(&mut self, op: BinOp, a: Value, b: Value) -> Value {
        let w = self.width(a);
        self.val(Op::Bin(op, a, b), w)
    }

    /// A comparison.
    pub fn cmp(&mut self, op: CmpOp, a: Value, b: Value) -> Value {
        self.val(Op::Cmp(op, a, b), Width::W1)
    }

    /// `a + b`.
    pub fn add(&mut self, a: Value, b: Value) -> Value {
        self.bin(BinOp::Add, a, b)
    }

    /// `a - b`.
    pub fn sub(&mut self, a: Value, b: Value) -> Value {
        self.bin(BinOp::Sub, a, b)
    }

    /// `a & b`.
    pub fn and(&mut self, a: Value, b: Value) -> Value {
        self.bin(BinOp::And, a, b)
    }

    /// `a | b`.
    pub fn or(&mut self, a: Value, b: Value) -> Value {
        self.bin(BinOp::Or, a, b)
    }

    /// `a ^ b`.
    pub fn xor(&mut self, a: Value, b: Value) -> Value {
        self.bin(BinOp::Xor, a, b)
    }

    /// `!a`.
    pub fn not(&mut self, a: Value) -> Value {
        self.un(UnOp::Not, a)
    }

    /// `cond ? a : b`.
    pub fn select(&mut self, cond: Value, a: Value, b: Value) -> Value {
        let w = self.width(a);
        self.val(Op::Select(cond, a, b), w)
    }

    /// Zero-extends to `width`; returns `a` when it already has that width.
    pub fn zext(&mut self, a: Value, width: Width) -> Value {
        if self.width(a) == width {
            return a;
        }
        self.val(Op::Ext(ExtKind::Zero, a), width)
    }

    /// Sign-extends to `width`; returns `a` when it already has that width.
    pub fn sext(&mut self, a: Value, width: Width) -> Value {
        if self.width(a) == width {
            return a;
        }
        self.val(Op::Ext(ExtKind::Sign, a), width)
    }

    /// `width` bits of `a` starting at bit `lo`; returns `a` when that is all of it.
    pub fn extract(&mut self, a: Value, lo: u32, width: Width) -> Value {
        if lo == 0 && self.width(a) == width {
            return a;
        }
        let lo = u8::try_from(lo).unwrap_or(u8::MAX);
        self.val(Op::Extract(a, lo), width)
    }

    /// The low `width` bits of `a`.
    pub fn trunc(&mut self, a: Value, width: Width) -> Value {
        self.extract(a, 0, width)
    }

    /// Bit `bit` of `a`, as one bit.
    pub fn bit(&mut self, a: Value, bit: u32) -> Value {
        self.extract(a, bit, Width::W1)
    }

    /// `hi` above `lo`.
    pub fn concat(&mut self, hi: Value, lo: Value) -> Value {
        let w = Width::new(self.width(hi).bits() + self.width(lo).bits());
        self.val(Op::Concat(hi, lo), w)
    }

    /// Loads `width` bits from `addr`.
    pub fn load(&mut self, addr: Value, width: Width, endian: Endian, order: Order) -> Value {
        self.val(
            Op::Load {
                addr,
                endian,
                order,
            },
            width,
        )
    }

    /// Stores `value` at `addr`.
    pub fn store(&mut self, addr: Value, value: Value, endian: Endian, order: Order) {
        self.push(
            Op::Store {
                addr,
                value,
                endian,
                order,
            },
            None,
        );
    }

    /// Loads `width` bits from `addr` and arms the exclusive monitor.
    pub fn load_exclusive(
        &mut self,
        addr: Value,
        width: Width,
        endian: Endian,
        order: Order,
    ) -> Value {
        self.val(
            Op::LoadExclusive {
                addr,
                endian,
                order,
            },
            width,
        )
    }

    /// Stores `value` at `addr` if the monitor allows it; gives 1 when it stored.
    pub fn store_exclusive(
        &mut self,
        addr: Value,
        value: Value,
        endian: Endian,
        order: Order,
    ) -> Value {
        self.val(
            Op::StoreExclusive {
                addr,
                value,
                endian,
                order,
            },
            Width::W1,
        )
    }

    /// Leaves the block for slow path `code` when `cond` (one bit) is 1;
    /// see [`Op::Guard`].
    pub fn guard(&mut self, cond: Value, code: u32) {
        self.push(Op::Guard(cond, code), None);
    }

    /// Clears the exclusive monitor.
    pub fn clear_exclusive(&mut self) {
        self.push(Op::ClearExclusive, None);
    }

    /// A fence.
    pub fn fence(&mut self, order: Order) {
        self.push(Op::Fence(order), None);
    }

    /// A floating-point operation of `width` (32 or 64).
    pub fn fp(&mut self, op: FpOp, args: &[Value], width: Width) -> Value {
        self.val(Op::Fp(op, args.to_vec()), width)
    }

    /// Advances the guest clock by `cycles`; see [`Op::Tick`].
    pub fn tick(&mut self, cycles: u32) {
        self.push(Op::Tick(cycles), None);
    }

    /// Advances the guest clock by `cycles` when `cond` (one bit) is 1; see
    /// [`Op::TickIf`].
    pub fn tick_if(&mut self, cond: Value, cycles: u32) {
        self.push(Op::TickIf(cond, cycles), None);
    }

    /// Loads `width` bits from `addr`, where a device register may live; see
    /// [`Op::IoLoad`].
    pub fn io_load(&mut self, addr: Value, width: Width, endian: Endian, kind: IoKind) -> Value {
        self.val(Op::IoLoad { addr, endian, kind }, width)
    }

    /// Stores `value` at `addr`, where a device register may live; see
    /// [`Op::IoStore`].
    pub fn io_store(&mut self, addr: Value, value: Value, endian: Endian, kind: IoKind) {
        self.push(
            Op::IoStore {
                addr,
                value,
                endian,
                kind,
            },
            None,
        );
    }

    /// Makes the exit a sync point; see [`Op::Sync`]. Push it last.
    pub fn sync(&mut self) {
        self.push(Op::Sync, None);
    }

    /// Calls a named intrinsic.
    pub fn intrinsic(&mut self, name: &'static str, args: &[Value], width: Option<Width>) -> Value {
        self.push(Op::Intrinsic(name, args.to_vec()), width)
    }

    /// `a + b + carry_in` with its carry out and signed overflow, all three
    /// computed with ordinary operations. `carry_in` is one bit.
    ///
    /// Returns `(sum, carry, overflow)`; carry and overflow are one bit.
    pub fn add_with_carry(&mut self, a: Value, b: Value, carry_in: Value) -> (Value, Value, Value) {
        let w = self.width(a);
        assert!(w.bits() < 128, "add_with_carry needs a width below 128");
        let wide = Width::new(w.bits() + 1);
        let ax = self.zext(a, wide);
        let bx = self.zext(b, wide);
        let cx = self.zext(carry_in, wide);
        let s1 = self.add(ax, bx);
        let s2 = self.add(s1, cx);
        let sum = self.trunc(s2, w);
        let carry = self.bit(s2, w.bits());
        let x1 = self.xor(a, sum);
        let x2 = self.xor(b, sum);
        let both = self.and(x1, x2);
        let overflow = self.bit(both, w.bits() - 1);
        (sum, carry, overflow)
    }

    /// Ends the block with `exit` after `len` guest bytes.
    ///
    /// # Panics
    ///
    /// Panics if the exit refers to an ill-typed value.
    pub fn finish(self, len: u32, exit: Exit) -> Block {
        let block = Block {
            regs: self.regs,
            addr_width: self.addr_width,
            pc: self.pc,
            len,
            insts: self.insts,
            exit,
            bank: self.bank,
        };
        if let Err(e) = block.verify() {
            panic!("ill-typed block: {e}");
        }
        block
    }

    /// Ends the block with a jump to `target`.
    pub fn jump(self, len: u32, target: u64) -> Block {
        self.finish(
            len,
            Exit {
                next: Next::Const(target),
                flow: Flow::Jump,
            },
        )
    }
}

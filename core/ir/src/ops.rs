use crate::Reg;

/// The width of a value in bits, from 1 to 128.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Width(u8);

impl Width {
    /// One bit: conditions and flags.
    pub const W1: Width = Width(1);
    /// A byte.
    pub const W8: Width = Width(8);
    /// 16 bits.
    pub const W16: Width = Width(16);
    /// 32 bits.
    pub const W32: Width = Width(32);
    /// 64 bits.
    pub const W64: Width = Width(64);
    /// 128 bits, the widest value.
    pub const W128: Width = Width(128);

    /// A width of `bits` bits.
    ///
    /// # Panics
    ///
    /// Panics unless `1 <= bits <= 128`.
    pub const fn new(bits: u32) -> Width {
        assert!(bits >= 1 && bits <= 128, "width out of range");
        Width(bits as u8)
    }

    /// A width of `bits` bits, or `None` outside 1 to 128.
    pub const fn try_new(bits: u32) -> Option<Width> {
        if bits >= 1 && bits <= 128 {
            Some(Width(bits as u8))
        } else {
            None
        }
    }

    /// The number of bits.
    pub const fn bits(self) -> u32 {
        self.0 as u32
    }

    /// The all-ones value of this width.
    pub const fn mask(self) -> u128 {
        if self.0 == 128 {
            u128::MAX
        } else {
            (1u128 << self.0) - 1
        }
    }

    /// The number of whole bytes, or `None` when the width is not a multiple of 8.
    pub const fn bytes(self) -> Option<u32> {
        if self.0.is_multiple_of(8) {
            Some(self.0 as u32 / 8)
        } else {
            None
        }
    }
}

/// An SSA value: the result of the instruction at this index in its [`crate::Block`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Value(pub u32);

impl Value {
    /// The instruction index as a `usize`.
    pub fn index(self) -> usize {
        self.0 as usize
    }
}

/// One-operand integer operations. The result has the operand's width.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum UnOp {
    /// Bitwise complement.
    Not,
    /// Two's complement negation.
    Neg,
    /// Count of leading zero bits; the width for zero.
    Clz,
    /// Count of trailing zero bits; the width for zero.
    Ctz,
    /// Count of one bits.
    Popcount,
    /// Bit order reversed.
    BitReverse,
    /// Byte order reversed; the width must be a multiple of 8.
    ByteSwap,
}

/// Two-operand integer operations.
///
/// Arithmetic and bitwise operations take two operands of one width and give
/// that width, modulo 2^width. Shifts and rotates take an amount of any width,
/// read as unsigned, and give the first operand's width.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum BinOp {
    /// Wrapping sum.
    Add,
    /// Wrapping difference.
    Sub,
    /// Low half of the product.
    Mul,
    /// Unsigned quotient, rounded toward zero; 0 when dividing by zero.
    UDiv,
    /// Signed quotient, rounded toward zero; 0 when dividing by zero and the
    /// minimum value for the minimum value divided by -1.
    SDiv,
    /// Unsigned remainder; the dividend when dividing by zero.
    URem,
    /// Signed remainder with the dividend's sign; the dividend when dividing
    /// by zero and 0 for the minimum value divided by -1.
    SRem,
    /// Bitwise and.
    And,
    /// Bitwise or.
    Or,
    /// Bitwise exclusive or.
    Xor,
    /// Left shift; 0 once the amount reaches the width.
    Shl,
    /// Logical right shift; 0 once the amount reaches the width.
    LShr,
    /// Arithmetic right shift; all sign bits once the amount reaches the width.
    AShr,
    /// Right rotate by the amount modulo the width.
    RotR,
}

/// Comparisons of two operands of one width, giving one bit.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum CmpOp {
    /// Equal.
    Eq,
    /// Not equal.
    Ne,
    /// Unsigned less than.
    Ult,
    /// Unsigned less than or equal.
    Ule,
    /// Signed less than.
    Slt,
    /// Signed less than or equal.
    Sle,
}

/// How [`Op::Ext`] fills the new high bits.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ExtKind {
    /// With zeros.
    Zero,
    /// With copies of the sign bit.
    Sign,
}

/// The byte order of a memory access.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Endian {
    /// Least significant byte at the lowest address.
    Little,
    /// Most significant byte at the lowest address.
    Big,
}

/// The ordering of a memory access or fence, as in C11.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Order {
    /// No ordering beyond single-threaded program order.
    Plain,
    /// Later accesses are not observed before this one.
    Acquire,
    /// Earlier accesses are observed before this one.
    Release,
    /// Both acquire and release.
    AcqRel,
    /// Acquire and release, in one total order with every other `SeqCst` access.
    SeqCst,
}

/// A rounding direction for [`FpOp`] conversions to integral values.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FpRound {
    /// To nearest, ties to even.
    TiesEven,
    /// To nearest, ties away from zero.
    TiesAway,
    /// Toward minus infinity.
    Floor,
    /// Toward plus infinity.
    Ceil,
    /// Toward zero.
    Zero,
}

/// Floating-point operations on IEEE 754 binary32 or binary64 bit patterns.
///
/// Each is an IEEE primitive with one fixed meaning, so an emitter can map it
/// to a host instruction or a libm call: round to nearest even, subnormals
/// kept (no flush), no exception flags. A NaN result may be any NaN; the
/// conversions are exact about their edge cases instead: `ToSInt` and
/// `ToUInt` saturate and give 0 for a NaN (an emitter must not use a raw
/// x86 `cvttsd2si`, which gives the integer indefinite value).
///
/// Guest semantics are built by the frontend around these primitives, from
/// ordinary IR operations. For A64 (`cpus/a64/src/lift/sail/rt.rs`) that is
/// NaN propagation and quieting, the default NaN, FMAX/FMIN/FMAXNM/FMINNM
/// with signed zeros, the FMA invalid case and FCVT NaN payloads.
///
/// The A64 templates are generated for FPCR zero (round to nearest, no FZ,
/// FZ16, DN, AH or AHP). Every block whose semantics assume that starts with
/// an [`Op::Guard`] on `FPCR != 0`, so with any other FPCR it leaves before
/// any effect for the runtime's slow path (`runtime/cpus/a64/fp.c`); an
/// emitter may hoist that check to function entry and after each write of
/// FPCR. Known gap: no FPSR cumulative flags are produced, which is right
/// only for code that saves and restores FPSR (`fegetenv`, `fesetenv`) but
/// never tests the flags (`fetestexcept`); code that tests them needs the
/// flags produced.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FpOp {
    /// `a + b`.
    Add,
    /// `a - b`.
    Sub,
    /// `a * b`.
    Mul,
    /// `a / b`.
    Div,
    /// The square root of `a`.
    Sqrt,
    /// `a * b + c` with one rounding.
    MulAdd,
    /// `-a`, a sign flip.
    Neg,
    /// `|a|`, a sign clear.
    Abs,
    /// `a` rounded to an integral value in the same format; NaN stays NaN.
    RoundInt(FpRound),
    /// `a` rounded to a signed integer, saturating; NaN gives 0.
    ToSInt(FpRound),
    /// `a` rounded to an unsigned integer, saturating; NaN gives 0.
    ToUInt(FpRound),
    /// The signed integer `a`, rounded to nearest even.
    FromSInt,
    /// The unsigned integer `a`, rounded to nearest even.
    FromUInt,
    /// `a` in the other format, rounded to nearest even.
    Convert,
}

impl FpOp {
    /// Whether the operand width may differ from the result width.
    pub fn converts(self) -> bool {
        matches!(
            self,
            FpOp::ToSInt(_) | FpOp::ToUInt(_) | FpOp::FromSInt | FpOp::FromUInt | FpOp::Convert
        )
    }

    /// The number of operands.
    pub fn arity(self) -> usize {
        match self {
            FpOp::Sqrt
            | FpOp::Neg
            | FpOp::Abs
            | FpOp::RoundInt(_)
            | FpOp::ToSInt(_)
            | FpOp::ToUInt(_)
            | FpOp::FromSInt
            | FpOp::FromUInt
            | FpOp::Convert => 1,
            FpOp::Add | FpOp::Sub | FpOp::Mul | FpOp::Div => 2,
            FpOp::MulAdd => 3,
        }
    }
}

/// One IR operation. Operands are earlier [`Value`]s of the same block.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Op {
    /// A constant of the instruction's width.
    Const(u128),
    /// The value a slot held when the block was entered.
    Get(Reg),
    /// Writes a slot. Writes take effect when the block exits, so a later
    /// `Get` of the same slot still reads the entry value.
    Set(Reg, Value),
    /// A one-operand operation.
    Un(UnOp, Value),
    /// A two-operand operation.
    Bin(BinOp, Value, Value),
    /// A comparison, giving one bit.
    Cmp(CmpOp, Value, Value),
    /// `if cond { a } else { b }`; `cond` is one bit.
    Select(Value, Value, Value),
    /// Widens to the instruction's width.
    Ext(ExtKind, Value),
    /// The instruction's width of bits starting at bit `lo`.
    Extract(Value, u8),
    /// `hi` above `lo`; the width is the sum.
    Concat(Value, Value),
    /// Reads the instruction's width (whole bytes) at `addr`.
    Load {
        /// The address, of the block's address width.
        addr: Value,
        /// The byte order.
        endian: Endian,
        /// The ordering.
        order: Order,
    },
    /// Writes `value` (whole bytes) at `addr`.
    Store {
        /// The address, of the block's address width.
        addr: Value,
        /// The value written.
        value: Value,
        /// The byte order.
        endian: Endian,
        /// The ordering.
        order: Order,
    },
    /// A load that also arms the exclusive monitor for its address and size.
    LoadExclusive {
        /// The address.
        addr: Value,
        /// The byte order.
        endian: Endian,
        /// The ordering.
        order: Order,
    },
    /// A store that happens only when the monitor is armed for exactly this
    /// address and size. Gives one bit, 1 when it stored. The monitor is
    /// cleared either way.
    StoreExclusive {
        /// The address.
        addr: Value,
        /// The value written.
        value: Value,
        /// The byte order.
        endian: Endian,
        /// The ordering.
        order: Order,
    },
    /// Clears the exclusive monitor.
    ClearExclusive,
    /// A memory fence.
    Fence(Order),
    /// A floating-point operation of the instruction's width (32 or 64).
    Fp(FpOp, Vec<Value>),
    /// Leaves the block before it has any effect when `cond` (one bit) is 1:
    /// the block's assumption does not hold, and the runtime runs the
    /// instruction on its slow path, the one named by `code`. A guard comes
    /// before every operation with an effect, so leaving discards nothing.
    Guard(Value, u32),
    /// A call to a named intrinsic the frontend and runtime define, giving
    /// the instruction's width, or nothing when the instruction has none.
    Intrinsic(&'static str, Vec<Value>),
    /// Advances the guest clock by this many cycles, in the unit the CPU
    /// frontend defines (Game Boy: T-cycles). The clock moves at this
    /// position: an I/O access or sync point after it sees the time
    /// including it, one before it does not. A frontend gives each guest
    /// instruction its cost here and may split it around the instruction's
    /// I/O accesses so that each access happens at its own cycle. Guests
    /// without a clock never emit it. See docs/abi.md section 13.
    Tick(u32),
    /// Advances the guest clock by this many cycles when `cond` (one bit) is 1 and
    /// does nothing otherwise: the extra cost of a taken branch, or of one
    /// data-dependent step.
    TickIf(Value, u32),
}

impl Op {
    /// The operands, in order.
    pub fn operands(&self) -> Vec<Value> {
        match self {
            Op::Const(_) | Op::Get(_) | Op::ClearExclusive | Op::Fence(_) | Op::Tick(_) => {
                Vec::new()
            }
            Op::Set(_, v)
            | Op::Un(_, v)
            | Op::Ext(_, v)
            | Op::Extract(v, _)
            | Op::Guard(v, _)
            | Op::TickIf(v, _) => {
                vec![*v]
            }
            Op::Bin(_, a, b) | Op::Cmp(_, a, b) | Op::Concat(a, b) => vec![*a, *b],
            Op::Select(c, a, b) => vec![*c, *a, *b],
            Op::Load { addr, .. } | Op::LoadExclusive { addr, .. } => vec![*addr],
            Op::Store { addr, value, .. } | Op::StoreExclusive { addr, value, .. } => {
                vec![*addr, *value]
            }
            Op::Fp(_, args) | Op::Intrinsic(_, args) => args.clone(),
        }
    }

    /// Whether the operation has an effect besides its result: state, memory,
    /// the monitor, the guest clock, or an intrinsic the IR cannot see into.
    pub fn has_effect(&self) -> bool {
        matches!(
            self,
            Op::Set(..)
                | Op::Load { .. }
                | Op::Store { .. }
                | Op::LoadExclusive { .. }
                | Op::StoreExclusive { .. }
                | Op::ClearExclusive
                | Op::Fence(_)
                | Op::Intrinsic(..)
                | Op::Guard(..)
                | Op::Tick(_)
                | Op::TickIf(..)
        )
    }
}

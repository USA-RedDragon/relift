/// An 8-bit register.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Reg8 {
    /// Register A, the accumulator.
    A,
    /// Register B.
    B,
    /// Register C.
    C,
    /// Register D.
    D,
    /// Register E.
    E,
    /// Register H.
    H,
    /// Register L.
    L,
}

/// A 16-bit register or register pair.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Reg16 {
    /// B and C.
    Bc,
    /// D and E.
    De,
    /// H and L.
    Hl,
    /// The stack pointer.
    Sp,
    /// A and the flags; only PUSH and POP use it.
    Af,
}

/// A branch condition.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Cond {
    /// Zero flag clear.
    Nz,
    /// Zero flag set.
    Z,
    /// Carry flag clear.
    Nc,
    /// Carry flag set.
    C,
}

/// What an operand slot of an instruction holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Operand {
    /// The slot is not used.
    None,
    /// An 8-bit register.
    R8(Reg8),
    /// A 16-bit register or pair.
    R16(Reg16),
    /// Memory at the address in a register pair: `(BC)`, `(DE)` or `(HL)`.
    Indirect(Reg16),
    /// `(HL+)`: memory at HL, then HL is incremented.
    HlInc,
    /// `(HL-)`: memory at HL, then HL is decremented.
    HlDec,
    /// `(C)`: memory at `0xFF00 + C`.
    IndirectC,
    /// An 8-bit immediate.
    Imm8,
    /// A 16-bit immediate.
    Imm16,
    /// A signed 8-bit immediate: a relative jump offset or an SP offset.
    SignedImm8,
    /// `(a16)`: memory at a 16-bit immediate address.
    IndirectImm16,
    /// `(a8)`: memory at `0xFF00 +` an 8-bit immediate.
    HighImm8,
    /// `SP + e8` as the source of `LD HL,SP+e8`.
    SpPlusSigned8,
    /// A branch condition.
    Cond(Cond),
    /// A bit number 0 to 7 of BIT, RES and SET.
    Bit(u8),
    /// A RST target address.
    Vector(u8),
}

/// The operation an opcode performs.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Mnemonic {
    /// No operation.
    Nop,
    /// Enter low-power mode; the byte after it is padding.
    Stop,
    /// Suspend the CPU until an interrupt is pending.
    Halt,
    /// Any load, including the high-page forms.
    Ld,
    /// Increment.
    Inc,
    /// Decrement.
    Dec,
    /// Add.
    Add,
    /// Add with carry.
    Adc,
    /// Subtract.
    Sub,
    /// Subtract with carry.
    Sbc,
    /// Bitwise and.
    And,
    /// Bitwise exclusive or.
    Xor,
    /// Bitwise or.
    Or,
    /// Compare.
    Cp,
    /// Rotate A left, bit 7 to carry; Z is cleared.
    Rlca,
    /// Rotate A right, bit 0 to carry; Z is cleared.
    Rrca,
    /// Rotate A left through carry; Z is cleared.
    Rla,
    /// Rotate A right through carry; Z is cleared.
    Rra,
    /// Decimal adjust A.
    Daa,
    /// Complement A.
    Cpl,
    /// Set carry.
    Scf,
    /// Complement carry.
    Ccf,
    /// Relative jump.
    Jr,
    /// Absolute jump.
    Jp,
    /// Call.
    Call,
    /// Return.
    Ret,
    /// Return and enable interrupts.
    Reti,
    /// Restart: call a fixed low-page vector.
    Rst,
    /// Push a register pair.
    Push,
    /// Pop a register pair.
    Pop,
    /// Disable interrupts.
    Di,
    /// Enable interrupts after the next instruction.
    Ei,
    /// The `0xCB` prefix byte.
    Prefix,
    /// Rotate left, bit 7 to carry (CB).
    Rlc,
    /// Rotate right, bit 0 to carry (CB).
    Rrc,
    /// Rotate left through carry (CB).
    Rl,
    /// Rotate right through carry (CB).
    Rr,
    /// Shift left arithmetic (CB).
    Sla,
    /// Shift right arithmetic (CB).
    Sra,
    /// Swap nibbles (CB).
    Swap,
    /// Shift right logical (CB).
    Srl,
    /// Test a bit (CB).
    Bit,
    /// Clear a bit (CB).
    Res,
    /// Set a bit (CB).
    Set,
    /// One of the 11 unused base opcodes; hardware locks up.
    Illegal,
}

/// Which table an entry belongs to.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Prefix {
    /// The 256 base opcodes.
    None,
    /// The 256 opcodes after the `0xCB` prefix.
    Cb,
}

/// The cost of an instruction in M-cycles.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Cycles {
    /// The cost when a conditional branch is not taken; the only cost of every
    /// other instruction.
    pub not_taken: u8,
    /// The cost when a conditional branch is taken; equal to `not_taken` for
    /// every other instruction.
    pub taken: u8,
}

impl Cycles {
    /// The cost for the given branch outcome.
    pub const fn get(self, taken: bool) -> u8 {
        if taken { self.taken } else { self.not_taken }
    }

    /// Whether the cost depends on a branch outcome.
    pub const fn is_conditional(self) -> bool {
        self.taken != self.not_taken
    }
}

/// A set of CPU flags, laid out as in the F register.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub struct FlagSet(pub u8);

impl FlagSet {
    /// No flag.
    pub const NONE: FlagSet = FlagSet(0);
    /// Zero, F bit 7.
    pub const Z: FlagSet = FlagSet(0x80);
    /// Subtract, F bit 6.
    pub const N: FlagSet = FlagSet(0x40);
    /// Half carry, F bit 5.
    pub const H: FlagSet = FlagSet(0x20);
    /// Carry, F bit 4.
    pub const C: FlagSet = FlagSet(0x10);

    /// The union of two sets.
    pub const fn union(self, other: FlagSet) -> FlagSet {
        FlagSet(self.0 | other.0)
    }

    /// Whether every flag of `other` is in this set.
    pub const fn contains(self, other: FlagSet) -> bool {
        self.0 & other.0 == other.0
    }

    /// Whether the set is empty.
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
}

/// What an instruction does to one flag.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FlagEffect {
    /// The flag is not touched.
    Keep,
    /// The flag is set.
    Set,
    /// The flag is cleared.
    Clear,
    /// The flag is computed from the operation's result.
    Compute,
    /// The flag is loaded from memory (POP AF).
    Load,
}

/// The flags an instruction reads and how it writes each flag.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Flags {
    /// The flags the instruction reads.
    pub read: FlagSet,
    /// The effect on the zero flag.
    pub z: FlagEffect,
    /// The effect on the subtract flag.
    pub n: FlagEffect,
    /// The effect on the half-carry flag.
    pub h: FlagEffect,
    /// The effect on the carry flag.
    pub c: FlagEffect,
}

impl Flags {
    /// An instruction that neither reads nor writes any flag.
    pub const NONE: Flags = Flags {
        read: FlagSet::NONE,
        z: FlagEffect::Keep,
        n: FlagEffect::Keep,
        h: FlagEffect::Keep,
        c: FlagEffect::Keep,
    };

    /// The flags the instruction writes in any way.
    pub const fn written(self) -> FlagSet {
        let mut bits = 0;
        if !matches!(self.z, FlagEffect::Keep) {
            bits |= FlagSet::Z.0;
        }
        if !matches!(self.n, FlagEffect::Keep) {
            bits |= FlagSet::N.0;
        }
        if !matches!(self.h, FlagEffect::Keep) {
            bits |= FlagSet::H.0;
        }
        if !matches!(self.c, FlagEffect::Keep) {
            bits |= FlagSet::C.0;
        }
        FlagSet(bits)
    }
}

/// Whether a bus access fetches an instruction byte, reads data or writes data.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum AccessKind {
    /// An opcode, prefix or immediate byte fetched at the program counter.
    Fetch,
    /// A data read.
    Read,
    /// A data write.
    Write,
}

/// The address a bus access uses.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum AccessTarget {
    /// The program counter, which advances after each fetch.
    Pc,
    /// The address in BC.
    Bc,
    /// The address in DE.
    De,
    /// The address in HL.
    Hl,
    /// The 16-bit immediate.
    Imm16,
    /// The 16-bit immediate plus one.
    Imm16Plus1,
    /// `0xFF00` plus the 8-bit immediate.
    HighImm8,
    /// `0xFF00` plus C.
    HighC,
    /// The low byte popped from SP; SP then increases by one.
    PopLow,
    /// The high byte popped from SP; SP then increases by one.
    PopHigh,
    /// The high byte pushed to SP minus one; SP decreases by one first.
    PushHigh,
    /// The low byte pushed to SP minus one; SP decreases by one first.
    PushLow,
}

/// One bus access of an instruction.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct MemAccess {
    /// The M-cycle of the instruction, starting at 1, in which it happens.
    pub cycle: u8,
    /// Fetch, read or write.
    pub kind: AccessKind,
    /// Where the address comes from.
    pub target: AccessTarget,
}

/// The bus accesses of one path through an instruction, in cycle order.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Accesses {
    len: u8,
    items: [MemAccess; Accesses::CAPACITY],
}

impl Accesses {
    /// The most accesses any instruction makes.
    pub const CAPACITY: usize = 6;

    const EMPTY: MemAccess = MemAccess {
        cycle: 0,
        kind: AccessKind::Fetch,
        target: AccessTarget::Pc,
    };

    pub(crate) const fn new() -> Accesses {
        Accesses {
            len: 0,
            items: [Accesses::EMPTY; Accesses::CAPACITY],
        }
    }

    pub(crate) const fn with(mut self, cycle: u8, kind: AccessKind, target: AccessTarget) -> Self {
        self.items[self.len as usize] = MemAccess {
            cycle,
            kind,
            target,
        };
        self.len += 1;
        self
    }

    /// The accesses in the order they happen.
    pub fn as_slice(&self) -> &[MemAccess] {
        &self.items[..self.len as usize]
    }
}

/// One entry of the opcode table.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Opcode {
    /// The opcode byte (after the prefix for a CB entry).
    pub code: u8,
    /// The table the entry is in.
    pub prefix: Prefix,
    /// The operation.
    pub mnemonic: Mnemonic,
    /// The destination operand, or the condition or bit number.
    pub dst: Operand,
    /// The source operand.
    pub src: Operand,
    /// The length in bytes, including the prefix of a CB entry.
    pub length: u8,
    /// The cost in M-cycles.
    pub cycles: Cycles,
    /// The flags read and written.
    pub flags: Flags,
    pub(crate) accesses_taken: Accesses,
    pub(crate) accesses_not_taken: Accesses,
}

impl Opcode {
    /// The bus accesses for the given branch outcome. Unconditional
    /// instructions return the same list for both outcomes.
    pub fn accesses(&self, taken: bool) -> &[MemAccess] {
        if taken {
            self.accesses_taken.as_slice()
        } else {
            self.accesses_not_taken.as_slice()
        }
    }

    /// Whether this is one of the 11 unused base opcodes.
    pub const fn is_illegal(&self) -> bool {
        matches!(self.mnemonic, Mnemonic::Illegal)
    }
}

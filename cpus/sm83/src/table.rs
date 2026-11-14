use crate::types::AccessKind::{Fetch, Read, Write};
use crate::types::AccessTarget as T;
use crate::types::FlagEffect::{Clear as Cl, Compute as X, Keep as K, Load, Set as S};
use crate::types::{
    Accesses, Cond, Cycles, FlagSet, Flags, Mnemonic as M, Opcode, Operand, Operand as O, Prefix,
    Reg8, Reg16,
};

/// The 256 base opcodes, indexed by opcode byte.
pub static BASE: [Opcode; 256] = build_base();

/// The 256 opcodes that follow the `0xCB` prefix, indexed by the second byte.
pub static CB: [Opcode; 256] = build_cb();

const fn build_base() -> [Opcode; 256] {
    let mut table = [base(0); 256];
    let mut i = 0;
    while i < 256 {
        table[i] = base(i as u8);
        i += 1;
    }
    table
}

const fn build_cb() -> [Opcode; 256] {
    let mut table = [cb(0); 256];
    let mut i = 0;
    while i < 256 {
        table[i] = cb(i as u8);
        i += 1;
    }
    table
}

const fn r8(index: u8) -> Operand {
    match index {
        0 => O::R8(Reg8::B),
        1 => O::R8(Reg8::C),
        2 => O::R8(Reg8::D),
        3 => O::R8(Reg8::E),
        4 => O::R8(Reg8::H),
        5 => O::R8(Reg8::L),
        6 => O::Indirect(Reg16::Hl),
        _ => O::R8(Reg8::A),
    }
}

const fn rp(index: u8) -> Reg16 {
    match index {
        0 => Reg16::Bc,
        1 => Reg16::De,
        2 => Reg16::Hl,
        _ => Reg16::Sp,
    }
}

const fn rp2(index: u8) -> Reg16 {
    match index {
        0 => Reg16::Bc,
        1 => Reg16::De,
        2 => Reg16::Hl,
        _ => Reg16::Af,
    }
}

const fn cond(index: u8) -> Cond {
    match index {
        0 => Cond::Nz,
        1 => Cond::Z,
        2 => Cond::Nc,
        _ => Cond::C,
    }
}

const fn cond_flag(c: Cond) -> FlagSet {
    match c {
        Cond::Nz | Cond::Z => FlagSet::Z,
        Cond::Nc | Cond::C => FlagSet::C,
    }
}

const fn fetches(count: u8) -> Accesses {
    let mut acc = Accesses::new();
    let mut cycle = 1;
    while cycle <= count {
        acc = acc.with(cycle, Fetch, T::Pc);
        cycle += 1;
    }
    acc
}

const fn flags(
    read: FlagSet,
    z: crate::FlagEffect,
    n: crate::FlagEffect,
    h: crate::FlagEffect,
    c: crate::FlagEffect,
) -> Flags {
    Flags { read, z, n, h, c }
}

const NF: Flags = Flags::NONE;

struct Draft {
    code: u8,
    prefix: Prefix,
    mnemonic: M,
    dst: Operand,
    src: Operand,
    length: u8,
    not_taken: u8,
    taken: u8,
    flags: Flags,
    accesses_taken: Accesses,
    accesses_not_taken: Accesses,
}

impl Draft {
    const fn new(code: u8, prefix: Prefix, mnemonic: M, length: u8, cycles: u8) -> Draft {
        Draft {
            code,
            prefix,
            mnemonic,
            dst: O::None,
            src: O::None,
            length,
            not_taken: cycles,
            taken: cycles,
            flags: NF,
            accesses_taken: fetches(length),
            accesses_not_taken: fetches(length),
        }
    }

    const fn dst(mut self, dst: Operand) -> Draft {
        self.dst = dst;
        self
    }

    const fn src(mut self, src: Operand) -> Draft {
        self.src = src;
        self
    }

    const fn flags(mut self, flags: Flags) -> Draft {
        self.flags = flags;
        self
    }

    const fn then(mut self, cycle: u8, kind: crate::AccessKind, target: T) -> Draft {
        self.accesses_taken = self.accesses_taken.with(cycle, kind, target);
        self.accesses_not_taken = self.accesses_not_taken.with(cycle, kind, target);
        self
    }

    const fn only_taken(mut self, cycle: u8, kind: crate::AccessKind, target: T) -> Draft {
        self.accesses_taken = self.accesses_taken.with(cycle, kind, target);
        self
    }

    const fn conditional(mut self, taken_cycles: u8, c: Cond) -> Draft {
        self.taken = taken_cycles;
        self.flags.read = cond_flag(c);
        self
    }

    const fn only_opcode_fetched(mut self) -> Draft {
        self.accesses_taken = fetches(1);
        self.accesses_not_taken = fetches(1);
        self
    }

    const fn done(self) -> Opcode {
        Opcode {
            code: self.code,
            prefix: self.prefix,
            mnemonic: self.mnemonic,
            dst: self.dst,
            src: self.src,
            length: self.length,
            cycles: Cycles {
                not_taken: self.not_taken,
                taken: self.taken,
            },
            flags: self.flags,
            accesses_taken: self.accesses_taken,
            accesses_not_taken: self.accesses_not_taken,
        }
    }
}

const fn d(code: u8, mnemonic: M, length: u8, cycles: u8) -> Draft {
    Draft::new(code, Prefix::None, mnemonic, length, cycles)
}

const fn alu_mnemonic(y: u8) -> M {
    match y {
        0 => M::Add,
        1 => M::Adc,
        2 => M::Sub,
        3 => M::Sbc,
        4 => M::And,
        5 => M::Xor,
        6 => M::Or,
        _ => M::Cp,
    }
}

const fn alu_flags(y: u8) -> Flags {
    match y {
        0 => flags(FlagSet::NONE, X, Cl, X, X),
        1 => flags(FlagSet::C, X, Cl, X, X),
        2 | 7 => flags(FlagSet::NONE, X, S, X, X),
        3 => flags(FlagSet::C, X, S, X, X),
        4 => flags(FlagSet::NONE, X, Cl, S, Cl),
        _ => flags(FlagSet::NONE, X, Cl, Cl, Cl),
    }
}

const fn base(code: u8) -> Opcode {
    let x = code >> 6;
    let y = (code >> 3) & 7;
    let z = code & 7;
    let p = y >> 1;
    let q = y & 1;
    let draft = match x {
        0 => base_x0(code, y, z, p, q),
        1 => base_x1(code, y, z),
        2 => base_x2(code, y, z),
        _ => base_x3(code, y, z, p, q),
    };
    draft.done()
}

const fn base_x0(code: u8, y: u8, z: u8, p: u8, q: u8) -> Draft {
    match z {
        0 => match y {
            0 => d(code, M::Nop, 1, 1),
            1 => d(code, M::Ld, 3, 5)
                .dst(O::IndirectImm16)
                .src(O::R16(Reg16::Sp))
                .then(4, Write, T::Imm16)
                .then(5, Write, T::Imm16Plus1),
            2 => d(code, M::Stop, 2, 1).only_opcode_fetched(),
            3 => d(code, M::Jr, 2, 3).src(O::SignedImm8),
            _ => d(code, M::Jr, 2, 2)
                .dst(O::Cond(cond(y - 4)))
                .src(O::SignedImm8)
                .conditional(3, cond(y - 4)),
        },
        1 => {
            if q == 0 {
                d(code, M::Ld, 3, 3).dst(O::R16(rp(p))).src(O::Imm16)
            } else {
                d(code, M::Add, 1, 2)
                    .dst(O::R16(Reg16::Hl))
                    .src(O::R16(rp(p)))
                    .flags(flags(FlagSet::NONE, K, Cl, X, X))
            }
        }
        2 => {
            let mem = match p {
                0 => O::Indirect(Reg16::Bc),
                1 => O::Indirect(Reg16::De),
                2 => O::HlInc,
                _ => O::HlDec,
            };
            let target = match p {
                0 => T::Bc,
                1 => T::De,
                _ => T::Hl,
            };
            if q == 0 {
                d(code, M::Ld, 1, 2)
                    .dst(mem)
                    .src(O::R8(Reg8::A))
                    .then(2, Write, target)
            } else {
                d(code, M::Ld, 1, 2)
                    .dst(O::R8(Reg8::A))
                    .src(mem)
                    .then(2, Read, target)
            }
        }
        3 => {
            let m = if q == 0 { M::Inc } else { M::Dec };
            d(code, m, 1, 2).dst(O::R16(rp(p)))
        }
        4 | 5 => {
            let m = if z == 4 { M::Inc } else { M::Dec };
            let n = if z == 4 { Cl } else { S };
            let f = flags(FlagSet::NONE, X, n, X, K);
            if y == 6 {
                d(code, m, 1, 3)
                    .dst(r8(y))
                    .flags(f)
                    .then(2, Read, T::Hl)
                    .then(3, Write, T::Hl)
            } else {
                d(code, m, 1, 1).dst(r8(y)).flags(f)
            }
        }
        6 => {
            if y == 6 {
                d(code, M::Ld, 2, 3)
                    .dst(r8(y))
                    .src(O::Imm8)
                    .then(3, Write, T::Hl)
            } else {
                d(code, M::Ld, 2, 2).dst(r8(y)).src(O::Imm8)
            }
        }
        _ => {
            let a = O::R8(Reg8::A);
            match y {
                0 => d(code, M::Rlca, 1, 1)
                    .dst(a)
                    .flags(flags(FlagSet::NONE, Cl, Cl, Cl, X)),
                1 => d(code, M::Rrca, 1, 1)
                    .dst(a)
                    .flags(flags(FlagSet::NONE, Cl, Cl, Cl, X)),
                2 => d(code, M::Rla, 1, 1)
                    .dst(a)
                    .flags(flags(FlagSet::C, Cl, Cl, Cl, X)),
                3 => d(code, M::Rra, 1, 1)
                    .dst(a)
                    .flags(flags(FlagSet::C, Cl, Cl, Cl, X)),
                4 => d(code, M::Daa, 1, 1).dst(a).flags(flags(
                    FlagSet::N.union(FlagSet::H).union(FlagSet::C),
                    X,
                    K,
                    Cl,
                    X,
                )),
                5 => d(code, M::Cpl, 1, 1)
                    .dst(a)
                    .flags(flags(FlagSet::NONE, K, S, S, K)),
                6 => d(code, M::Scf, 1, 1).flags(flags(FlagSet::NONE, K, Cl, Cl, S)),
                _ => d(code, M::Ccf, 1, 1).flags(flags(FlagSet::C, K, Cl, Cl, X)),
            }
        }
    }
}

const fn base_x1(code: u8, y: u8, z: u8) -> Draft {
    if y == 6 && z == 6 {
        return d(code, M::Halt, 1, 1);
    }
    if y == 6 {
        d(code, M::Ld, 1, 2)
            .dst(r8(y))
            .src(r8(z))
            .then(2, Write, T::Hl)
    } else if z == 6 {
        d(code, M::Ld, 1, 2)
            .dst(r8(y))
            .src(r8(z))
            .then(2, Read, T::Hl)
    } else {
        d(code, M::Ld, 1, 1).dst(r8(y)).src(r8(z))
    }
}

const fn base_x2(code: u8, y: u8, z: u8) -> Draft {
    let base = d(code, alu_mnemonic(y), 1, if z == 6 { 2 } else { 1 })
        .dst(O::R8(Reg8::A))
        .src(r8(z))
        .flags(alu_flags(y));
    if z == 6 {
        base.then(2, Read, T::Hl)
    } else {
        base
    }
}

const fn base_x3(code: u8, y: u8, z: u8, p: u8, q: u8) -> Draft {
    match z {
        0 => match y {
            0..=3 => d(code, M::Ret, 1, 2)
                .dst(O::Cond(cond(y)))
                .conditional(5, cond(y))
                .only_taken(3, Read, T::PopLow)
                .only_taken(4, Read, T::PopHigh),
            4 => d(code, M::Ld, 2, 3)
                .dst(O::HighImm8)
                .src(O::R8(Reg8::A))
                .then(3, Write, T::HighImm8),
            5 => d(code, M::Add, 2, 4)
                .dst(O::R16(Reg16::Sp))
                .src(O::SignedImm8)
                .flags(flags(FlagSet::NONE, Cl, Cl, X, X)),
            6 => d(code, M::Ld, 2, 3)
                .dst(O::R8(Reg8::A))
                .src(O::HighImm8)
                .then(3, Read, T::HighImm8),
            _ => d(code, M::Ld, 2, 3)
                .dst(O::R16(Reg16::Hl))
                .src(O::SpPlusSigned8)
                .flags(flags(FlagSet::NONE, Cl, Cl, X, X)),
        },
        1 => {
            if q == 0 {
                let f = if p == 3 {
                    flags(FlagSet::NONE, Load, Load, Load, Load)
                } else {
                    NF
                };
                d(code, M::Pop, 1, 3)
                    .dst(O::R16(rp2(p)))
                    .flags(f)
                    .then(2, Read, T::PopLow)
                    .then(3, Read, T::PopHigh)
            } else {
                match p {
                    0 => d(code, M::Ret, 1, 4)
                        .then(2, Read, T::PopLow)
                        .then(3, Read, T::PopHigh),
                    1 => d(code, M::Reti, 1, 4)
                        .then(2, Read, T::PopLow)
                        .then(3, Read, T::PopHigh),
                    2 => d(code, M::Jp, 1, 1).src(O::R16(Reg16::Hl)),
                    _ => d(code, M::Ld, 1, 2)
                        .dst(O::R16(Reg16::Sp))
                        .src(O::R16(Reg16::Hl)),
                }
            }
        }
        2 => match y {
            0..=3 => d(code, M::Jp, 3, 3)
                .dst(O::Cond(cond(y)))
                .src(O::Imm16)
                .conditional(4, cond(y)),
            4 => d(code, M::Ld, 1, 2)
                .dst(O::IndirectC)
                .src(O::R8(Reg8::A))
                .then(2, Write, T::HighC),
            5 => d(code, M::Ld, 3, 4)
                .dst(O::IndirectImm16)
                .src(O::R8(Reg8::A))
                .then(4, Write, T::Imm16),
            6 => d(code, M::Ld, 1, 2)
                .dst(O::R8(Reg8::A))
                .src(O::IndirectC)
                .then(2, Read, T::HighC),
            _ => d(code, M::Ld, 3, 4)
                .dst(O::R8(Reg8::A))
                .src(O::IndirectImm16)
                .then(4, Read, T::Imm16),
        },
        3 => match y {
            0 => d(code, M::Jp, 3, 4).src(O::Imm16),
            1 => Draft::new(code, Prefix::None, M::Prefix, 1, 1),
            6 => d(code, M::Di, 1, 1),
            7 => d(code, M::Ei, 1, 1),
            _ => d(code, M::Illegal, 1, 1),
        },
        4 => match y {
            0..=3 => d(code, M::Call, 3, 3)
                .dst(O::Cond(cond(y)))
                .src(O::Imm16)
                .conditional(6, cond(y))
                .only_taken(5, Write, T::PushHigh)
                .only_taken(6, Write, T::PushLow),
            _ => d(code, M::Illegal, 1, 1),
        },
        5 => {
            if q == 0 {
                d(code, M::Push, 1, 4)
                    .src(O::R16(rp2(p)))
                    .then(3, Write, T::PushHigh)
                    .then(4, Write, T::PushLow)
            } else if p == 0 {
                d(code, M::Call, 3, 6)
                    .src(O::Imm16)
                    .then(5, Write, T::PushHigh)
                    .then(6, Write, T::PushLow)
            } else {
                d(code, M::Illegal, 1, 1)
            }
        }
        6 => d(code, alu_mnemonic(y), 2, 2)
            .dst(O::R8(Reg8::A))
            .src(O::Imm8)
            .flags(alu_flags(y)),
        _ => d(code, M::Rst, 1, 4)
            .dst(O::Vector(y * 8))
            .then(3, Write, T::PushHigh)
            .then(4, Write, T::PushLow),
    }
}

const fn cb(code: u8) -> Opcode {
    let x = code >> 6;
    let y = (code >> 3) & 7;
    let z = code & 7;
    let in_memory = z == 6;
    let (mnemonic, cycles, f) = match x {
        0 => {
            let m = match y {
                0 => M::Rlc,
                1 => M::Rrc,
                2 => M::Rl,
                3 => M::Rr,
                4 => M::Sla,
                5 => M::Sra,
                6 => M::Swap,
                _ => M::Srl,
            };
            let read = if y == 2 || y == 3 {
                FlagSet::C
            } else {
                FlagSet::NONE
            };
            let carry = if y == 6 { Cl } else { X };
            (
                m,
                if in_memory { 4 } else { 2 },
                flags(read, X, Cl, Cl, carry),
            )
        }
        1 => (
            M::Bit,
            if in_memory { 3 } else { 2 },
            flags(FlagSet::NONE, X, Cl, S, K),
        ),
        2 => (M::Res, if in_memory { 4 } else { 2 }, NF),
        _ => (M::Set, if in_memory { 4 } else { 2 }, NF),
    };
    let mut draft = Draft::new(code, Prefix::Cb, mnemonic, 2, cycles).flags(f);
    draft = if x == 0 {
        draft.dst(r8(z))
    } else {
        draft.dst(O::Bit(y)).src(r8(z))
    };
    if in_memory {
        draft = draft.then(3, Read, T::Hl);
        if x != 1 {
            draft = draft.then(4, Write, T::Hl);
        }
    }
    draft.done()
}

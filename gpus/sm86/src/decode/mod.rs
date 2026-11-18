use crate::Word;
use relift_gpu_nv::{CBank, Inst, Operand, RegMods, Swz};

mod alu;
mod ctl;
mod fields;
mod mem;
mod tex;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DecodeError {
    pub word: Word,
    pub what: String,
}

impl std::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:032x}: {}", self.word.0, self.what)
    }
}

impl std::error::Error for DecodeError {}

pub type Result<T> = std::result::Result<T, DecodeError>;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Num {
    F32,
    F64,
    Int,
    UInt,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Slot {
    A,
    B,
    C,
}

pub(crate) struct D {
    pub w: Word,
    pub pc: u64,
    pub i: Inst,
}

pub(crate) fn mk(name: &'static str) -> Inst {
    Inst::new(name)
}

impl D {
    pub fn get(&self, lo: u32, len: u32) -> u32 {
        self.w.get(lo, len)
    }

    pub fn bit(&self, n: u32) -> bool {
        self.w.bit(n)
    }

    pub fn form(&self) -> u32 {
        self.get(9, 3)
    }

    pub fn err<T>(&self, what: impl Into<String>) -> Result<T> {
        Err(DecodeError {
            word: self.w,
            what: what.into(),
        })
    }

    pub fn m(&mut self, s: &'static str) {
        self.i.mods.push(s);
    }

    pub fn m_if(&mut self, c: bool, s: &'static str) {
        if c {
            self.i.mods.push(s);
        }
    }

    pub fn op(&mut self, o: Operand) {
        self.i.ops.push(o);
    }

    pub fn reuse_bit(lo: u32) -> Option<u32> {
        match lo {
            24 => Some(122),
            32 => Some(123),
            64 => Some(124),
            _ => None,
        }
    }

    pub fn reg_at(&self, lo: u32) -> Operand {
        let reuse = D::reuse_bit(lo).is_some_and(|b| self.bit(b));
        Operand::Reg(
            self.get(lo, 8) as u8,
            RegMods {
                reuse,
                ..RegMods::default()
            },
        )
    }

    pub fn reg_plain(&self, lo: u32) -> Operand {
        Operand::Reg(self.get(lo, 8) as u8, RegMods::default())
    }

    pub fn ureg_at(&self, lo: u32) -> Operand {
        Operand::UReg(self.get(lo, 6) as u8, RegMods::default())
    }

    pub fn dst(&self) -> Operand {
        self.reg_plain(16)
    }

    pub fn udst(&self) -> Operand {
        self.ureg_at(16)
    }

    pub fn pred_at(&self, lo: u32) -> Operand {
        Operand::Pred(self.get(lo, 3) as u8, false)
    }

    pub fn pred_src(&self, lo: u32, not: u32) -> Operand {
        Operand::Pred(self.get(lo, 3) as u8, self.bit(not))
    }

    pub fn upred_at(&self, lo: u32) -> Operand {
        Operand::UPred(self.get(lo, 3) as u8, false)
    }

    pub fn upred_src(&self, lo: u32, not: u32) -> Operand {
        Operand::UPred(self.get(lo, 3) as u8, self.bit(not))
    }

    pub fn cbuf_at(&self, lo: u32, ux: Option<u32>) -> Operand {
        let raw = self.get(lo, 27);
        let offset = ((raw >> 6) & 0xffff) as u16 as i16 as i64;
        let bank = if ux.is_some_and(|b| self.bit(b)) {
            CBank::UReg((raw & 0x3f) as u8)
        } else {
            CBank::Imm(raw >> 22)
        };
        Operand::CBuf {
            bank,
            index: None,
            offset,
            mods: RegMods::default(),
            space: false,
        }
    }

    pub fn imm32(&self, n: Num) -> Operand {
        let v = self.get(32, 32);
        match n {
            Num::F32 => Operand::F32(v, false),
            Num::F64 => Operand::F64Hi(v),
            Num::Int => Operand::Int(v as i32 as i64),
            Num::UInt => Operand::Hex(v as u64),
        }
    }

    pub fn with_mods(o: Operand, neg: bool, abs: bool, int: bool) -> Operand {
        match o {
            Operand::Reg(n, mut m) => {
                if int {
                    m.neg = neg;
                } else {
                    m.neg = neg;
                    m.abs = abs;
                }
                Operand::Reg(n, m)
            }
            Operand::UReg(n, mut m) => {
                m.neg = neg;
                if !int {
                    m.abs = abs;
                }
                Operand::UReg(n, m)
            }
            Operand::CBuf {
                bank,
                index,
                offset,
                mut mods,
                space,
            } => {
                mods.neg = neg;
                if !int {
                    mods.abs = abs;
                }
                Operand::CBuf {
                    bank,
                    index,
                    offset,
                    mods,
                    space,
                }
            }
            o => o,
        }
    }

    pub fn not_mods(o: Operand, not: bool) -> Operand {
        match o {
            Operand::Reg(n, mut m) => {
                m.not = not;
                Operand::Reg(n, m)
            }
            Operand::UReg(n, mut m) => {
                m.not = not;
                Operand::UReg(n, m)
            }
            Operand::CBuf {
                bank,
                index,
                offset,
                mut mods,
                space,
            } => {
                mods.not = not;
                Operand::CBuf {
                    bank,
                    index,
                    offset,
                    mods,
                    space,
                }
            }
            o => o,
        }
    }

    pub fn swz(o: Operand, s: Swz) -> Operand {
        match o {
            Operand::Reg(n, mut m) => {
                m.swz = s;
                Operand::Reg(n, m)
            }
            Operand::UReg(n, mut m) => {
                m.swz = s;
                Operand::UReg(n, m)
            }
            Operand::CBuf {
                bank,
                index,
                offset,
                mut mods,
                space,
            } => {
                mods.swz = s;
                Operand::CBuf {
                    bank,
                    index,
                    offset,
                    mods,
                    space,
                }
            }
            o => o,
        }
    }

    pub fn phys(&self, s: Slot) -> Phys {
        match (s, self.form()) {
            (Slot::A, _) => Phys::R24,
            (Slot::B, 1) => Phys::R32,
            (Slot::B, 2 | 3 | 7) => Phys::R64,
            (Slot::B, 4) => Phys::Imm,
            (Slot::B, 5) => Phys::Cb,
            (Slot::B, 6) => Phys::U32,
            (Slot::C, 1 | 4 | 5 | 6) => Phys::R64,
            (Slot::C, 2) => Phys::Imm,
            (Slot::C, 3) => Phys::Cb,
            (Slot::C, 7) => Phys::U32,
            _ => Phys::Bad,
        }
    }

    pub fn src(&self, s: Slot, n: Num, mods: bool) -> Result<Operand> {
        self.src_phys(self.phys(s), s, n, mods)
    }

    pub fn reg_reuse(&self, lo: u32, s: Slot) -> Operand {
        let b = match s {
            Slot::A => 122,
            Slot::B => 123,
            Slot::C => 124,
        };
        let n = self.get(lo, 8) as u8;
        Operand::Reg(
            n,
            RegMods {
                reuse: n != 255 && self.bit(b),
                ..RegMods::default()
            },
        )
    }

    pub fn src_phys(&self, p: Phys, s: Slot, n: Num, mods: bool) -> Result<Operand> {
        let int = matches!(n, Num::Int | Num::UInt);
        let (o, nb, ab) = match p {
            Phys::R24 => (self.reg_reuse(24, s), 72, 73),
            Phys::R32 => (self.reg_reuse(32, s), 63, 62),
            Phys::R64 => (self.reg_reuse(64, s), 75, 74),
            Phys::Imm => return Ok(self.imm32(n)),
            Phys::Cb => (self.cbuf_at(32, Some(91)), 63, 62),
            Phys::U32 => (self.ureg_at(32), 63, 62),
            Phys::Bad => return self.err(format!("form {}", self.form())),
        };
        if !mods {
            return Ok(o);
        }
        Ok(D::with_mods(o, self.bit(nb), self.bit(ab), int))
    }

    pub fn src_neg_bit(&self, s: Slot) -> Option<u32> {
        match self.phys(s) {
            Phys::R24 => Some(72),
            Phys::R32 | Phys::Cb | Phys::U32 => Some(63),
            Phys::R64 => Some(75),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Phys {
    R24,
    R32,
    R64,
    Imm,
    Cb,
    U32,
    Bad,
}

pub fn decode(w: Word, pc: u64) -> Result<Inst> {
    let mut d = D { w, pc, i: mk("") };
    let g = d.get(12, 3) as u8;
    let gn = d.bit(15);
    let op9 = d.get(0, 9);
    let guard = if g == 7 && !gn {
        None
    } else if (0x80..0x100).contains(&op9) {
        Some(Operand::UPred(g, gn))
    } else {
        Some(Operand::Pred(g, gn))
    };
    let ok = alu::decode(&mut d, op9)?
        || ctl::decode(&mut d, op9)?
        || mem::decode(&mut d, op9)?
        || tex::decode(&mut d, op9)?;
    if !ok {
        return d.err(format!("opcode {:#05x}", d.get(0, 12)));
    }
    d.i.guard = guard;
    fields::fill(w, &mut d.i);
    Ok(d.i)
}

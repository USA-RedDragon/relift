//! A toy frontend for testing the core stages without a real CPU.
//!
//! Instructions are 4-byte little-endian words: the opcode in byte 0, a
//! register `a` in byte 1 and a 16-bit immediate in bytes 2 and 3 (a second
//! register `b` in byte 2 where noted). Branch offsets are signed and count
//! words from the instruction. Registers `r0` to `r3` are 64 bits (`r3` is
//! the link register, `r2` is kept across calls) plus a zero flag `z`; they live where the A64 runtime
//! keeps `x0` to `x3` and Z, so emitted toy code builds against the A64
//! runtime header.
//!
//! | op | instruction |
//! |---|---|
//! | 0x00 | `nop` |
//! | 0x01 | `b off` |
//! | 0x02 | `bl off` (link in `r3`) |
//! | 0x03 | `ret` (to `r3`) |
//! | 0x04 | `li ra, imm` (zero-extended) |
//! | 0x05 | `add ra, rb` |
//! | 0x06 | `br ra` |
//! | 0x07 | `cbz ra, off` |
//! | 0x08 | `trap imm` |
//! | 0x09 | `addi ra, imm` (sign-extended) |
//! | 0x0a | `ld ra, [rb]` (64 bits) |
//! | 0x0b | `st ra, [rb]` (64 bits) |
//! | 0x0c | `blr ra` |
//! | 0x0d | `cmpz ra` (sets `z`) |
//! | 0x0e | `bz off` (branch when `z`) |

use relift_ir::{
    Block, Builder, CmpOp, Endian, Exit, Flow, FpOp, Next, Order, Reg, RegDecl, RegFile, Width,
};

use crate::{Frontend, Home, LiftFailure};

static DECLS: [RegDecl; 5] = [
    RegDecl {
        name: "r0",
        width: Width::W64,
    },
    RegDecl {
        name: "r1",
        width: Width::W64,
    },
    RegDecl {
        name: "r2",
        width: Width::W64,
    },
    RegDecl {
        name: "r3",
        width: Width::W64,
    },
    RegDecl {
        name: "z",
        width: Width::W1,
    },
];

/// The zero flag.
pub const Z: Reg = Reg(4);
/// The link register.
pub const LR: Reg = Reg(3);

/// The toy frontend.
#[derive(Clone, Copy, Debug, Default)]
pub struct Toy;

/// Encodes one toy instruction.
pub fn word(op: u8, a: u8, imm: u16) -> [u8; 4] {
    let [lo, hi] = imm.to_le_bytes();
    [op, a, lo, hi]
}

/// Encodes a branch-type toy instruction with a word offset.
pub fn branch(op: u8, a: u8, off: i16) -> [u8; 4] {
    word(op, a, off as u16)
}

impl Frontend for Toy {
    fn granule(&self) -> u64 {
        4
    }

    fn regs(&self) -> RegFile {
        RegFile::new("toy", &DECLS)
    }

    fn stack_pointer(&self) -> Option<Reg> {
        Some(Reg(2))
    }

    fn pad_resume(&self) -> Option<&'static str> {
        Some("rl_toy_pad_resume")
    }

    fn preserved(&self) -> Vec<Reg> {
        vec![Reg(2)]
    }

    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        let w: [u8; 4] = bytes
            .get(..4)
            .and_then(|s| s.try_into().ok())
            .ok_or_else(|| LiftFailure {
                insn: 0,
                message: "past the end".into(),
            })?;
        let raw = u64::from(u32::from_le_bytes(w));
        let fail = || LiftFailure {
            insn: raw,
            message: format!("toy opcode {:#x} is undefined", w[0]),
        };
        let (op, a) = (w[0], Reg(u16::from(w[1] & 3)));
        let rb = Reg(u16::from(w[2] & 3));
        let imm = u16::from_le_bytes([w[2], w[3]]);
        let rel = pc.wrapping_add((i64::from(imm as i16) * 4) as u64);
        let next = pc + 4;
        let mut b = Builder::new(self.regs(), Width::W64, pc);
        let exit = |next: Next, flow: Flow| Exit { next, flow };
        let e = match op {
            0x00 => exit(Next::Const(next), Flow::Jump),
            0x01 => exit(Next::Const(rel), Flow::Jump),
            0x02 => {
                let l = b.konst(Width::W64, u128::from(next));
                b.set(LR, l);
                exit(Next::Const(rel), Flow::Call { ret: next })
            }
            0x03 => exit(Next::Dynamic(b.get(LR)), Flow::Return),
            0x04 => {
                let v = b.konst(Width::W64, u128::from(imm));
                b.set(a, v);
                exit(Next::Const(next), Flow::Jump)
            }
            0x05 => {
                let (x, y) = (b.get(a), b.get(rb));
                let s = b.add(x, y);
                b.set(a, s);
                exit(Next::Const(next), Flow::Jump)
            }
            0x06 => exit(Next::Dynamic(b.get(a)), Flow::Jump),
            0x07 => {
                let x = b.get(a);
                let z = b.konst(Width::W64, 0);
                let c = b.cmp(CmpOp::Eq, x, z);
                exit(
                    Next::Select {
                        cond: c,
                        taken: rel,
                        not_taken: next,
                    },
                    Flow::Jump,
                )
            }
            0x08 => exit(Next::Const(next), Flow::Trap(u32::from(imm))),
            0x09 => {
                let x = b.get(a);
                let k = b.konst(Width::W64, (i64::from(imm as i16)) as u64 as u128);
                let s = b.add(x, k);
                b.set(a, s);
                exit(Next::Const(next), Flow::Jump)
            }
            0x0a => {
                let p = b.get(rb);
                let v = b.load(p, Width::W64, Endian::Little, Order::Plain);
                b.set(a, v);
                exit(Next::Const(next), Flow::Jump)
            }
            0x0b => {
                let (p, v) = (b.get(rb), b.get(a));
                b.store(p, v, Endian::Little, Order::Plain);
                exit(Next::Const(next), Flow::Jump)
            }
            0x0c => {
                let t = b.get(a);
                let l = b.konst(Width::W64, u128::from(next));
                b.set(LR, l);
                exit(Next::Dynamic(t), Flow::Call { ret: next })
            }
            0x0d => {
                let x = b.get(a);
                let z = b.konst(Width::W64, 0);
                let c = b.cmp(CmpOp::Eq, x, z);
                b.set(Z, c);
                exit(Next::Const(next), Flow::Jump)
            }
            0x0e => {
                let c = b.get(Z);
                exit(
                    Next::Select {
                        cond: c,
                        taken: rel,
                        not_taken: next,
                    },
                    Flow::Jump,
                )
            }
            _ => return Err(fail()),
        };
        Ok(b.finish(4, e))
    }

    fn abi_headers(&self) -> &'static [&'static str] {
        &["relift/a64/abi.h"]
    }

    fn home(&self, reg: Reg) -> Home {
        if reg == Z {
            Home::Flag {
                word: "c->nzcv".into(),
                bit: 30,
            }
        } else {
            Home::Field(format!("c->x[{}]", reg.0))
        }
    }

    fn intrinsic(&self, _name: &str, _args: &[String]) -> Option<String> {
        None
    }

    fn trap(&self, code: u32) -> String {
        format!("rl_svc(c, mem, {code:#x}u)")
    }

    fn undefined(&self, pc: u64, insn: u64) -> String {
        format!("rl_undefined(c, mem, UINT64_C({pc:#x}), {insn:#x}u)")
    }

    fn monitor(&self) -> &'static str {
        "&c->excl"
    }

    fn fp(
        &self,
        _op: FpOp,
        _from: Width,
        _to: Width,
        _args: &[String],
        _reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        None
    }

    fn fp_state(&self) -> Vec<Reg> {
        Vec::new()
    }

    fn guard_state(&self, _code: u32) -> Option<Reg> {
        None
    }

    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        (code == 7).then(|| format!("toy_slow(c, mem, UINT64_C({pc:#x}), {insn:#x}u)"))
    }
}

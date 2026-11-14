//! A tiny synthetic guest that the core is tested against, so a guest assumption
//! in the core breaks a test.
//!
//! The guest is deliberately unlike ARM64: a 32-bit, big-endian machine with
//! four 32-bit registers `r0` to `r3`, one zero flag `z`, and 16-bit
//! instructions stored big-endian. An instruction is
//! `op:4 | rd:2 | rs:2 | imm:8`, high bits first:
//!
//! | op | name | meaning |
//! |---|---|---|
//! | 0 | `nop` | nothing |
//! | 1 | `li rd, imm` | `rd = sext(imm)` |
//! | 2 | `add rd, rs` | `rd += rs`; `z = rd == 0` |
//! | 3 | `sub rd, rs` | `rd -= rs`; `z = rd == 0` |
//! | 4 | `ld rd, [rs]` | `rd` = the big-endian word at `rs` |
//! | 5 | `st rd, [rs]` | the big-endian word at `rs` = `rd` |
//! | 6 | `bnz imm` | if `z == 0`, jump `sext(imm) * 2` bytes from the next instruction |
//! | 7 | `jr rs` | jump to `rs` |
//! | 8 | `sys imm` | trap to the runtime with code `imm` |
//!
//! Other opcodes are undefined.

use relift_ir::{Builder, Endian, Exit, Flow, Next, Order, Reg, RegDecl, RegFile, Width};

static DECLS: [RegDecl; 5] = [
    RegDecl {
        name: "r0",
        width: Width::W32,
    },
    RegDecl {
        name: "r1",
        width: Width::W32,
    },
    RegDecl {
        name: "r2",
        width: Width::W32,
    },
    RegDecl {
        name: "r3",
        width: Width::W32,
    },
    RegDecl {
        name: "z",
        width: Width::W1,
    },
];

/// The flag slot's index in [`regs`].
pub const Z: Reg = Reg(4);

/// The guest's register file: `r0` to `r3` at indexes 0 to 3, then `z`.
pub fn regs() -> RegFile {
    RegFile::new("testguest", &DECLS)
}

/// Encodes one instruction.
pub fn encode(op: u16, rd: u16, rs: u16, imm: u8) -> u16 {
    (op << 12) | ((rd & 3) << 10) | ((rs & 3) << 8) | u16::from(imm)
}

/// Lifts the instruction `word` at `pc`, or `None` when it is undefined.
pub fn lift(word: u16, pc: u32) -> Option<relift_ir::Block> {
    let op = word >> 12;
    let rd = Reg((word >> 10) & 3);
    let rs = Reg((word >> 8) & 3);
    let imm = (word & 0xff) as u8;
    let next = u64::from(pc.wrapping_add(2));
    let mut b = Builder::new(regs(), Width::W32, u64::from(pc));
    let flag = |b: &mut Builder, v| {
        let zero = b.konst(Width::W32, 0);
        let z = b.cmp(relift_ir::CmpOp::Eq, v, zero);
        b.set(Z, z);
    };
    match op {
        0 => {}
        1 => {
            let v = b.konst(Width::W32, imm as i8 as i32 as u32 as u128);
            b.set(rd, v);
        }
        2 | 3 => {
            let x = b.get(rd);
            let y = b.get(rs);
            let v = if op == 2 { b.add(x, y) } else { b.sub(x, y) };
            b.set(rd, v);
            flag(&mut b, v);
        }
        4 => {
            let a = b.get(rs);
            let v = b.load(a, Width::W32, Endian::Big, Order::Plain);
            b.set(rd, v);
        }
        5 => {
            let a = b.get(rs);
            let v = b.get(rd);
            b.store(a, v, Endian::Big, Order::Plain);
        }
        6 => {
            let z = b.get(Z);
            let target = pc
                .wrapping_add(2)
                .wrapping_add((imm as i8 as i32 as u32).wrapping_mul(2));
            return Some(b.finish(
                2,
                Exit {
                    next: Next::Select {
                        cond: z,
                        taken: next,
                        not_taken: u64::from(target),
                    },
                    flow: Flow::Jump,
                },
            ));
        }
        7 => {
            let t = b.get(rs);
            return Some(b.finish(
                2,
                Exit {
                    next: Next::Dynamic(t),
                    flow: Flow::Jump,
                },
            ));
        }
        8 => {
            return Some(b.finish(
                2,
                Exit {
                    next: Next::Const(next),
                    flow: Flow::Trap(u32::from(imm)),
                },
            ));
        }
        _ => return None,
    }
    Some(b.jump(2, next))
}

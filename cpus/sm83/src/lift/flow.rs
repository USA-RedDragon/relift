use relift_ir::{CmpOp, Flow, Next, Value, Width};

use super::{BUG, Cx, Done, HALF, Kind, LiftError, Lifted};
use crate::regs::{self, FC, FH, FN, FZ};
use crate::{Cond, Mnemonic, Opcode, Operand, Reg16, irq};

/// Whether `op` is a `CALL` or `RET` with a condition, which lifts to two
/// blocks.
pub(super) fn is_conditional_transfer(op: &Opcode) -> bool {
    matches!(op.mnemonic, Mnemonic::Call | Mnemonic::Ret) && matches!(op.dst, Operand::Cond(_))
}

fn condition(cx: &mut Cx, c: Cond) -> Value {
    let (flag, want) = match c {
        Cond::Nz => (FZ, false),
        Cond::Z => (FZ, true),
        Cond::Nc => (FC, false),
        Cond::C => (FC, true),
    };
    let f = cx.get(flag);
    if want { f } else { cx.b.not(f) }
}

fn cond_of(op: &Opcode) -> Option<Cond> {
    match op.dst {
        Operand::Cond(c) => Some(c),
        _ => None,
    }
}

fn done(next: Next, flow: Flow, kind: Kind) -> Lifted<Done> {
    Ok(Done { next, flow, kind })
}

fn push_return(cx: &mut Cx, ret: u16) -> Lifted<()> {
    let sp = cx.pair(Reg16::Sp);
    let one = cx.k16(1);
    let two = cx.k16(2);
    let hi_at = cx.b.sub(sp, one);
    let lo_at = cx.b.sub(sp, two);
    let hi = cx.k8((ret >> 8) as u8);
    let lo = cx.k8(ret as u8);
    cx.store(hi_at, hi)?;
    cx.store(lo_at, lo)?;
    cx.set_pair(Reg16::Sp, lo_at);
    Ok(())
}

fn pop_address(cx: &mut Cx) -> Lifted<Value> {
    let sp = cx.pair(Reg16::Sp);
    let one = cx.k16(1);
    let two = cx.k16(2);
    let hi_at = cx.b.add(sp, one);
    let lo = cx.load(sp)?;
    let hi = cx.load(hi_at)?;
    let after = cx.b.add(sp, two);
    cx.set_pair(Reg16::Sp, after);
    Ok(cx.b.concat(hi, lo))
}

fn decimal_adjust(cx: &mut Cx) {
    let a = cx.get(regs::A);
    let n = cx.get(FN);
    let h = cx.get(FH);
    let c = cx.get(FC);
    let not_n = cx.b.not(n);
    let mask = cx.k8(0x0F);
    let low = cx.b.and(a, mask);
    let nine = cx.k8(9);
    let low_big = cx.b.cmp(CmpOp::Ult, nine, low);
    let by_low = cx.b.and(not_n, low_big);
    let adj6 = cx.b.or(h, by_low);
    let top = cx.k8(0x99);
    let a_big = cx.b.cmp(CmpOp::Ult, top, a);
    let by_a = cx.b.and(not_n, a_big);
    let adj60 = cx.b.or(c, by_a);
    let six = cx.k8(6);
    let sixty = cx.k8(0x60);
    let zero = cx.k8(0);
    let lo_part = cx.b.select(adj6, six, zero);
    let hi_part = cx.b.select(adj60, sixty, zero);
    let adjust = cx.b.or(lo_part, hi_part);
    let up = cx.b.add(a, adjust);
    let down = cx.b.sub(a, adjust);
    let res = cx.b.select(n, down, up);
    cx.b.set(regs::A, res);
    let z = cx.is_zero(res);
    let no = cx.bit_const(false);
    cx.flags(Some(z), None, Some(no), Some(adj60));
}

pub(super) fn lift(cx: &mut Cx, op: &Opcode) -> Lifted<Done> {
    let pc = cx.pc;
    let after = cx.after();
    let fall = Next::Const(after);
    match op.mnemonic {
        Mnemonic::Nop => {
            cx.finish_insn()?;
            done(fall, Flow::Jump, Kind::Plain)
        }
        Mnemonic::Daa => {
            decimal_adjust(cx);
            cx.finish_insn()?;
            done(fall, Flow::Jump, Kind::Plain)
        }
        Mnemonic::Di => {
            cx.effect(irq::DI);
            cx.finish_insn()?;
            done(fall, Flow::Jump, Kind::Plain)
        }
        Mnemonic::Ei => {
            cx.finish_insn()?;
            done(fall, Flow::Jump, Kind::Plain)
        }
        Mnemonic::Halt => {
            cx.finish_insn()?;
            let skipped = cx.b.intrinsic(irq::HALT, &[], Some(Width::W1));
            done(
                Next::Select {
                    cond: skipped,
                    taken: BUG | u64::from(pc),
                    not_taken: after,
                },
                Flow::Jump,
                Kind::Control,
            )
        }
        Mnemonic::Stop => {
            cx.finish_insn()?;
            let one_byte = cx.b.intrinsic(irq::STOP, &[], Some(Width::W1));
            let two = cx.b.not(one_byte);
            cx.b.tick_if(two, 4);
            done(
                Next::Select {
                    cond: one_byte,
                    taken: after.wrapping_sub(1) & 0xFFFF,
                    not_taken: after,
                },
                Flow::Jump,
                Kind::Control,
            )
        }
        Mnemonic::Jr => {
            let Some(rel) = (match cx.imm {
                crate::Immediate::I8(e) => Some(e),
                _ => None,
            }) else {
                return Err(LiftError::Table("JR without an offset"));
            };
            let target = after.wrapping_add_signed(i64::from(rel)) & 0xFFFF;
            cx.finish_insn()?;
            branch(cx, op, target, after)
        }
        Mnemonic::Jp if matches!(op.src, Operand::R16(Reg16::Hl)) => {
            let hl = cx.pair(Reg16::Hl);
            cx.finish_insn()?;
            done(Next::Dynamic(hl), Flow::Jump, Kind::Control)
        }
        Mnemonic::Jp => {
            let target = u64::from(cx.imm16()?);
            cx.finish_insn()?;
            branch(cx, op, target, after)
        }
        Mnemonic::Call => {
            if let Some(c) = cond_of(op)
                && !cx.taken
            {
                cx.finish_insn()?;
                let cond = condition(cx, c);
                return done(
                    Next::Select {
                        cond,
                        taken: HALF | cx.view | u64::from(pc),
                        not_taken: after,
                    },
                    Flow::Jump,
                    Kind::Half,
                );
            }
            let target = u64::from(cx.imm16()?);
            push_return(cx, after as u16)?;
            cx.finish_insn()?;
            done(
                Next::Const(target),
                Flow::Call { ret: after },
                Kind::Control,
            )
        }
        Mnemonic::Rst => {
            let Operand::Vector(v) = op.dst else {
                return Err(LiftError::Table("RST without a vector"));
            };
            push_return(cx, after as u16)?;
            cx.finish_insn()?;
            done(
                Next::Const(u64::from(v)),
                Flow::Call { ret: after },
                Kind::Control,
            )
        }
        Mnemonic::Ret | Mnemonic::Reti => {
            if let Some(c) = cond_of(op)
                && !cx.taken
            {
                cx.finish_insn()?;
                let cond = condition(cx, c);
                return done(
                    Next::Select {
                        cond,
                        taken: HALF | cx.view | u64::from(pc),
                        not_taken: after,
                    },
                    Flow::Jump,
                    Kind::Half,
                );
            }
            let target = pop_address(cx)?;
            if op.mnemonic == Mnemonic::Reti {
                cx.effect(irq::RETI);
            }
            cx.finish_insn()?;
            done(Next::Dynamic(target), Flow::Return, Kind::Control)
        }
        _ => Err(LiftError::Table("not a control instruction")),
    }
}

fn branch(cx: &mut Cx, op: &Opcode, target: u64, after: u64) -> Lifted<Done> {
    match cond_of(op) {
        None => done(Next::Const(target), Flow::Jump, Kind::Control),
        Some(c) => {
            let cond = condition(cx, c);
            let extra = 4 * u32::from(op.cycles.taken - op.cycles.not_taken);
            cx.b.tick_if(cond, extra);
            done(
                Next::Select {
                    cond,
                    taken: target,
                    not_taken: after,
                },
                Flow::Jump,
                Kind::Control,
            )
        }
    }
}

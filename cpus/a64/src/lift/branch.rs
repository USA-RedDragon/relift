use relift_ir::{Builder, CmpOp, Exit, Flow, Next, Order, Width};

use super::{End, LiftError, LiftFn, condition_holds, read_x, sext_imm};
use crate::decode::{Encoding, Insn};
use crate::regs;

pub(super) fn lifter(enc: Encoding) -> Option<LiftFn> {
    use Encoding::*;
    Some(match enc {
        B_only_branch_imm | BL_only_branch_imm => branch_imm,
        B_only_condbranch => condbranch,
        CBZ_32_compbranch | CBNZ_32_compbranch | CBZ_64_compbranch | CBNZ_64_compbranch => {
            compbranch
        }
        TBZ_only_testbranch | TBNZ_only_testbranch => testbranch,
        BR_64_branch_reg | BLR_64_branch_reg | RET_64R_branch_reg => branch_reg,
        NOP_HI_hints => nop,
        CLREX_BN_barriers => clrex,
        DMB_BO_barriers | DSB_BO_barriers | ISB_BI_barriers => barrier,
        _ => return None,
    })
}

fn target(b: &Builder, offset: i64) -> u64 {
    b.pc().wrapping_add(offset as u64)
}

fn exit(next: Next, flow: Flow) -> End {
    End::Exit(Exit { next, flow })
}

fn branch_imm(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let t = target(b, sext_imm(insn.get("imm26"), 26) << 2);
    if insn.get("op") == 1 {
        let ret = b.pc().wrapping_add(4);
        let r = b.konst(Width::W64, u128::from(ret));
        b.set(regs::x(30), r);
        return Ok(exit(Next::Const(t), Flow::Call { ret }));
    }
    Ok(exit(Next::Const(t), Flow::Jump))
}

fn cond_exit(b: &Builder, cond: relift_ir::Value, offset: i64) -> End {
    exit(
        Next::Select {
            cond,
            taken: target(b, offset),
            not_taken: b.pc().wrapping_add(4),
        },
        Flow::Jump,
    )
}

fn condbranch(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let c = condition_holds(b, insn.get("cond"));
    Ok(cond_exit(b, c, sext_imm(insn.get("imm19"), 19) << 2))
}

fn compbranch(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = if insn.get("sf") == 1 {
        Width::W64
    } else {
        Width::W32
    };
    let x = read_x(b, insn.get("Rt"), w);
    let zero = b.konst(w, 0);
    let op = if insn.get("op") == 1 {
        CmpOp::Ne
    } else {
        CmpOp::Eq
    };
    let c = b.cmp(op, x, zero);
    Ok(cond_exit(b, c, sext_imm(insn.get("imm19"), 19) << 2))
}

fn testbranch(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let bit = (insn.get("b5") << 5) | insn.get("b40");
    let x = read_x(b, insn.get("Rt"), Width::W64);
    let mut c = b.bit(x, bit);
    if insn.get("op") == 0 {
        c = b.not(c);
    }
    Ok(cond_exit(b, c, sext_imm(insn.get("imm14"), 14) << 2))
}

fn branch_reg(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let t = read_x(b, insn.get("Rn"), Width::W64);
    let flow = match insn.encoding {
        Encoding::BLR_64_branch_reg => {
            let ret = b.pc().wrapping_add(4);
            let r = b.konst(Width::W64, u128::from(ret));
            b.set(regs::x(30), r);
            Flow::Call { ret }
        }
        Encoding::RET_64R_branch_reg => Flow::Return,
        _ => Flow::Jump,
    };
    Ok(exit(Next::Dynamic(t), flow))
}

fn nop(_insn: Insn, _b: &mut Builder) -> Result<End, LiftError> {
    Ok(End::Next)
}

fn clrex(_insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    b.clear_exclusive();
    Ok(End::Next)
}

fn barrier(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    if insn.encoding != Encoding::ISB_BI_barriers {
        b.fence(Order::SeqCst);
    }
    Ok(End::Next)
}

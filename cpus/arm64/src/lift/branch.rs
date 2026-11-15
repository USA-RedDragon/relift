use relift_ir::{Builder, CmpOp, Exit, Flow, Next, Width};

use super::{End, LiftError, LiftFn, condition_holds, read_x, sext_imm};
use crate::decode::{Encoding, Insn};

pub(super) fn lifter(enc: Encoding) -> Option<LiftFn> {
    use Encoding::*;
    Some(match enc {
        B_only_condbranch => condbranch,
        CBZ_32_compbranch | CBZ_64_compbranch => compbranch,
        CLREX_BN_barriers => clrex,
        _ => return None,
    })
}

fn target(b: &Builder, offset: i64) -> u64 {
    b.pc().wrapping_add(offset as u64)
}

fn exit(next: Next, flow: Flow) -> End {
    End::Exit(Exit { next, flow })
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

fn clrex(_insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    b.clear_exclusive();
    Ok(End::Next)
}

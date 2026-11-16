//! Atomics: ATOM and ATOMG (global, through buffer device addresses), RED
//! (the same without a result) and ATOMS (shared).
//!
//! Operations SPIR-V has no instruction for (INC, DEC, and the float ones)
//! run as a compare-exchange loop. The generic `ATOM` is treated as global.
//! Atomic floats are done on the bit pattern, so no device float atomic
//! extension is needed, with the guest's flush-to-zero rounding on any
//! host. 64-bit global atomics take a lock (see [`super::atomic64`]) on
//! hosts without them.

use super::scratch::Space;
use super::{addr_operand, spv, store_words};
use crate::spirv::op;
use crate::translate::float::Round;
use crate::translate::{Ctx, Res, bad};
use crate::{Inst, Operand};

const SCOPE_DEVICE: u32 = 1;
const SCOPE_WORKGROUP: u32 = 2;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Aop {
    Add,
    Min,
    Max,
    Inc,
    Dec,
    And,
    Or,
    Xor,
    Exch,
    Cas,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Ty {
    U32,
    S32,
    U64,
    S64,
    F32,
}

impl Ty {
    fn wide(self) -> bool {
        matches!(self, Ty::U64 | Ty::S64)
    }

    fn signed(self) -> bool {
        matches!(self, Ty::S32 | Ty::S64)
    }
}

struct Spec {
    op: Aop,
    ty: Ty,
}

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "ATOMG" | "ATOM" => atom(c, i),
        "RED" => red(c, i),
        "ATOMS" => atoms(c, i),
        _ => return None,
    })
}

fn spec_of(i: &Inst) -> Res<Spec> {
    let mut op = None;
    let mut ty = Ty::U32;
    for m in &i.mods {
        match *m {
            "ADD" => op = Some(Aop::Add),
            "MIN" => op = Some(Aop::Min),
            "MAX" => op = Some(Aop::Max),
            "INC" => op = Some(Aop::Inc),
            "DEC" => op = Some(Aop::Dec),
            "AND" => op = Some(Aop::And),
            "OR" => op = Some(Aop::Or),
            "XOR" => op = Some(Aop::Xor),
            "EXCH" => op = Some(Aop::Exch),
            "CAS" | "CAST" => op = Some(Aop::Cas),
            "SPIN" => {}
            "S32" => ty = Ty::S32,
            "64" => ty = Ty::U64,
            "S64" => ty = Ty::S64,
            "F32.FTZ.RN" => ty = Ty::F32,
            "E" | "EF" | "EL" | "LU" | "EU" | "NA" => {}
            m if m.starts_with("STRONG") || m.starts_with("CONSTANT") || m.starts_with("LTC") => {}
            m => return bad(format!("{}.{m}", i.mnemonic)),
        }
    }
    let Some(op) = op else {
        return bad(format!("{} without an operation", i.mnemonic));
    };
    if ty == Ty::F32 && !matches!(op, Aop::Add | Aop::Min | Aop::Max | Aop::Exch) {
        return bad("float atomic operation");
    }
    Ok(Spec { op, ty })
}

fn atom(c: &mut Ctx, i: &Inst) -> Res<()> {
    let s = spec_of(i)?;
    let cas = s.op == Aop::Cas;
    if i.ops.len() != 4 + usize::from(cas) {
        return bad(format!("{} operands", i.mnemonic));
    }
    if !matches!(i.ops[0], Operand::Pred(7, false)) {
        return bad("ATOMG with a predicate output");
    }
    let d = Ctx::dst_reg(&i.ops[1])?;
    let a = addr_operand(i, 2)?;
    let x = Ctx::dst_reg(&i.ops[3])?;
    let y = if cas {
        Some(Ctx::dst_reg(&i.ops[4])?)
    } else {
        None
    };
    let addr = c.global_address(&a)?;
    let old = global_rmw(c, addr, &s, x, y)?;
    write_old(c, d, &s, old);
    Ok(())
}

fn red(c: &mut Ctx, i: &Inst) -> Res<()> {
    let s = spec_of(i)?;
    if s.op == Aop::Cas || i.ops.len() != 2 {
        return bad("RED form");
    }
    let a = addr_operand(i, 0)?;
    let x = Ctx::dst_reg(&i.ops[1])?;
    let addr = c.global_address(&a)?;
    global_rmw(c, addr, &s, x, None)?;
    Ok(())
}

fn atoms(c: &mut Ctx, i: &Inst) -> Res<()> {
    let s = spec_of(i)?;
    let cas = s.op == Aop::Cas;
    if s.ty.wide() || s.ty == Ty::F32 {
        return bad("64-bit or float shared atomic");
    }
    if i.ops.len() != 3 + usize::from(cas) {
        return bad("ATOMS operands");
    }
    let d = Ctx::dst_reg(&i.ops[0])?;
    let a = addr_operand(i, 1)?;
    let x = Ctx::dst_reg(&i.ops[2])?;
    let y = if cas {
        Some(Ctx::dst_reg(&i.ops[3])?)
    } else {
        None
    };
    let addr = c.scratch_address(&a, 4, Space::Shared)?;
    let ptr = c.scratch_word(Space::Shared, addr, 0);
    let u = c.u32t();
    let xv = c.load_reg(x);
    let yv = y.map(|y| c.load_reg(y));
    let scope = c.m.c_u32(SCOPE_WORKGROUP);
    let old = rmw(c, ptr, scope, &s, u, xv, yv);
    write_old(c, d, &s, old);
    Ok(())
}

fn global_rmw(c: &mut Ctx, addr: u32, s: &Spec, x: u8, y: Option<u8>) -> Res<u32> {
    let t = if s.ty.wide() {
        c.cap(12);
        c.m.t_u64()
    } else {
        c.u32t()
    };
    let load = |c: &mut Ctx, r: u8| {
        if s.ty.wide() {
            let lo = c.load_reg(r);
            let hi = c.load_reg(r.wrapping_add(1));
            let u = c.u32t();
            let v2 = c.m.t_vec(u, 2);
            let pair = c.m.emit(op::COMPOSITE_CONSTRUCT, v2, &[lo, hi]);
            c.m.emit(op::BITCAST, t, &[pair])
        } else {
            c.load_reg(r)
        }
    };
    let xv = load(c, x);
    let yv = y.map(|y| load(c, y));
    if s.ty.wide() && !c.host.int64_atomics_buffer {
        return Ok(c.locked_rmw64(addr, |c, old| apply64(c, s, t, old, xv, yv)));
    }
    let ptr = c.global_ptr(addr, t);
    let scope = c.m.c_u32(SCOPE_DEVICE);
    Ok(rmw(c, ptr, scope, s, t, xv, yv))
}

fn write_old(c: &mut Ctx, d: u8, s: &Spec, old: u32) {
    if d == 255 {
        return;
    }
    if s.ty.wide() {
        let u = c.u32t();
        let v2 = c.m.t_vec(u, 2);
        let pair = c.m.emit(op::BITCAST, v2, &[old]);
        let lo = c.m.emit(op::COMPOSITE_EXTRACT, u, &[pair, 0]);
        let hi = c.m.emit(op::COMPOSITE_EXTRACT, u, &[pair, 1]);
        store_words(c, d, &[lo, hi]);
    } else {
        store_words(c, d, &[old]);
    }
}

fn rmw(c: &mut Ctx, ptr: u32, scope: u32, s: &Spec, t: u32, x: u32, y: Option<u32>) -> u32 {
    let sem = c.m.c_u32(0);
    let signed = s.ty.signed();
    let simple = |c: &mut Ctx, o: u16| c.m.emit(o, t, &[ptr, scope, sem, x]);
    if s.ty == Ty::F32 {
        return match s.op {
            Aop::Exch => simple(c, spv::ATOMIC_EXCHANGE),
            o => cas_loop(c, ptr, scope, t, |c, old| match o {
                Aop::Add => c.fadd_exact(old, x, Round::Nearest, true),
                Aop::Min => c.fminmax(old, x, true).0,
                _ => c.fminmax(old, x, true).1,
            }),
        };
    }
    match s.op {
        Aop::Add => simple(c, spv::ATOMIC_I_ADD),
        Aop::Min => simple(
            c,
            if signed {
                spv::ATOMIC_S_MIN
            } else {
                spv::ATOMIC_U_MIN
            },
        ),
        Aop::Max => simple(
            c,
            if signed {
                spv::ATOMIC_S_MAX
            } else {
                spv::ATOMIC_U_MAX
            },
        ),
        Aop::And => simple(c, spv::ATOMIC_AND),
        Aop::Or => simple(c, spv::ATOMIC_OR),
        Aop::Xor => simple(c, spv::ATOMIC_XOR),
        Aop::Exch => simple(c, spv::ATOMIC_EXCHANGE),
        Aop::Cas => {
            let new = y.unwrap_or(x);
            c.m.emit(
                spv::ATOMIC_COMPARE_EXCHANGE,
                t,
                &[ptr, scope, sem, sem, new, x],
            )
        }
        Aop::Inc => cas_loop(c, ptr, scope, t, |c, old| {
            let b = c.boolt();
            let one = const_of(c, t, 1);
            let zero = const_of(c, t, 0);
            let ge = c.m.emit(op::U_GREATER_THAN_EQUAL, b, &[old, x]);
            let inc = c.m.emit(op::I_ADD, t, &[old, one]);
            c.select(t, ge, zero, inc)
        }),
        Aop::Dec => cas_loop(c, ptr, scope, t, |c, old| {
            let b = c.boolt();
            let one = const_of(c, t, 1);
            let zero = const_of(c, t, 0);
            let z = c.m.emit(op::I_EQUAL, b, &[old, zero]);
            let gt = c.m.emit(op::U_GREATER_THAN, b, &[old, x]);
            let reset = c.m.emit(op::LOGICAL_OR, b, &[z, gt]);
            let dec = c.m.emit(op::I_SUB, t, &[old, one]);
            c.select(t, reset, x, dec)
        }),
    }
}

/// The new value of a 64-bit atomic from the old one.
fn apply64(c: &mut Ctx, s: &Spec, t: u32, old: u32, x: u32, y: Option<u32>) -> u32 {
    let b = c.boolt();
    let signed = s.ty.signed();
    let pick = |c: &mut Ctx, o: u16, keep_old_if: bool| {
        let cmp = c.m.emit(o, b, &[old, x]);
        if keep_old_if {
            c.select(t, cmp, old, x)
        } else {
            c.select(t, cmp, x, old)
        }
    };
    match s.op {
        Aop::Add => c.m.emit(op::I_ADD, t, &[old, x]),
        Aop::Min => pick(
            c,
            if signed {
                op::S_LESS_THAN
            } else {
                op::U_LESS_THAN
            },
            true,
        ),
        Aop::Max => pick(
            c,
            if signed {
                op::S_GREATER_THAN
            } else {
                op::U_GREATER_THAN
            },
            true,
        ),
        Aop::And => c.m.emit(op::BITWISE_AND, t, &[old, x]),
        Aop::Or => c.m.emit(op::BITWISE_OR, t, &[old, x]),
        Aop::Xor => c.m.emit(op::BITWISE_XOR, t, &[old, x]),
        Aop::Exch => x,
        Aop::Cas => {
            let new = y.unwrap_or(x);
            let eq = c.m.emit(op::I_EQUAL, b, &[old, x]);
            c.select(t, eq, new, old)
        }
        Aop::Inc => {
            let one = const_of(c, t, 1);
            let zero = const_of(c, t, 0);
            let ge = c.m.emit(op::U_GREATER_THAN_EQUAL, b, &[old, x]);
            let inc = c.m.emit(op::I_ADD, t, &[old, one]);
            c.select(t, ge, zero, inc)
        }
        Aop::Dec => {
            let one = const_of(c, t, 1);
            let zero = const_of(c, t, 0);
            let z = c.m.emit(op::I_EQUAL, b, &[old, zero]);
            let gt = c.m.emit(op::U_GREATER_THAN, b, &[old, x]);
            let reset = c.m.emit(op::LOGICAL_OR, b, &[z, gt]);
            let dec = c.m.emit(op::I_SUB, t, &[old, one]);
            c.select(t, reset, x, dec)
        }
    }
}

fn const_of(c: &mut Ctx, t: u32, v: u64) -> u32 {
    if t == c.m.t_u64() {
        c.m.c_u64(v)
    } else {
        c.m.c_u32(v as u32)
    }
}

/// Repeats `old -> f(old)` with a compare-exchange until it lands; gives
/// the value that was replaced.
fn cas_loop(
    c: &mut Ctx,
    ptr: u32,
    scope: u32,
    t: u32,
    f: impl FnOnce(&mut Ctx, u32) -> u32,
) -> u32 {
    let sem = c.m.c_u32(0);
    let header = c.m.id();
    let body = c.m.id();
    let cont = c.m.id();
    let merge = c.m.id();
    c.m.emit_void(op::BRANCH, &[header]);
    c.m.emit_void(op::LABEL, &[header]);
    c.m.emit_void(spv::LOOP_MERGE, &[merge, cont, 0]);
    c.m.emit_void(op::BRANCH, &[body]);
    c.m.emit_void(op::LABEL, &[body]);
    let old = c.m.emit(spv::ATOMIC_LOAD, t, &[ptr, scope, sem]);
    let new = f(c, old);
    let prev = c.m.emit(
        spv::ATOMIC_COMPARE_EXCHANGE,
        t,
        &[ptr, scope, sem, sem, new, old],
    );
    let b = c.boolt();
    let done = c.m.emit(op::I_EQUAL, b, &[prev, old]);
    c.m.emit_void(op::BRANCH_CONDITIONAL, &[done, merge, cont]);
    c.m.emit_void(op::LABEL, &[cont]);
    c.m.emit_void(op::BRANCH, &[header]);
    c.m.emit_void(op::LABEL, &[merge]);
    old
}

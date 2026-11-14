//! The small functions every module defines for operations WebAssembly has
//! no instruction for, and the dispatch loop of modules without tail calls.

use wasm_encoder::{BlockType, Function, Instruction as I, ValType};

use crate::func::ma;
use crate::{
    Helper, RC_CHAIN, T_F32, T_F64, T_LIFTED, T_MULHI, T_PAIR, T_RUN, T_SH128, T_UN64, ctx, rt,
};

/// The type index of helper `h`.
pub(crate) fn type_of(h: Helper) -> u32 {
    match h {
        Helper::MulHi => T_MULHI,
        Helper::Shl128 | Helper::LShr128 | Helper::AShr128 => T_SH128,
        Helper::RoundAway32 => T_F32,
        Helper::RoundAway64 => T_F64,
        Helper::BitReverse64 | Helper::ByteSwap64 => T_UN64,
        Helper::Run => T_RUN,
    }
}

fn emit(f: &mut Function, xs: &[I<'_>]) {
    for x in xs {
        f.instruction(x);
    }
}

/// Swaps the groups of `bits` bits under `mask` in the `i64` in local 0.
fn swap_step(f: &mut Function, bits: i64, mask: i64) {
    emit(
        f,
        &[
            I::LocalGet(0),
            I::I64Const(bits),
            I::I64ShrU,
            I::I64Const(mask),
            I::I64And,
            I::LocalGet(0),
            I::I64Const(mask),
            I::I64And,
            I::I64Const(bits),
            I::I64Shl,
            I::I64Or,
            I::LocalSet(0),
        ],
    );
}

fn byte_steps(f: &mut Function) {
    swap_step(f, 8, 0x00ff_00ff_00ff_00ff);
    swap_step(f, 16, 0x0000_ffff_0000_ffff);
    emit(f, &[I::LocalGet(0), I::I64Const(32), I::I64Rotl, I::End]);
}

/// The body of helper `h`.
pub(crate) fn body(h: Helper) -> Function {
    match h {
        Helper::MulHi => mulhi(),
        Helper::Shl128 => shift128(0),
        Helper::LShr128 => shift128(1),
        Helper::AShr128 => shift128(2),
        Helper::RoundAway32 => round_away(true),
        Helper::RoundAway64 => round_away(false),
        Helper::BitReverse64 => {
            let mut f = Function::new([]);
            swap_step(&mut f, 1, 0x5555_5555_5555_5555);
            swap_step(&mut f, 2, 0x3333_3333_3333_3333);
            swap_step(&mut f, 4, 0x0f0f_0f0f_0f0f_0f0f);
            byte_steps(&mut f);
            f
        }
        Helper::ByteSwap64 => {
            let mut f = Function::new([]);
            byte_steps(&mut f);
            f
        }
        Helper::Run => {
            let mut f = Function::new([]);
            emit(
                &mut f,
                &[
                    I::Block(BlockType::Empty),
                    I::Loop(BlockType::Empty),
                    I::LocalGet(2),
                    I::I32Const(RC_CHAIN),
                    I::I32Ne,
                    I::BrIf(1),
                    I::LocalGet(0),
                    I::LocalGet(1),
                    I::LocalGet(0),
                    I::LocalGet(0),
                    I::I64Load(ma(u64::from(ctx::PC))),
                    I::Call(rt::LOOKUP),
                    I::CallIndirect {
                        type_index: T_LIFTED,
                        table_index: 0,
                    },
                    I::LocalSet(2),
                    I::Br(0),
                    I::End,
                    I::End,
                    I::LocalGet(2),
                    I::End,
                ],
            );
            f
        }
    }
}

/// The high half of the unsigned 128-bit product of two `i64`s, by 32-bit
/// limbs.
fn mulhi() -> Function {
    let mut f = Function::new([(6, ValType::I64)]);
    let m32 = 0xffff_ffffi64;
    emit(
        &mut f,
        &[
            I::LocalGet(0),
            I::I64Const(m32),
            I::I64And,
            I::LocalSet(2),
            I::LocalGet(0),
            I::I64Const(32),
            I::I64ShrU,
            I::LocalSet(3),
            I::LocalGet(1),
            I::I64Const(m32),
            I::I64And,
            I::LocalSet(4),
            I::LocalGet(1),
            I::I64Const(32),
            I::I64ShrU,
            I::LocalSet(5),
            I::LocalGet(2),
            I::LocalGet(4),
            I::I64Mul,
            I::I64Const(32),
            I::I64ShrU,
            I::LocalSet(6),
            I::LocalGet(3),
            I::LocalGet(4),
            I::I64Mul,
            I::LocalGet(6),
            I::I64Add,
            I::LocalSet(6),
            I::LocalGet(6),
            I::I64Const(m32),
            I::I64And,
            I::LocalGet(2),
            I::LocalGet(5),
            I::I64Mul,
            I::I64Add,
            I::LocalSet(7),
            I::LocalGet(3),
            I::LocalGet(5),
            I::I64Mul,
            I::LocalGet(6),
            I::I64Const(32),
            I::I64ShrU,
            I::I64Add,
            I::LocalGet(7),
            I::I64Const(32),
            I::I64ShrU,
            I::I64Add,
            I::End,
        ],
    );
    f
}

/// `(lo, hi)` shifted by `k` below 128: left, logical right, or arithmetic
/// right (`kind` 0, 1, 2).
fn shift128(kind: u8) -> Function {
    let mut f = Function::new([]);
    let (lo, hi, k) = (0, 1, 2);
    emit(
        &mut f,
        &[
            I::LocalGet(k),
            I::I64Eqz,
            I::If(BlockType::FunctionType(T_PAIR)),
            I::LocalGet(lo),
            I::LocalGet(hi),
            I::Else,
            I::LocalGet(k),
            I::I64Const(64),
            I::I64LtU,
            I::If(BlockType::FunctionType(T_PAIR)),
        ],
    );
    if kind == 0 {
        emit(
            &mut f,
            &[
                I::LocalGet(lo),
                I::LocalGet(k),
                I::I64Shl,
                I::LocalGet(hi),
                I::LocalGet(k),
                I::I64Shl,
                I::LocalGet(lo),
                I::I64Const(64),
                I::LocalGet(k),
                I::I64Sub,
                I::I64ShrU,
                I::I64Or,
                I::Else,
                I::I64Const(0),
                I::LocalGet(lo),
                I::LocalGet(k),
                I::I64Const(64),
                I::I64Sub,
                I::I64Shl,
            ],
        );
    } else {
        let sh = if kind == 1 { I::I64ShrU } else { I::I64ShrS };
        emit(
            &mut f,
            &[
                I::LocalGet(lo),
                I::LocalGet(k),
                I::I64ShrU,
                I::LocalGet(hi),
                I::I64Const(64),
                I::LocalGet(k),
                I::I64Sub,
                I::I64Shl,
                I::I64Or,
                I::LocalGet(hi),
                I::LocalGet(k),
                sh.clone(),
                I::Else,
                I::LocalGet(hi),
                I::LocalGet(k),
                I::I64Const(64),
                I::I64Sub,
                sh,
            ],
        );
        if kind == 1 {
            f.instruction(&I::I64Const(0));
        } else {
            emit(&mut f, &[I::LocalGet(hi), I::I64Const(63), I::I64ShrS]);
        }
    }
    emit(&mut f, &[I::End, I::End, I::End]);
    f
}

/// Rounds to an integral value, ties away from zero: `t = trunc(x)`, then
/// `t + copysign(1, x)` when `|x - t| >= 0.5`.
fn round_away(f32: bool) -> Function {
    let ty = if f32 { ValType::F32 } else { ValType::F64 };
    let mut f = Function::new([(1, ty)]);
    let xs: Vec<I<'_>> = if f32 {
        vec![
            I::LocalGet(0),
            I::F32Trunc,
            I::LocalSet(1),
            I::LocalGet(1),
            I::F32Const(1.0.into()),
            I::LocalGet(0),
            I::F32Copysign,
            I::F32Add,
            I::LocalGet(1),
            I::LocalGet(0),
            I::LocalGet(1),
            I::F32Sub,
            I::F32Abs,
            I::F32Const(0.5.into()),
            I::F32Ge,
            I::Select,
            I::End,
        ]
    } else {
        vec![
            I::LocalGet(0),
            I::F64Trunc,
            I::LocalSet(1),
            I::LocalGet(1),
            I::F64Const(1.0.into()),
            I::LocalGet(0),
            I::F64Copysign,
            I::F64Add,
            I::LocalGet(1),
            I::LocalGet(0),
            I::LocalGet(1),
            I::F64Sub,
            I::F64Abs,
            I::F64Const(0.5.into()),
            I::F64Ge,
            I::Select,
            I::End,
        ]
    };
    emit(&mut f, &xs);
    f
}

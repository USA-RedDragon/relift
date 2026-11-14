//! The small functions every module defines for operations WebAssembly has
//! no instruction for, and the dispatch loop of modules without tail calls.

use relift_ir::CryptoOp;
use relift_ir::interp::eval_crypto;
use wasm_encoder::{BlockType, Function, Instruction as I, ValType};

use crate::func::ma;
use crate::{
    Helper, ModuleCx, RC_CHAIN, T_BYTE, T_D2H, T_F32, T_F64, T_H2D, T_LIFTED, T_MULHI, T_PAIR,
    T_RUN, T_SH128, T_UN64, ctx, rt,
};

/// The type index of helper `h`.
pub(crate) fn type_of(h: Helper) -> u32 {
    match h {
        Helper::MulHi => T_MULHI,
        Helper::Shl128 | Helper::LShr128 | Helper::AShr128 => T_SH128,
        Helper::RoundAway32 => T_F32,
        Helper::RoundAway64 => T_F64,
        Helper::BitReverse64
        | Helper::ByteSwap64
        | Helper::MixColumns64
        | Helper::InvMixColumns64
        | Helper::SubBytesAes64
        | Helper::SubBytesAesInv64 => T_UN64,
        Helper::Run => T_RUN,
        Helper::F16ToF64 => T_H2D,
        Helper::F64ToF16 => T_D2H,
        Helper::SboxAes | Helper::SboxAesInv | Helper::SboxSm4 => T_BYTE,
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

/// The body of helper `h` in the module `cx`.
pub(crate) fn body(h: Helper, cx: &ModuleCx<'_>) -> Function {
    match h {
        Helper::F16ToF64 => f16_to_f64(),
        Helper::F64ToF16 => f64_to_f16(),
        Helper::MixColumns64 => mix_columns(false),
        Helper::InvMixColumns64 => mix_columns(true),
        Helper::SboxAes => sbox(CryptoOp::AesSubBytes),
        Helper::SboxAesInv => sbox(CryptoOp::AesInvSubBytes),
        Helper::SboxSm4 => sbox(CryptoOp::Sm4Sbox),
        Helper::SubBytesAes64 => sub_bytes(cx.helper(Helper::SboxAes)),
        Helper::SubBytesAesInv64 => sub_bytes(cx.helper(Helper::SboxAesInv)),
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

/// The binary64 value of the binary16 pattern in the `i64` local 0, exact:
/// a subnormal or zero is its significand times 2^-24 with the sign; any
/// other pattern moves its fields to binary64 (exponent rebiased by 1008,
/// all ones for infinity and NaN, the fraction on top so a NaN keeps its
/// payload).
fn f16_to_f64() -> Function {
    let mut f = Function::new([(1, ValType::I64)]);
    let (h, e) = (0, 1);
    let sign = [
        I::LocalGet(h),
        I::I64Const(15),
        I::I64ShrU,
        I::I64Const(63),
        I::I64Shl,
    ];
    let frac = [I::LocalGet(h), I::I64Const(0x3ff), I::I64And];
    emit(
        &mut f,
        &[
            I::LocalGet(h),
            I::I64Const(10),
            I::I64ShrU,
            I::I64Const(0x1f),
            I::I64And,
            I::LocalSet(e),
            I::LocalGet(e),
            I::I64Eqz,
            I::If(BlockType::Result(ValType::F64)),
        ],
    );
    emit(&mut f, &frac);
    emit(
        &mut f,
        &[
            I::F64ConvertI64U,
            I::F64Const(f64::from_bits(0x3e70_0000_0000_0000).into()),
            I::F64Mul,
        ],
    );
    emit(&mut f, &sign);
    emit(&mut f, &[I::F64ReinterpretI64, I::F64Copysign, I::Else]);
    emit(&mut f, &sign);
    emit(
        &mut f,
        &[
            I::I64Const(0x7ff),
            I::LocalGet(e),
            I::I64Const(1008),
            I::I64Add,
            I::LocalGet(e),
            I::I64Const(31),
            I::I64Eq,
            I::Select,
            I::I64Const(52),
            I::I64Shl,
            I::I64Or,
        ],
    );
    emit(&mut f, &frac);
    emit(
        &mut f,
        &[
            I::I64Const(42),
            I::I64Shl,
            I::I64Or,
            I::F64ReinterpretI64,
            I::End,
            I::End,
        ],
    );
    f
}

/// The `f64` in local 0 rounded to nearest even binary16, as the IR's
/// `half::from_f64`: a NaN keeps its sign and the top of its payload,
/// quieted; 65520 and above overflow to infinity; below 2^-14 the result is
/// the subnormal `nearest(|x| * 2^24)` (exact scaling, one rounding, and
/// 1024 is the smallest normal's encoding); otherwise the exponent is
/// rebiased and the fraction rounded by adding half an ulp less one plus
/// the kept lowest bit, which also carries into the exponent.
fn f64_to_f16() -> Function {
    let mut f = Function::new([(3, ValType::I64)]);
    let (x, b, a, s) = (0, 1, 2, 3);
    emit(
        &mut f,
        &[
            I::LocalGet(x),
            I::I64ReinterpretF64,
            I::LocalTee(b),
            I::I64Const(48),
            I::I64ShrU,
            I::I64Const(0x8000),
            I::I64And,
            I::LocalSet(s),
            I::LocalGet(b),
            I::I64Const(0x7fff_ffff_ffff_ffff),
            I::I64And,
            I::LocalSet(a),
            I::LocalGet(a),
            I::I64Const(0x7ff0_0000_0000_0000),
            I::I64GtU,
            I::If(BlockType::Result(ValType::I64)),
            I::LocalGet(b),
            I::I64Const(42),
            I::I64ShrU,
            I::I64Const(0x3ff),
            I::I64And,
            I::I64Const(0x7e00),
            I::I64Or,
            I::Else,
            I::LocalGet(a),
            I::I64Const(0x40ef_fe00_0000_0000),
            I::I64GeU,
            I::If(BlockType::Result(ValType::I64)),
            I::I64Const(0x7c00),
            I::Else,
            I::LocalGet(a),
            I::I64Const(0x3f10_0000_0000_0000),
            I::I64LtU,
            I::If(BlockType::Result(ValType::I64)),
            I::LocalGet(a),
            I::F64ReinterpretI64,
            I::F64Const(16_777_216.0.into()),
            I::F64Mul,
            I::F64Nearest,
            I::I64TruncF64U,
            I::Else,
            I::LocalGet(a),
            I::I64Const(1008 << 52),
            I::I64Sub,
            I::LocalTee(a),
            I::I64Const((1 << 41) - 1),
            I::I64Add,
            I::LocalGet(a),
            I::I64Const(42),
            I::I64ShrU,
            I::I64Const(1),
            I::I64And,
            I::I64Add,
            I::I64Const(42),
            I::I64ShrU,
            I::End,
            I::End,
            I::End,
            I::LocalGet(s),
            I::I64Or,
            I::End,
        ],
    );
    f
}

/// Pushes `2 * x` in GF(2^8) for each byte of the `i64` in local `l`.
fn xtime(f: &mut Function, l: u32) {
    emit(
        f,
        &[
            I::LocalGet(l),
            I::I64Const(0x7f7f_7f7f_7f7f_7f7f),
            I::I64And,
            I::I64Const(1),
            I::I64Shl,
            I::LocalGet(l),
            I::I64Const(7),
            I::I64ShrU,
            I::I64Const(0x0101_0101_0101_0101),
            I::I64And,
            I::I64Const(0x1b),
            I::I64Mul,
            I::I64Xor,
        ],
    );
}

/// Pushes each 32-bit column of the `i64` in local `l` with its bytes
/// rotated down by `k`: byte `r` of a column becomes byte `r + k mod 4`.
fn rot_rows(f: &mut Function, l: u32, k: u32) {
    let keep: i64 = match k {
        1 => 0x00ff_ffff_00ff_ffff,
        2 => 0x0000_ffff_0000_ffff,
        _ => 0x0000_00ff_0000_00ff,
    };
    emit(
        f,
        &[
            I::LocalGet(l),
            I::I64Const(i64::from(8 * k)),
            I::I64ShrU,
            I::I64Const(keep),
            I::I64And,
            I::LocalGet(l),
            I::I64Const(i64::from(32 - 8 * k)),
            I::I64Shl,
            I::I64Const(!keep),
            I::I64And,
            I::I64Or,
        ],
    );
}

/// AES `MixColumns` (or `InvMixColumns`) on the two columns in the `i64`
/// in local 0, row 0 lowest: with `rk` the rows rotated by `k`, out is
/// `2 x ^ 3 r1 ^ r2 ^ r3 = xtime(x ^ r1) ^ r1 ^ r2 ^ r3`. The inverse is
/// `MixColumns` after `x ^= 4 (x ^ r2)`, since the circulant `[14 11 13 9]`
/// is `[2 3 1 1]` times `[5 0 4 0]`.
fn mix_columns(inverse: bool) -> Function {
    let mut f = Function::new([(2, ValType::I64)]);
    let (x, r1, t) = (0, 1, 2);
    if inverse {
        emit(&mut f, &[I::LocalGet(x)]);
        rot_rows(&mut f, x, 2);
        emit(&mut f, &[I::I64Xor, I::LocalSet(t)]);
        xtime(&mut f, t);
        emit(&mut f, &[I::LocalSet(t)]);
        xtime(&mut f, t);
        emit(&mut f, &[I::LocalGet(x), I::I64Xor, I::LocalSet(x)]);
    }
    rot_rows(&mut f, x, 1);
    emit(
        &mut f,
        &[
            I::LocalSet(r1),
            I::LocalGet(x),
            I::LocalGet(r1),
            I::I64Xor,
            I::LocalSet(t),
        ],
    );
    xtime(&mut f, t);
    emit(&mut f, &[I::LocalGet(r1), I::I64Xor]);
    rot_rows(&mut f, x, 2);
    emit(&mut f, &[I::I64Xor]);
    rot_rows(&mut f, x, 3);
    emit(&mut f, &[I::I64Xor, I::End]);
    f
}

/// A byte table lookup for the byte in the `i32` local 0: the 256 entries
/// of cipher step `op` (computed from `relift_ir`'s definition) as 32
/// `i64` words, one picked by a `br_table` on the byte's top five bits and
/// shifted by its low three.
fn sbox(op: CryptoOp) -> Function {
    let entry = |i: u64| -> u64 { (eval_crypto(op, u128::from(i)) & 0xff) as u64 };
    let words: Vec<i64> = (0..32u64)
        .map(|k| (0..8).fold(0u64, |w, j| w | (entry(8 * k + j) << (8 * j))) as i64)
        .collect();
    let mut f = Function::new([(1, ValType::I64)]);
    let word = 1;
    f.instruction(&I::Block(BlockType::Empty));
    for _ in 0..32 {
        f.instruction(&I::Block(BlockType::Empty));
    }
    let cases: Vec<u32> = (0..32).collect();
    emit(
        &mut f,
        &[
            I::LocalGet(0),
            I::I32Const(3),
            I::I32ShrU,
            I::BrTable(cases.into(), 31),
        ],
    );
    for (k, w) in words.iter().enumerate() {
        emit(&mut f, &[I::End, I::I64Const(*w), I::LocalSet(word)]);
        if k < 31 {
            f.instruction(&I::Br(31 - k as u32));
        }
    }
    emit(
        &mut f,
        &[
            I::End,
            I::LocalGet(word),
            I::LocalGet(0),
            I::I32Const(7),
            I::I32And,
            I::I32Const(3),
            I::I32Shl,
            I::I64ExtendI32U,
            I::I64ShrU,
            I::I32WrapI64,
            I::I32Const(0xff),
            I::I32And,
            I::End,
        ],
    );
    f
}

/// Each byte of the `i64` in local 0 through the byte table helper `table`.
fn sub_bytes(table: u32) -> Function {
    let mut f = Function::new([]);
    for k in 0..8i64 {
        emit(
            &mut f,
            &[
                I::LocalGet(0),
                I::I64Const(8 * k),
                I::I64ShrU,
                I::I32WrapI64,
                I::I32Const(0xff),
                I::I32And,
                I::Call(table),
                I::I64ExtendI32U,
                I::I64Const(8 * k),
                I::I64Shl,
            ],
        );
        if k > 0 {
            f.instruction(&I::I64Or);
        }
    }
    f.instruction(&I::End);
    f
}

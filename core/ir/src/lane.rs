//! Per-lane operations on 128-bit values.
//!
//! A 128-bit value is read as a vector of equal lanes of 8, 16, 32 or 64
//! bits ([`LaneWidth`]). **Lane 0 holds the least significant bits** of the
//! value, lane `n - 1` the most significant, so the value is what a
//! little-endian load of the lanes in memory order gives. Byte `i` of a value
//! is bits `8i .. 8i + 8`.
//!
//! The operations are generic: nothing here belongs to one guest. They are
//! chosen so that each has a direct or a two-instruction form in ARM NEON,
//! x86 SSE2 to SSE4.1 (SSSE3 for [`Permute`]) and WebAssembly SIMD128; the
//! table below is the contract for an emitter. All are pure.
//!
//! # Semantics
//!
//! Every operation reads its operands and writes its result lane by lane
//! unless it says otherwise. "Mask" lanes are all ones (true) or all zeros
//! (false). `lw` is the lane width in bits.
//!
//! | Kind | Operands | Result lane | Lane widths |
//! |------|----------|-------------|-------------|
//! | `Add`, `Sub` | `a`, `b` | wrapping sum, difference | 8 16 32 64 |
//! | `AddSatS`, `SubSatS` | `a`, `b` | signed saturating | 8 16 |
//! | `AddSatU`, `SubSatU` | `a`, `b` | unsigned saturating (difference clamps at 0) | 8 16 |
//! | `MulLo` | `a`, `b` | low `lw` bits of the product | 16 32 |
//! | `MulHiS`, `MulHiU`, `MulHiSU` | `a`, `b` | high `lw` bits of the `2 lw`-bit product; signed x signed, unsigned x unsigned, signed `a` x unsigned `b` | 16 32 |
//! | `And`, `Or`, `Xor`, `AndNot`, `Not` | `a`, `b` | `a & b`, `a \| b`, `a ^ b`, `a & !b`, `!a` (one operand) | bitwise, see below |
//! | `MinS`, `MinU`, `MaxS`, `MaxU` | `a`, `b` | signed or unsigned minimum, maximum | 8 16 32 |
//! | `Abs` | `a` | wrapping absolute value (the minimum stays) | 8 16 32 |
//! | `Neg` | `a` | wrapping negation | 8 16 32 64 |
//! | `Eq`, `Ne` | `a`, `b` | mask | 8 16 32 64 |
//! | `Slt`, `Sle`, `Ult`, `Ule` | `a`, `b` | mask, signed or unsigned `<`, `<=` | 8 16 32 |
//! | `ShlImm`, `LShrImm`, `AShrImm` | `a`, immediate `0 .. lw` | shift left, logical right, arithmetic right | 8 16 32 64 |
//! | `Shl`, `LShr`, `AShr` | `a`, `b` | shift by the unsigned lane of `b`; an amount of `lw` or more gives 0 (`AShr`: the sign fill) | 8 16 32 64 |
//! | `Select` | `m`, `a`, `b` | bitwise `m ? a : b`: `(m & a) \| (!m & b)` | bitwise |
//! | `WidenLoS`, `WidenLoU` | `a` | lane `i` of `a`, sign or zero extended to `2 lw` | 8 16 32 |
//! | `WidenHiS`, `WidenHiU` | `a` | lane `n/2 + i` of `a` (the upper half), extended | 8 16 32 |
//! | `NarrowTrunc` | `a`, `b` | the low `lw/2` bits of each source lane; `a` fills the low half of the result, `b` the high half | 16 32 64 |
//! | `NarrowSS` | `a`, `b` | signed source clamped to the signed `lw/2` range | 16 32 |
//! | `NarrowUU` | `a`, `b` | unsigned source clamped to the unsigned `lw/2` range | 16 32 |
//! | `NarrowSU` | `a`, `b` | signed source clamped to the unsigned `lw/2` range (negative gives 0) | 16 32 |
//! | `MulWideLo*`, `MulWideHi*` | `a`, `b` | full `2 lw`-bit product of lane `i` (low half) or `n/2 + i` (high half); suffix `SS`, `UU`, `SU` as for `MulHi` | 8 16 32 |
//! | `Broadcast(i)` | `a` | every lane is lane `i` of `a` | 8 16 32 64 |
//! | `Extract(i)` | `a` | **scalar** of `lw` bits, zero extended from lane `i` | 8 16 32 64 |
//! | `Insert(i)` | `a`, scalar `s` of `lw` bits | `a` with lane `i` replaced by `s` | 8 16 32 64 |
//! | `Splat` | scalar `s` of `lw` bits | every lane is `s` | 8 16 32 64 |
//!
//! The bitwise kinds (and `Select`) do not depend on the lane width; they
//! carry [`LaneWidth::L64`] and the verifier requires it, so that equal
//! operations compare equal.
//!
//! The lane-width columns are the widths that map to a single instruction or
//! a short fixed sequence on all three targets; the verifier rejects the
//! rest, and a frontend composes them from the others.
//!
//! # Host mapping
//!
//! Direct means one instruction; "2" and "3" count instructions on the
//! worst target. Widths not listed for a target use the same pattern.
//!
//! | Kind | NEON | SSE2 to SSE4.1 | wasm SIMD128 |
//! |------|------|----------------|--------------|
//! | `Add`, `Sub`, `Neg` | `add`, `sub`, `neg` | `padd{b,w,d,q}`, `psub*`, `psub*` from zero | `iNxM.add`, `.sub`, `.neg` |
//! | `AddSat*`, `SubSat*` | `sqadd uqadd sqsub uqsub` | `padds{b,w}`, `paddus*`, `psubs*`, `psubus*` | `iNxM.add_sat_{s,u}`, `.sub_sat_{s,u}` |
//! | `MulLo` | `mul` | `pmullw`; 32: `pmulld` (SSE4.1) | `i16x8.mul`, `i32x4.mul` |
//! | `MulHiS`, `MulHiU` (16) | `smull`+`smull2`, `uzp2` (2) | `pmulhw`, `pmulhuw` | `i32x4.extmul_{low,high}_i16x8_s`+shuffle (3) |
//! | `MulHiSU` (16) | `sshll`, `ushll` of both halves, `mul`, `shrn` (about 5) | `pmulhuw(a, b) - (b & psraw(a, 15))` (4) | extend both, `mul`, shift, narrow (about 5) |
//! | `And`, `Or`, `Xor`, `Not` | `and orr eor mvn` | `pand por pxor`, `pxor` with ones | `v128.and or xor not` |
//! | `AndNot` | `bic` | `pandn` (operands swapped) | `v128.andnot` |
//! | `Min*`, `Max*` | `smin umin smax umax` | `pmin{sb,sw,sd,ub,uw,ud}` (SSE4.1; SSE2 has `pminsw`, `pminub`, `pmaxsw`, `pmaxub` only, others 2 to 3) | `iNxM.min_{s,u}`, `.max_{s,u}` |
//! | `Abs` | `abs` | `pabs{b,w,d}` (SSSE3) | `iNxM.abs` |
//! | `Eq` | `cmeq` | `pcmpeq{b,w,d}`, 64: `pcmpeqq` (SSE4.1) | `iNxM.eq` |
//! | `Ne` | `cmeq`, `mvn` (2) | `pcmpeq`, `pxor` (2) | `iNxM.ne` |
//! | `Slt`, `Sle` | `cmgt`, `cmge` with swapped operands | `pcmpgt` swapped; `Sle` is `pcmpgt` + `pxor` (2) | `iNxM.lt_s`, `.le_s` |
//! | `Ult`, `Ule` | `cmhi`, `cmhs` swapped | `pminu`+`pcmpeq` (2), or sign-bias xor + `pcmpgt` (3) | `iNxM.lt_u`, `.le_u` |
//! | `ShlImm`, `LShrImm`, `AShrImm` | `shl ushr sshr` (8-bit shifts, SSE: 2 to 3) | `psll{w,d,q}`, `psrl*`, `psra{w,d}`; 8-bit and 64-bit `AShr`: 2 to 3 | `iNxM.shl`, `.shr_u`, `.shr_s` |
//! | `Shl`, `LShr`, `AShr` | `ushl` (negated amount for right shifts, `sshl`), amounts above `lw` need `umin` (2) | AVX2 `psllv` is not in the base; SSE2 to SSE4.1: scalarized through 16 or 32-bit shifts, or a lookup (about 4 to 8); an emitter may extract lanes | scalarized (about 4 to 8) |
//! | `Select` | `bsl` | `pand`, `pandn`, `por` (3) or `pblendvb` (SSE4.1) | `v128.bitselect` |
//! | `Widen*` | `sxtl`, `uxtl`, `sxtl2`, `uxtl2` | `pmovsx*`, `pmovzx*` (SSE4.1); high: `psrldq 8` first (2) | `iNxM.extend_{low,high}_..._{s,u}` |
//! | `NarrowTrunc` | `xtn`, `xtn2` (2) | `pshufb` per source and `punpcklqdq` (3) | `i8x16.shuffle` (1) |
//! | `NarrowSS` | `sqxtn`, `sqxtn2` (2) | `packsswb`, `packssdw` | `i8x16.narrow_i16x8_s`, `i16x8.narrow_i32x4_s` |
//! | `NarrowUU` | `uqxtn`, `uqxtn2` (2) | `pminu` with the maximum then `packuswb` / `packusdw` (SSE4.1) (3) | `pmin_u` then `narrow_*_u` after masking (3) |
//! | `NarrowSU` | `sqxtun`, `sqxtun2` (2) | `packuswb`, `packusdw` (SSE4.1) | `i8x16.narrow_i16x8_u`, `i16x8.narrow_i32x4_u` |
//! | `MulWide*` | `smull`, `umull`, `smull2`, `umull2`; `SU`: widen both, `mul` (3) | `pmovsx`/`pmovzx` both, `pmul*` (3), 32: `pmuldq`/`pmuludq` (2 to 3) | `iNxM.extmul_{low,high}_..._{s,u}`; `SU`: extend both, `mul` (3) |
//! | `Broadcast(i)` | `dup` (lane) | `pshufd`, `pshuflw`+`pshufd` (16), `pshufb` (8) | `i8x16.shuffle` / `iNxM.splat` of `extract_lane` |
//! | `Extract(i)` | `umov` | `movd`, `pextr{b,w,d,q}` (SSE4.1; `pextrw` SSE2) | `iNxM.extract_lane_u` |
//! | `Insert(i)` | `ins` | `pinsr{b,d,q}` (SSE4.1), `pinsrw` (SSE2) | `iNxM.replace_lane` |
//! | `Splat` | `dup` (general) | `movd` + `pshufd`/`pshufb` | `iNxM.splat` |
//! | [`Permute`] | `tbl` / `tbl2` (out of range gives 0 as defined) | `pshufb` with the constant pattern remapped so that out-of-range bytes have bit 7 set; two sources: two `pshufb`, `por` (3) | `i8x16.swizzle`; two sources: `i8x16.shuffle` of both with a zero vector |
//!
//! Constants that an emitter derives from an immediate or a [`Permute`]
//! pattern are known at translation time and cost nothing at run time.

use crate::{Value, Width};

/// The width of one lane of a 128-bit value.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum LaneWidth {
    /// 8-bit lanes, sixteen of them.
    L8,
    /// 16-bit lanes, eight of them.
    L16,
    /// 32-bit lanes, four of them.
    L32,
    /// 64-bit lanes, two of them.
    L64,
}

impl LaneWidth {
    /// The lane width in bits.
    pub const fn bits(self) -> u32 {
        match self {
            LaneWidth::L8 => 8,
            LaneWidth::L16 => 16,
            LaneWidth::L32 => 32,
            LaneWidth::L64 => 64,
        }
    }

    /// The number of lanes in 128 bits.
    pub const fn lanes(self) -> u32 {
        128 / self.bits()
    }

    /// The lane width of `bits` bits, if it is 8, 16, 32 or 64.
    pub const fn from_bits(bits: u32) -> Option<LaneWidth> {
        match bits {
            8 => Some(LaneWidth::L8),
            16 => Some(LaneWidth::L16),
            32 => Some(LaneWidth::L32),
            64 => Some(LaneWidth::L64),
            _ => None,
        }
    }

    /// The type of one lane as a scalar.
    pub const fn scalar(self) -> Width {
        Width::new(self.bits())
    }
}

/// What a [`LaneOp`] does; see the module documentation for the table.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum LaneKind {
    /// Wrapping sum.
    Add,
    /// Wrapping difference.
    Sub,
    /// Signed saturating sum.
    AddSatS,
    /// Unsigned saturating sum.
    AddSatU,
    /// Signed saturating difference.
    SubSatS,
    /// Unsigned saturating difference.
    SubSatU,
    /// Low half of the product.
    MulLo,
    /// High half of the signed product.
    MulHiS,
    /// High half of the unsigned product.
    MulHiU,
    /// High half of the product of signed `a` and unsigned `b`.
    MulHiSU,
    /// Bitwise and.
    And,
    /// Bitwise or.
    Or,
    /// Bitwise exclusive or.
    Xor,
    /// `a & !b`.
    AndNot,
    /// Bitwise complement.
    Not,
    /// Signed minimum.
    MinS,
    /// Unsigned minimum.
    MinU,
    /// Signed maximum.
    MaxS,
    /// Unsigned maximum.
    MaxU,
    /// Wrapping absolute value.
    Abs,
    /// Wrapping negation.
    Neg,
    /// Equal, as a mask.
    Eq,
    /// Not equal, as a mask.
    Ne,
    /// Signed less than, as a mask.
    Slt,
    /// Signed less than or equal, as a mask.
    Sle,
    /// Unsigned less than, as a mask.
    Ult,
    /// Unsigned less than or equal, as a mask.
    Ule,
    /// Left shift by the immediate.
    ShlImm,
    /// Logical right shift by the immediate.
    LShrImm,
    /// Arithmetic right shift by the immediate.
    AShrImm,
    /// Left shift by the lane of `b`.
    Shl,
    /// Logical right shift by the lane of `b`.
    LShr,
    /// Arithmetic right shift by the lane of `b`.
    AShr,
    /// Bitwise select by a mask.
    Select,
    /// Lower half, sign extended to double width.
    WidenLoS,
    /// Lower half, zero extended to double width.
    WidenLoU,
    /// Upper half, sign extended to double width.
    WidenHiS,
    /// Upper half, zero extended to double width.
    WidenHiU,
    /// Two vectors narrowed by truncation.
    NarrowTrunc,
    /// Two vectors narrowed, signed to signed saturating.
    NarrowSS,
    /// Two vectors narrowed, unsigned to unsigned saturating.
    NarrowUU,
    /// Two vectors narrowed, signed to unsigned saturating.
    NarrowSU,
    /// Product of the lower halves, signed x signed.
    MulWideLoSS,
    /// Product of the lower halves, unsigned x unsigned.
    MulWideLoUU,
    /// Product of the lower halves, signed `a` x unsigned `b`.
    MulWideLoSU,
    /// Product of the upper halves, signed x signed.
    MulWideHiSS,
    /// Product of the upper halves, unsigned x unsigned.
    MulWideHiUU,
    /// Product of the upper halves, signed `a` x unsigned `b`.
    MulWideHiSU,
    /// One lane copied to every lane; the immediate is the lane index.
    Broadcast,
    /// One lane as a scalar; the immediate is the lane index.
    Extract,
    /// A scalar replaces one lane; the immediate is the lane index.
    Insert,
    /// A scalar copied to every lane.
    Splat,
}

impl LaneKind {
    /// The number of operands.
    pub fn arity(self) -> usize {
        use LaneKind::*;
        match self {
            Select => 3,
            Not | Abs | Neg | ShlImm | LShrImm | AShrImm | WidenLoS | WidenLoU | WidenHiS
            | WidenHiU | Broadcast | Extract | Splat => 1,
            _ => 2,
        }
    }

    /// Whether the operation reads its immediate.
    pub fn has_imm(self) -> bool {
        use LaneKind::*;
        matches!(
            self,
            ShlImm | LShrImm | AShrImm | Broadcast | Extract | Insert
        )
    }

    /// Whether the operation does not depend on the lane width, and so
    /// carries [`LaneWidth::L64`].
    pub fn bitwise(self) -> bool {
        use LaneKind::*;
        matches!(self, And | Or | Xor | AndNot | Not | Select)
    }

    /// Whether the lane width is one the kind supports.
    pub fn allows(self, lane: LaneWidth) -> bool {
        use LaneKind::*;
        use LaneWidth::*;
        match self {
            Add | Sub | Neg | Eq | Ne | ShlImm | LShrImm | AShrImm | Shl | LShr | AShr
            | Broadcast | Extract | Insert | Splat => true,
            AddSatS | AddSatU | SubSatS | SubSatU => matches!(lane, L8 | L16),
            MulLo | MulHiS | MulHiU | MulHiSU => matches!(lane, L16 | L32),
            MinS | MinU | MaxS | MaxU | Abs | Slt | Sle | Ult | Ule => {
                matches!(lane, L8 | L16 | L32)
            }
            WidenLoS | WidenLoU | WidenHiS | WidenHiU | MulWideLoSS | MulWideLoUU | MulWideLoSU
            | MulWideHiSS | MulWideHiUU | MulWideHiSU => {
                matches!(lane, L8 | L16 | L32)
            }
            NarrowTrunc => matches!(lane, L16 | L32 | L64),
            NarrowSS | NarrowUU | NarrowSU => matches!(lane, L16 | L32),
            And | Or | Xor | AndNot | Not | Select => lane == L64,
        }
    }
}

/// One per-lane operation: what it does, on which lane width, with which
/// immediate. The operands are in [`crate::Op::Lane`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct LaneOp {
    /// What it does.
    pub kind: LaneKind,
    /// The lane width of the operands (of the sources, for widening and
    /// narrowing).
    pub lane: LaneWidth,
    /// The shift amount or lane index when [`LaneKind::has_imm`], else 0.
    pub imm: u8,
}

impl LaneOp {
    /// An operation without an immediate.
    pub const fn new(kind: LaneKind, lane: LaneWidth) -> LaneOp {
        LaneOp { kind, lane, imm: 0 }
    }

    /// An operation with an immediate.
    pub const fn with_imm(kind: LaneKind, lane: LaneWidth, imm: u8) -> LaneOp {
        LaneOp { kind, lane, imm }
    }

    /// The width of the result: one lane for [`LaneKind::Extract`], else 128.
    pub fn result_width(self) -> Width {
        if self.kind == LaneKind::Extract {
            self.lane.scalar()
        } else {
            Width::W128
        }
    }

    /// The width of operand `i`: one lane for the scalar of
    /// [`LaneKind::Insert`] and [`LaneKind::Splat`], else 128.
    pub fn operand_width(self, i: usize) -> Width {
        match (self.kind, i) {
            (LaneKind::Insert, 1) | (LaneKind::Splat, 0) => self.lane.scalar(),
            _ => Width::W128,
        }
    }

    /// Checks the lane width, the immediate, the operand widths and the
    /// result width.
    ///
    /// # Errors
    ///
    /// Says what is wrong.
    pub fn check(self, operands: &[Width], result: Option<Width>) -> Result<(), String> {
        let k = self.kind;
        if !k.allows(self.lane) {
            return Err(format!(
                "{k:?} does not support {}-bit lanes",
                self.lane.bits()
            ));
        }
        if k.has_imm() {
            let limit = match k {
                LaneKind::ShlImm | LaneKind::LShrImm | LaneKind::AShrImm => self.lane.bits(),
                _ => self.lane.lanes(),
            };
            if u32::from(self.imm) >= limit {
                return Err(format!("{k:?} immediate {} is not below {limit}", self.imm));
            }
        } else if self.imm != 0 {
            return Err(format!("{k:?} takes no immediate"));
        }
        if operands.len() != k.arity() {
            return Err(format!("{k:?} takes {} operands", k.arity()));
        }
        for (i, w) in operands.iter().enumerate() {
            if *w != self.operand_width(i) {
                return Err(format!(
                    "operand {i} of {k:?} is {} bits, expected {}",
                    w.bits(),
                    self.operand_width(i).bits()
                ));
            }
        }
        match result {
            Some(w) if w == self.result_width() => Ok(()),
            Some(w) => Err(format!(
                "{k:?} gives {} bits, not {}",
                self.result_width().bits(),
                w.bits()
            )),
            None => Err("missing result width".to_string()),
        }
    }
}

/// A constant byte permutation of one or two 128-bit sources, the payload of
/// [`crate::Op::Permute`].
///
/// Result byte `i` is source byte `pattern[i]`: index 0 to 15 selects a byte
/// of `a`, index 16 to 31 a byte of `b` when there is one, and any other
/// index (including 16 to 31 with a single source) gives 0.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Permute {
    /// For each result byte, the source byte.
    pub pattern: [u8; 16],
    /// The first source.
    pub a: Value,
    /// The second source, if any.
    pub b: Option<Value>,
}

impl Permute {
    /// The pattern as a 128-bit value, byte `i` of the value being
    /// `pattern[i]`; this is the text form.
    pub fn pattern_value(&self) -> u128 {
        u128::from_le_bytes(self.pattern)
    }
}

fn lane_mask(bits: u32) -> u128 {
    if bits == 128 {
        u128::MAX
    } else {
        (1u128 << bits) - 1
    }
}

fn get(x: u128, bits: u32, i: u32) -> u128 {
    (x >> (i * bits)) & lane_mask(bits)
}

fn sx(v: u128, bits: u32) -> i128 {
    let s = 128 - bits;
    ((v << s) as i128) >> s
}

fn put(out: &mut u128, bits: u32, i: u32, v: u128) {
    *out |= (v & lane_mask(bits)) << (i * bits);
}

fn clamp_s(v: i128, bits: u32) -> u128 {
    let hi = (1i128 << (bits - 1)) - 1;
    let lo = -(1i128 << (bits - 1));
    v.clamp(lo, hi) as u128
}

fn clamp_u(v: i128, bits: u32) -> u128 {
    v.clamp(0, (1i128 << bits) - 1) as u128
}

fn map1(a: u128, bits: u32, f: impl Fn(u128) -> u128) -> u128 {
    let mut out = 0;
    for i in 0..128 / bits {
        put(&mut out, bits, i, f(get(a, bits, i)));
    }
    out
}

fn map2(a: u128, b: u128, bits: u32, f: impl Fn(u128, u128) -> u128) -> u128 {
    let mut out = 0;
    for i in 0..128 / bits {
        put(&mut out, bits, i, f(get(a, bits, i), get(b, bits, i)));
    }
    out
}

fn mask_of(c: bool, bits: u32) -> u128 {
    if c { lane_mask(bits) } else { 0 }
}

fn shift(kind: LaneKind, x: u128, amount: u128, bits: u32) -> u128 {
    let over = amount >= u128::from(bits);
    match kind {
        LaneKind::Shl | LaneKind::ShlImm => {
            if over {
                0
            } else {
                x << amount
            }
        }
        LaneKind::LShr | LaneKind::LShrImm => {
            if over {
                0
            } else {
                x >> amount
            }
        }
        _ => {
            let s = sx(x, bits);
            let n = amount.min(u128::from(bits) - 1);
            (s >> n) as u128
        }
    }
}

/// The result of `op` on `args` (as many as [`LaneKind::arity`]), each a
/// 128-bit value, or one lane for the scalar operands of `Insert` and
/// `Splat`. The reference semantics the emitters follow.
///
/// # Panics
///
/// Panics when `op` or `args` do not pass [`LaneOp::check`].
pub fn eval(op: LaneOp, args: &[u128]) -> u128 {
    use LaneKind::*;
    let bits = op.lane.bits();
    let n = op.lane.lanes();
    let a = args[0];
    let b = args.get(1).copied().unwrap_or(0);
    let s = |v: u128| sx(v, bits);
    let u = |v: u128| v as i128;
    match op.kind {
        Add => map2(a, b, bits, |x, y| x.wrapping_add(y)),
        Sub => map2(a, b, bits, |x, y| x.wrapping_sub(y)),
        AddSatS => map2(a, b, bits, |x, y| clamp_s(s(x) + s(y), bits)),
        AddSatU => map2(a, b, bits, |x, y| clamp_u(u(x) + u(y), bits)),
        SubSatS => map2(a, b, bits, |x, y| clamp_s(s(x) - s(y), bits)),
        SubSatU => map2(a, b, bits, |x, y| clamp_u(u(x) - u(y), bits)),
        MulLo => map2(a, b, bits, |x, y| x.wrapping_mul(y)),
        MulHiS => map2(a, b, bits, |x, y| ((s(x) * s(y)) >> bits) as u128),
        MulHiU => map2(a, b, bits, |x, y| (x * y) >> bits),
        MulHiSU => map2(a, b, bits, |x, y| ((s(x) * u(y)) >> bits) as u128),
        And => a & b,
        Or => a | b,
        Xor => a ^ b,
        AndNot => a & !b,
        Not => !a,
        MinS => map2(a, b, bits, |x, y| if s(x) <= s(y) { x } else { y }),
        MinU => map2(a, b, bits, |x, y| x.min(y)),
        MaxS => map2(a, b, bits, |x, y| if s(x) >= s(y) { x } else { y }),
        MaxU => map2(a, b, bits, |x, y| x.max(y)),
        Abs => map1(a, bits, |x| s(x).wrapping_abs() as u128),
        Neg => map1(a, bits, |x| x.wrapping_neg()),
        Eq => map2(a, b, bits, |x, y| mask_of(x == y, bits)),
        Ne => map2(a, b, bits, |x, y| mask_of(x != y, bits)),
        Slt => map2(a, b, bits, |x, y| mask_of(s(x) < s(y), bits)),
        Sle => map2(a, b, bits, |x, y| mask_of(s(x) <= s(y), bits)),
        Ult => map2(a, b, bits, |x, y| mask_of(x < y, bits)),
        Ule => map2(a, b, bits, |x, y| mask_of(x <= y, bits)),
        ShlImm | LShrImm | AShrImm => {
            map1(a, bits, |x| shift(op.kind, x, u128::from(op.imm), bits))
        }
        Shl | LShr | AShr => map2(a, b, bits, |x, y| shift(op.kind, x, y, bits)),
        Select => (a & b) | (!a & args[2]),
        WidenLoS | WidenLoU | WidenHiS | WidenHiU => {
            let base = if matches!(op.kind, WidenHiS | WidenHiU) {
                n / 2
            } else {
                0
            };
            let signed = matches!(op.kind, WidenLoS | WidenHiS);
            let mut out = 0;
            for i in 0..n / 2 {
                let x = get(a, bits, base + i);
                let v = if signed { s(x) as u128 } else { x };
                put(&mut out, bits * 2, i, v);
            }
            out
        }
        NarrowTrunc | NarrowSS | NarrowUU | NarrowSU => {
            let d = bits / 2;
            let mut out = 0;
            for i in 0..n {
                for (half, src) in [a, b].into_iter().enumerate() {
                    let x = get(src, bits, i);
                    let v = match op.kind {
                        NarrowTrunc => x,
                        NarrowSS => clamp_s(s(x), d),
                        NarrowUU => clamp_u(u(x), d),
                        _ => clamp_u(s(x), d),
                    };
                    put(&mut out, d, half as u32 * n + i, v);
                }
            }
            out
        }
        MulWideLoSS | MulWideLoUU | MulWideLoSU | MulWideHiSS | MulWideHiUU | MulWideHiSU => {
            let base = if matches!(op.kind, MulWideHiSS | MulWideHiUU | MulWideHiSU) {
                n / 2
            } else {
                0
            };
            let mut out = 0;
            for i in 0..n / 2 {
                let x = get(a, bits, base + i);
                let y = get(b, bits, base + i);
                let p = match op.kind {
                    MulWideLoSS | MulWideHiSS => s(x) * s(y),
                    MulWideLoUU | MulWideHiUU => u(x) * u(y),
                    _ => s(x) * u(y),
                };
                put(&mut out, bits * 2, i, p as u128);
            }
            out
        }
        Broadcast => {
            let x = get(a, bits, u32::from(op.imm));
            map1(0, bits, |_| x)
        }
        Extract => get(a, bits, u32::from(op.imm)),
        Insert => {
            let at = u32::from(op.imm) * bits;
            (a & !(lane_mask(bits) << at)) | ((b & lane_mask(bits)) << at)
        }
        Splat => map1(0, bits, |_| a & lane_mask(bits)),
    }
}

/// The result of a byte permutation; see [`Permute`].
pub fn eval_permute(pattern: &[u8; 16], a: u128, b: Option<u128>) -> u128 {
    let mut out = 0u128;
    for (i, &p) in pattern.iter().enumerate() {
        let byte = match (p, b) {
            (0..=15, _) => (a >> (8 * u32::from(p))) & 0xff,
            (16..=31, Some(b)) => (b >> (8 * u32::from(p - 16))) & 0xff,
            _ => 0,
        };
        out |= byte << (8 * i);
    }
    out
}

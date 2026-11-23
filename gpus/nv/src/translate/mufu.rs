//! MUFU, the multi-function unit, bit-exact on every host.
//!
//! The unit evaluates each function as a quadratic `A + B t + C s` in the low
//! bits `t` of a fixed-point argument, with `A`, `B` and `C` read from a table
//! by the high bits and `s` the square of `t` from a squarer that keeps only
//! the partial products at or above a fixed column, shifted down by that
//! column. [`tables`] holds the coefficients, solved from the outputs of an
//! RTX 3090 for every input; the constructions below reproduce all 2^32
//! inputs of RCP, RSQ, SQRT, EX2, LG2, SIN, COS, RCP64H and RSQ64H with 32-bit
//! integer operations only:
//!
//! - RCP: 7 mantissa bits index, 16 bits of `t`, squarer columns from 17,
//!   `P >> 16` is the mantissa (2^24 for a power of two).
//! - RSQ, SQRT: the exponent's parity and 6 mantissa bits index, 17 bits of
//!   `t`, columns from 19, `P >> 18`.
//! - EX2: the input as a fixed-point number with 23 fraction bits, cut toward
//!   zero, then negated by one's complement when it has a fraction (two's
//!   complement when it has none); the integer part is the exponent and the
//!   fraction indexes as for SQRT, `P >> 16`.
//! - LG2: 6 mantissa bits index, `P >> 2` is the logarithm of the mantissa
//!   with 36 fraction bits, added to the exponent; a negative sum is
//!   complemented (one's complement) and the magnitude cut toward zero to
//!   single precision. One is the only exact zero.
//! - SIN, COS: the operand in turns, as a fixed-point number with 25 fraction
//!   bits cut toward zero; the quarter picks the sign and, when odd, mirrors
//!   the fraction by one's complement; `P` has 37 fraction bits and is cut
//!   toward zero. COS adds a quarter turn to the magnitude.
//! - RCP64H, RSQ64H: the RCP and RSQ tables on the 20 mantissa bits of a
//!   double's high word, the result cut to 20 bits.
//! - `.F16`: the single-precision result for the half input, cut toward zero
//!   to half precision, infinite from a magnitude of 65536 up.
//!
//! With [`Options::exact_mufu`](super::Options) off, on the nvidia preset
//! only, RCP, RSQ, SQRT, EX2 and LG2 use the GLSL operations (`1 / x`,
//! InverseSqrt, Sqrt, Exp2, Log2), which that driver lowers to these same
//! MUFU operations. That was proven per function over all 2^32 inputs: a
//! one-instruction MUFU kernel built with CUDA for SM86 was translated, run
//! through Vulkan on an SM86 device, and compared bit for bit with the same
//! kernel run under CUDA on that device (driver 615.78), with no difference,
//! NaN payloads included. Every other host uses the model; a new driver
//! should be compared the same way. SIN and COS always use the
//! model (the driver scales the operand by 1/2pi itself). TANH has no model:
//! it always uses the GLSL operation, which does not match the unit (the same
//! comparison differs on 6% of the inputs), and the program is reported
//! inexact ([`HostReport::inexact`](super::HostReport)).

use super::{Ctx, Math, Res, bad};
use crate::spirv::{glsl, op};
use crate::{Inst, RegMods, Swz};

mod tables;

const PRIVATE: u32 = 6;
const TABLE_KEY: u32 = 0x2_0100;

#[derive(Clone, Copy)]
enum Table {
    Rcp,
    Rsq,
    Sqrt,
    Ex2,
    Lg2,
    Sin,
}

impl Table {
    fn rows(self) -> &'static [[u32; 4]] {
        match self {
            Table::Rcp => &tables::RCP,
            Table::Rsq => &tables::RSQ,
            Table::Sqrt => &tables::SQRT,
            Table::Ex2 => &tables::EX2,
            Table::Lg2 => &tables::LG2,
            Table::Sin => &tables::SIN,
        }
    }
}

/// The two words of a 64-bit integer, (low, high).
type W64 = (u32, u32);

/// The layout of an RCP or RSQ operand: a float, or a double's high word.
#[derive(Clone, Copy)]
struct Fmt {
    mbits: u32,
    emax: u32,
    bias: u32,
    inf: u32,
}

const F32: Fmt = Fmt {
    mbits: 23,
    emax: 255,
    bias: 127,
    inf: 0x7f80_0000,
};

const F64H: Fmt = Fmt {
    mbits: 20,
    emax: 2047,
    bias: 1023,
    inf: 0x7ff0_0000,
};

const NAN: u32 = 0x7fff_ffff;
const SIGN: u32 = 0x8000_0000;

pub(crate) fn mufu(c: &mut Ctx, i: &Inst) -> Res<()> {
    let kind = i.mods.first().copied().unwrap_or("");
    let half = i.has("F16");
    if i.mods.len() != 1 + usize::from(half) || (half && i.mods[1] != "F16") {
        return bad(format!("MUFU {:?}", i.mods));
    }
    let (v, m) = c.raw(&i.ops[1])?;
    if m.not {
        return bad("MUFU source modifiers");
    }
    if half && kind.ends_with("64H") {
        return bad(format!("MUFU.{kind}.F16"));
    }
    let out = if half {
        let h = super::int::conv::pick(c, v, m.swz, 16, false)?;
        let h = sign_mods(c, h, m, 15);
        let x = c.f16_to_f32(h, false);
        let r = func(c, kind, x)?;
        to_half(c, r)
    } else {
        if m.swz != Swz::None {
            return bad("MUFU source selector");
        }
        let x = sign_mods(c, v, m, 31);
        func(c, kind, x)?
    };
    c.store_dst(&i.ops[0], out)
}

/// Absolute value and negation of the operand as sign-bit operations.
fn sign_mods(c: &mut Ctx, v: u32, m: RegMods, bit: u32) -> u32 {
    let v = if m.abs { c.and_k(v, !(1 << bit)) } else { v };
    if m.neg {
        c.sh(op::BITWISE_XOR, v, 1 << bit)
    } else {
        v
    }
}

/// A single-precision result cut toward zero to half precision.
fn to_half(c: &mut Ctx, r: u32) -> u32 {
    let h = c.f32_to_f16(r, 3, false);
    let a = c.and_k(r, !SIGN);
    let big = c.cmp_k(op::U_GREATER_THAN_EQUAL, a, 0x4780_0000);
    let fin = c.cmp_k(op::U_LESS_THAN, a, F32.inf);
    let over = c.land(big, fin);
    let s = c.sh(op::SHIFT_RIGHT_LOGICAL, r, 16);
    let s = c.and_k(s, 0x8000);
    let inf = c.sh(op::BITWISE_OR, s, 0x7c00);
    c.bpick(over, inf, h)
}

fn func(c: &mut Ctx, kind: &str, x: u32) -> Res<u32> {
    let owned = kind.to_string();
    c.shared(format!("MUFU {kind}"), &[x], move |c, p| {
        func_body(c, &owned, p[0])
    })
}

fn func_body(c: &mut Ctx, kind: &str, x: u32) -> Res<u32> {
    let native = !c.exact_mufu;
    Ok(match kind {
        "RCP" if native => native_op(c, x, None),
        "RSQ" if native => native_op(c, x, Some(glsl::INVERSE_SQRT)),
        "SQRT" if native => native_op(c, x, Some(glsl::SQRT)),
        "EX2" if native => native_op(c, x, Some(glsl::EXP2)),
        "LG2" if native => native_op(c, x, Some(glsl::LOG2)),
        "SIN" if c.math == Math::Native => native_turns(c, x, glsl::SIN),
        "COS" if c.math == Math::Native => native_turns(c, x, glsl::COS),
        "RCP" => rcp(c, x, F32),
        "RSQ" => rsq(c, x, F32),
        "SQRT" => sqrt(c, x),
        "EX2" => ex2(c, x),
        "LG2" => lg2(c, x),
        "SIN" => sincos(c, x, false),
        "COS" => sincos(c, x, true),
        "RCP64H" => rcp(c, x, F64H),
        "RSQ64H" => rsq(c, x, F64H),
        "TANH" => {
            let note = "MUFU.TANH uses the host's tanh, which differs from the SM86 unit";
            if !c.iface.host.inexact.iter().any(|l| l == note) {
                c.iface.host.inexact.push(note.to_string());
            }
            native_op(c, x, Some(glsl::TANH))
        }
        k => return bad(format!("MUFU.{k}")),
    })
}

/// The GLSL sine or cosine `f` of the bits `x`, an angle in turns as the
/// unit takes it, for [`Math::Native`].
fn native_turns(c: &mut Ctx, x: u32, f: u32) -> u32 {
    let ft = c.f32t();
    let a = c.as_float(x);
    let k = c.m.c_f32(std::f32::consts::TAU.to_bits());
    let r = c.m.emit(op::F_MUL, ft, &[a, k]);
    let s = c.ext(ft, f, &[r]);
    c.as_bits(s)
}

/// The GLSL operation `f` (a reciprocal for `None`) on the bits `x`.
fn native_op(c: &mut Ctx, x: u32, f: Option<u32>) -> u32 {
    let ft = c.f32t();
    let a = c.as_float(x);
    let r = match f {
        Some(f) => c.ext(ft, f, &[a]),
        None => {
            let one = c.m.c_f32(1.0f32.to_bits());
            c.m.emit(op::F_DIV, ft, &[one, a])
        }
    };
    c.as_bits(r)
}

impl Ctx {
    /// The table as a constant-initialized private array of four-word
    /// vectors, one per row: `A` low, `A` high, `B`, `C`.
    fn mufu_table(&mut self, t: Table) -> u32 {
        let key = TABLE_KEY + t as u32;
        if let Some(&v) = self.builtins.get(&key) {
            return v;
        }
        let u = self.u32t();
        let v4 = self.m.t_vec(u, 4);
        let rows: Vec<u32> = t
            .rows()
            .iter()
            .map(|r| {
                let ws: Vec<u32> = r.iter().map(|&w| self.m.c_u32(w)).collect();
                self.m.c_composite(v4, &ws)
            })
            .collect();
        let arr = self.m.t_array(v4, rows.len() as u32);
        let init = self.m.c_composite(arr, &rows);
        let p = self.m.t_ptr(PRIVATE, arr);
        let var = self.m.id();
        crate::spirv::inst(&mut self.m.types, op::VARIABLE, &[p, var, PRIVATE, init]);
        self.builtins.insert(key, var);
        self.globals.push(var);
        var
    }

    fn mufu_row(&mut self, t: Table, idx: u32) -> [u32; 4] {
        let var = self.mufu_table(t);
        let u = self.u32t();
        let v4 = self.m.t_vec(u, 4);
        let p = self.m.t_ptr(PRIVATE, v4);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, idx]);
        let row = self.m.emit(op::LOAD, v4, &[ptr]);
        std::array::from_fn(|k| self.m.emit(op::COMPOSITE_EXTRACT, u, &[row, k as u32]))
    }

    /// The squarer: `t * t` (`t` of `nb` bits) without the partial products
    /// below column `k` (odd), shifted right by `k`. The dropped products of
    /// the low `(k + 1) / 2` bits are their whole square; each higher bit `j`
    /// drops its products with the bits below `k - 1 - j`.
    fn mufu_square(&mut self, t: u32, nb: u32, k: u32) -> u32 {
        let m = k.div_ceil(2);
        let low = self.and_k(t, (1 << m) - 1);
        let mut d = self.ib(op::I_MUL, low, low);
        let u = self.u32t();
        let one = self.kc(1);
        for j in m..nb {
            let w = k - 1 - j;
            let at = self.kc(j);
            let bit = self.m.emit(op::BIT_FIELD_U_EXTRACT, u, &[t, at, one]);
            let part = self.and_k(t, (1 << w) - 1);
            let term = self.ib(op::I_MUL, bit, part);
            let term = self.sh(op::SHIFT_LEFT_LOGICAL, term, j + 1);
            d = self.ib(op::I_ADD, d, term);
        }
        let (lo, hi) = self.mul_wide(t, t, false);
        let borrow = self.icmp_op(op::U_LESS_THAN, lo, d);
        let lo = self.ib(op::I_SUB, lo, d);
        let b = self.b2u(borrow);
        let hi = self.ib(op::I_SUB, hi, b);
        self.shr64((lo, hi), k).0
    }

    /// `v >> k` for a constant `0 < k < 32`, logical.
    fn shr64(&mut self, v: W64, k: u32) -> W64 {
        let lo = self.sh(op::SHIFT_RIGHT_LOGICAL, v.0, k);
        let up = self.sh(op::SHIFT_LEFT_LOGICAL, v.1, 32 - k);
        let lo = self.ib(op::BITWISE_OR, lo, up);
        let hi = self.sh(op::SHIFT_RIGHT_LOGICAL, v.1, k);
        (lo, hi)
    }

    /// `A + B t + C s` for the row at `idx`, with `t` of `nb` bits and the
    /// squarer cut at column `k`.
    fn mufu_poly(&mut self, tab: Table, idx: u32, t: u32, nb: u32, k: u32) -> W64 {
        let row = self.mufu_row(tab, idx);
        let s = self.mufu_square(t, nb, k);
        let bt = self.mul_wide(row[2], t, true);
        let cs = self.ib(op::I_MUL, row[3], s);
        let csh = self.sh(op::SHIFT_RIGHT_ARITHMETIC, cs, 31);
        let (l, h, _) = self.add64((row[0], row[1]), bt, None);
        let (l, h, _) = self.add64((l, h), (cs, csh), None);
        (l, h)
    }

    /// The float of sign `sign` and magnitude `v` (nonzero, below 2^43, with
    /// `fb` fraction bits), cut toward zero; zero gives a signed zero.
    fn mufu_cut(&mut self, v: W64, fb: u32, sign: u32) -> u32 {
        let u = self.u32t();
        let hz = self.cmp_k(op::I_EQUAL, v.1, 0);
        let ml = self.ext(u, glsl::FIND_U_MSB, &[v.0]);
        let mh = self.ext(u, glsl::FIND_U_MSB, &[v.1]);
        let mh = self.sh(op::I_ADD, mh, 32);
        let p = self.bpick(hz, ml, mh);
        let wide = self.cmp_k(op::S_GREATER_THAN_EQUAL, p, 23);
        let s = self.sh(op::I_SUB, p, 23);
        let s = self.and_k(s, 31);
        let down = self.ib(op::SHIFT_RIGHT_LOGICAL, v.0, s);
        let k31 = self.kc(31);
        let back = self.ib(op::I_SUB, k31, s);
        let up = self.sh(op::SHIFT_LEFT_LOGICAL, v.1, 1);
        let up = self.ib(op::SHIFT_LEFT_LOGICAL, up, back);
        let down = self.ib(op::BITWISE_OR, down, up);
        let k23 = self.kc(23);
        let ls = self.ib(op::I_SUB, k23, p);
        let ls = self.and_k(ls, 31);
        let left = self.ib(op::SHIFT_LEFT_LOGICAL, v.0, ls);
        let mant = self.bpick(wide, down, left);
        let mant = self.and_k(mant, 0x7f_ffff);
        let e = self.sh(op::I_ADD, p, 127u32.wrapping_sub(fb));
        let e = self.sh(op::SHIFT_LEFT_LOGICAL, e, 23);
        let r = self.ib(op::BITWISE_OR, e, mant);
        let lz = self.cmp_k(op::I_EQUAL, v.0, 0);
        let zero = self.land(hz, lz);
        let z = self.kc(0);
        let r = self.bpick(zero, z, r);
        self.ib(op::BITWISE_OR, r, sign)
    }

    /// Sign, biased exponent and mantissa of `x` in `f`.
    fn mufu_split(&mut self, x: u32, f: Fmt) -> (u32, u32, u32) {
        let s = self.and_k(x, SIGN);
        let e = self.sh(op::SHIFT_RIGHT_LOGICAL, x, f.mbits);
        let e = self.and_k(e, f.emax);
        let m = self.and_k(x, (1 << f.mbits) - 1);
        (s, e, m)
    }

    /// The result word from biased exponent `re` and a 24-bit-or-2^24
    /// mantissa `y`: a mantissa of 2^24 steps the exponent.
    fn mufu_pack(&mut self, y: u32, re: u32, f: Fmt) -> (u32, u32) {
        let over = self.cmp_k(op::U_GREATER_THAN_EQUAL, y, 1 << 24);
        let ov = self.b2u(over);
        let y = self.ib(op::SHIFT_RIGHT_LOGICAL, y, ov);
        let re = self.ib(op::I_ADD, re, ov);
        let y = self.sh(op::SHIFT_RIGHT_LOGICAL, y, 23 - f.mbits);
        let y = self.and_k(y, (1 << f.mbits) - 1);
        let e = self.sh(op::SHIFT_LEFT_LOGICAL, re, f.mbits);
        (self.ib(op::BITWISE_OR, e, y), re)
    }
}

fn rcp(c: &mut Ctx, x: u32, f: Fmt) -> u32 {
    let (s, e, m) = c.mufu_split(x, f);
    let m = c.sh(op::SHIFT_LEFT_LOGICAL, m, 23 - f.mbits);
    let idx = c.sh(op::SHIFT_RIGHT_LOGICAL, m, 16);
    let t = c.and_k(m, 0xffff);
    let p = c.mufu_poly(Table::Rcp, idx, t, 16, 17);
    let y = c.shr64(p, 16).0;
    let k = c.kc(2 * f.bias - 1);
    let re = c.ib(op::I_SUB, k, e);
    let (r, re) = c.mufu_pack(y, re, f);
    let r = c.ib(op::BITWISE_OR, r, s);
    let under = c.cmp_k(op::S_LESS_THAN_EQUAL, re, 0);
    let r = c.bpick(under, s, r);
    let m_nz = c.cmp_k(op::I_NOT_EQUAL, m, 0);
    let nan = c.kc(NAN);
    let top = c.bpick(m_nz, nan, s);
    let is_top = c.cmp_k(op::I_EQUAL, e, f.emax);
    let r = c.bpick(is_top, top, r);
    let inf = c.sh(op::BITWISE_OR, s, f.inf);
    let is0 = c.cmp_k(op::I_EQUAL, e, 0);
    c.bpick(is0, inf, r)
}

/// Table row, offset and halved unbiased exponent of an RSQ or SQRT operand
/// with mantissa `m` (23 bits): the table's first half serves odd biased
/// exponents.
fn root_arg(c: &mut Ctx, e: u32, m: u32, bias: u32) -> (u32, u32, u32) {
    let odd = c.and_k(e, 1);
    let even = c.sh(op::BITWISE_XOR, odd, 1);
    let even = c.sh(op::SHIFT_LEFT_LOGICAL, even, 6);
    let hi = c.sh(op::SHIFT_RIGHT_LOGICAL, m, 17);
    let idx = c.ib(op::BITWISE_OR, even, hi);
    let t = c.and_k(m, 0x1ffff);
    let k = c.ib(op::I_ADD, e, odd);
    let k = c.sh(op::I_SUB, k, bias + 1);
    let k = c.sh(op::SHIFT_RIGHT_ARITHMETIC, k, 1);
    (idx, t, k)
}

fn rsq(c: &mut Ctx, x: u32, f: Fmt) -> u32 {
    let (s, e, m) = c.mufu_split(x, f);
    let m = c.sh(op::SHIFT_LEFT_LOGICAL, m, 23 - f.mbits);
    let (idx, t, k) = root_arg(c, e, m, f.bias);
    let p = c.mufu_poly(Table::Rsq, idx, t, 17, 19);
    let y = c.shr64(p, 18).0;
    let odd = c.and_k(e, 1);
    let m2 = c.sh(op::SHIFT_LEFT_LOGICAL, m, 1);
    let one = c.ib(op::BITWISE_OR, odd, m2);
    let exact = c.cmp_k(op::I_EQUAL, one, 1);
    let pow = c.kc(1 << 24);
    let y = c.bpick(exact, pow, y);
    let kb = c.kc(f.bias - 1);
    let re = c.ib(op::I_SUB, kb, k);
    let r = c.mufu_pack(y, re, f).0;
    let nan = c.kc(NAN);
    let m_nz = c.cmp_k(op::I_NOT_EQUAL, m, 0);
    let z = c.kc(0);
    let top = c.bpick(m_nz, nan, z);
    let is_top = c.cmp_k(op::I_EQUAL, e, f.emax);
    let r = c.bpick(is_top, top, r);
    let neg = c.cmp_k(op::I_NOT_EQUAL, s, 0);
    let r = c.bpick(neg, nan, r);
    let inf = c.sh(op::BITWISE_OR, s, f.inf);
    let is0 = c.cmp_k(op::I_EQUAL, e, 0);
    c.bpick(is0, inf, r)
}

fn sqrt(c: &mut Ctx, x: u32) -> u32 {
    let (s, e, m) = c.mufu_split(x, F32);
    let (idx, t, k) = root_arg(c, e, m, 127);
    let p = c.mufu_poly(Table::Sqrt, idx, t, 17, 19);
    let y = c.shr64(p, 18).0;
    let re = c.sh(op::I_ADD, k, 127);
    let r = c.mufu_pack(y, re, F32).0;
    let nan = c.kc(NAN);
    let m_nz = c.cmp_k(op::I_NOT_EQUAL, m, 0);
    let top = c.bpick(m_nz, nan, x);
    let is_top = c.cmp_k(op::I_EQUAL, e, 255);
    let r = c.bpick(is_top, top, r);
    let neg = c.cmp_k(op::I_NOT_EQUAL, s, 0);
    let r = c.bpick(neg, nan, r);
    let is0 = c.cmp_k(op::I_EQUAL, e, 0);
    c.bpick(is0, s, r)
}

/// `mm` shifted left by `sh` (signed, below 32) or right by `-sh`, as a
/// fixed-point cut toward zero.
fn fixed(c: &mut Ctx, mm: u32, sh: u32) -> u32 {
    let left = c.cmp_k(op::S_GREATER_THAN_EQUAL, sh, 0);
    let ls = c.and_k(sh, 31);
    let l = c.ib(op::SHIFT_LEFT_LOGICAL, mm, ls);
    let z = c.kc(0);
    let rs = c.ib(op::I_SUB, z, sh);
    let u = c.u32t();
    let k31 = c.kc(31);
    let rs = c.ext(u, glsl::U_MIN, &[rs, k31]);
    let r = c.ib(op::SHIFT_RIGHT_LOGICAL, mm, rs);
    c.bpick(left, l, r)
}

fn ex2(c: &mut Ctx, x: u32) -> u32 {
    let (s, e, m) = c.mufu_split(x, F32);
    let mm = c.sh(op::BITWISE_OR, m, 0x80_0000);
    let sh = c.sh(op::I_SUB, e, 127);
    let fx = fixed(c, mm, sh);
    let z = c.kc(0);
    let is0 = c.cmp_k(op::I_EQUAL, e, 0);
    let fx = c.bpick(is0, z, fx);
    let frac = c.and_k(fx, 0x7f_ffff);
    let has_frac = c.cmp_k(op::I_NOT_EQUAL, frac, 0);
    let ones = c.inot(fx);
    let twos = c.ib(op::I_SUB, z, fx);
    let negv = c.bpick(has_frac, ones, twos);
    let neg = c.cmp_k(op::I_NOT_EQUAL, s, 0);
    let xs = c.bpick(neg, negv, fx);
    let n = c.sh(op::SHIFT_RIGHT_ARITHMETIC, xs, 23);
    let f = c.and_k(xs, 0x7f_ffff);
    let idx = c.sh(op::SHIFT_RIGHT_LOGICAL, f, 17);
    let t = c.and_k(f, 0x1ffff);
    let p = c.mufu_poly(Table::Ex2, idx, t, 17, 19);
    let y = c.shr64(p, 16).0;
    let y = c.and_k(y, 0x7f_ffff);
    let re = c.sh(op::I_ADD, n, 127);
    let ef = c.sh(op::SHIFT_LEFT_LOGICAL, re, 23);
    let r = c.ib(op::BITWISE_OR, ef, y);
    let inf = c.kc(F32.inf);
    let hi = c.cmp_k(op::S_GREATER_THAN_EQUAL, re, 255);
    let r = c.bpick(hi, inf, r);
    let lo = c.cmp_k(op::S_LESS_THAN_EQUAL, re, 0);
    let r = c.bpick(lo, z, r);
    let one = c.kc(1.0f32.to_bits());
    let fz = c.cmp_k(op::I_EQUAL, fx, 0);
    let r = c.bpick(fz, one, r);
    let sat = c.bpick(neg, z, inf);
    let big = c.cmp_k(op::U_GREATER_THAN_EQUAL, e, 134);
    let r = c.bpick(big, sat, r);
    let nan = c.kc(NAN);
    let m_nz = c.cmp_k(op::I_NOT_EQUAL, m, 0);
    let top = c.cmp_k(op::I_EQUAL, e, 255);
    let is_nan = c.land(top, m_nz);
    c.bpick(is_nan, nan, r)
}

fn lg2(c: &mut Ctx, x: u32) -> u32 {
    let (s, e, m) = c.mufu_split(x, F32);
    let idx = c.sh(op::SHIFT_RIGHT_LOGICAL, m, 17);
    let t = c.and_k(m, 0x1ffff);
    let p = c.mufu_poly(Table::Lg2, idx, t, 17, 19);
    let v = c.shr64(p, 2);
    let n = c.sh(op::I_SUB, e, 127);
    let n = c.sh(op::SHIFT_LEFT_LOGICAL, n, 4);
    let hi = c.ib(op::I_ADD, v.1, n);
    let neg = c.cmp_k(op::S_LESS_THAN, hi, 0);
    let nl = c.inot(v.0);
    let nh = c.inot(hi);
    let ml = c.bpick(neg, nl, v.0);
    let mh = c.bpick(neg, nh, hi);
    let sign = c.kc(SIGN);
    let z = c.kc(0);
    let sg = c.bpick(neg, sign, z);
    let r = c.mufu_cut((ml, mh), 36, sg);
    let is1 = c.cmp_k(op::I_EQUAL, x, 1.0f32.to_bits());
    let r = c.bpick(is1, z, r);
    let nan = c.kc(NAN);
    let inf = c.kc(F32.inf);
    let m_nz = c.cmp_k(op::I_NOT_EQUAL, m, 0);
    let top = c.bpick(m_nz, nan, inf);
    let is_top = c.cmp_k(op::I_EQUAL, e, 255);
    let r = c.bpick(is_top, top, r);
    let negx = c.cmp_k(op::I_NOT_EQUAL, s, 0);
    let r = c.bpick(negx, nan, r);
    let ninf = c.kc(0xff80_0000);
    let is0 = c.cmp_k(op::I_EQUAL, e, 0);
    c.bpick(is0, ninf, r)
}

fn sincos(c: &mut Ctx, x: u32, cosine: bool) -> u32 {
    let (s, e, m) = c.mufu_split(x, F32);
    let mm = c.sh(op::BITWISE_OR, m, 0x80_0000);
    let sh = c.sh(op::I_SUB, e, 125);
    let fx = fixed(c, mm, sh);
    let z = c.kc(0);
    let gone = c.cmp_k(op::S_GREATER_THAN_EQUAL, sh, 25);
    let is0 = c.cmp_k(op::I_EQUAL, e, 0);
    let gone = c.lor(gone, is0);
    let fx = c.bpick(gone, z, fx);
    let (fx, s) = if cosine {
        (c.sh(op::I_ADD, fx, 1 << 23), z)
    } else {
        (fx, s)
    };
    let u = c.u32t();
    let k23 = c.kc(23);
    let k2 = c.kc(2);
    let q = c.m.emit(op::BIT_FIELD_U_EXTRACT, u, &[fx, k23, k2]);
    let f = c.and_k(fx, 0x7f_ffff);
    let odd = c.and_k(q, 1);
    let odd = c.cmp_k(op::I_NOT_EQUAL, odd, 0);
    let mir = c.sh(op::BITWISE_XOR, f, 0x7f_ffff);
    let f = c.bpick(odd, mir, f);
    let idx = c.sh(op::SHIFT_RIGHT_LOGICAL, f, 17);
    let t = c.and_k(f, 0x1ffff);
    let p = c.mufu_poly(Table::Sin, idx, t, 17, 19);
    let back = c.sh(op::SHIFT_RIGHT_LOGICAL, q, 1);
    let back = c.sh(op::SHIFT_LEFT_LOGICAL, back, 31);
    let sign = c.ib(op::BITWISE_XOR, s, back);
    let r = c.mufu_cut(p, 37, sign);
    let nan = c.kc(NAN);
    let top = c.cmp_k(op::I_EQUAL, e, 255);
    c.bpick(top, nan, r)
}

//! 32-bit float ALU: FADD, FMUL, FFMA with every rounding mode, FMNMX,
//! FSEL, FSETP, FSET and FRND.
//!
//! SPIR-V has no per-instruction rounding mode for arithmetic, so RZ, RM
//! and RP round to nearest and then step one ulp when the exact error says
//! the nearest value lies on the wrong side. The error comes from error-free
//! transformations: Knuth's TwoSum for sums and, for products, a fused
//! multiply-add where the host promises one, Dekker's product otherwise
//! (exact as long as no intermediate underflows). FFMA without a fused host
//! multiply-add is built from integer operations (the `fma` module). FFMA's error sums
//! four terms with a TwoSum cascade. Results that overflowed to infinity
//! from finite operands step back to the largest finite value where the
//! mode says so.

use super::{Ctx, Math, Res, bad};
use crate::Inst;
use crate::program::Stage;
use crate::spirv::{glsl, op};

const NO_CONTRACTION: u32 = 42;
const OP_FMA_KHR: u16 = 4427;
const CAP_FMA_KHR: u32 = 6030;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Round {
    Nearest,
    Zero,
    Down,
    Up,
}

pub(crate) fn round_of(i: &Inst) -> Round {
    if i.has("RZ") {
        Round::Zero
    } else if i.has("RM") {
        Round::Down
    } else if i.has("RP") {
        Round::Up
    } else {
        Round::Nearest
    }
}

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "FADD" | "FMUL" | "FFMA" => arith(c, i),
        "MUFU" => super::mufu::mufu(c, i),
        "FMNMX" => fmnmx(c, i),
        "FSEL" => fsel(c, i),
        "FSETP" => fsetp(c, i),
        "FSET" => fset(c, i),
        "FRND" => frnd(c, i),
        "FSWZADD" => fswzadd(c, i),
        _ => return None,
    })
}

impl Ctx {
    pub(crate) fn exact(&mut self, op: u16, a: u32, b: u32) -> u32 {
        let f = self.f32t();
        let r = self.m.emit(op, f, &[a, b]);
        self.m.deco(r, NO_CONTRACTION, &[]);
        r
    }

    /// Knuth's TwoSum: `(s, e)` with `s = a + b` rounded and `a + b = s + e`
    /// exactly.
    pub(crate) fn two_sum(&mut self, a: u32, b: u32) -> (u32, u32) {
        let s = self.exact(op::F_ADD, a, b);
        let bb = self.exact(op::F_SUB, s, a);
        let aa = self.exact(op::F_SUB, s, bb);
        let da = self.exact(op::F_SUB, a, aa);
        let db = self.exact(op::F_SUB, b, bb);
        let e = self.exact(op::F_ADD, da, db);
        (s, e)
    }

    /// The fused `a * b + c`, through `OpFmaKHR`: only for a host with
    /// [`HostProfile::fused_fma`](super::HostProfile::fused_fma).
    pub(crate) fn fma(&mut self, a: u32, b: u32, c: u32) -> u32 {
        debug_assert!(self.host.fused_fma);
        self.cap(CAP_FMA_KHR);
        self.exts.insert("SPV_KHR_fma");
        self.iface.host.fma_khr = true;
        let f = self.f32t();
        self.m.emit(OP_FMA_KHR, f, &[a, b, c])
    }

    /// `a * b - p` exactly, for the nearest product `p` of `a` and `b` with
    /// no intermediate below the normal range: a fused multiply-add where
    /// the host promises one, else Dekker's product of halves split with
    /// Veltkamp's constant, which needs only correctly rounded multiplies
    /// and adds.
    pub(crate) fn prod_err(&mut self, a: u32, b: u32, p: u32) -> u32 {
        if self.host.fused_fma {
            let np = self.fneg(p);
            return self.fma(a, b, np);
        }
        let (ah, al) = self.split(a);
        let (bh, bl) = self.split(b);
        let hh = self.exact(op::F_MUL, ah, bh);
        let t = self.exact(op::F_SUB, hh, p);
        let hl = self.exact(op::F_MUL, ah, bl);
        let t = self.exact(op::F_ADD, t, hl);
        let lh = self.exact(op::F_MUL, al, bh);
        let t = self.exact(op::F_ADD, t, lh);
        let ll = self.exact(op::F_MUL, al, bl);
        self.exact(op::F_ADD, t, ll)
    }

    /// `(hi, lo)` with `hi + lo = a` exactly and each half of 12 bits.
    fn split(&mut self, a: u32) -> (u32, u32) {
        let k = self.m.c_f32(4097.0f32.to_bits());
        let t = self.exact(op::F_MUL, k, a);
        let d = self.exact(op::F_SUB, t, a);
        let hi = self.exact(op::F_SUB, t, d);
        let lo = self.exact(op::F_SUB, a, hi);
        (hi, lo)
    }

    /// `-a` by its sign bit, which keeps a denormal on any host.
    pub(crate) fn fneg(&mut self, a: u32) -> u32 {
        let u = self.u32t();
        let bits = self.as_bits(a);
        let k = self.m.c_u32(0x8000_0000);
        let n = self.m.emit(op::BITWISE_XOR, u, &[bits, k]);
        self.as_float(n)
    }

    pub(crate) fn is_inf(&mut self, a: u32) -> u32 {
        let b = self.boolt();
        self.m.emit(op::IS_INF, b, &[a])
    }

    pub(crate) fn fcmp(&mut self, opc: u16, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(opc, t, &[a, b])
    }

    pub(crate) fn and(&mut self, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(op::LOGICAL_AND, t, &[a, b])
    }

    pub(crate) fn or(&mut self, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(op::LOGICAL_OR, t, &[a, b])
    }

    pub(crate) fn not(&mut self, a: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(op::LOGICAL_NOT, t, &[a])
    }

    /// Steps the nearest-rounded `s` to the result of `mode`, given whether
    /// the exact value lies above (`up`) or below (`down`) it. With `ftz` a
    /// zero stays zero and a step into the subnormal range gives a signed
    /// zero, as the hardware flushes after rounding.
    pub(crate) fn directed(&mut self, s: u32, up: u32, down: u32, mode: Round, ftz: bool) -> u32 {
        let u = self.u32t();
        let zero = self.m.c_f32(0);
        let pos = self.fcmp(op::F_ORD_GREATER_THAN, s, zero);
        let neg = self.fcmp(op::F_ORD_LESS_THAN, s, zero);
        let (go_up, go_down) = match mode {
            Round::Zero => (self.and(up, neg), self.and(down, pos)),
            Round::Down => (self.m.c_bool(false), down),
            Round::Up => (up, self.m.c_bool(false)),
            Round::Nearest => return s,
        };
        let bits = self.as_bits(s);
        let one = self.m.c_u32(1);
        let inc = self.m.emit(op::I_ADD, u, &[bits, one]);
        let dec = self.m.emit(op::I_SUB, u, &[bits, one]);
        let nonzero = self.or(pos, neg);
        let up_bits = self.select(u, pos, inc, dec);
        let down_bits = self.select(u, pos, dec, inc);
        let (up_bits, down_bits) = if ftz {
            (
                self.select(u, nonzero, up_bits, bits),
                self.select(u, nonzero, down_bits, bits),
            )
        } else {
            let tiny_pos = self.m.c_u32(1);
            let tiny_neg = self.m.c_u32(0x8000_0001);
            (
                self.select(u, nonzero, up_bits, tiny_pos),
                self.select(u, nonzero, down_bits, tiny_neg),
            )
        };
        let r = self.select(u, go_up, up_bits, bits);
        let mut r = self.select(u, go_down, down_bits, r);
        if ftz {
            let b = self.boolt();
            let mag_mask = self.m.c_u32(0x7fff_ffff);
            let sign_mask = self.m.c_u32(0x8000_0000);
            let min_normal = self.m.c_u32(0x0080_0000);
            let mag = self.m.emit(op::BITWISE_AND, u, &[r, mag_mask]);
            let tiny = self.m.emit(op::U_LESS_THAN, b, &[mag, min_normal]);
            let sign = self.m.emit(op::BITWISE_AND, u, &[r, sign_mask]);
            r = self.select(u, tiny, sign, r);
        }
        self.as_float(r)
    }

    /// `(above, below)`: whether the exact value lies above or below the
    /// rounded `s`, from the error term `e`, with the overflow case added.
    pub(crate) fn error_sides(&mut self, s: u32, e: u32, finite_inputs: u32) -> (u32, u32) {
        let zero = self.m.c_f32(0);
        let ep = self.fcmp(op::F_ORD_GREATER_THAN, e, zero);
        let en = self.fcmp(op::F_ORD_LESS_THAN, e, zero);
        let inf = self.is_inf(s);
        let ovf = self.and(inf, finite_inputs);
        let sp = self.fcmp(op::F_ORD_GREATER_THAN, s, zero);
        let sn = self.fcmp(op::F_ORD_LESS_THAN, s, zero);
        let ovf_up = self.and(ovf, sn);
        let ovf_down = self.and(ovf, sp);
        let not_ovf = self.not(ovf);
        let ep = self.and(ep, not_ovf);
        let en = self.and(en, not_ovf);
        (self.or(ep, ovf_up), self.or(en, ovf_down))
    }

    /// None of `xs` is infinite or NaN.
    pub(crate) fn all_finite(&mut self, xs: &[u32]) -> u32 {
        let mut acc = self.m.c_bool(true);
        for &x in xs {
            let i = self.is_inf(x);
            let b = self.boolt();
            let n = self.m.emit(op::IS_NAN, b, &[x]);
            let i = self.or(i, n);
            let ni = self.not(i);
            acc = self.and(acc, ni);
        }
        acc
    }

    pub(crate) fn i32t(&mut self) -> u32 {
        self.m.t_i32()
    }

    /// The unbiased exponent field of `x` as a signed integer.
    pub(crate) fn fexp(&mut self, x: u32) -> u32 {
        let u = self.u32t();
        let i = self.i32t();
        let bits = self.as_bits(x);
        let k23 = self.m.c_u32(23);
        let ff = self.m.c_u32(0xff);
        let sh = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[bits, k23]);
        let f = self.m.emit(op::BITWISE_AND, u, &[sh, ff]);
        let fi = self.m.emit(op::BITCAST, i, &[f]);
        let bias = self.m.c_i32(127);
        self.m.emit(op::I_SUB, i, &[fi, bias])
    }

    /// `x * 2^k` for `|k|` up to 254, as up to three multiplications by
    /// powers of two built from their bits. The pieces share the sign of
    /// `k`, so no step overflows or underflows unless the result does.
    /// GLSL `Ldexp` costs a driver about as long to compile as two hundred
    /// plain instructions, which the exact paths' call sites multiply.
    pub(crate) fn ldexp(&mut self, x: u32, k: u32) -> u32 {
        let f = self.f32t();
        let i = self.i32t();
        let lo = self.m.c_i32(-126);
        let hi = self.m.c_i32(126);
        let a = self.ext(i, glsl::S_CLAMP, &[k, lo, hi]);
        let r = self.m.emit(op::I_SUB, i, &[k, a]);
        let b = self.ext(i, glsl::S_CLAMP, &[r, lo, hi]);
        let c = self.m.emit(op::I_SUB, i, &[r, b]);
        let mut y = x;
        for e in [a, b, c] {
            let p = self.pow2(e);
            y = self.m.emit(op::F_MUL, f, &[y, p]);
        }
        y
    }

    /// `2^e` for `e` from -126 to 127, from its bits.
    fn pow2(&mut self, e: u32) -> u32 {
        let u = self.u32t();
        let i = self.i32t();
        let bias = self.m.c_i32(127);
        let k23 = self.m.c_u32(23);
        let biased = self.m.emit(op::I_ADD, i, &[e, bias]);
        let bits = self.m.emit(op::BITCAST, u, &[biased]);
        let sh = self.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[bits, k23]);
        self.as_float(sh)
    }

    pub(crate) fn ineg(&mut self, k: u32) -> u32 {
        let i = self.i32t();
        self.m.emit(op::S_NEGATE, i, &[k])
    }

    /// `xs`, or a tiny normal value with the sign of `x` if scaling left a
    /// nonzero `x` (after the guest's input flush with `ftz`) below the
    /// normal range, where a flushing host would read it as zero: it then
    /// only breaks ties, which keeps the sign of the error right. Both tests
    /// are on the bits, which no host flushes.
    pub(crate) fn keep_tiny(&mut self, xs: u32, x: u32, ftz: bool) -> u32 {
        let u = self.u32t();
        let f = self.f32t();
        let b = self.boolt();
        let mag_mask = self.m.c_u32(0x7fff_ffff);
        let min_normal = self.m.c_u32(0x0080_0000);
        let sbits = self.as_bits(xs);
        let smag = self.m.emit(op::BITWISE_AND, u, &[sbits, mag_mask]);
        let small = self.m.emit(op::U_LESS_THAN, b, &[smag, min_normal]);
        let bits = self.as_bits(x);
        let mag = self.m.emit(op::BITWISE_AND, u, &[bits, mag_mask]);
        let floor = self.m.c_u32(if ftz { 0x0080_0000 } else { 1 });
        let x_nz = self.m.emit(op::U_GREATER_THAN_EQUAL, b, &[mag, floor]);
        let lost = self.and(small, x_nz);
        let sign_mask = self.m.c_u32(0x8000_0000);
        let sign = self.m.emit(op::BITWISE_AND, u, &[bits, sign_mask]);
        let tiny = self.m.c_u32(0x0d80_0000);
        let t = self.m.emit(op::BITWISE_OR, u, &[sign, tiny]);
        let tf = self.as_float(t);
        self.select(f, lost, tf, xs)
    }

    /// `x` is +0, counting a positive subnormal (flushed on input) as +0.
    pub(crate) fn is_pos_zero(&mut self, x: u32) -> u32 {
        let b = self.boolt();
        let u = self.u32t();
        let bits = self.as_bits(x);
        let sign_mask = self.m.c_u32(0x8000_0000);
        let sign = self.m.emit(op::BITWISE_AND, u, &[bits, sign_mask]);
        let z = self.m.c_u32(0);
        let pos = self.m.emit(op::I_EQUAL, b, &[sign, z]);
        let fz = self.m.c_f32(0);
        let zero = self.fcmp(op::F_ORD_EQUAL, x, fz);
        self.and(pos, zero)
    }

    /// Under RM an exact zero from operands that are not both +0 is -0.
    pub(crate) fn rm_zero(&mut self, r: u32, exact_zero: u32, plus: u32) -> u32 {
        let f = self.f32t();
        let nplus = self.not(plus);
        let neg = self.and(exact_zero, nplus);
        let nz = self.m.c_f32(0x8000_0000);
        self.select(f, neg, nz, r)
    }

    /// The hardware rounds to 24 bits with an unbounded exponent and then
    /// flushes a subnormal result. When the nearest result `r` flushed to
    /// zero, a directed rounding away from zero still reaches the smallest
    /// normal if the scaled nearest result `alt` (scaled by `2^-k`) is the
    /// largest 24-bit value below it and the exact value lies beyond `alt`.
    pub(crate) fn ftz_tiny(
        &mut self,
        out: u32,
        r: u32,
        alt: u32,
        e: u32,
        k: u32,
        mode: Round,
    ) -> u32 {
        let f = self.f32t();
        let u = self.u32t();
        let zero = self.m.c_f32(0);
        let (alt_side, e_side) = match mode {
            Round::Down => (op::F_ORD_LESS_THAN, op::F_ORD_LESS_THAN),
            Round::Up => (op::F_ORD_GREATER_THAN, op::F_ORD_GREATER_THAN),
            _ => return out,
        };
        let rz = self.fcmp(op::F_ORD_EQUAL, r, zero);
        let below = self.m.c_f32(0x3f7f_ffff);
        let nk = self.ineg(k);
        let i = self.i32t();
        let m126 = self.m.c_i32(-126);
        let sh = self.m.emit(op::I_ADD, i, &[m126, nk]);
        let t = self.ldexp(below, sh);
        let abs_alt = self.ext(f, glsl::F_ABS, &[alt]);
        let at = self.fcmp(op::F_ORD_EQUAL, abs_alt, t);
        let side = self.fcmp(alt_side, alt, zero);
        let beyond = self.fcmp(e_side, e, zero);
        let hit = self.and(rz, at);
        let hit = self.and(hit, side);
        let hit = self.and(hit, beyond);
        let bits = self.as_bits(alt);
        let sign_mask = self.m.c_u32(0x8000_0000);
        let sign = self.m.emit(op::BITWISE_AND, u, &[bits, sign_mask]);
        let min_normal = self.m.c_u32(0x0080_0000);
        let v = self.m.emit(op::BITWISE_OR, u, &[sign, min_normal]);
        let v = self.as_float(v);
        self.select(f, hit, v, out)
    }

    /// `a + b` rounded by `mode`, from the nearest-rounded sum and the
    /// sign of its error (see [`Ctx::add_error`]).
    pub(crate) fn fadd_round(&mut self, a: u32, b: u32, mode: Round, ftz: bool) -> u32 {
        let s = self.exact(op::F_ADD, a, b);
        if mode == Round::Nearest {
            return s;
        }
        let (s1, e) = self.add_error(a, b, ftz);
        let fin = self.all_finite(&[a, b]);
        let (up, down) = self.error_sides(s, e, fin);
        let r = self.directed(s, up, down, mode, ftz);
        if mode != Round::Down {
            return r;
        }
        let zero = self.m.c_f32(0);
        let ez = self.fcmp(op::F_ORD_EQUAL, s1, zero);
        let pa = self.is_pos_zero(a);
        let pb = self.is_pos_zero(b);
        let plus = self.and(pa, pb);
        self.rm_zero(r, ez, plus)
    }

    /// The nearest-rounded sum `a + b`, or a copy of it scaled by a power of
    /// two, and the exact error of that sum (TwoSum). When each operand is
    /// zero or between `2^-103` and `2^126` in magnitude, every step of
    /// TwoSum on the operands themselves is a multiple of `2^-126` below
    /// `2^128`: zero or a normal value, so a flushing host gets the exact
    /// error too and nothing overflows. Otherwise the error is found on
    /// copies scaled to the larger operand's binade, so it never lands in the
    /// subnormal range; that path is a branch, taken only by such operands.
    pub(crate) fn add_error(&mut self, a: u32, b: u32, ftz: bool) -> (u32, u32) {
        let u = self.u32t();
        let bt = self.boolt();
        let mag_mask = self.m.c_u32(0x7fff_ffff);
        let low = self.m.c_u32(24 << 23);
        let span = self.m.c_u32((253 - 24) << 23);
        let zero = self.m.c_u32(0);
        let mut direct = self.m.c_bool(true);
        for x in [a, b] {
            let bits = self.as_bits(x);
            let mag = self.m.emit(op::BITWISE_AND, u, &[bits, mag_mask]);
            let off = self.m.emit(op::I_SUB, u, &[mag, low]);
            let mid = self.m.emit(op::U_LESS_THAN, bt, &[off, span]);
            let z = self.m.emit(op::I_EQUAL, bt, &[mag, zero]);
            let ok = self.or(mid, z);
            direct = self.and(direct, ok);
        }
        let fast = self.m.id();
        let slow = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[direct, fast, slow]);
        self.m.emit_void(op::LABEL, &[fast]);
        let (sf, ef) = self.two_sum(a, b);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[slow]);
        let i = self.i32t();
        let ea = self.fexp(a);
        let eb = self.fexp(b);
        let m = self.ext(i, glsl::S_MAX, &[ea, eb]);
        let nm = self.ineg(m);
        let a1 = self.ldexp(a, nm);
        let b1 = self.ldexp(b, nm);
        let a1 = self.keep_tiny(a1, a, ftz);
        let b1 = self.keep_tiny(b1, b, ftz);
        let (ss, es) = self.two_sum(a1, b1);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
        let f = self.f32t();
        let s1 = self.m.emit(op::PHI, f, &[sf, fast, ss, slow]);
        let e = self.m.emit(op::PHI, f, &[ef, fast, es, slow]);
        (s1, e)
    }

    /// `a * b` rounded by `mode`, its error found on the significands.
    pub(crate) fn fmul_round(&mut self, a: u32, b: u32, mode: Round, ftz: bool) -> u32 {
        let p = self.exact(op::F_MUL, a, b);
        if mode == Round::Nearest {
            return p;
        }
        let ea = self.fexp(a);
        let eb = self.fexp(b);
        let na = self.ineg(ea);
        let nb = self.ineg(eb);
        let a1 = self.ldexp(a, na);
        let b1 = self.ldexp(b, nb);
        let p1 = self.exact(op::F_MUL, a1, b1);
        let e = self.prod_err(a1, b1, p1);
        let fin = self.all_finite(&[a, b]);
        let (up, down) = self.error_sides(p, e, fin);
        let out = self.directed(p, up, down, mode, ftz);
        if !ftz {
            return out;
        }
        let i = self.i32t();
        let k = self.m.emit(op::I_ADD, i, &[ea, eb]);
        self.ftz_tiny(out, p, p1, e, k, mode)
    }

    /// `a * b + c` with one rounding by `mode`. The residual
    /// `a * b + c - r` is summed exactly (TwoSum cascade) on copies scaled
    /// by the product's exponent, with `c` clamped where it can only break
    /// ties or where it alone decides the result.
    pub(crate) fn ffma_round(&mut self, a: u32, b: u32, c: u32, mode: Round, ftz: bool) -> u32 {
        let r = self.fma(a, b, c);
        if mode == Round::Nearest {
            return r;
        }
        let f = self.f32t();
        let i = self.i32t();
        let bt = self.boolt();
        let ea = self.fexp(a);
        let eb = self.fexp(b);
        let ec = self.fexp(c);
        let k = self.m.emit(op::I_ADD, i, &[ea, eb]);
        let d = self.m.emit(op::I_SUB, i, &[ec, k]);
        let na = self.ineg(ea);
        let nb = self.ineg(eb);
        let nk = self.ineg(k);
        let nc = self.ineg(ec);
        let a1 = self.ldexp(a, na);
        let b1 = self.ldexp(b, nb);
        let lo = self.m.c_i32(-100);
        let hi = self.m.c_i32(60);
        let dc = self.ext(i, glsl::S_CLAMP, &[d, lo, hi]);
        let mc = self.ldexp(c, nc);
        let c1 = self.ldexp(mc, dc);
        let p1 = self.exact(op::F_MUL, a1, b1);
        let r1 = self.ldexp(r, nk);
        let alt = self.fma(a1, b1, c1);
        let fz = self.m.c_f32(0);
        let r_zero = self.fcmp(op::F_ORD_EQUAL, r, fz);
        let r1 = if ftz {
            self.select(f, r_zero, alt, r1)
        } else {
            r1
        };
        let np1 = self.fneg(p1);
        let pe = self.fma(a1, b1, np1);
        let (u, ue) = self.two_sum(p1, c1);
        let nr = self.fneg(r1);
        let (v, ve) = self.two_sum(u, nr);
        let (t1, f1) = self.two_sum(ue, pe);
        let (t2, f2) = self.two_sum(ve, t1);
        let (t3, f3) = self.two_sum(v, t2);
        let low = self.exact(op::F_ADD, f2, f1);
        let low = self.exact(op::F_ADD, f3, low);
        let zero = self.m.c_f32(0);
        let t3z = self.fcmp(op::F_ORD_EQUAL, t3, zero);
        let e = self.select(f, t3z, low, t3);
        let big = self.m.emit(op::S_GREATER_THAN, bt, &[d, hi]);
        let c_nz = self.fcmp(op::F_UNORD_NOT_EQUAL, c, zero);
        let big = self.and(big, c_nz);
        let e = self.select(f, big, p1, e);
        let pz = self.fcmp(op::F_ORD_EQUAL, p1, zero);
        let e = self.select(f, pz, zero, e);
        let fin = self.all_finite(&[a, b, c]);
        let e = self.select(f, fin, e, zero);
        let (up, down) = self.error_sides(r, e, fin);
        let out = self.directed(r, up, down, mode, ftz);
        let out = if ftz {
            self.ftz_tiny(out, r, alt, e, k, mode)
        } else {
            out
        };
        if mode != Round::Down {
            return out;
        }
        let uz = self.fcmp(op::F_ORD_EQUAL, u, zero);
        let uez = self.fcmp(op::F_ORD_EQUAL, ue, zero);
        let pez = self.fcmp(op::F_ORD_EQUAL, pe, zero);
        let ez = self.and(uz, uez);
        let ez = self.and(ez, pez);
        let nbig = self.not(big);
        let ez = self.and(ez, nbig);
        let pa = self.as_bits(a);
        let pb = self.as_bits(b);
        let u32t = self.u32t();
        let sx = self.m.emit(op::BITWISE_XOR, u32t, &[pa, pb]);
        let sign_mask = self.m.c_u32(0x8000_0000);
        let sp = self.m.emit(op::BITWISE_AND, u32t, &[sx, sign_mask]);
        let z = self.m.c_u32(0);
        let prod_pos = self.m.emit(op::I_EQUAL, bt, &[sp, z]);
        let prod_zero = pz;
        let pc = self.is_pos_zero(c);
        let plus = self.and(prod_zero, prod_pos);
        let plus = self.and(plus, pc);
        let cz = self.fcmp(op::F_ORD_EQUAL, c, zero);
        let exact_zero = self.select(bt, prod_zero, cz, ez);
        self.rm_zero(out, exact_zero, plus)
    }

    /// The float comparison `cmp` of `a` and `b`.
    pub(crate) fn fcompare(&mut self, cmp: &str, a: u32, b: u32) -> Res<u32> {
        let opc = match cmp {
            "F" => return Ok(self.m.c_bool(false)),
            "T" => return Ok(self.m.c_bool(true)),
            "LT" => op::F_ORD_LESS_THAN,
            "EQ" => op::F_ORD_EQUAL,
            "LE" => op::F_ORD_LESS_THAN_EQUAL,
            "GT" => op::F_ORD_GREATER_THAN,
            "NE" => op::F_ORD_NOT_EQUAL,
            "GE" => op::F_ORD_GREATER_THAN_EQUAL,
            "LTU" => op::F_UNORD_LESS_THAN,
            "EQU" => op::F_UNORD_EQUAL,
            "LEU" => op::F_UNORD_LESS_THAN_EQUAL,
            "GTU" => op::F_UNORD_GREATER_THAN,
            "NEU" => op::F_UNORD_NOT_EQUAL,
            "GEU" => op::F_UNORD_GREATER_THAN_EQUAL,
            "NUM" | "NAN" => {
                let b_ = self.boolt();
                let na = self.m.emit(op::IS_NAN, b_, &[a]);
                let nb = self.m.emit(op::IS_NAN, b_, &[b]);
                let any = self.or(na, nb);
                return Ok(if cmp == "NAN" { any } else { self.not(any) });
            }
            c => return bad(format!("float comparison {c}")),
        };
        Ok(self.fcmp(opc, a, b))
    }

    /// `a bop b` for the AND, OR, XOR of predicate-combining instructions.
    pub(crate) fn bool_op(&mut self, bop: &str, a: u32, b: u32) -> Res<u32> {
        let t = self.boolt();
        Ok(match bop {
            "AND" => self.m.emit(op::LOGICAL_AND, t, &[a, b]),
            "OR" => self.m.emit(op::LOGICAL_OR, t, &[a, b]),
            "XOR" => self.m.emit(op::LOGICAL_NOT_EQUAL, t, &[a, b]),
            o => return bad(format!("boolean op {o}")),
        })
    }
}

fn arith(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(
        i,
        &[
            "FTZ", "SAT", "RZ", "RM", "RP", "D2", "D4", "D8", "M2", "M4", "M8", "DNZ",
        ],
    )?;
    if i.has("DNZ") {
        return bad("FMUL.DNZ");
    }
    let mode = round_of(i);
    let ftz = i.has("FTZ");
    let mut args = vec![c.fsrc_bits(&i.ops[1])?, c.fsrc_bits(&i.ops[2])?];
    if i.mnemonic == "FFMA" {
        args.push(c.fsrc_bits(&i.ops[3])?);
    }
    let scale = [
        ("D2", -1),
        ("D4", -2),
        ("D8", -3),
        ("M2", 1),
        ("M4", 2),
        ("M8", 3),
    ]
    .iter()
    .find(|(m, _)| i.has(m))
    .map_or(0, |(_, s)| *s);
    let sat = i.has("SAT");
    let name = i.mnemonic;
    if c.math == Math::Native && mode == Round::Nearest {
        let u = c.shared_pure(format!("native {name} {scale} {sat}"), &args, |c, p| {
            c.native_arith(name, p, scale, sat)
        });
        return c.store_dst(&i.ops[0], u);
    }
    let key = format!("{name} {mode:?} {ftz} {scale} {sat}");
    let u = c.shared_pure(key, &args, |c, p| {
        let r = match name {
            "FADD" => c.fadd_exact(p[0], p[1], mode, ftz),
            "FMUL" => {
                let r = c.fmul_exact(p[0], p[1], mode, ftz, scale);
                c.mul_zero_sign(r, p[0], p[1])
            }
            _ => c.ffma_exact(p[0], p[1], p[2], mode, ftz),
        };
        c.sat_bits(r, sat)
    });
    c.store_dst(&i.ops[0], u)
}

impl Ctx {
    /// FADD, FMUL or FFMA on the bits `p` with the host's own arithmetic
    /// ([`Math::Native`]): GLSL `Fma` for FFMA, plain OpFAdd and OpFMul
    /// otherwise, the scale of `.D2` to `.M8` as a multiply by its power of
    /// two, and `.SAT` as a float clamp. `.FTZ` is what the shader's
    /// denormal mode makes of it.
    fn native_arith(&mut self, name: &str, p: &[u32], scale: i32, sat: bool) -> u32 {
        let f = self.f32t();
        let x: Vec<u32> = p.iter().map(|&v| self.as_float(v)).collect();
        let mut r = match name {
            "FADD" => self.m.emit(op::F_ADD, f, &[x[0], x[1]]),
            "FMUL" => self.m.emit(op::F_MUL, f, &[x[0], x[1]]),
            _ => self.ext(f, glsl::FMA, &[x[0], x[1], x[2]]),
        };
        if scale != 0 {
            let k = self.m.c_f32(2.0f32.powi(scale).to_bits());
            r = self.m.emit(op::F_MUL, f, &[r, k]);
        }
        if sat {
            let lo = self.m.c_f32(0);
            let hi = self.m.c_f32(1.0f32.to_bits());
            r = self.ext(f, glsl::N_CLAMP, &[r, lo, hi]);
        }
        self.as_bits(r)
    }
}

/// FMNMX, and with `.NAN` a NaN in either input gives the canonical NaN
/// `0x7fffffff` instead of the other input.
fn fmnmx(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["FTZ", "NAN"])?;
    let u = c.u32t();
    let a = c.fsrc_bits(&i.ops[1])?;
    let b = c.fsrc_bits(&i.ops[2])?;
    let p = c.pred_operand(&i.ops[3])?;
    let (one, zero) = (c.m.c_u32(1), c.m.c_u32(0));
    let pu = c.select(u, p, one, zero);
    let (ftz, nan) = (i.has("FTZ"), i.has("NAN"));
    let r = c.shared_pure(format!("FMNMX {ftz} {nan}"), &[a, b, pu], |c, x| {
        let u = c.u32t();
        let (a, b) = (x[0], x[1]);
        let (lo, hi) = c.fminmax(a, b, ftz);
        let z = c.m.c_u32(0);
        let b_ = c.boolt();
        let p = c.m.emit(op::I_NOT_EQUAL, b_, &[x[2], z]);
        let r = c.select(u, p, lo, hi);
        if nan {
            let na = c.is_nan_bits(a);
            let nb = c.is_nan_bits(b);
            let any = c.or(na, nb);
            let qnan = c.m.c_u32(0x7fff_ffff);
            c.select(u, any, qnan, r)
        } else {
            r
        }
    });
    c.store_dst(&i.ops[0], r)
}

fn fsel(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["FTZ"])?;
    let a = c.fsrc_bits(&i.ops[1])?;
    let b = c.fsrc_bits(&i.ops[2])?;
    let p = c.pred_operand(&i.ops[3])?;
    let u = c.u32t();
    let r = c.select(u, p, a, b);
    c.store_dst(&i.ops[0], r)
}

fn cmp_parts(i: &Inst) -> Res<(&'static str, &'static str)> {
    let mut cmp = None;
    let mut bop = None;
    for m in &i.mods {
        match *m {
            "FTZ" | "BF" => {}
            "AND" | "OR" | "XOR" => bop = Some(*m),
            m => cmp = Some(m),
        }
    }
    match (cmp, bop) {
        (Some(c), Some(b)) => Ok((c, b)),
        _ => bad(format!("{} form", i.mnemonic)),
    }
}

fn fsetp(c: &mut Ctx, i: &Inst) -> Res<()> {
    let (cmp, bop) = cmp_parts(i)?;
    let a = c.fsrc_bits(&i.ops[2])?;
    let b = c.fsrc_bits(&i.ops[3])?;
    let pc = c.pred_operand(&i.ops[4])?;
    let r = c.fcompare_exact(cmp, a, b, i.has("FTZ"))?;
    let nr = c.not(r);
    let p = c.bool_op(bop, r, pc)?;
    let q = c.bool_op(bop, nr, pc)?;
    c.store_pdst(&i.ops[0], p)?;
    c.store_pdst(&i.ops[1], q)
}

fn fset(c: &mut Ctx, i: &Inst) -> Res<()> {
    let (cmp, bop) = cmp_parts(i)?;
    let a = c.fsrc_bits(&i.ops[1])?;
    let b = c.fsrc_bits(&i.ops[2])?;
    let pc = c.pred_operand(&i.ops[3])?;
    let r = c.fcompare_exact(cmp, a, b, i.has("FTZ"))?;
    let p = c.bool_op(bop, r, pc)?;
    let one = if i.has("BF") {
        1.0f32.to_bits()
    } else {
        u32::MAX
    };
    let u = c.bool_to_u32(p, one);
    c.store_dst(&i.ops[0], u)
}

fn frnd(c: &mut Ctx, i: &Inst) -> Res<()> {
    super::int::conv::frnd(c, i)
}

/// The invocation's position in its 2x2 quad: in a fragment shader from the
/// pixel's parity (lane 0 top left, 1 top right, 2 bottom left, 3 bottom
/// right, as the hardware packs quads), which needs no subgroup; elsewhere
/// from the lane id.
fn quad_lane(c: &mut Ctx) -> u32 {
    let u = c.u32t();
    let one = c.m.c_u32(1);
    if c.stage != Stage::Fragment {
        let l = c.lane_id();
        let three = c.m.c_u32(3);
        return c.m.emit(op::BITWISE_AND, u, &[l, three]);
    }
    let f = c.f32t();
    let v4 = c.m.t_vec(f, 4);
    let fc = c.builtin(15, super::INPUT, v4);
    let p = c.m.t_ptr(super::INPUT, f);
    let mut bits = Vec::new();
    for k in 0..2 {
        let ki = c.m.c_u32(k);
        let ptr = c.m.emit(op::ACCESS_CHAIN, p, &[fc, ki]);
        let x = c.m.emit(op::LOAD, f, &[ptr]);
        let xi = c.m.emit(op::CONVERT_F_TO_U, u, &[x]);
        bits.push(c.m.emit(op::BITWISE_AND, u, &[xi, one]));
    }
    let y2 = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[bits[1], one]);
    c.m.emit(op::BITWISE_OR, u, &[bits[0], y2])
}

/// FSWZADD: per quad lane, `a + b`, `a - b`, `b - a` or `+0 + b`, picked
/// by the lane's two bits of the control byte (lane 0 takes the top two
/// bits), with the instruction's rounding.
fn fswzadd(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["FTZ", "NDV", "RZ", "RM", "RP"])?;
    let mode = round_of(i);
    let ftz = i.has("FTZ");
    let a = c.fsrc_bits(&i.ops[1])?;
    let b = c.fsrc_bits(&i.ops[2])?;
    let ctl = u32::from(c.fields.swizzle);
    let lane = quad_lane(c);
    let u = c.u32t();
    let two = c.m.c_u32(2);
    let three_l = c.m.c_u32(3);
    let rev = c.m.emit(op::I_SUB, u, &[three_l, lane]);
    let sh = c.m.emit(op::I_MUL, u, &[rev, two]);
    let k = c.m.c_u32(ctl);
    let code = c.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[k, sh]);
    let three = c.m.c_u32(3);
    let code = c.m.emit(op::BITWISE_AND, u, &[code, three]);
    let bt = c.boolt();
    let sign = c.m.c_u32(0x8000_0000);
    let na = c.m.emit(op::BITWISE_XOR, u, &[a, sign]);
    let nb = c.m.emit(op::BITWISE_XOR, u, &[b, sign]);
    let zero = c.m.c_u32(0);
    let one_c = c.m.c_u32(1);
    let two_c = c.m.c_u32(2);
    let three_c = c.m.c_u32(3);
    let is1 = c.m.emit(op::I_EQUAL, bt, &[code, one_c]);
    let is2 = c.m.emit(op::I_EQUAL, bt, &[code, two_c]);
    let is3 = c.m.emit(op::I_EQUAL, bt, &[code, three_c]);
    let x = c.select(u, is2, na, a);
    let x = c.select(u, is3, zero, x);
    let y = c.select(u, is1, nb, b);
    let r = c.fadd_exact(x, y, mode, ftz);
    c.store_dst(&i.ops[0], r)
}

//! FFMA (and FMUL) from integer operations, for hosts whose GLSL `Fma` is
//! not promised to be fused: Vulkan only requires it to be as precise as a
//! multiply followed by an add.
//!
//! The significands' 48-bit product and the addend sit in a 64-bit word with
//! the leading bit at or below bit 61; the operand with the smaller exponent
//! is shifted right with the bits it loses jammed into the lowest bit, which
//! keeps every rounding decision of the 24-bit result. The sum is rounded by
//! the instruction's mode to the 24-bit grid, or to the denormal grid
//! without `.FTZ`; with `.FTZ` a result below the normal range after
//! rounding becomes a signed zero, and denormal inputs read as zero.
//! Nothing depends on the host's float arithmetic.
//!
//! Round-to-nearest FFMA, nearly all of them, is built from float
//! operations instead ([`Ctx::float_fma`]): the exact product as a sum of
//! two floats (Dekker's product) and the correctly rounded sum of that pair
//! and the addend by Boldo and Melquiond's rounding to odd ("Emulation of FMA
//! and correctly rounded sums: proved algorithms using rounding to odd",
//! IEEE Trans. Computers 57(4), 2008). It needs only correctly rounded adds
//! and multiplies, which Vulkan promises, on operands rescaled so that no
//! step leaves the normal range, and no branch: a branch per FFMA would end
//! a block per FFMA and keep the compiler from interleaving them.

use super::Ctx;
use super::float::Round;
use crate::spirv::{glsl, op};

/// The two words of a 64-bit integer, (low, high).
type W64 = (u32, u32);

const NAN: u32 = 0x7fff_ffff;
const SIGN: u32 = 0x8000_0000;

/// One operand: sign bit, significand (24 bits, leading bit 23, zero for
/// zero), exponent of its leading bit, and the class tests.
struct Parts {
    sign: u32,
    sig: u32,
    exp: u32,
    zero: u32,
    inf: u32,
    nan: u32,
}

impl Ctx {
    fn sf_parts(&mut self, x: u32, ftz: bool) -> Parts {
        let x = if ftz { self.flush_in(x) } else { x };
        let sign = self.and_k(x, SIGN);
        let mag = self.and_k(x, !SIGN);
        let field = self.sh(op::SHIFT_RIGHT_LOGICAL, mag, 23);
        let man = self.and_k(x, 0x7f_ffff);
        let zero = self.cmp_k(op::I_EQUAL, mag, 0);
        let inf = self.cmp_k(op::I_EQUAL, mag, 0x7f80_0000);
        let nan = self.cmp_k(op::U_GREATER_THAN, mag, 0x7f80_0000);
        let sub = self.cmp_k(op::I_EQUAL, field, 0);
        let u = self.u32t();
        let msb = self.ext(u, glsl::FIND_U_MSB, &[man]);
        let k23 = self.kc(23);
        let shift = self.ib(op::I_SUB, k23, msb);
        let shift = self.and_k(shift, 31);
        let sub_sig = self.ib(op::SHIFT_LEFT_LOGICAL, man, shift);
        let k126 = self.kc((-126i32) as u32);
        let sub_e = self.ib(op::I_SUB, k126, shift);
        let norm_sig = self.sh(op::BITWISE_OR, man, 0x80_0000);
        let norm_e = self.sh(op::I_SUB, field, 127);
        let sig = self.bpick(sub, sub_sig, norm_sig);
        let exp = self.bpick(sub, sub_e, norm_e);
        let z = self.kc(0);
        let sig = self.bpick(zero, z, sig);
        Parts {
            sign,
            sig,
            exp,
            zero,
            inf,
            nan,
        }
    }

    fn sf_or(&mut self, v: W64) -> u32 {
        let o = self.ib(op::BITWISE_OR, v.0, v.1);
        self.cmp_k(op::I_NOT_EQUAL, o, 0)
    }

    fn sf_eq(&mut self, a: W64, b: W64) -> u32 {
        let l = self.icmp_op(op::I_EQUAL, a.0, b.0);
        let h = self.icmp_op(op::I_EQUAL, a.1, b.1);
        self.land(l, h)
    }

    /// `a >= b`, unsigned.
    fn sf_ge(&mut self, a: W64, b: W64) -> u32 {
        let hg = self.icmp_op(op::U_GREATER_THAN, a.1, b.1);
        let he = self.icmp_op(op::I_EQUAL, a.1, b.1);
        let lg = self.icmp_op(op::U_GREATER_THAN_EQUAL, a.0, b.0);
        let e = self.land(he, lg);
        self.lor(hg, e)
    }

    fn sf_sub(&mut self, a: W64, b: W64) -> W64 {
        let nl = self.inot(b.0);
        let nh = self.inot(b.1);
        let one = self.kc(1);
        let (l, h, _) = self.add64(a, (nl, nh), Some(one));
        (l, h)
    }

    fn sf_pick(&mut self, p: u32, a: W64, b: W64) -> W64 {
        (self.bpick(p, a.0, b.0), self.bpick(p, a.1, b.1))
    }

    /// `v >> s` for `s` in `[0, 64]` (larger counts as 64), with any bit
    /// shifted out ORed into the lowest bit.
    fn sf_shr_jam(&mut self, v: W64, s: u32) -> W64 {
        let u = self.u32t();
        let k64 = self.kc(64);
        let s = self.ext(u, glsl::U_MIN, &[s, k64]);
        let r = self.shift64(v.0, v.1, s, false, false);
        let back = self.shift64(r.0, r.1, s, true, false);
        let same = self.sf_eq(back, v);
        let lost = self.lnot(same);
        let j = self.b2u(lost);
        (self.ib(op::BITWISE_OR, r.0, j), r.1)
    }

    /// The bit index of the highest set bit of a nonzero `v`.
    fn sf_msb(&mut self, v: W64) -> u32 {
        let u = self.u32t();
        let hz = self.cmp_k(op::I_EQUAL, v.1, 0);
        let ml = self.ext(u, glsl::FIND_U_MSB, &[v.0]);
        let mh = self.ext(u, glsl::FIND_U_MSB, &[v.1]);
        let mh = self.sh(op::I_ADD, mh, 32);
        self.bpick(hz, ml, mh)
    }

    /// `x` (not infinite or NaN) as the bits of a float with its sign and
    /// significand and a magnitude in `[1, 2)`, its exponent, and its
    /// exponent field. Without `ftz` a denormal is normalized; with it, and
    /// for a zero, the result is that of the exponent field taken as it is,
    /// which the callers never use.
    fn bl_parts(&mut self, x: u32, ftz: bool) -> (u32, u32, u32) {
        let field = self.sh(op::SHIFT_RIGHT_LOGICAL, x, 23);
        let field = self.and_k(field, 0xff);
        let m = self.and_k(x, 0x807f_ffff);
        let m = self.sh(op::BITWISE_OR, m, 0x3f80_0000);
        let e = self.sh(op::I_SUB, field, 127);
        if ftz {
            return (m, e, field);
        }
        let u = self.u32t();
        let man = self.and_k(x, 0x7f_ffff);
        let msb = self.ext(u, glsl::FIND_U_MSB, &[man]);
        let k23 = self.kc(23);
        let shift = self.ib(op::I_SUB, k23, msb);
        let shift = self.and_k(shift, 31);
        let sm = self.ib(op::SHIFT_LEFT_LOGICAL, man, shift);
        let sm = self.and_k(sm, 0x7f_ffff);
        let sign = self.and_k(x, SIGN);
        let dm = self.sh(op::BITWISE_OR, sign, 0x3f80_0000);
        let dm = self.ib(op::BITWISE_OR, dm, sm);
        let k = self.kc((-126i32) as u32);
        let de = self.ib(op::I_SUB, k, shift);
        let sub = self.cmp_k(op::I_EQUAL, field, 0);
        (self.bpick(sub, dm, m), self.bpick(sub, de, e), field)
    }

    /// `x` is a zero, or with `ftz` a zero or a denormal: zero as the
    /// instruction reads it.
    fn bl_zero(&mut self, x: u32, field: u32, ftz: bool) -> u32 {
        if ftz {
            return self.cmp_k(op::I_EQUAL, field, 0);
        }
        let x2 = self.sh(op::SHIFT_LEFT_LOGICAL, x, 1);
        self.cmp_k(op::I_EQUAL, x2, 0)
    }

    /// `a * b + c` rounded to nearest, as SM86 FFMA computes it (with
    /// `.FTZ` when `ftz`), from float adds and multiplies on any host that
    /// rounds them correctly, flushing or not, with no branch.
    ///
    /// The factors are rebuilt as `x = a / 2^ea` and `y = b / 2^eb` with
    /// magnitudes in `[1, 2)`, the addend as `z = c / 2^(ea + eb)`; a `z`
    /// below `2^-50` (which then only decides ties and the sticky side)
    /// becomes `2^-60` of its sign, and a `z` from `2^29` up gives `c` as
    /// the result. So every value below is zero or normal, of magnitude
    /// from `2^-120` to below `2^40`, and a flushing host loses nothing:
    ///
    /// - The 12-bit halves of `x` and `y` are their bits with the low 12
    ///   bits cleared and the difference; Dekker's product of the halves
    ///   gives `ph + pl = x * y` exactly.
    /// - TwoSum gives `th + tl = z + ph`; `v` is `tl + pl` rounded to odd
    ///   (the nearest sum stepped to the odd neighbour toward the TwoSum
    ///   error when it is inexact, which is the sum rounded toward zero
    ///   with the last bit set). `r = th + v` rounded to nearest is then
    ///   `x * y + z` rounded once to 24 bits (Boldo and Melquiond,
    ///   Theorem 7 and its use for the FMA).
    /// - The result is `r` with `ea + eb` added to its exponent field: an
    ///   overflow gives an infinity; below the normal range `.FTZ` gives the
    ///   signed zero, and otherwise `r` is rounded again to the denormal
    ///   grid with integer operations, its ties broken by the sign of the
    ///   exact remainder `x * y + z - r` (the TwoSum error of `th + v`, or
    ///   where that is zero `tl + pl - v`), which makes that rounding the
    ///   rounding of the exact value.
    /// - An exact zero takes the sign rounding to nearest gives a zero sum,
    ///   negative only for `-0 + -0`; a zero product gives `c`; infinities
    ///   and NaNs come from the host's own multiply and add on the rebuilt
    ///   factors (an infinite or NaN `c` with finite factors is `c`), NaNs
    ///   as the guest's `0x7fffffff`. For `.FTZ` in a module that runs with
    ///   the device flushing as `.FTZ` does, the factors as they are: the
    ///   device reads a denormal one as the zero `.FTZ` asks for.
    pub(crate) fn float_fma(&mut self, a: u32, b: u32, c: u32, ftz: bool) -> u32 {
        let (ma, ea, fa) = self.bl_parts(a, ftz);
        let (mb, eb, fb) = self.bl_parts(b, ftz);
        let (mc, ec, fc) = self.bl_parts(c, ftz);
        let az = self.bl_zero(a, fa, ftz);
        let bz = self.bl_zero(b, fb, ftz);
        let cz = self.bl_zero(c, fc, ftz);
        let csign = self.and_k(c, SIGN);
        let c = if ftz { self.bpick(cz, csign, c) } else { c };
        let k = self.ib(op::I_ADD, ea, eb);
        let d = self.ib(op::I_SUB, ec, k);
        let low = self.cmp_k(op::S_LESS_THAN, d, (-50i32) as u32);
        let big = self.cmp_k(op::S_GREATER_THAN, d, 28);
        let zf = self.sh(op::I_ADD, d, 127);
        let zf = self.sh(op::SHIFT_LEFT_LOGICAL, zf, 23);
        let zm = self.and_k(mc, 0x807f_ffff);
        let zs = self.ib(op::BITWISE_OR, zm, zf);
        let tiny_z = self.sh(op::BITWISE_OR, csign, 67 << 23);
        let zs = self.bpick(low, tiny_z, zs);
        let zs = self.bpick(cz, csign, zs);
        let x = self.as_float(ma);
        let y = self.as_float(mb);
        let z = self.as_float(zs);
        let xhb = self.and_k(ma, 0xffff_f000);
        let yhb = self.and_k(mb, 0xffff_f000);
        let xh = self.as_float(xhb);
        let yh = self.as_float(yhb);
        let xl = self.exact(op::F_SUB, x, xh);
        let yl = self.exact(op::F_SUB, y, yh);
        let ph = self.exact(op::F_MUL, x, y);
        let hh = self.exact(op::F_MUL, xh, yh);
        let pl = self.exact(op::F_SUB, hh, ph);
        let hl = self.exact(op::F_MUL, xh, yl);
        let pl = self.exact(op::F_ADD, pl, hl);
        let lh = self.exact(op::F_MUL, xl, yh);
        let pl = self.exact(op::F_ADD, pl, lh);
        let ll = self.exact(op::F_MUL, xl, yl);
        let pl = self.exact(op::F_ADD, pl, ll);
        let (th, tl) = self.two_sum(z, ph);
        let (vs, ve) = self.two_sum(tl, pl);
        let vb = self.as_bits(vs);
        let eb_ = self.as_bits(ve);
        let e2 = self.sh(op::SHIFT_LEFT_LOGICAL, eb_, 1);
        let inexact = self.cmp_k(op::I_NOT_EQUAL, e2, 0);
        let inexact = self.b2u(inexact);
        let opp = self.ib(op::BITWISE_XOR, vb, eb_);
        let opp = self.sh(op::SHIFT_RIGHT_LOGICAL, opp, 31);
        let down = self.ib(op::BITWISE_AND, opp, inexact);
        let vo = self.ib(op::I_SUB, vb, down);
        let vo = self.ib(op::BITWISE_OR, vo, inexact);
        let v = self.as_float(vo);
        let (r, er) = if ftz {
            (self.exact(op::F_ADD, th, v), None)
        } else {
            let (r, er) = self.two_sum(th, v);
            (r, Some(er))
        };
        let rb = self.as_bits(r);
        let rsign = self.and_k(rb, SIGN);
        let fr = self.sh(op::SHIFT_RIGHT_LOGICAL, rb, 23);
        let fr = self.and_k(fr, 0xff);
        let re = self.ib(op::I_ADD, fr, k);
        let ks = self.sh(op::SHIFT_LEFT_LOGICAL, k, 23);
        let mut out = self.ib(op::I_ADD, rb, ks);
        let tiny = self.cmp_k(op::S_LESS_THAN, re, 1);
        let small = match er {
            None => rsign,
            Some(er) => self.bl_denormal(rb, re, er, vs, ve, v),
        };
        out = self.bpick(tiny, small, out);
        let ovf = self.cmp_k(op::S_GREATER_THAN, re, 254);
        let inf = self.sh(op::BITWISE_OR, rsign, 0x7f80_0000);
        out = self.bpick(ovf, inf, out);
        let pz = self.lor(az, bz);
        let r2 = self.sh(op::SHIFT_LEFT_LOGICAL, rb, 1);
        let rz = self.cmp_k(op::I_EQUAL, r2, 0);
        let zsign = self.ib(op::BITWISE_XOR, a, b);
        let zsign = self.ib(op::BITWISE_AND, zsign, c);
        let zsign = self.and_k(zsign, SIGN);
        let zero_out = self.lor(rz, cz);
        let zero_out = self.land(zero_out, pz);
        let zero_out = self.lor(zero_out, rz);
        out = self.bpick(zero_out, zsign, out);
        let ncz = self.lnot(cz);
        let take_c = self.lor(big, pz);
        let take_c = self.land(take_c, ncz);
        out = self.bpick(take_c, c, out);
        let fa = self.cmp_k(op::I_EQUAL, fa, 0xff);
        let fb = self.cmp_k(op::I_EQUAL, fb, 0xff);
        let fc = self.cmp_k(op::I_EQUAL, fc, 0xff);
        let (xs, ys) = if ftz && self.dn_dmode() == super::Denorm::Flush {
            (a, b)
        } else {
            let asign = self.and_k(a, SIGN);
            let bsign = self.and_k(b, SIGN);
            let xs = self.bpick(az, asign, ma);
            let xs = self.bpick(fa, a, xs);
            let ys = self.bpick(bz, bsign, mb);
            (xs, self.bpick(fb, b, ys))
        };
        let xs = self.as_float(xs);
        let ys = self.as_float(ys);
        let cf = self.as_float(c);
        let sp = self.exact(op::F_MUL, xs, ys);
        let ss = self.exact(op::F_ADD, sp, cf);
        let ssb = self.as_bits(ss);
        let fab = self.lor(fa, fb);
        let nfab = self.lnot(fab);
        let only_c = self.land(fc, nfab);
        let sv = self.bpick(only_c, c, ssb);
        let sv2 = self.sh(op::SHIFT_LEFT_LOGICAL, sv, 1);
        let nan = self.cmp_k(op::U_GREATER_THAN, sv2, 0xff00_0000);
        let kn = self.kc(NAN);
        let sv = self.bpick(nan, kn, sv);
        let special = self.lor(fab, fc);
        self.bpick(special, sv, out)
    }

    /// The bits of `r` (bits `rb`, a 24-bit rounding of a value `w` whose
    /// exponent field would be `re`, below 1) rounded to the denormal grid
    /// as `w` itself rounds to nearest: `w - r` is `er` where that is not
    /// zero, else `(vs + ve) - v`, with `v` adjacent to `vs`.
    fn bl_denormal(&mut self, rb: u32, re: u32, er: u32, vs: u32, ve: u32, v: u32) -> u32 {
        let u = self.u32t();
        let i = self.m.t_i32();
        let dv = self.exact(op::F_SUB, vs, v);
        let rest = self.exact(op::F_ADD, dv, ve);
        let erb = self.as_bits(er);
        let e2 = self.sh(op::SHIFT_LEFT_LOGICAL, erb, 1);
        let enz = self.cmp_k(op::I_NOT_EQUAL, e2, 0);
        let rsb = self.as_bits(rest);
        let side = self.bpick(enz, erb, rsb);
        let s2 = self.sh(op::SHIFT_LEFT_LOGICAL, side, 1);
        let snz = self.cmp_k(op::I_NOT_EQUAL, s2, 0);
        let rel = self.ib(op::BITWISE_XOR, side, rb);
        let same = self.cmp_k(op::U_LESS_THAN, rel, SIGN);
        let above = self.land(snz, same);
        let k1 = self.kc(1);
        let s = self.ib(op::I_SUB, k1, re);
        let si = self.m.emit(op::BITCAST, i, &[s]);
        let lo = self.m.c_i32(1);
        let hi = self.m.c_i32(25);
        let s = self.ext(i, glsl::S_CLAMP, &[si, lo, hi]);
        let s = self.m.emit(op::BITCAST, u, &[s]);
        let man = self.and_k(rb, 0x7f_ffff);
        let sig = self.sh(op::BITWISE_OR, man, 0x80_0000);
        let keep = self.ib(op::SHIFT_RIGHT_LOGICAL, sig, s);
        let unit = self.ib(op::SHIFT_LEFT_LOGICAL, k1, s);
        let mask = self.ib(op::I_SUB, unit, k1);
        let drop = self.ib(op::BITWISE_AND, sig, mask);
        let sm1 = self.ib(op::I_SUB, s, k1);
        let half = self.ib(op::SHIFT_LEFT_LOGICAL, k1, sm1);
        let gt = self.icmp_op(op::U_GREATER_THAN, drop, half);
        let eq = self.icmp_op(op::I_EQUAL, drop, half);
        let odd = self.ib(op::BITWISE_AND, keep, k1);
        let odd = self.cmp_k(op::I_NOT_EQUAL, odd, 0);
        let tie_up = self.lnot(snz);
        let tie_up = self.land(tie_up, odd);
        let tie_up = self.lor(above, tie_up);
        let tie_up = self.land(eq, tie_up);
        let up = self.lor(gt, tie_up);
        let up = self.b2u(up);
        let keep = self.ib(op::I_ADD, keep, up);
        let sign = self.and_k(rb, SIGN);
        self.ib(op::BITWISE_OR, sign, keep)
    }

    /// `a * b * 2^scale + c` (`c` absent for FMUL) rounded once by `mode`,
    /// as SM86 FFMA and FMUL compute it, including the guest's NaN.
    pub(crate) fn soft_fma(
        &mut self,
        a: u32,
        b: u32,
        c: Option<u32>,
        mode: Round,
        ftz: bool,
        scale: i32,
    ) -> u32 {
        let pa = self.sf_parts(a, ftz);
        let pb = self.sf_parts(b, ftz);
        let z = self.kc(0);
        let pc = match c {
            Some(c) => self.sf_parts(c, ftz),
            None => {
                let f = self.m.c_bool(false);
                let t = self.m.c_bool(true);
                Parts {
                    sign: z,
                    sig: z,
                    exp: z,
                    zero: t,
                    inf: f,
                    nan: f,
                }
            }
        };
        let sp = self.ib(op::BITWISE_XOR, pa.sign, pb.sign);
        let (lo, hi) = self.mul_wide(pa.sig, pb.sig, false);
        let k14 = self.kc(14);
        let x = self.shift64(lo, hi, k14, true, false);
        let ep = self.ib(op::I_ADD, pa.exp, pb.exp);
        let ep = self.sh(op::I_ADD, ep, scale as u32);
        let ec = self.bpick(pc.zero, ep, pc.exp);
        let csig = self.bpick(pc.zero, z, pc.sig);
        let k37 = self.kc(37);
        let y = self.shift64(csig, z, k37, true, false);
        let d = self.ib(op::I_SUB, ec, ep);
        let c_big = self.cmp_k(op::S_GREATER_THAN_EQUAL, d, 0);
        let nd = self.ib(op::I_SUB, z, d);
        let xs = self.sf_shr_jam(x, d);
        let ys = self.sf_shr_jam(y, nd);
        let x1 = self.sf_pick(c_big, xs, x);
        let y1 = self.sf_pick(c_big, y, ys);
        let e = self.bpick(c_big, ec, ep);
        let same = self.icmp_op(op::I_EQUAL, sp, pc.sign);
        let (sl, sh, _) = self.add64(x1, y1, None);
        let x_ge = self.sf_ge(x1, y1);
        let dxy = self.sf_sub(x1, y1);
        let dyx = self.sf_sub(y1, x1);
        let diff = self.sf_pick(x_ge, dxy, dyx);
        let r = self.sf_pick(same, (sl, sh), diff);
        let dsign = self.bpick(x_ge, sp, pc.sign);
        let sign = self.bpick(same, sp, dsign);
        let rbits = self.round_fixed(r, e, sign, mode, ftz);
        let rz = self.sf_or(r);
        let rzero = self.lnot(rz);
        let cancel = self.kc(if mode == Round::Down { SIGN } else { 0 });
        let out = self.bpick(rzero, cancel, rbits);

        let prod_zero = self.lor(pa.zero, pb.zero);
        let prod_inf = self.lor(pa.inf, pb.inf);
        let az_bi = self.land(pa.zero, pb.inf);
        let ai_bz = self.land(pa.inf, pb.zero);
        let prod_nan = self.lor(az_bi, ai_bz);
        let nan_in = self.lor(pa.nan, pb.nan);
        let nan_in = self.lor(nan_in, pc.nan);
        let opp = self.lnot(same);
        let inf_cancel = self.land(prod_inf, pc.inf);
        let inf_cancel = self.land(inf_cancel, opp);
        let nan = self.lor(nan_in, prod_nan);
        let nan = self.lor(nan, inf_cancel);
        let zeros = match c {
            Some(_) => {
                let both = self.land(prod_zero, pc.zero);
                let zsign = self.bpick(same, sp, cancel);
                let cb = c.map_or(z, |v| if ftz { self.flush_in(v) } else { v });
                let r = self.bpick(prod_zero, cb, out);
                self.bpick(both, zsign, r)
            }
            None => self.bpick(prod_zero, sp, out),
        };
        let pinf = self.sh(op::BITWISE_OR, sp, 0x7f80_0000);
        let cinf = self.sh(op::BITWISE_OR, pc.sign, 0x7f80_0000);
        let r = self.bpick(pc.inf, cinf, zeros);
        let r = self.bpick(prod_inf, pinf, r);
        let k = self.kc(NAN);
        self.bpick(nan, k, r)
    }

    /// The float bits of `sign` and `r * 2^(e - 60)` (`r` nonzero), rounded
    /// by `mode`.
    fn round_fixed(&mut self, r: W64, e: u32, sign: u32, mode: Round, ftz: bool) -> u32 {
        let u = self.u32t();
        let z = self.kc(0);
        let q = self.sf_msb(r);
        let lead = self.ib(op::I_ADD, e, q);
        let lead = self.sh(op::I_SUB, lead, 60);
        let k = self.sh(op::I_SUB, q, 23);
        let k = if ftz {
            k
        } else {
            let k89 = self.kc((-89i32) as u32);
            let sub_k = self.ib(op::I_SUB, k89, e);
            let i = self.m.t_i32();
            let k = self.m.emit(op::BITCAST, i, &[k]);
            let sub_k = self.m.emit(op::BITCAST, i, &[sub_k]);
            let m = self.ext(i, glsl::S_MAX, &[k, sub_k]);
            self.m.emit(op::BITCAST, u, &[m])
        };
        let i = self.m.t_i32();
        let ki = self.m.emit(op::BITCAST, i, &[k]);
        let k0 = self.m.c_i32(0);
        let k64 = self.m.c_i32(64);
        let kk = self.ext(i, glsl::S_CLAMP, &[ki, k0, k64]);
        let kk = self.m.emit(op::BITCAST, u, &[kk]);
        let m64 = self.shift64(r.0, r.1, kk, false, false);
        let back = self.shift64(m64.0, m64.1, kk, true, false);
        let lost = self.sf_sub(r, back);
        let one = self.kc(1);
        let km1 = self.ib(op::I_SUB, kk, one);
        let km1 = self.and_k(km1, 127);
        let half = self.shift64(one, z, km1, true, false);
        let kpos = self.cmp_k(op::I_NOT_EQUAL, kk, 0);
        let half = self.sf_pick(kpos, half, (z, z));
        let inexact = self.sf_or(lost);
        let ge = self.sf_ge(lost, half);
        let eq = self.sf_eq(lost, half);
        let ne = self.lnot(eq);
        let above = self.land(ge, ne);
        let tie = self.land(eq, kpos);
        let neg = self.cmp_k(op::I_NOT_EQUAL, sign, 0);
        let left = self.cmp_k(op::S_LESS_THAN, k, 0);
        let nk = self.ib(op::I_SUB, z, k);
        let nk = self.and_k(nk, 31);
        let up = self.ib(op::SHIFT_LEFT_LOGICAL, r.0, nk);
        let mant = self.bpick(left, up, m64.0);
        let inc = match mode {
            Round::Nearest => {
                let odd = self.and_k(mant, 1);
                let odd = self.cmp_k(op::I_NOT_EQUAL, odd, 0);
                let t = self.land(tie, odd);
                self.lor(above, t)
            }
            Round::Zero => self.m.c_bool(false),
            Round::Down => self.land(inexact, neg),
            Round::Up => {
                let pos = self.lnot(neg);
                self.land(inexact, pos)
            }
        };
        let incv = self.b2u(inc);
        let mant = self.ib(op::I_ADD, mant, incv);
        let be = self.sh(op::I_ADD, lead, 126);
        let be = self.sh(op::SHIFT_LEFT_LOGICAL, be, 23);
        let normal = self.ib(op::I_ADD, be, mant);
        let below = self.cmp_k(op::S_LESS_THAN, lead, (-126i32) as u32);
        let carry = self.sh(op::SHIFT_RIGHT_LOGICAL, mant, 24);
        let lead_r = self.ib(op::I_ADD, lead, carry);
        let tiny = self.cmp_k(op::S_LESS_THAN, lead_r, (-126i32) as u32);
        let bits = if ftz {
            self.bpick(tiny, z, normal)
        } else {
            self.bpick(below, mant, normal)
        };
        let ovf = self.cmp_k(op::S_GREATER_THAN, lead_r, 127);
        let to_inf = match mode {
            Round::Nearest => self.m.c_bool(true),
            Round::Zero => self.m.c_bool(false),
            Round::Down => neg,
            Round::Up => self.lnot(neg),
        };
        let inf = self.kc(0x7f80_0000);
        let max = self.kc(0x7f7f_ffff);
        let big = self.bpick(to_inf, inf, max);
        let bits = self.bpick(ovf, big, bits);
        self.ib(op::BITWISE_OR, bits, sign)
    }
}

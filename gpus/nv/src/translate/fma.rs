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

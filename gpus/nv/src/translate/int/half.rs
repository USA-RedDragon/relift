//! Packed half-precision arithmetic: HADD2, HMUL2, HFMA2, HMNMX2, HSET2
//! and HSETP2, with the `.F32` forms that produce a single float.
//!
//! The lanes are widened to single precision and narrowed with a rounding
//! built from integer operations. A product of two halves is exact in
//! single precision; a sum need not be (65504 + 2^-24 spans 40 bits), but
//! rounding it to single and then to half precision gives the correctly
//! rounded half because 24 >= 2 * 11 + 2. A fused multiply-add sums in
//! single precision with the low bit forced odd when the sum is inexact, so
//! rounding it again to half precision cannot double-round.

use crate::spirv::op;
use crate::translate::{Ctx, FIX_ZERO_SIGN, Res, bad};
use crate::{Inst, Operand, Swz};

const NO_CONTRACTION: u32 = 42;

pub(super) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "HADD2" | "HMUL2" | "HFMA2" | "HFMA2.MMA" => arith(c, i),
        "HMNMX2" => hmnmx(c, i),
        "HSET2" => hset(c, i),
        "HSETP2" => hsetp(c, i),
        _ => return None,
    })
}

impl Ctx {
    /// The product of two floats that came from halves, with the sign of a
    /// zero product as [`Ctx::mul_zero_sign`] gives it.
    fn half_product(&mut self, x: u32, y: u32) -> u32 {
        let p = self.nc(op::F_MUL, x, y);
        if self.host.flush_fix.is_none_or(|f| f & FIX_ZERO_SIGN == 0) {
            return p;
        }
        let (pb, xb, yb) = (self.as_bits(p), self.as_bits(x), self.as_bits(y));
        let r = self.mul_zero_sign(pb, xb, yb);
        self.as_float(r)
    }

    fn nc(&mut self, opc: u16, a: u32, b: u32) -> u32 {
        let f = self.f32t();
        let r = self.m.emit(opc, f, &[a, b]);
        self.m.deco(r, NO_CONTRACTION, &[]);
        r
    }

    /// The two 16-bit lanes of a half operand after its swizzle,
    /// absolute value and negation.
    fn hlanes(&mut self, o: &Operand) -> Res<(u32, u32)> {
        if let Operand::F16x2(hi, lo) = o {
            let a = self.kc(u32::from(*lo));
            let b = self.kc(u32::from(*hi));
            return Ok((a, b));
        }
        let (v, m) = self.raw(o)?;
        if m.not {
            return bad("half operand inversion");
        }
        let lo = self.and_k(v, 0xffff);
        let hi = self.sh(op::SHIFT_RIGHT_LOGICAL, v, 16);
        let (mut l, mut h) = match m.swz {
            Swz::None => (lo, hi),
            Swz::H0H0 => (lo, lo),
            Swz::H1H1 => (hi, hi),
            Swz::H1H0 => (hi, lo),
            Swz::H0NH1 => (self.sh(op::BITWISE_XOR, hi, 0x8000), lo),
            s => return bad(format!("half swizzle {s:?}")),
        };
        if m.abs {
            l = self.and_k(l, 0x7fff);
            h = self.and_k(h, 0x7fff);
        }
        if m.neg {
            l = self.sh(op::BITWISE_XOR, l, 0x8000);
            h = self.sh(op::BITWISE_XOR, h, 0x8000);
        }
        Ok((l, h))
    }

    fn h_sat(&mut self, h: u32) -> u32 {
        let ab = self.and_k(h, 0x7fff);
        let nan = self.cmp_k(op::U_GREATER_THAN, ab, 0x7c00);
        let neg = {
            let s = self.and_k(h, 0x8000);
            self.cmp_k(op::I_NOT_EQUAL, s, 0)
        };
        let zero = self.kc(0);
        let one = self.kc(0x3c00);
        let u = self.u32t();
        let cl = self.ext(u, crate::spirv::glsl::U_MIN, &[h, one]);
        let z = self.lor(nan, neg);
        self.bpick(z, zero, cl)
    }

    fn h_relu(&mut self, h: u32) -> u32 {
        let ab = self.and_k(h, 0x7fff);
        let nan = self.cmp_k(op::U_GREATER_THAN, ab, 0x7c00);
        let s = self.and_k(h, 0x8000);
        let neg = self.cmp_k(op::I_NOT_EQUAL, s, 0);
        let nn = self.lnot(nan);
        let z = self.land(neg, nn);
        let zero = self.kc(0);
        self.bpick(z, zero, h)
    }

    fn f_sat_bits(&mut self, b: u32) -> u32 {
        let ab = self.and_k(b, 0x7fff_ffff);
        let nan = self.cmp_k(op::U_GREATER_THAN, ab, 0x7f80_0000);
        let neg = {
            let s = self.and_k(b, 0x8000_0000);
            self.cmp_k(op::I_NOT_EQUAL, s, 0)
        };
        let zero = self.kc(0);
        let one = self.kc(0x3f80_0000);
        let u = self.u32t();
        let cl = self.ext(u, crate::spirv::glsl::U_MIN, &[b, one]);
        let z = self.lor(nan, neg);
        self.bpick(z, zero, cl)
    }

    /// The packed result of a native half `a + b` (kind 0), `a * b` (1) or
    /// fused `a * b + z` (2) on lanes (low, high), as a 32-bit word.
    fn native_half(&mut self, kind: u8, a: (u32, u32), b: (u32, u32), z: (u32, u32)) -> u32 {
        self.use_native_f16();
        let u = self.u32t();
        let h = self.m.t_f16();
        let v2 = self.m.t_vec(h, 2);
        let pack = |c: &mut Ctx, t: (u32, u32)| {
            let hi = c.sh(op::SHIFT_LEFT_LOGICAL, t.1, 16);
            let w = c.ib(op::BITWISE_OR, hi, t.0);
            c.m.emit(op::BITCAST, v2, &[w])
        };
        let x = pack(self, a);
        let y = pack(self, b);
        let r = match kind {
            0 | 1 => {
                let opc = if kind == 0 { op::F_ADD } else { op::F_MUL };
                let r = self.m.emit(opc, v2, &[x, y]);
                self.m.deco(r, NO_CONTRACTION, &[]);
                r
            }
            _ => {
                let w = pack(self, z);
                self.cap(6030);
                self.exts.insert("SPV_KHR_fma");
                self.iface.host.fma_khr = true;
                self.m.emit(4427, v2, &[x, y, w])
            }
        };
        self.m.emit(op::BITCAST, u, &[r])
    }

    /// `p + z` in single precision with a sticky low bit: the result is the
    /// round-to-odd sum.
    fn sum_odd(&mut self, p: u32, z: u32) -> u32 {
        let (s, e) = {
            let s = self.nc(op::F_ADD, p, z);
            let bb = self.nc(op::F_SUB, s, p);
            let aa = self.nc(op::F_SUB, s, bb);
            let da = self.nc(op::F_SUB, p, aa);
            let db = self.nc(op::F_SUB, z, bb);
            let e = self.nc(op::F_ADD, da, db);
            (s, e)
        };
        let zf = self.m.c_f32(0);
        let t = self.boolt();
        let e_nz = self.m.emit(op::F_ORD_NOT_EQUAL, t, &[e, zf]);
        let sb = self.as_bits(s);
        let even = {
            let l = self.and_k(sb, 1);
            self.cmp_k(op::I_EQUAL, l, 0)
        };
        let adj = self.land(e_nz, even);
        let s_neg = self.m.emit(op::F_ORD_LESS_THAN, t, &[s, zf]);
        let e_neg = self.m.emit(op::F_ORD_LESS_THAN, t, &[e, zf]);
        let diff = self.m.emit(op::LOGICAL_NOT_EQUAL, t, &[s_neg, e_neg]);
        let one = self.kc(1);
        let up = self.ib(op::I_ADD, sb, one);
        let down = self.ib(op::I_SUB, sb, one);
        let moved = self.bpick(diff, down, up);
        let r = self.bpick(adj, moved, sb);
        self.as_float(r)
    }
}

struct Mods {
    ftz: bool,
    sat: bool,
    relu: bool,
    f32: bool,
}

fn mods(i: &Inst) -> Res<Mods> {
    let mut r = Mods {
        ftz: false,
        sat: false,
        relu: false,
        f32: false,
    };
    for m in &i.mods {
        match *m {
            "FTZ" => r.ftz = true,
            "SAT" => r.sat = true,
            "RELU" => r.relu = true,
            "F32" => r.f32 = true,
            m => return bad(format!("{}.{m}", i.mnemonic)),
        }
    }
    Ok(r)
}

fn arith(c: &mut Ctx, i: &Inst) -> Res<()> {
    let md = mods(i)?;
    let kind = if i.mnemonic == "HADD2" {
        0
    } else if i.mnemonic == "HMUL2" {
        1
    } else {
        2
    };
    let a = c.hlanes(&i.ops[1])?;
    let b = c.hlanes(&i.ops[2])?;
    let z = if kind == 2 {
        c.hlanes(&i.ops[3])?
    } else {
        (a.0, a.1)
    };
    let relu_pred = match i.ops.get(4) {
        Some(o) if md.relu => Some(c.pred_operand(o)?),
        _ => None,
    };
    if c.host.native_f16 && !md.ftz && !md.f32 {
        let (sat, relu) = (md.sat, md.relu);
        let has_pred = relu_pred.is_some();
        let pred = match relu_pred {
            Some(p) => c.b2u(p),
            None => c.kc(0),
        };
        let key = format!("H native {kind} {sat} {relu} {has_pred}");
        let args = [a.0, a.1, b.0, b.1, z.0, z.1, pred];
        let r = c.shared_pure(key, &args, |c, p| {
            let v = c.native_half(kind, (p[0], p[1]), (p[2], p[3]), (p[4], p[5]));
            let relu_pred = if has_pred {
                Some(c.cmp_k(op::I_NOT_EQUAL, p[6], 0))
            } else {
                None
            };
            let mut halves = [c.and_k(v, 0xffff), c.sh(op::SHIFT_RIGHT_LOGICAL, v, 16)];
            for h in &mut halves {
                let ab = c.and_k(*h, 0x7fff);
                let nan = c.cmp_k(op::U_GREATER_THAN, ab, 0x7c00);
                let canon = c.kc(0x7fff);
                *h = c.bpick(nan, canon, *h);
                if relu {
                    let r = c.h_relu(*h);
                    *h = match relu_pred {
                        Some(p) => c.bpick(p, r, *h),
                        None => r,
                    };
                }
                if sat {
                    *h = c.h_sat(*h);
                }
            }
            let hi = c.sh(op::SHIFT_LEFT_LOGICAL, halves[1], 16);
            c.ib(op::BITWISE_OR, hi, halves[0])
        });
        return c.store_dst(&i.ops[0], r);
    }
    let lanes: &[usize] = if md.f32 { &[0] } else { &[0, 1] };
    let mut outs = Vec::new();
    for &k in lanes {
        let pick = |t: (u32, u32)| if k == 0 { t.0 } else { t.1 };
        let (ftz, f32) = (md.ftz, md.f32);
        let key = format!("{} {kind} {ftz} {f32}", i.mnemonic);
        let vb = c.shared_pure(key, &[pick(a), pick(b), pick(z)], |c, p| {
            let x = c.f16_to_f32(p[0], ftz);
            let y = c.f16_to_f32(p[1], ftz);
            let xf = c.as_float(x);
            let yf = c.as_float(y);
            let v = match kind {
                0 => c.nc(op::F_ADD, xf, yf),
                1 => c.half_product(xf, yf),
                _ => {
                    let zz = c.f16_to_f32(p[2], ftz);
                    let zf = c.as_float(zz);
                    let p = c.half_product(xf, yf);
                    c.sum_odd(p, zf)
                }
            };
            let mut vb = c.as_bits(v);
            if ftz && !f32 {
                let ab = c.and_k(vb, 0x7fff_ffff);
                let tiny = c.cmp_k(op::U_LESS_THAN, ab, 0x3880_0000);
                let carry = c.cmp_k(op::U_GREATER_THAN_EQUAL, ab, 0x387f_f000);
                let sign = c.and_k(vb, 0x8000_0000);
                let min_norm = c.sh(op::BITWISE_OR, sign, 0x3880_0000);
                let flushed = c.bpick(carry, min_norm, sign);
                vb = c.bpick(tiny, flushed, vb);
            }
            vb
        });
        outs.push(vb);
    }
    if md.f32 {
        if kind == 2 {
            return bad("HFMA2.F32");
        }
        let mut r = outs[0];
        if md.sat {
            r = c.f_sat_bits(r);
        }
        return c.store_dst(&i.ops[0], r);
    }
    let mut halves = Vec::new();
    for vb in outs {
        let mut h = c.f32_to_f16(vb, 0, md.ftz);
        if md.relu {
            let r = c.h_relu(h);
            h = match relu_pred {
                Some(p) => c.bpick(p, r, h),
                None => r,
            };
        }
        if md.sat {
            h = c.h_sat(h);
        }
        halves.push(h);
    }
    let hi = c.sh(op::SHIFT_LEFT_LOGICAL, halves[1], 16);
    let r = c.ib(op::BITWISE_OR, hi, halves[0]);
    c.store_dst(&i.ops[0], r)
}

fn hmnmx(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut ftz = false;
    let mut nan = false;
    for m in &i.mods {
        match *m {
            "FTZ" => ftz = true,
            "NAN" => nan = true,
            m => return bad(format!("HMNMX2.{m}")),
        }
    }
    let a = c.hlanes(&i.ops[1])?;
    let b = c.hlanes(&i.ops[2])?;
    let p = c.pred_operand(&i.ops[3])?;
    let f = c.f32t();
    let mut halves = Vec::new();
    for k in 0..2 {
        let pick = |t: (u32, u32)| if k == 0 { t.0 } else { t.1 };
        let x = c.f16_to_f32(pick(a), ftz);
        let y = c.f16_to_f32(pick(b), ftz);
        let xf = c.as_float(x);
        let yf = c.as_float(y);
        let lo = c.ext(f, crate::spirv::glsl::N_MIN, &[xf, yf]);
        let hi = c.ext(f, crate::spirv::glsl::N_MAX, &[xf, yf]);
        let r = c.select(f, p, lo, hi);
        let rb = c.as_bits(r);
        let both_zero = {
            let o = c.ib(op::BITWISE_OR, x, y);
            let ab = c.and_k(o, 0x7fff_ffff);
            c.cmp_k(op::I_EQUAL, ab, 0)
        };
        let zmin = c.ib(op::BITWISE_OR, x, y);
        let zmax = c.ib(op::BITWISE_AND, x, y);
        let zr = c.bpick(p, zmin, zmax);
        let rb = c.bpick(both_zero, zr, rb);
        let rb = if nan {
            let t = c.boolt();
            let xn = c.m.emit(op::IS_NAN, t, &[xf]);
            let yn = c.m.emit(op::IS_NAN, t, &[yf]);
            let any = c.lor(xn, yn);
            let cn = c.kc(0x7fff_ffff);
            c.bpick(any, cn, rb)
        } else {
            rb
        };
        halves.push(c.f32_to_f16(rb, 0, ftz));
    }
    let hi = c.sh(op::SHIFT_LEFT_LOGICAL, halves[1], 16);
    let r = c.ib(op::BITWISE_OR, hi, halves[0]);
    c.store_dst(&i.ops[0], r)
}

fn cmp_parts(i: &Inst) -> Res<(&'static str, &'static str, bool, bool, bool)> {
    let mut cmp = None;
    let mut bop = None;
    let mut ftz = false;
    let mut bf = false;
    let mut h_and = false;
    for m in &i.mods {
        match *m {
            "FTZ" => ftz = true,
            "BF" => bf = true,
            "H_AND" => h_and = true,
            "AND" | "OR" | "XOR" => bop = Some(*m),
            m => cmp = Some(m),
        }
    }
    match (cmp, bop) {
        (Some(c), Some(b)) => Ok((c, b, ftz, bf, h_and)),
        _ => bad(format!("{} form", i.mnemonic)),
    }
}

fn lane_compare(c: &mut Ctx, i: &Inst, at: usize, cmp: &str, ftz: bool) -> Res<(u32, u32)> {
    let a = c.hlanes(&i.ops[at])?;
    let b = c.hlanes(&i.ops[at + 1])?;
    let mut rs = Vec::new();
    for k in 0..2 {
        let pick = |t: (u32, u32)| if k == 0 { t.0 } else { t.1 };
        let x = c.f16_to_f32(pick(a), ftz);
        let y = c.f16_to_f32(pick(b), ftz);
        let xf = c.as_float(x);
        let yf = c.as_float(y);
        rs.push(c.fcompare(cmp, xf, yf)?);
    }
    Ok((rs[0], rs[1]))
}

fn hset(c: &mut Ctx, i: &Inst) -> Res<()> {
    let (cmp, bop, ftz, bf, _) = cmp_parts(i)?;
    let (r0, r1) = lane_compare(c, i, 1, cmp, ftz)?;
    let pc = c.pred_operand(&i.ops[3])?;
    let p0 = c.bool_op(bop, r0, pc)?;
    let p1 = c.bool_op(bop, r1, pc)?;
    let one = if bf { 0x3c00 } else { 0xffff };
    let lo = {
        let k = c.kc(one);
        let z = c.kc(0);
        c.bpick(p0, k, z)
    };
    let hi = {
        let k = c.kc(one << 16);
        let z = c.kc(0);
        c.bpick(p1, k, z)
    };
    let r = c.ib(op::BITWISE_OR, hi, lo);
    c.store_dst(&i.ops[0], r)
}

fn hsetp(c: &mut Ctx, i: &Inst) -> Res<()> {
    let (cmp, bop, ftz, _, h_and) = cmp_parts(i)?;
    let (r0, r1) = lane_compare(c, i, 2, cmp, ftz)?;
    let pc = c.pred_operand(&i.ops[4])?;
    if h_and {
        let both = c.land(r0, r1);
        let nb = c.lnot(both);
        let p = c.bool_op(bop, both, pc)?;
        let q = c.bool_op(bop, nb, pc)?;
        c.store_pdst(&i.ops[0], p)?;
        return c.store_pdst(&i.ops[1], q);
    }
    let p = c.bool_op(bop, r0, pc)?;
    let q = c.bool_op(bop, r1, pc)?;
    c.store_pdst(&i.ops[0], p)?;
    c.store_pdst(&i.ops[1], q)
}

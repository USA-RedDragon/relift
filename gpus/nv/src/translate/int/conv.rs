//! Conversions between integers and floats: F2I, I2F, I2FP, F2F, F2FP,
//! F2IP and the half-precision form of FRND.
//!
//! Every rounding step is built from integer operations on the bit
//! patterns, so the results do not depend on how the device converts or
//! flushes. Rounding modes are numbered 0 (nearest even), 1 (down), 2 (up)
//! and 3 (toward zero).

use crate::spirv::{glsl, op};
use crate::translate::{Ctx, Res, bad};
use crate::{Inst, Swz};

pub(super) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "F2I" => f2i(c, i),
        "I2F" | "I2FP" => i2f(c, i),
        "F2F" => f2f(c, i),
        "F2FP" | "UF2FP" => f2fp(c, i),
        "F2IP" => f2ip(c, i),
        _ => return None,
    })
}

/// What a rounding mode does when the magnitude is cut.
fn directed(mode: u32, neg: bool) -> Option<bool> {
    match mode {
        1 => Some(neg),
        2 => Some(!neg),
        3 => Some(false),
        _ => None,
    }
}

impl Ctx {
    pub(crate) fn sh(&mut self, opc: u16, a: u32, k: u32) -> u32 {
        let k = self.kc(k);
        self.ib(opc, a, k)
    }

    pub(crate) fn and_k(&mut self, a: u32, k: u32) -> u32 {
        let k = self.kc(k);
        self.ib(op::BITWISE_AND, a, k)
    }

    pub(crate) fn cmp_k(&mut self, opc: u16, a: u32, k: u32) -> u32 {
        let k = self.kc(k);
        self.icmp_op(opc, a, k)
    }

    pub(crate) fn lnot(&mut self, a: u32) -> u32 {
        self.not_if(a, true)
    }

    pub(crate) fn land(&mut self, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(op::LOGICAL_AND, t, &[a, b])
    }

    pub(crate) fn lor(&mut self, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(op::LOGICAL_OR, t, &[a, b])
    }

    /// Half-precision bits (low 16 of `h`) to single-precision bits.
    /// A NaN becomes the canonical `0x7fffffff`; with `ftz` a subnormal
    /// input reads as a signed zero.
    pub(crate) fn f16_to_f32(&mut self, h: u32, ftz: bool) -> u32 {
        if self.host.native_f16 && !ftz {
            return self.native_f16_to_f32(h);
        }
        self.shared_pure(format!("f16_to_f32 {ftz}"), &[h], |c, p| {
            c.f16_to_f32_body(p[0], ftz)
        })
    }

    /// [`Ctx::f16_to_f32`] by the host's conversion, which is exact.
    fn native_f16_to_f32(&mut self, h: u32) -> u32 {
        self.use_native_f16();
        let u = self.u32t();
        let f = self.f32t();
        let hf = self.m.t_f16();
        let v2 = self.m.t_vec(hf, 2);
        let pair = self.m.emit(op::BITCAST, v2, &[h]);
        let lo = self.m.emit(op::COMPOSITE_EXTRACT, hf, &[pair, 0]);
        let w = self.m.emit(op::F_CONVERT, f, &[lo]);
        let bits = self.m.emit(op::BITCAST, u, &[w]);
        let ab = self.and_k(h, 0x7fff);
        let nan = self.cmp_k(op::U_GREATER_THAN, ab, 0x7c00);
        let canon = self.kc(0x7fff_ffff);
        self.bpick(nan, canon, bits)
    }

    /// [`Ctx::f32_to_f16`] to nearest even by the host's conversion.
    fn native_f32_to_f16(&mut self, x: u32) -> u32 {
        self.use_native_f16();
        let u = self.u32t();
        let f = self.f32t();
        let hf = self.m.t_f16();
        let v2 = self.m.t_vec(hf, 2);
        let xf = self.m.emit(op::BITCAST, f, &[x]);
        let r = self.m.emit(op::F_CONVERT, hf, &[xf]);
        let z = self.m.c_f16(0);
        let pair = self.m.emit(op::COMPOSITE_CONSTRUCT, v2, &[r, z]);
        let bits = self.m.emit(op::BITCAST, u, &[pair]);
        let ab = self.and_k(x, 0x7fff_ffff);
        let nan = self.cmp_k(op::U_GREATER_THAN, ab, 0x7f80_0000);
        let canon = self.kc(0x7fff);
        self.bpick(nan, canon, bits)
    }

    /// Declares what native half-precision arithmetic needs (see
    /// [`HostProfile::native_f16`](crate::translate::HostProfile::native_f16)).
    pub(crate) fn use_native_f16(&mut self) {
        for cap in [9, 4464, 4467, 4466] {
            self.cap(cap);
        }
        self.denorm.f16 = true;
    }

    fn f16_to_f32_body(&mut self, h: u32, ftz: bool) -> u32 {
        let sign = self.and_k(h, 0x8000);
        let sign32 = self.sh(op::SHIFT_LEFT_LOGICAL, sign, 16);
        let e = {
            let t = self.sh(op::SHIFT_RIGHT_LOGICAL, h, 10);
            self.and_k(t, 31)
        };
        let m = self.and_k(h, 0x3ff);
        let e_is0 = self.cmp_k(op::I_EQUAL, e, 0);
        let m_is0 = self.cmp_k(op::I_EQUAL, m, 0);
        let e_is31 = self.cmp_k(op::I_EQUAL, e, 31);
        let normal = {
            let e2 = self.sh(op::I_ADD, e, 112);
            let e2 = self.sh(op::SHIFT_LEFT_LOGICAL, e2, 23);
            let m2 = self.sh(op::SHIFT_LEFT_LOGICAL, m, 13);
            self.ib(op::BITWISE_OR, e2, m2)
        };
        let sub = {
            let u = self.u32t();
            let p = self.ext(u, glsl::FIND_U_MSB, &[m]);
            let ef = self.sh(op::I_ADD, p, 103);
            let ef = self.sh(op::SHIFT_LEFT_LOGICAL, ef, 23);
            let k23 = self.kc(23);
            let back = self.ib(op::I_SUB, k23, p);
            let mm = self.ib(op::SHIFT_LEFT_LOGICAL, m, back);
            let mm = self.and_k(mm, 0x7fffff);
            self.ib(op::BITWISE_OR, ef, mm)
        };
        let mag = self.bpick(e_is0, sub, normal);
        let zero = self.kc(0);
        let flush = if ftz { e_is0 } else { self.land(e_is0, m_is0) };
        let mag = self.bpick(flush, zero, mag);
        let inf = self.kc(0x7f80_0000);
        let nan = self.kc(0x7fff_ffff);
        let special = self.bpick(m_is0, inf, nan);
        let mag = self.bpick(e_is31, special, mag);
        let m_nz = self.lnot(m_is0);
        let is_nan = self.land(e_is31, m_nz);
        let r = self.ib(op::BITWISE_OR, mag, sign32);
        self.bpick(is_nan, nan, r)
    }

    /// Single-precision bits to half-precision bits (in the low 16) under a
    /// rounding mode; NaN becomes `0x7fff`; with `ftz` a subnormal result
    /// is a signed zero.
    pub(crate) fn f32_to_f16(&mut self, u: u32, mode: u32, ftz: bool) -> u32 {
        if self.host.native_f16 && mode == 0 && !ftz {
            return self.native_f32_to_f16(u);
        }
        self.shared_pure(format!("f32_to_f16 {mode} {ftz}"), &[u], |c, p| {
            c.f32_to_f16_body(p[0], mode, ftz)
        })
    }

    fn f32_to_f16_body(&mut self, u: u32, mode: u32, ftz: bool) -> u32 {
        let sign = self.sh(op::SHIFT_RIGHT_LOGICAL, u, 31);
        let neg = self.cmp_k(op::I_EQUAL, sign, 1);
        let ab = self.and_k(u, 0x7fff_ffff);
        let e = self.sh(op::SHIFT_RIGHT_LOGICAL, ab, 23);
        let m = self.and_k(ab, 0x7f_ffff);
        let e_is0 = self.cmp_k(op::I_EQUAL, e, 0);
        let hidden = self.kc(0x80_0000);
        let zero = self.kc(0);
        let hid = self.bpick(e_is0, zero, hidden);
        let mant24 = self.ib(op::BITWISE_OR, m, hid);
        let kbias = self.kc(127);
        let kmin = self.kc(-126i32 as u32);
        let ee = self.ib(op::I_SUB, e, kbias);
        let ee = self.bpick(e_is0, kmin, ee);
        let kn14 = self.kc(-14i32 as u32);
        let is_norm = self.icmp_op(op::S_GREATER_THAN_EQUAL, ee, kn14);
        let big = self.cmp_k(op::S_GREATER_THAN, ee, 15);
        let k13 = self.kc(13);
        let km1 = self.kc(u32::MAX);
        let dsub = self.ib(op::I_SUB, km1, ee);
        let d = self.bpick(is_norm, k13, dsub);
        let k31 = self.kc(31);
        let d = {
            let u = self.u32t();
            self.ext(u, glsl::U_MIN, &[d, k31])
        };
        let r = self.ib(op::SHIFT_RIGHT_LOGICAL, mant24, d);
        let one = self.kc(1);
        let dm1 = self.ib(op::I_SUB, d, one);
        let half = self.ib(op::SHIFT_LEFT_LOGICAL, one, dm1);
        let mask = {
            let s = self.ib(op::SHIFT_LEFT_LOGICAL, one, d);
            self.ib(op::I_SUB, s, one)
        };
        let rem = self.ib(op::BITWISE_AND, mant24, mask);
        let up = match directed(mode, false) {
            None => {
                let gt = self.icmp_op(op::U_GREATER_THAN, rem, half);
                let eq = self.icmp_op(op::I_EQUAL, rem, half);
                let odd = self.and_k(r, 1);
                let odd = self.cmp_k(op::I_EQUAL, odd, 1);
                let tie = self.land(eq, odd);
                self.lor(gt, tie)
            }
            Some(_) => {
                let nz = self.cmp_k(op::I_NOT_EQUAL, rem, 0);
                match mode {
                    1 => self.land(nz, neg),
                    2 => {
                        let pos = self.lnot(neg);
                        self.land(nz, pos)
                    }
                    _ => self.m.c_bool(false),
                }
            }
        };
        let inc = self.b2u(up);
        let eb = self.sh(op::I_ADD, ee, 14);
        let eb = self.bpick(is_norm, eb, zero);
        let eb = self.sh(op::SHIFT_LEFT_LOGICAL, eb, 10);
        let asm = self.ib(op::I_ADD, eb, r);
        let asm = self.ib(op::I_ADD, asm, inc);
        let kinf = self.kc(0x7c00);
        let ovf_big = self.icmp_op(op::U_GREATER_THAN_EQUAL, asm, kinf);
        let ovf = self.lor(big, ovf_big);
        let to_inf = match mode {
            0 => self.m.c_bool(true),
            1 => neg,
            2 => self.lnot(neg),
            _ => self.m.c_bool(false),
        };
        let infv = self.kc(0x7c00);
        let maxv = self.kc(0x7bff);
        let ov = self.bpick(to_inf, infv, maxv);
        let v = self.bpick(ovf, ov, asm);
        let v = if ftz {
            let ex = self.and_k(v, 0x7c00);
            let flush = self.cmp_k(op::I_EQUAL, ex, 0);
            self.bpick(flush, zero, v)
        } else {
            v
        };
        let sbit = self.sh(op::SHIFT_LEFT_LOGICAL, sign, 15);
        let res = self.ib(op::BITWISE_OR, v, sbit);
        let e_is255 = self.cmp_k(op::I_EQUAL, e, 255);
        let m_nz = self.cmp_k(op::I_NOT_EQUAL, m, 0);
        let is_nan = self.land(e_is255, m_nz);
        let inf_res = {
            let i = self.kc(0x7c00);
            self.ib(op::BITWISE_OR, i, sbit)
        };
        let res = self.bpick(e_is255, inf_res, res);
        let nan = self.kc(0x7fff);
        self.bpick(is_nan, nan, res)
    }

    /// A 64-bit magnitude (low, high) and its sign to float bits with
    /// `mbits` explicit mantissa bits: single (23) or half (10) precision.
    /// A single-precision result of magnitude up to `2^24` is exact, so in
    /// any rounding mode it is the host's own conversion; other magnitudes
    /// branch to the integer rounding.
    fn int_to_float(&mut self, lo: u32, hi: u32, neg: u32, mode: u32, half: bool) -> u32 {
        if half {
            return self.shared_pure(
                format!("int_to_float {mode} {half}"),
                &[lo, hi, neg],
                |c, p| c.int_to_float_body(p[0], p[1], p[2], mode, half),
            );
        }
        let hi_z = self.cmp_k(op::I_EQUAL, hi, 0);
        let lim = self.kc(1 << 24);
        let small = self.icmp_op(op::U_LESS_THAN_EQUAL, lo, lim);
        let direct = self.land(hi_z, small);
        let fast = self.m.id();
        let slow = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[direct, fast, slow]);
        self.m.emit_void(op::LABEL, &[fast]);
        let f = self.f32t();
        let u = self.u32t();
        let v = self.m.emit(op::CONVERT_U_TO_F, f, &[lo]);
        let v = self.m.emit(op::BITCAST, u, &[v]);
        let sign = self.sh(op::SHIFT_LEFT_LOGICAL, neg, 31);
        let rf = self.ib(op::BITWISE_OR, v, sign);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[slow]);
        let rs = self.shared_pure_call(
            format!("int_to_float {mode} {half}"),
            &[lo, hi, neg],
            |c, p| c.int_to_float_body(p[0], p[1], p[2], mode, half),
        );
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
        self.m.emit(op::PHI, u, &[rf, fast, rs, slow])
    }

    fn int_to_float_body(&mut self, lo: u32, hi: u32, neg: u32, mode: u32, half: bool) -> u32 {
        let mbits: u32 = if half { 10 } else { 23 };
        let zero = self.kc(0);
        let u = self.u32t();
        let hi_nz = self.cmp_k(op::I_NOT_EQUAL, hi, 0);
        let lo_nz = self.cmp_k(op::I_NOT_EQUAL, lo, 0);
        let any = self.lor(hi_nz, lo_nz);
        let phi = self.ext(u, glsl::FIND_U_MSB, &[hi]);
        let plo = self.ext(u, glsl::FIND_U_MSB, &[lo]);
        let phi32 = self.sh(op::I_ADD, phi, 32);
        let p = self.bpick(hi_nz, phi32, plo);
        let kmb = self.kc(mbits);
        let exact = self.icmp_op(op::U_LESS_THAN_EQUAL, p, kmb);
        let sub = self.ib(op::I_SUB, p, kmb);
        let k1 = self.kc(1);
        let sub = self.bpick(exact, k1, sub);
        let up_amt = self.ib(op::I_SUB, kmb, p);
        let up_amt = self.bpick(exact, up_amt, zero);
        let (el, _) = self.shift64(lo, hi, up_amt, true, false);
        let (ml, _) = self.shift64(lo, hi, sub, false, false);
        let one = self.kc(1);
        let sm1 = self.ib(op::I_SUB, sub, one);
        let (rl, _) = self.shift64(lo, hi, sm1, false, false);
        let round = self.and_k(rl, 1);
        let round = self.cmp_k(op::I_EQUAL, round, 1);
        let k65 = self.kc(65);
        let back = self.ib(op::I_SUB, k65, sub);
        let (sl, sh) = self.shift64(lo, hi, back, true, false);
        let st = self.ib(op::BITWISE_OR, sl, sh);
        let sticky = self.cmp_k(op::I_NOT_EQUAL, st, 0);
        let mant = self.bpick(exact, el, ml);
        let up = match mode {
            0 => {
                let odd = self.and_k(mant, 1);
                let odd = self.cmp_k(op::I_EQUAL, odd, 1);
                let t = self.lor(sticky, odd);
                self.land(round, t)
            }
            1 => {
                let any_r = self.lor(round, sticky);
                let n = self.cmp_k(op::I_NOT_EQUAL, neg, 0);
                self.land(any_r, n)
            }
            2 => {
                let any_r = self.lor(round, sticky);
                let n = self.cmp_k(op::I_EQUAL, neg, 0);
                self.land(any_r, n)
            }
            _ => self.m.c_bool(false),
        };
        let up = {
            let ne = self.lnot(exact);
            self.land(up, ne)
        };
        let inc = self.b2u(up);
        let mant = self.ib(op::I_ADD, mant, inc);
        let carry = self.cmp_k(op::I_EQUAL, mant, 1 << (mbits + 1));
        let mant_s = self.sh(op::SHIFT_RIGHT_LOGICAL, mant, 1);
        let mant = self.bpick(carry, mant_s, mant);
        let p1 = self.ib(op::I_ADD, p, one);
        let p = self.bpick(carry, p1, p);
        let frac = self.and_k(mant, (1 << mbits) - 1);
        let bias = if half { 15 } else { 127 };
        let ef = self.sh(op::I_ADD, p, bias);
        let ef = self.sh(op::SHIFT_LEFT_LOGICAL, ef, mbits);
        let bits = self.ib(op::BITWISE_OR, ef, frac);
        let sbit = self.sh(
            op::SHIFT_LEFT_LOGICAL,
            one,
            mbits + if half { 5 } else { 8 },
        );
        let nb = self.cmp_k(op::I_NOT_EQUAL, neg, 0);
        let sgn = self.bpick(nb, sbit, zero);
        let mut res = self.ib(op::BITWISE_OR, bits, sgn);
        if half {
            let ovf = self.cmp_k(op::U_GREATER_THAN_EQUAL, p, 16);
            let to_inf = match mode {
                0 => self.m.c_bool(true),
                1 => nb,
                2 => self.lnot(nb),
                _ => self.m.c_bool(false),
            };
            let infv = self.kc(0x7c00);
            let maxv = self.kc(0x7bff);
            let ov = self.bpick(to_inf, infv, maxv);
            let ov = self.ib(op::BITWISE_OR, ov, sgn);
            res = self.bpick(ovf, ov, res);
        }
        self.bpick(any, res, zero)
    }
}

fn rounding(i: &Inst) -> u32 {
    for m in &i.mods {
        match *m {
            "RM" => return 1,
            "RP" => return 2,
            "RZ" => return 3,
            _ => {}
        }
    }
    0
}

/// The selected piece of an integer or half source: whole word, a byte
/// (`.B1`..) or a half (`.H1`).
pub(crate) fn pick(c: &mut Ctx, v: u32, swz: Swz, bits: u32, signed: bool) -> Res<u32> {
    let off = match (swz, bits) {
        (Swz::None, _) => 0,
        (Swz::B(n), 8) => 8 * u32::from(n),
        (Swz::H(1), 16) => 16,
        (Swz::H(0), 16) => 0,
        (s, _) => return bad(format!("conversion source selector {s:?}")),
    };
    if bits == 32 {
        return Ok(v);
    }
    let u = c.u32t();
    let ko = c.kc(off);
    let kc = c.kc(bits);
    let opc = if signed {
        op::BIT_FIELD_S_EXTRACT
    } else {
        op::BIT_FIELD_U_EXTRACT
    };
    Ok(c.m.emit(opc, u, &[v, ko, kc]))
}

fn f2i(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut ftz = false;
    let mut ntz = false;
    let mut f16 = false;
    let mut signed = true;
    let mut size = 32u32;
    let mut how = glsl::ROUND_EVEN;
    let mut mode = 0;
    for m in &i.mods {
        match *m {
            "FTZ" => ftz = true,
            "NTZ" => ntz = true,
            "F16" => f16 = true,
            "FLOOR" => (how, mode) = (glsl::FLOOR, 1),
            "CEIL" => (how, mode) = (glsl::CEIL, 2),
            "TRUNC" => (how, mode) = (glsl::TRUNC, 3),
            "U8" => (signed, size) = (false, 8),
            "S8" => (signed, size) = (true, 8),
            "U16" => (signed, size) = (false, 16),
            "S16" => (signed, size) = (true, 16),
            "U32" => (signed, size) = (false, 32),
            "U64" => (signed, size) = (false, 64),
            "S64" => (signed, size) = (true, 64),
            m => return bad(format!("F2I.{m}")),
        }
    }
    if f16 && ftz {
        return bad("F2I.F16.FTZ");
    }
    let (v, m) = c.raw(&i.ops[1])?;
    if m.not {
        return bad("F2I source modifiers");
    }
    let spec = F2i {
        ntz,
        signed,
        size,
        how,
        mode,
    };
    if !f16 && size != 64 {
        if m.swz != Swz::None {
            return bad("F2I source selector");
        }
        let key = format!("F2I {ftz} {ntz} {signed} {size} {mode} {} {}", m.abs, m.neg);
        let (abs, neg) = (m.abs, m.neg);
        let r = c.shared(key, &[v], |c, p| {
            let (bits, den) = f2i_bits(c, p[0], abs, neg, ftz);
            Ok(f2i_core(c, bits, den, &spec)?.0)
        })?;
        return c.store_dst(&i.ops[0], r);
    }
    let (bits, den) = if f16 {
        let h = pick(c, v, m.swz, 16, false)?;
        let h = if m.abs { c.and_k(h, 0x7fff) } else { h };
        let h = if m.neg {
            c.sh(op::BITWISE_XOR, h, 0x8000)
        } else {
            h
        };
        (c.f16_to_f32(h, false), None)
    } else {
        if m.swz != Swz::None {
            return bad("F2I source selector");
        }
        f2i_bits(c, v, m.abs, m.neg, ftz)
    };
    let (lo, hi) = f2i_core(c, bits, den, &spec)?;
    if size == 64 {
        c.store_pair(&i.ops[0], lo, hi)
    } else {
        c.store_dst(&i.ops[0], lo)
    }
}

/// What F2I converts: its flags and the integer it produces.
struct F2i {
    ntz: bool,
    signed: bool,
    size: u32,
    how: u32,
    mode: u32,
}

/// The float bits F2I converts from the source word `v` with its `.ABS` and
/// `.NEG`, a denormal read as the signed zero, and the denormal's test where
/// the instruction has no `.FTZ`.
fn f2i_bits(c: &mut Ctx, v: u32, abs: bool, neg: bool, ftz: bool) -> (u32, Option<u32>) {
    let b = if abs { c.and_k(v, 0x7fff_ffff) } else { v };
    let b = if neg {
        c.sh(op::BITWISE_XOR, b, 0x8000_0000)
    } else {
        b
    };
    let ex = c.and_k(b, 0x7f80_0000);
    let mn = c.and_k(b, 0x7f_ffff);
    let ex0 = c.cmp_k(op::I_EQUAL, ex, 0);
    let mnz = c.cmp_k(op::I_NOT_EQUAL, mn, 0);
    let denorm = c.land(ex0, mnz);
    let sign = c.and_k(b, 0x8000_0000);
    let zeroed = sign;
    if ftz {
        (c.bpick(ex0, zeroed, b), None)
    } else {
        (c.bpick(denorm, zeroed, b), Some(denorm))
    }
}

/// The converted integer (low word, high word) of the float bits `bits`.
fn f2i_core(c: &mut Ctx, bits: u32, den: Option<u32>, f: &F2i) -> Res<(u32, u32)> {
    let F2i {
        ntz,
        signed,
        size,
        how,
        mode,
        ..
    } = *f;
    let fl = c.f32t();
    let x = c.as_float(bits);
    let r = c.ext(fl, how, &[x]);
    let is_nan = {
        let t = c.boolt();
        c.m.emit(op::IS_NAN, t, &[x])
    };
    let neg = {
        let s = c.and_k(bits, 0x8000_0000);
        c.cmp_k(op::I_NOT_EQUAL, s, 0)
    };
    let nan_hi = if size == 64 {
        0x8000_0000
    } else {
        1u32 << (size - 1)
    };
    let (lo, hi) = if size == 64 {
        f2i64(c, r, signed, is_nan, ntz, nan_hi)?
    } else {
        (f2i_small(c, r, signed, size, is_nan, ntz), c.kc(0))
    };
    Ok(match den {
        None => (lo, hi),
        Some(d) => {
            let pos = c.lnot(neg);
            let one = c.kc(1);
            let zero = c.kc(0);
            let m1 = c.kc(u32::MAX);
            let small_lo = match mode {
                2 => c.bpick(pos, one, zero),
                1 if signed => c.bpick(neg, m1, zero),
                _ => zero,
            };
            let small_hi = match mode {
                1 if signed && size == 64 => c.bpick(neg, m1, zero),
                _ => zero,
            };
            (c.bpick(d, small_lo, lo), c.bpick(d, small_hi, hi))
        }
    })
}

/// F2IP: two floats to saturated 8-bit integers, packed under the low half
/// (or with `.H1` the high half) of the third source: byte 0 from the
/// second source, byte 1 from the first, bytes 2 and 3 from the third.
/// Denormal inputs round to zero and `.NTZ` makes a NaN zero.
fn f2ip(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut signed = false;
    let mut how = glsl::ROUND_EVEN;
    for m in &i.mods {
        match *m {
            "U8" | "F32" | "NTZ" => {}
            "S8" => signed = true,
            "TRUNC" => how = glsl::TRUNC,
            m => return bad(format!("F2IP.{m}")),
        }
    }
    if !i.has("NTZ") {
        return bad("F2IP without NTZ");
    }
    let mut bytes = Vec::new();
    for o in &i.ops[1..3] {
        let v = c.usrc(o)?;
        let f = c.f32t();
        let x = c.as_float(v);
        let r = c.ext(f, how, &[x]);
        let t = c.boolt();
        let is_nan = c.m.emit(op::IS_NAN, t, &[x]);
        let b = f2i_small(c, r, signed, 8, is_nan, true);
        bytes.push(c.and_k(b, 0xff));
    }
    let (v, m) = c.raw(&i.ops[3])?;
    if m.neg || m.abs || m.not {
        return bad("F2IP third source modifiers");
    }
    let top = match m.swz {
        Swz::None => c.sh(op::SHIFT_LEFT_LOGICAL, v, 16),
        Swz::H(1) => c.and_k(v, 0xffff_0000),
        s => return bad(format!("F2IP selector {s:?}")),
    };
    let hi = c.sh(op::SHIFT_LEFT_LOGICAL, bytes[0], 8);
    let r = c.ib(op::BITWISE_OR, top, hi);
    let r = c.ib(op::BITWISE_OR, r, bytes[1]);
    c.store_dst(&i.ops[0], r)
}

fn f2i_small(c: &mut Ctx, r: u32, signed: bool, size: u32, is_nan: u32, ntz: bool) -> u32 {
    let f = c.f32t();
    let u = c.u32t();
    let (hi_f, lo_f, hi_v, lo_v) = if signed {
        let p = (1u64 << (size - 1)) as f32;
        (
            p,
            -p,
            (1u32 << (size - 1)) - 1,
            (-(1i64 << (size - 1))) as u32,
        )
    } else {
        ((1u64 << size) as f32, 0.0, ((1u64 << size) - 1) as u32, 0)
    };
    let hi_c = c.m.c_f32(hi_f.to_bits());
    let lo_c = c.m.c_f32(lo_f.to_bits());
    let t = c.boolt();
    let ge = c.m.emit(op::F_ORD_GREATER_THAN_EQUAL, t, &[r, hi_c]);
    let le = c.m.emit(op::F_ORD_LESS_THAN_EQUAL, t, &[r, lo_c]);
    let zf = c.m.c_f32(0);
    let safe = c.select(f, ge, zf, r);
    let safe = c.select(f, le, zf, safe);
    let conv = c.m.emit(
        if signed {
            op::CONVERT_F_TO_S
        } else {
            op::CONVERT_F_TO_U
        },
        u,
        &[safe],
    );
    let khi = c.kc(hi_v);
    let v = c.bpick(ge, khi, conv);
    let v = {
        let k = c.kc(lo_v);
        c.bpick(le, k, v)
    };
    let nanv = c.kc(if ntz { 0 } else { 1u32 << (size - 1) });
    c.bpick(is_nan, nanv, v)
}

fn f2i64(
    c: &mut Ctx,
    r: u32,
    signed: bool,
    is_nan: u32,
    ntz: bool,
    nan_hi: u32,
) -> Res<(u32, u32)> {
    let bits = c.as_bits(r);
    let sign_set = {
        let s = c.and_k(bits, 0x8000_0000);
        c.cmp_k(op::I_NOT_EQUAL, s, 0)
    };
    let ef = {
        let t = c.sh(op::SHIFT_RIGHT_LOGICAL, bits, 23);
        c.and_k(t, 0xff)
    };
    let e = c.sh(op::I_SUB, ef, 127);
    let m24 = {
        let m = c.and_k(bits, 0x7f_ffff);
        c.sh(op::BITWISE_OR, m, 0x80_0000)
    };
    let zero = c.kc(0);
    let big = c.cmp_k(op::S_GREATER_THAN_EQUAL, e, 23);
    let s_left = {
        let d = c.sh(op::I_SUB, e, 23);
        c.bpick(big, d, zero)
    };
    let s_right = {
        let k = c.kc(23);
        let d = c.ib(op::I_SUB, k, e);
        let d = c.bpick(big, zero, d);
        let k64 = c.kc(64);
        let u = c.u32t();
        c.ext(u, glsl::U_MIN, &[d, k64])
    };
    let (ll, lh) = c.shift64(m24, zero, s_left, true, false);
    let (rl, rh) = c.shift64(m24, zero, s_right, false, false);
    let ml = c.bpick(big, ll, rl);
    let mh = c.bpick(big, lh, rh);
    let neg_e = c.cmp_k(op::S_LESS_THAN, e, 0);
    let ml = c.bpick(neg_e, zero, ml);
    let mh = c.bpick(neg_e, zero, mh);
    let (lim, max_lo, max_hi) = if signed {
        (63, u32::MAX, 0x7fff_ffff)
    } else {
        (64, u32::MAX, u32::MAX)
    };
    let ovf = c.cmp_k(op::S_GREATER_THAN_EQUAL, e, lim);
    let (vl, vh) = if signed {
        let nl = c.inot(ml);
        let nh = c.inot(mh);
        let one = c.kc(1);
        let (nl, cy) = c.addc(nl, one);
        let nh = c.ib(op::I_ADD, nh, cy);
        (c.bpick(sign_set, nl, ml), c.bpick(sign_set, nh, mh))
    } else {
        (c.bpick(sign_set, zero, ml), c.bpick(sign_set, zero, mh))
    };
    let (ol, oh) = if signed {
        let pl = c.kc(max_lo);
        let ph = c.kc(max_hi);
        let nlo = c.kc(0);
        let nhi = c.kc(0x8000_0000);
        (c.bpick(sign_set, nlo, pl), c.bpick(sign_set, nhi, ph))
    } else {
        let a = c.kc(max_lo);
        let b = c.kc(max_hi);
        (c.bpick(sign_set, zero, a), c.bpick(sign_set, zero, b))
    };
    let lo = c.bpick(ovf, ol, vl);
    let hi = c.bpick(ovf, oh, vh);
    let nl = c.kc(0);
    let nh = c.kc(if ntz { 0 } else { nan_hi });
    Ok((c.bpick(is_nan, nl, lo), c.bpick(is_nan, nh, hi)))
}

fn i2f(c: &mut Ctx, i: &Inst) -> Res<()> {
    i2f_with(c, i, false, false)
}

/// `I2F` of a 32-bit or smaller source taken as its absolute value
/// (`abs`, wrapping: the most negative integer stays itself) and then
/// negated (`neg`, wrapping), as Maxwell's integer `.ABS` and `.NEG` do;
/// both false is SM86's `I2F`.
pub(crate) fn i2f_with(c: &mut Ctx, i: &Inst, abs: bool, neg: bool) -> Res<()> {
    let mut half = false;
    let mut signed = true;
    let mut size = 32u32;
    let mut mode = 0;
    for m in &i.mods {
        match *m {
            "F16" => half = true,
            "F32" => {}
            "RM" => mode = 1,
            "RP" => mode = 2,
            "RZ" => mode = 3,
            "U8" => (signed, size) = (false, 8),
            "S8" => (signed, size) = (true, 8),
            "U16" => (signed, size) = (false, 16),
            "S16" => (signed, size) = (true, 16),
            "U32" => (signed, size) = (false, 32),
            "S32" => (signed, size) = (true, 32),
            "U64" => (signed, size) = (false, 64),
            "S64" => (signed, size) = (true, 64),
            m => return bad(format!("{}.{m}", i.mnemonic)),
        }
    }
    let (lo, hi, neg) = if size == 64 {
        let (lo, hi) = c.src_pair(&i.ops[1])?;
        if signed {
            let n = c.sh(op::SHIFT_RIGHT_LOGICAL, hi, 31);
            let nl = c.inot(lo);
            let nh = c.inot(hi);
            let one = c.kc(1);
            let (nl, cy) = c.addc(nl, one);
            let nh = c.ib(op::I_ADD, nh, cy);
            let isn = c.cmp_k(op::I_EQUAL, n, 1);
            (c.bpick(isn, nl, lo), c.bpick(isn, nh, hi), n)
        } else {
            (lo, hi, c.kc(0))
        }
    } else {
        let (v, m) = c.raw(&i.ops[1])?;
        if m.abs || m.neg || m.not {
            return bad("integer conversion source modifiers");
        }
        let zero = c.kc(0);
        let v = if abs {
            let s = c.sh(op::SHIFT_RIGHT_LOGICAL, v, 31);
            let isn = c.cmp_k(op::I_EQUAL, s, 1);
            let nv = c.ib(op::I_SUB, zero, v);
            c.bpick(isn, nv, v)
        } else {
            v
        };
        let v = if neg { c.ib(op::I_SUB, zero, v) } else { v };
        let v = pick(c, v, m.swz, size, signed)?;
        if signed {
            let n = c.sh(op::SHIFT_RIGHT_LOGICAL, v, 31);
            let isn = c.cmp_k(op::I_EQUAL, n, 1);
            let neg_v = c.ib(op::I_SUB, zero, v);
            (c.bpick(isn, neg_v, v), zero, n)
        } else {
            (v, zero, zero)
        }
    };
    let key = format!("I2F {mode} {half}");
    let r = c.shared_pure(key, &[lo, hi, neg], |c, p| {
        c.int_to_float(p[0], p[1], p[2], mode, half)
    });
    c.store_dst(&i.ops[0], r)
}

fn f2f(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["FTZ", "F16", "F32", "F64", "BF16", "RM", "RP", "RZ"])?;
    let ftz = i.has("FTZ");
    let mode = rounding(i);
    let tys: Vec<&str> = i
        .mods
        .iter()
        .copied()
        .filter(|m| matches!(*m, "F16" | "F32" | "F64" | "BF16"))
        .collect();
    let (v, m) = c.raw(&i.ops[1])?;
    if m.not {
        return bad("F2F source modifiers");
    }
    match tys.as_slice() {
        ["F16", "F32"] => {
            if m.swz != Swz::None {
                return bad("F2F source selector");
            }
            let b = if m.abs { c.and_k(v, 0x7fff_ffff) } else { v };
            let b = if m.neg {
                c.sh(op::BITWISE_XOR, b, 0x8000_0000)
            } else {
                b
            };
            let r = c.f32_to_f16(b, mode, ftz);
            c.store_dst(&i.ops[0], r)
        }
        ["F32", "F16"] => {
            let h = pick(c, v, m.swz, 16, false)?;
            let h = if m.abs { c.and_k(h, 0x7fff) } else { h };
            let h = if m.neg {
                c.sh(op::BITWISE_XOR, h, 0x8000)
            } else {
                h
            };
            let r = c.f16_to_f32(h, ftz);
            c.store_dst(&i.ops[0], r)
        }
        t => bad(format!("F2F {t:?}")),
    }
}

fn f2fp(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut relu = false;
    let mut sat = false;
    let mut mode = 0;
    let mut merge = false;
    for m in &i.mods {
        match *m {
            "PACK_AB" => {}
            "MERGE_C" => merge = true,
            "RELU" => relu = true,
            "SATFINITE" => sat = true,
            "RZ" => mode = 3,
            m => return bad(format!("{}.{m}", i.mnemonic)),
        }
    }
    let a = c.usrc(&i.ops[1])?;
    let b = c.usrc(&i.ops[2])?;
    let key = format!("F2FP {relu} {sat} {mode} {merge}");
    let r = c.shared_pure(key, &[a, b], |c, p| {
        let (a, b) = (p[0], p[1]);
        let mut halves = Vec::new();
        let convert = if merge { vec![a] } else { vec![a, b] };
        for v in convert {
            let mut h = c.f32_to_f16(v, mode, false);
            if relu {
                let s = c.and_k(h, 0x8000);
                let n = c.cmp_k(op::I_NOT_EQUAL, s, 0);
                let z = c.kc(0);
                h = c.bpick(n, z, h);
            }
            if sat {
                let ab = c.and_k(h, 0x7fff);
                let inf = c.cmp_k(op::I_EQUAL, ab, 0x7c00);
                let s = c.and_k(h, 0x8000);
                let mx = c.sh(op::BITWISE_OR, s, 0x7bff);
                h = c.bpick(inf, mx, h);
            }
            halves.push(h);
        }
        let (hi, lo) = if merge {
            (b, halves[0])
        } else {
            (halves[0], halves[1])
        };
        let hi = c.sh(op::SHIFT_LEFT_LOGICAL, hi, 16);
        c.ib(op::BITWISE_OR, hi, lo)
    });
    c.store_dst(&i.ops[0], r)
}

/// FRND: round to an integral value in place. Subnormal inputs read as
/// zero with `.FTZ`; without it they still round away from zero in the
/// directed modes, which the device arithmetic would flush.
pub(crate) fn frnd(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut how = glsl::ROUND_EVEN;
    let mut mode = 0;
    let mut ftz = false;
    let mut half = false;
    for m in &i.mods {
        match *m {
            "" => {}
            "F16" => half = true,
            "FTZ" => ftz = true,
            "FLOOR" => (how, mode) = (glsl::FLOOR, 1),
            "CEIL" => (how, mode) = (glsl::CEIL, 2),
            "TRUNC" => (how, mode) = (glsl::TRUNC, 3),
            m => return bad(format!("FRND.{m}")),
        }
    }
    let (v, m) = c.raw(&i.ops[1])?;
    if m.not {
        return bad("FRND source modifiers");
    }
    let (bits, den) = if half {
        let h = pick(c, v, m.swz, 16, false)?;
        let h = if m.abs { c.and_k(h, 0x7fff) } else { h };
        let h = if m.neg {
            c.sh(op::BITWISE_XOR, h, 0x8000)
        } else {
            h
        };
        (c.f16_to_f32(h, ftz), None)
    } else {
        if m.swz != Swz::None {
            return bad("FRND source selector");
        }
        let b = if m.abs { c.and_k(v, 0x7fff_ffff) } else { v };
        let b = if m.neg {
            c.sh(op::BITWISE_XOR, b, 0x8000_0000)
        } else {
            b
        };
        let ex = c.and_k(b, 0x7f80_0000);
        let mn = c.and_k(b, 0x7f_ffff);
        let ex0 = c.cmp_k(op::I_EQUAL, ex, 0);
        let mnz = c.cmp_k(op::I_NOT_EQUAL, mn, 0);
        let denorm = c.land(ex0, mnz);
        let sign = c.and_k(b, 0x8000_0000);
        (
            c.bpick(denorm, sign, b),
            if ftz { None } else { Some(denorm) },
        )
    };
    let f = c.f32t();
    let x = c.as_float(bits);
    let r = c.ext(f, how, &[x]);
    let mut rb = c.as_bits(r);
    if let Some(d) = den {
        let sign = c.and_k(bits, 0x8000_0000);
        let neg = c.cmp_k(op::I_NOT_EQUAL, sign, 0);
        let pos = c.lnot(neg);
        let one = c.kc(0x3f80_0000);
        let m_one = c.kc(0xbf80_0000);
        let special = match mode {
            1 => c.bpick(neg, m_one, sign),
            2 => c.bpick(pos, one, sign),
            _ => sign,
        };
        rb = c.bpick(d, special, rb);
    }
    let is_nan = {
        let t = c.boolt();
        c.m.emit(op::IS_NAN, t, &[x])
    };
    let canon = c.kc(0x7fff_ffff);
    rb = c.bpick(is_nan, canon, rb);
    let out = if half { c.f32_to_f16(rb, 0, false) } else { rb };
    c.store_dst(&i.ops[0], out)
}

//! Denormals on any host.
//!
//! The guest flushes 32-bit denormal inputs and results to signed zero in
//! `.FTZ` instructions (the result rounded to 24 bits with an unbounded
//! exponent first) and keeps them otherwise. A host either flushes, keeps
//! them, or gives no promise ([`Denorm`]). The translator asks for the
//! DenormPreserve execution mode where it can, and otherwise lets the host
//! arithmetic run and repairs the lanes whose result it cannot trust:
//!
//! - `.FTZ` inputs are flushed with integer operations unless the host
//!   flushes as the guest does.
//! - A lane takes the exact path when its result is tiny (below `2^-125`,
//!   zero included), or, for an instruction without `.FTZ` on a host that
//!   may flush, when an input is a denormal. On a host that keeps denormals
//!   only directed FMUL and FFMA take it: the host rounds a denormal
//!   result to nearest, and the error found on the scaled operands misses
//!   that rounding. The exact path rebuilds each
//!   operand as a significand in `[1, 2)` and an exponent with integer
//!   operations, does the arithmetic on copies scaled so that every value
//!   stays normal (TwoSum and fused multiply-add error terms make the
//!   rounding error exact), and rounds the scaled result to the 24-bit or the
//!   denormal grid itself, building a denormal result's bits as an integer.
//!   Nothing in it depends on the host's denormal mode.
//!
//! The exact path is one SPIR-V function per operation, rounding mode and
//! flush flag, called under a branch, so lanes with normal values pay a
//! compare.

use super::float::Round;
use super::{Ctx, Denorm, Res};
use crate::Inst;
use crate::spirv::{glsl, op};
use std::collections::BTreeMap;

const FUNCTION_PARAMETER: u16 = 55;
const FUNCTION_CALL: u16 = 57;
const RETURN_VALUE: u16 = 254;
const CAP_DENORM_PRESERVE: u32 = 4464;
const MODE_DENORM_PRESERVE: u32 = 4459;
const ZERO_EXP: i32 = -300;
const STICKY: i32 = -60;

/// Per-program denormal bookkeeping.
#[derive(Default)]
pub(crate) struct State {
    /// What the shader runs with.
    pub mode: Option<Denorm>,
    /// Exact-path functions by (operation, rounding, flush).
    funcs: BTreeMap<(u8, u8, bool, i32), u32>,
    /// The function variable that carries a repaired result.
    var: Option<u32>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Arith {
    Add,
    Mul,
    Fma,
}

fn round_code(m: Round) -> u8 {
    match m {
        Round::Nearest => 0,
        Round::Zero => 1,
        Round::Down => 2,
        Round::Up => 3,
    }
}

fn arith_code(a: Arith) -> u8 {
    match a {
        Arith::Add => 0,
        Arith::Mul => 1,
        Arith::Fma => 2,
    }
}

impl Ctx {
    /// Picks the denormal mode the shader runs with: the host's own, or
    /// DenormPreserve where the host can be asked for it.
    pub(crate) fn denorm_plan(&mut self, insts: &[Inst]) {
        let uses_float = insts.iter().any(|i| {
            matches!(
                i.mnemonic,
                "FADD" | "FMUL" | "FFMA" | "FSWZADD" | "FSETP" | "FSET" | "FMNMX"
            )
        });
        let mode = match self.host.denorm {
            Denorm::Unknown if self.host.denorm_preserve_mode && uses_float => {
                self.cap(CAP_DENORM_PRESERVE);
                self.iface.host.denorm_mode = Some(Denorm::Preserve);
                Denorm::Preserve
            }
            d => d,
        };
        if mode == Denorm::Flush && uses_float {
            self.iface.host.check_flush = true;
        }
        self.denorm.mode = Some(mode);
    }

    /// Writes the execution mode chosen by [`Ctx::denorm_plan`].
    pub(crate) fn denorm_mode(&mut self, main: u32) {
        if self.iface.host.denorm_mode == Some(Denorm::Preserve) {
            crate::spirv::inst(
                &mut self.m.modes,
                op::EXECUTION_MODE,
                &[main, MODE_DENORM_PRESERVE, 32],
            );
        }
    }

    fn dn_dmode(&self) -> Denorm {
        self.denorm.mode.unwrap_or(Denorm::Unknown)
    }

    fn dn_ku(&mut self, v: u32) -> u32 {
        self.m.c_u32(v)
    }

    fn dn_ki(&mut self, v: i32) -> u32 {
        self.m.c_i32(v)
    }

    fn dn_ub(&mut self, o: u16, a: u32, b: u32) -> u32 {
        let u = self.u32t();
        self.m.emit(o, u, &[a, b])
    }

    fn dn_ib(&mut self, o: u16, a: u32, b: u32) -> u32 {
        let i = self.i32t();
        self.m.emit(o, i, &[a, b])
    }

    fn dn_bc(&mut self, o: u16, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(o, t, &[a, b])
    }

    fn dn_shl1(&mut self, bits: u32) -> u32 {
        let one = self.dn_ku(1);
        self.dn_ub(op::SHIFT_LEFT_LOGICAL, bits, one)
    }

    /// The float with bits `bits` is a nonzero denormal. Every test here
    /// works on the doubled bits, a form a compiler does not turn into a
    /// float comparison (which a flushing host would answer for zero).
    pub(crate) fn is_denormal(&mut self, bits: u32) -> u32 {
        let b2 = self.dn_shl1(bits);
        let one = self.dn_ku(1);
        let m = self.dn_ub(op::I_SUB, b2, one);
        let lim = self.dn_ku(0x00ff_fffe);
        self.dn_bc(op::U_LESS_THAN, m, lim)
    }

    /// `bits` is a zero of either sign.
    fn is_zero_bits(&mut self, bits: u32) -> u32 {
        let b2 = self.dn_shl1(bits);
        let z = self.dn_ku(0);
        self.dn_bc(op::I_EQUAL, b2, z)
    }

    /// `bits` is neither infinite nor NaN.
    fn is_finite_bits(&mut self, bits: u32) -> u32 {
        let b2 = self.dn_shl1(bits);
        let lim = self.dn_ku(0xff00_0000);
        self.dn_bc(op::U_LESS_THAN, b2, lim)
    }

    /// `bits` is a NaN.
    fn is_nan_bits(&mut self, bits: u32) -> u32 {
        let b2 = self.dn_shl1(bits);
        let lim = self.dn_ku(0xff00_0000);
        self.dn_bc(op::U_GREATER_THAN, b2, lim)
    }

    /// `bits` with a denormal replaced by the signed zero.
    pub(crate) fn flush_in(&mut self, bits: u32) -> u32 {
        let b2 = self.dn_shl1(bits);
        let lim = self.dn_ku(0x0100_0000);
        let e0 = self.dn_bc(op::U_LESS_THAN, b2, lim);
        let sm = self.dn_ku(0x8000_0000);
        let s = self.dn_ub(op::BITWISE_AND, bits, sm);
        let u = self.u32t();
        self.select(u, e0, s, bits)
    }

    /// `bits` with a denormal replaced by the smallest normal of its sign:
    /// the same result whenever the other operands are infinite or NaN.
    fn denormal_to_min(&mut self, bits: u32) -> u32 {
        let d = self.is_denormal(bits);
        let sm = self.dn_ku(0x8000_0000);
        let s = self.dn_ub(op::BITWISE_AND, bits, sm);
        let mn = self.dn_ku(0x0080_0000);
        let v = self.dn_ub(op::BITWISE_OR, s, mn);
        let u = self.u32t();
        self.select(u, d, v, bits)
    }

    /// The inputs of an arithmetic instruction as the host should see
    /// them.
    fn arith_inputs(&mut self, xs: &[u32], ftz: bool) -> Vec<u32> {
        let mode = self.dn_dmode();
        xs.iter()
            .map(|&x| {
                let b = if ftz && mode != Denorm::Flush {
                    self.flush_in(x)
                } else if !ftz && mode != Denorm::Preserve {
                    self.denormal_to_min(x)
                } else {
                    x
                };
                self.as_float(b)
            })
            .collect()
    }

    /// SAT on result bits: a NaN and any value with the sign bit set (`-0`
    /// and negative denormals included) become `+0`, values from `1` up
    /// become `1`, and the rest stay, which no float clamp promises.
    pub(crate) fn sat_bits(&mut self, bits: u32, on: bool) -> u32 {
        if !on {
            return bits;
        }
        let i = self.i32t();
        let u = self.u32t();
        let sb = self.m.emit(op::BITCAST, i, &[bits]);
        let zi = self.dn_ki(0);
        let neg = self.dn_bc(op::S_LESS_THAN, sb, zi);
        let nan = self.is_nan_bits(bits);
        let one = self.dn_ku(0x3f80_0000);
        let big = self.dn_bc(op::U_GREATER_THAN_EQUAL, bits, one);
        let r = self.select(u, big, one, bits);
        let zero = self.or(neg, nan);
        let z = self.dn_ku(0);
        self.select(u, zero, z, r)
    }

    /// `a + b` rounded by `mode`, exact on any host.
    pub(crate) fn fadd_exact(&mut self, a: u32, b: u32, mode: Round, ftz: bool) -> u32 {
        self.arith_exact(Arith::Add, &[a, b], mode, ftz, 0)
    }

    /// `a * b * 2^scale` rounded once by `mode`, exact on any host.
    pub(crate) fn fmul_exact(&mut self, a: u32, b: u32, mode: Round, ftz: bool, scale: i32) -> u32 {
        self.arith_exact(Arith::Mul, &[a, b], mode, ftz, scale)
    }

    /// `a * b + c` rounded by `mode`, exact on any host.
    pub(crate) fn ffma_exact(&mut self, a: u32, b: u32, c: u32, mode: Round, ftz: bool) -> u32 {
        self.arith_exact(Arith::Fma, &[a, b, c], mode, ftz, 0)
    }

    fn arith_exact(&mut self, k: Arith, xs: &[u32], mode: Round, ftz: bool, scale: i32) -> u32 {
        if k == Arith::Fma && !self.host.fused_fma {
            return self.soft_fma(xs[0], xs[1], Some(xs[2]), mode, ftz, 0);
        }
        let dm = self.dn_dmode();
        let ins = self.arith_inputs(xs, ftz);
        let mut unscaled = None;
        let fast = match k {
            Arith::Add => self.fadd_round(ins[0], ins[1], mode, ftz),
            Arith::Mul => {
                let p = self.fmul_round(ins[0], ins[1], mode, ftz);
                if scale == 0 {
                    p
                } else {
                    unscaled = Some(p);
                    let f = self.f32t();
                    let s = self.m.c_f32(2f32.powi(scale).to_bits());
                    self.exact_mul(f, p, s)
                }
            }
            Arith::Fma => self.ffma_round(ins[0], ins[1], ins[2], mode, ftz),
        };
        let native = scale == 0
            && match dm {
                Denorm::Flush => ftz,
                Denorm::Preserve => !ftz && (mode == Round::Nearest || k == Arith::Add),
                Denorm::Unknown => false,
            };
        let fast_bits = self.as_bits(fast);
        if native {
            return self.canonical_nan(fast_bits, xs);
        }
        self.iface.host.denorm_fixups += 1;
        let xs: Vec<u32> = if ftz {
            xs.iter().map(|&x| self.flush_in(x)).collect()
        } else {
            xs.to_vec()
        };
        let b2 = self.dn_shl1(fast_bits);
        let lim = self.dn_ku(0x0200_0000);
        let mut slow = self.dn_bc(op::U_LESS_THAN, b2, lim);
        if let Some(p) = unscaled {
            let pb = self.as_bits(p);
            let p2 = self.dn_shl1(pb);
            let lo = self.dn_ku(0x3600_0000);
            let hi = self.dn_ku(0xc600_0000);
            let small = self.dn_bc(op::U_LESS_THAN, p2, lo);
            let big = self.dn_bc(op::U_GREATER_THAN_EQUAL, p2, hi);
            let edge = self.or(small, big);
            slow = self.or(slow, edge);
        }
        if !ftz && dm != Denorm::Preserve {
            for &x in &xs {
                let d = self.is_denormal(x);
                slow = self.or(slow, d);
            }
        }
        for &x in &xs {
            let fin = self.is_finite_bits(x);
            slow = self.and(slow, fin);
        }
        let zero = self.dn_ku(0);
        let z: Vec<u32> = xs.iter().map(|&x| self.is_zero_bits(x)).collect();
        let trivial = match k {
            Arith::Add => self.and(z[0], z[1]),
            Arith::Mul => self.or(z[0], z[1]),
            Arith::Fma => {
                let p = self.or(z[0], z[1]);
                self.and(p, z[2])
            }
        };
        let nt = self.not(trivial);
        slow = self.and(slow, nt);
        let func = self.exact_fn(k, mode, ftz, scale);
        let var = self.denorm_var();
        self.m.emit_void(op::STORE, &[var, fast_bits]);
        let then = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[slow, then, merge]);
        self.m.emit_void(op::LABEL, &[then]);
        let u = self.u32t();
        let mut args = vec![func];
        args.extend(xs.iter().copied());
        while args.len() < 4 {
            args.push(zero);
        }
        let r = self.m.emit(FUNCTION_CALL, u, &args);
        self.m.emit_void(op::STORE, &[var, r]);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
        let r = self.m.emit(op::LOAD, u, &[var]);
        self.canonical_nan(r, &xs)
    }

    /// `r`, or the guest's NaN `0x7fffffff` when `r` or an input is a NaN
    /// (the host may keep or make another payload).
    fn canonical_nan(&mut self, r: u32, xs: &[u32]) -> u32 {
        let mut nan = self.is_nan_bits(r);
        for &x in xs {
            let n = self.is_nan_bits(x);
            nan = self.or(nan, n);
        }
        let k = self.dn_ku(0x7fff_ffff);
        let u = self.u32t();
        self.select(u, nan, k, r)
    }

    fn exact_mul(&mut self, f: u32, a: u32, b: u32) -> u32 {
        let r = self.m.emit(op::F_MUL, f, &[a, b]);
        self.m.deco(r, 42, &[]);
        r
    }

    fn denorm_var(&mut self) -> u32 {
        if let Some(v) = self.denorm.var {
            return v;
        }
        let v = self.m.id();
        self.locals.push(v);
        self.denorm.var = Some(v);
        v
    }

    /// The exact-path function for one operation: `(a, b, c) -> bits`.
    fn exact_fn(&mut self, k: Arith, mode: Round, ftz: bool, scale: i32) -> u32 {
        let key = (arith_code(k), round_code(mode), ftz, scale);
        if let Some(&f) = self.denorm.funcs.get(&key) {
            return f;
        }
        let saved = std::mem::take(&mut self.m.body);
        let u = self.u32t();
        let fnt = self.m.t_fn_args(u, &[u, u, u]);
        let id = self.m.id();
        self.m.emit_void(op::FUNCTION, &[u, id, 0, fnt]);
        let ps: Vec<u32> = (0..3)
            .map(|_| self.m.emit(FUNCTION_PARAMETER, u, &[]))
            .collect();
        let entry = self.m.id();
        self.m.emit_void(op::LABEL, &[entry]);
        let r = self.exact_body(k, &ps, mode, ftz, scale);
        self.m.emit_void(RETURN_VALUE, &[r]);
        self.m.emit_void(op::FUNCTION_END, &[]);
        let body = std::mem::replace(&mut self.m.body, saved);
        self.funcs.extend(body);
        self.denorm.funcs.insert(key, id);
        id
    }

    /// `(significand, exponent)` of `x` from its bits: `x = sig * 2^exp`
    /// with `|sig|` in `[1, 2)`, or a signed zero with a very small
    /// exponent. Denormals are normalized; with `ftz` the caller has
    /// flushed them.
    fn dn_decompose(&mut self, bits: u32) -> (u32, u32) {
        let u = self.u32t();
        let i = self.i32t();
        let sm = self.dn_ku(0x8000_0000);
        let mm = self.dn_ku(0x007f_ffff);
        let one = self.dn_ku(0x3f80_0000);
        let k23 = self.dn_ku(23);
        let ff = self.dn_ku(0xff);
        let z = self.dn_ku(0);
        let sign = self.dn_ub(op::BITWISE_AND, bits, sm);
        let mant = self.dn_ub(op::BITWISE_AND, bits, mm);
        let sh = self.dn_ub(op::SHIFT_RIGHT_LOGICAL, bits, k23);
        let field = self.dn_ub(op::BITWISE_AND, sh, ff);
        let sub = self.dn_bc(op::I_EQUAL, field, z);
        let am = self.dn_ku(0x7fff_ffff);
        let mag = self.dn_ub(op::BITWISE_AND, bits, am);
        let zero = self.dn_bc(op::I_EQUAL, mag, z);
        let msb = self.ext(i, glsl::FIND_U_MSB, &[mant]);
        let i23 = self.dn_ki(23);
        let shift = self.dn_ib(op::I_SUB, i23, msb);
        let m2 = self.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[mant, shift]);
        let m2 = self.dn_ub(op::BITWISE_AND, m2, mm);
        let base = self.dn_ub(op::BITWISE_OR, sign, one);
        let sub_sig = self.dn_ub(op::BITWISE_OR, base, m2);
        let m126 = self.dn_ki(-126);
        let sub_e = self.dn_ib(op::I_SUB, m126, shift);
        let norm_sig = self.dn_ub(op::BITWISE_OR, base, mant);
        let fi = self.m.emit(op::BITCAST, i, &[field]);
        let bias = self.dn_ki(127);
        let norm_e = self.dn_ib(op::I_SUB, fi, bias);
        let sig = self.select(u, sub, sub_sig, norm_sig);
        let e = self.select(i, sub, sub_e, norm_e);
        let sig = self.select(u, zero, sign, sig);
        let ze = self.dn_ki(ZERO_EXP);
        let e = self.select(i, zero, ze, e);
        (self.as_float(sig), e)
    }

    fn dn_imax(&mut self, a: u32, b: u32) -> u32 {
        let i = self.i32t();
        self.ext(i, glsl::S_MAX, &[a, b])
    }

    fn dn_iclamp(&mut self, a: u32, lo: i32, hi: i32) -> u32 {
        let i = self.i32t();
        let l = self.dn_ki(lo);
        let h = self.dn_ki(hi);
        self.ext(i, glsl::S_CLAMP, &[a, l, h])
    }

    /// `sig * 2^max(d, STICKY)`: an operand far below the other only
    /// breaks ties, so it is kept as a small normal value of its sign.
    fn dn_scaled(&mut self, sig: u32, d: u32) -> u32 {
        let st = self.dn_ki(STICKY);
        let d = self.dn_imax(d, st);
        self.ldexp(sig, d)
    }

    /// The exact result's bits: `(r1 + E) * 2^r` with `r1` the scaled
    /// value rounded to nearest at 24 bits and `e` a value with the sign of
    /// the exact residual `E`.
    fn exact_body(&mut self, k: Arith, ps: &[u32], mode: Round, ftz: bool, scale: i32) -> u32 {
        let (r1, e, r) = match k {
            Arith::Add => {
                let (sa, ea) = self.dn_decompose(ps[0]);
                let (sb, eb) = self.dn_decompose(ps[1]);
                let r = self.dn_imax(ea, eb);
                let da = self.dn_ib(op::I_SUB, ea, r);
                let db = self.dn_ib(op::I_SUB, eb, r);
                let a1 = self.dn_scaled(sa, da);
                let b1 = self.dn_scaled(sb, db);
                let (s, e) = self.two_sum(a1, b1);
                (s, e, r)
            }
            Arith::Mul => {
                let (sa, ea) = self.dn_decompose(ps[0]);
                let (sb, eb) = self.dn_decompose(ps[1]);
                let f = self.f32t();
                let p = self.exact_mul(f, sa, sb);
                let e = self.prod_err(sa, sb, p);
                let r = self.dn_ib(op::I_ADD, ea, eb);
                let sc = self.dn_ki(scale);
                let r = self.dn_ib(op::I_ADD, r, sc);
                (p, e, r)
            }
            Arith::Fma => {
                let (sa, ea) = self.dn_decompose(ps[0]);
                let (sb, eb) = self.dn_decompose(ps[1]);
                let (sc, ec) = self.dn_decompose(ps[2]);
                let kk = self.dn_ib(op::I_ADD, ea, eb);
                let r = self.dn_imax(kk, ec);
                let dp = self.dn_ib(op::I_SUB, kk, r);
                let dc = self.dn_ib(op::I_SUB, ec, r);
                let b1 = self.dn_scaled(sb, dp);
                let c1 = self.dn_scaled(sc, dc);
                let a1 = sa;
                let r1 = self.fma(a1, b1, c1);
                let f = self.f32t();
                let p1 = self.exact_mul(f, a1, b1);
                let np1 = self.fneg(p1);
                let pe = self.fma(a1, b1, np1);
                let (uu, ue) = self.two_sum(p1, c1);
                let nr = self.fneg(r1);
                let (v, ve) = self.two_sum(uu, nr);
                let (t1, f1) = self.two_sum(ue, pe);
                let (t2, f2) = self.two_sum(ve, t1);
                let (t3, f3) = self.two_sum(v, t2);
                let low = self.exact(op::F_ADD, f2, f1);
                let low = self.exact(op::F_ADD, f3, low);
                let zero = self.m.c_f32(0);
                let t3z = self.fcmp(op::F_ORD_EQUAL, t3, zero);
                let e = self.select(f, t3z, low, t3);
                (r1, e, r)
            }
        };
        self.round_scaled(r1, e, r, mode, ftz)
    }

    /// Rounds `(r1 + E) * 2^r` by `mode` to the guest's result bits.
    fn round_scaled(&mut self, r1: u32, e: u32, r: u32, mode: Round, ftz: bool) -> u32 {
        let u = self.u32t();
        let zero = self.m.c_f32(0);
        let t = self.m.c_bool(true);
        let (up, down) = self.error_sides(r1, e, t);
        let out1 = self.directed(r1, up, down, mode, false);
        let bits1 = self.as_bits(out1);
        let sm = self.dn_ku(0x8000_0000);
        let sign = self.dn_ub(op::BITWISE_AND, bits1, sm);
        let rc = self.dn_iclamp(r, -254, 254);
        let normal = self.ldexp(out1, rc);
        let normal_bits = self.as_bits(normal);
        let eo = self.fexp(out1);
        let eo = self.dn_ib(op::I_ADD, eo, r);
        let k127 = self.dn_ki(127);
        let ovf = self.dn_bc(op::S_GREATER_THAN, eo, k127);
        let neg = {
            let z = self.dn_ku(0);
            self.dn_bc(op::I_NOT_EQUAL, sign, z)
        };
        let to_inf = match mode {
            Round::Nearest => self.m.c_bool(true),
            Round::Zero => self.m.c_bool(false),
            Round::Down => neg,
            Round::Up => self.not(neg),
        };
        let inf = self.dn_ku(0x7f80_0000);
        let max = self.dn_ku(0x7f7f_ffff);
        let big = self.select(u, to_inf, inf, max);
        let big = self.dn_ub(op::BITWISE_OR, sign, big);
        let normal_bits = self.select(u, ovf, big, normal_bits);
        let m126 = self.dn_ki(-126);
        let r1_zero = self.fcmp(op::F_ORD_EQUAL, r1, zero);
        let cancel = match mode {
            Round::Down => self.dn_ku(0x8000_0000),
            _ => self.dn_ku(0),
        };
        let res = if ftz {
            let tiny = self.dn_bc(op::S_LESS_THAN, eo, m126);
            self.select(u, tiny, sign, normal_bits)
        } else {
            let er = self.fexp(r1);
            let er = self.dn_ib(op::I_ADD, er, r);
            let below = self.dn_bc(op::S_LESS_THAN, er, m126);
            let at = self.dn_bc(op::I_EQUAL, er, m126);
            let rb = self.as_bits(r1);
            let mm = self.dn_ku(0x007f_ffff);
            let rm = self.dn_ub(op::BITWISE_AND, rb, mm);
            let z = self.dn_ku(0);
            let pow2 = self.dn_bc(op::I_EQUAL, rm, z);
            let at = self.and(at, pow2);
            let grid = self.or(below, at);
            let rsign = self.dn_ub(op::BITWISE_AND, rb, sm);
            let n = self.grid_round(r1, e, r, mode);
            let nb = self.m.emit(op::CONVERT_F_TO_U, u, &[n]);
            let gb = self.dn_ub(op::BITWISE_OR, rsign, nb);
            self.select(u, grid, gb, normal_bits)
        };
        self.select(u, r1_zero, cancel, res)
    }

    /// `|round(V / 2^-149)|` as an integral float, `V = (r1 + E) * 2^r`
    /// below the normal range.
    fn grid_round(&mut self, r1: u32, e: u32, r: u32, mode: Round) -> u32 {
        let f = self.f32t();
        let i = self.i32t();
        let zero = self.m.c_f32(0);
        let one = self.m.c_f32(1f32.to_bits());
        let half = self.m.c_f32(0.5f32.to_bits());
        let k149 = self.dn_ki(149);
        let sh = self.dn_ib(op::I_ADD, k149, r);
        let er = self.fexp(r1);
        let ner = self.ineg(er);
        let m2 = self.dn_ki(-2);
        let floor_sh = self.dn_ib(op::I_ADD, ner, m2);
        let sh = self.dn_imax(sh, floor_sh);
        let sh = self.dn_iclamp(sh, -254, 254);
        let s = self.ldexp(r1, sh);
        let fl = self.ext(f, glsl::FLOOR, &[s]);
        let y0 = self.exact(op::F_SUB, s, fl);
        let fl1 = self.exact(op::F_ADD, fl, one);
        let flm = self.exact(op::F_SUB, fl, one);
        let y0z = self.fcmp(op::F_ORD_EQUAL, y0, zero);
        let ep = self.fcmp(op::F_ORD_GREATER_THAN, e, zero);
        let en = self.fcmp(op::F_ORD_LESS_THAN, e, zero);
        let fl_down = {
            let c = self.and(y0z, en);
            self.select(f, c, flm, fl)
        };
        let fl_up = {
            let ny = self.not(y0z);
            let c = self.or(ny, ep);
            self.select(f, c, fl1, fl)
        };
        let n = match mode {
            Round::Down => fl_down,
            Round::Up => fl_up,
            Round::Zero => {
                let pos = self.fcmp(op::F_ORD_GREATER_THAN, r1, zero);
                self.select(f, pos, fl_down, fl_up)
            }
            Round::Nearest => {
                let d = self.exact(op::F_SUB, y0, half);
                let dz = self.fcmp(op::F_ORD_EQUAL, d, zero);
                let dp = self.fcmp(op::F_ORD_GREATER_THAN, d, zero);
                let upc = self.and(dz, ep);
                let go_up = self.or(dp, upc);
                let ez = {
                    let a = self.not(ep);
                    let b = self.not(en);
                    self.and(a, b)
                };
                let tie = self.and(dz, ez);
                let fi = self.m.emit(op::CONVERT_F_TO_S, i, &[fl]);
                let o = self.dn_ki(1);
                let par = self.dn_ib(op::BITWISE_AND, fi, o);
                let zi = self.dn_ki(0);
                let odd = self.dn_bc(op::I_NOT_EQUAL, par, zi);
                let tie_up = self.and(tie, odd);
                let go_up = self.or(go_up, tie_up);
                self.select(f, go_up, fl1, fl)
            }
        };
        self.ext(f, glsl::F_ABS, &[n])
    }

    /// The order key of float bits: unsigned keys compare as the numbers
    /// do, with both zeros equal (NaN excluded).
    fn order_key(&mut self, bits: u32) -> u32 {
        let u = self.u32t();
        let sm = self.dn_ku(0x8000_0000);
        let z = self.dn_ku(0);
        let s = self.dn_ub(op::BITWISE_AND, bits, sm);
        let neg = self.dn_bc(op::I_NOT_EQUAL, s, z);
        let inv = self.m.emit(op::NOT, u, &[bits]);
        let pos = self.dn_ub(op::BITWISE_OR, bits, sm);
        let k = self.select(u, neg, inv, pos);
        let zero = self.is_zero_bits(bits);
        self.select(u, zero, sm, k)
    }

    /// The float comparison `cmp` of the floats with bits `a` and `b` as
    /// the guest makes it: flushed inputs for `.FTZ`, and without `.FTZ`
    /// on a host that may flush, an integer order comparison when an input
    /// is a denormal.
    pub(crate) fn fcompare_exact(&mut self, cmp: &str, a: u32, b: u32, ftz: bool) -> Res<u32> {
        let mode = self.dn_dmode();
        if ftz {
            let (a, b) = if mode == Denorm::Flush {
                (a, b)
            } else {
                (self.flush_in(a), self.flush_in(b))
            };
            let fa = self.as_float(a);
            let fb = self.as_float(b);
            return self.fcompare(cmp, fa, fb);
        }
        let fa = self.as_float(a);
        let fb = self.as_float(b);
        let native = self.fcompare(cmp, fa, fb)?;
        if mode == Denorm::Preserve {
            return Ok(native);
        }
        let (base, unordered) = match cmp {
            "LT" | "LTU" => (op::U_LESS_THAN, cmp.ends_with('U')),
            "EQ" | "EQU" => (op::I_EQUAL, cmp.ends_with('U')),
            "LE" | "LEU" => (op::U_LESS_THAN_EQUAL, cmp.ends_with('U')),
            "GT" | "GTU" => (op::U_GREATER_THAN, cmp.ends_with('U')),
            "NE" | "NEU" => (op::I_NOT_EQUAL, cmp.ends_with('U')),
            "GE" | "GEU" => (op::U_GREATER_THAN_EQUAL, cmp.ends_with('U')),
            _ => return Ok(native),
        };
        let ka = self.order_key(a);
        let kb = self.order_key(b);
        let r = self.dn_bc(base, ka, kb);
        let na = self.is_nan_bits(a);
        let nb = self.is_nan_bits(b);
        let nan = self.or(na, nb);
        let r = if unordered {
            self.or(nan, r)
        } else {
            let n = self.not(nan);
            self.and(n, r)
        };
        let da = self.is_denormal(a);
        let db = self.is_denormal(b);
        let any = self.or(da, db);
        let bt = self.boolt();
        Ok(self.select(bt, any, r, native))
    }

    /// FMNMX on float bits, any host: `(min, max)` bits. The native
    /// minimum and maximum, the guest's signed zeros (`-0` is the smaller),
    /// an exact choice by the order keys when an input is a denormal, the
    /// other operand when one is a NaN and `0x7fffffff` when both are.
    pub(crate) fn fminmax(&mut self, a: u32, b: u32, ftz: bool) -> (u32, u32) {
        let f = self.f32t();
        let u = self.u32t();
        let mode = self.dn_dmode();
        let (a, b) = if ftz {
            (self.flush_in(a), self.flush_in(b))
        } else {
            (a, b)
        };
        let fa = self.as_float(a);
        let fb = self.as_float(b);
        let lo = self.ext(f, glsl::N_MIN, &[fa, fb]);
        let hi = self.ext(f, glsl::N_MAX, &[fa, fb]);
        let lo = self.as_bits(lo);
        let hi = self.as_bits(hi);
        let za = self.is_zero_bits(a);
        let zb = self.is_zero_bits(b);
        let both = self.and(za, zb);
        let o = self.dn_ub(op::BITWISE_OR, a, b);
        let n = self.dn_ub(op::BITWISE_AND, a, b);
        let lo = self.select(u, both, o, lo);
        let hi = self.select(u, both, n, hi);
        let (lo, hi) = if ftz || mode == Denorm::Preserve {
            (lo, hi)
        } else {
            let ka = self.order_key(a);
            let kb = self.order_key(b);
            let da = self.is_denormal(a);
            let db = self.is_denormal(b);
            let any = self.or(da, db);
            let lt = self.dn_bc(op::U_LESS_THAN, ka, kb);
            let ex_lo = self.select(u, lt, a, b);
            let ex_hi = self.select(u, lt, b, a);
            (
                self.select(u, any, ex_lo, lo),
                self.select(u, any, ex_hi, hi),
            )
        };
        let na = self.is_nan_bits(a);
        let nb = self.is_nan_bits(b);
        let lo = self.select(u, nb, a, lo);
        let hi = self.select(u, nb, a, hi);
        let lo = self.select(u, na, b, lo);
        let hi = self.select(u, na, b, hi);
        let two = self.and(na, nb);
        let k = self.dn_ku(0x7fff_ffff);
        (self.select(u, two, k, lo), self.select(u, two, k, hi))
    }
}

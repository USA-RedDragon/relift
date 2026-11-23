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
use super::host::{FIX_ROUND_AFTER, FIX_ZERO_SIGN};
use super::{Ctx, Denorm, Res};
use crate::Inst;
use crate::spirv::{glsl, op};
use std::collections::BTreeMap;

const FUNCTION_PARAMETER: u16 = 55;
const FUNCTION_CALL: u16 = 57;
const RETURN_VALUE: u16 = 254;
const CAP_DENORM_PRESERVE: u32 = 4464;
const MODE_DENORM_PRESERVE: u32 = 4459;
const CAP_DENORM_FLUSH: u32 = 4465;
const MODE_DENORM_FLUSH: u32 = 4460;
const CAP_SZINP: u32 = 4466;
const MODE_SZINP: u32 = 4461;
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
    pub(crate) var: Option<u32>,
    /// SignedZeroInfNanPreserve can be requested.
    szinp: bool,
    /// Native half arithmetic is used: declare the 16-bit modes.
    pub(crate) f16: bool,
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
    /// DenormPreserve where the host can be asked for it, else
    /// DenormFlushToZero where the runtime's probe found that mode follows
    /// SM86's flushing model exactly.
    pub(crate) fn denorm_plan(&mut self, insts: &[Inst]) {
        let uses_float = insts.iter().any(|i| {
            matches!(
                i.mnemonic,
                "FADD" | "FMUL" | "FFMA" | "FSWZADD" | "FSETP" | "FSET" | "FMNMX"
            )
        });
        if self.math == super::Math::Native {
            let ftz = insts.iter().any(|i| i.has("FTZ"));
            let mode = if self.host.denorm == Denorm::Unknown
                && self.host.denorm_flush_mode
                && uses_float
                && ftz
            {
                self.cap(CAP_DENORM_FLUSH);
                self.iface.host.denorm_mode = Some(Denorm::Flush);
                Denorm::Flush
            } else {
                self.host.denorm
            };
            self.denorm.mode = Some(mode);
            if self.host.signed_zero_inf_nan_preserve {
                self.cap(CAP_SZINP);
                self.denorm.szinp = true;
            }
            self.iface
                .host
                .inexact
                .push("native float math: results may differ from the console's".to_string());
            return;
        }
        let mode = match self.host.denorm {
            Denorm::Unknown if self.host.denorm_preserve_mode && uses_float => {
                self.cap(CAP_DENORM_PRESERVE);
                self.iface.host.denorm_mode = Some(Denorm::Preserve);
                Denorm::Preserve
            }
            Denorm::Unknown if self.host.denorm_flush_exact && uses_float => {
                self.cap(CAP_DENORM_FLUSH);
                self.iface.host.denorm_mode = Some(Denorm::Flush);
                Denorm::Flush
            }
            d => d,
        };
        if mode == Denorm::Flush && uses_float {
            self.iface.host.check_flush = true;
        }
        self.denorm.mode = Some(mode);
        if self.host.signed_zero_inf_nan_preserve {
            self.cap(CAP_SZINP);
            self.denorm.szinp = true;
        }
    }

    /// Writes the execution modes chosen by [`Ctx::denorm_plan`]:
    /// DenormPreserve, and SignedZeroInfNanPreserve wherever the program
    /// declares 32-bit floats. A program with floats on a host that cannot
    /// preserve them is reported inexact.
    pub(crate) fn denorm_mode(&mut self, main: u32) {
        if self.denorm.f16 {
            for mode in [MODE_DENORM_PRESERVE, 4462, MODE_SZINP] {
                crate::spirv::inst(&mut self.m.modes, op::EXECUTION_MODE, &[main, mode, 16]);
            }
        }
        if self.iface.host.denorm_mode == Some(Denorm::Preserve) {
            crate::spirv::inst(
                &mut self.m.modes,
                op::EXECUTION_MODE,
                &[main, MODE_DENORM_PRESERVE, 32],
            );
        }
        if self.iface.host.denorm_mode == Some(Denorm::Flush) {
            crate::spirv::inst(
                &mut self.m.modes,
                op::EXECUTION_MODE,
                &[main, MODE_DENORM_FLUSH, 32],
            );
        }
        if self.denorm.szinp {
            crate::spirv::inst(
                &mut self.m.modes,
                op::EXECUTION_MODE,
                &[main, MODE_SZINP, 32],
            );
        } else if self.declares_f32() {
            self.iface.host.inexact.push(
                "32-bit float arithmetic without SignedZeroInfNanPreserve: signed zeros, \
                 infinities and NaNs may be lost"
                    .to_string(),
            );
        }
    }

    /// The module declares the 32-bit float type.
    fn declares_f32(&self) -> bool {
        let t = &self.m.types;
        let mut k = 0;
        while k < t.len() {
            let n = (t[k] >> 16) as usize;
            if t[k] & 0xffff == u32::from(op::TYPE_FLOAT) && t.get(k + 2) == Some(&32) {
                return true;
            }
            k += n.max(1);
        }
        false
    }

    pub(crate) fn dn_dmode(&self) -> Denorm {
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
    pub(crate) fn is_nan_bits(&mut self, bits: u32) -> u32 {
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
        if ftz
            && scale == 0
            && mode == Round::Nearest
            && k != Arith::Fma
            && self.dn_dmode() == Denorm::Unknown
            && let Some(fix) = self.host.flush_fix
        {
            return self.arith_flush_fixed(k, xs, fix);
        }
        if k == Arith::Fma && !self.host.fused_fma {
            if mode == Round::Nearest {
                return self.float_fma(xs[0], xs[1], xs[2], ftz);
            }
            return self.soft_fma(xs[0], xs[1], Some(xs[2]), mode, ftz, 0);
        }
        let dm = self.dn_dmode();
        if !ftz && dm != Denorm::Preserve && scale == 0 && mode == Round::Nearest {
            return self.arith_guarded(k, xs);
        }
        self.arith_checked(k, xs, mode, ftz, scale)
    }

    /// The result `r` of a multiplication of the bits `a` and `b` with a zero
    /// result given the sign of the product's operands, on a device whose
    /// probe found its multiplication loses it ([`FIX_ZERO_SIGN`]); every
    /// other result is as it is.
    pub(crate) fn mul_zero_sign(&mut self, r: u32, a: u32, b: u32) -> u32 {
        if self.host.flush_fix.is_none_or(|f| f & FIX_ZERO_SIGN == 0) {
            return r;
        }
        let sm = self.dn_ku(0x8000_0000);
        let x = self.dn_ub(op::BITWISE_XOR, a, b);
        let sign = self.dn_ub(op::BITWISE_AND, x, sm);
        let mag = self.dn_ku(0x7fff_ffff);
        let m = self.dn_ub(op::BITWISE_AND, r, mag);
        let z = self.dn_ku(0);
        let zero = self.dn_bc(op::I_EQUAL, m, z);
        let u = self.u32t();
        self.select(u, zero, sign, r)
    }

    /// A round-to-nearest `.FTZ` add or multiply on a device that flushes
    /// as the instruction does except for what its probe found in the
    /// multiplication (see [`HostProfile::flush_fix`]): the device's result
    /// with each probed deviation corrected, nothing else.
    fn arith_flush_fixed(&mut self, k: Arith, xs: &[u32], fix: u8) -> u32 {
        let fs: Vec<u32> = xs.iter().map(|&x| self.as_float(x)).collect();
        let r = match k {
            Arith::Add => self.fadd_round(fs[0], fs[1], Round::Nearest, true),
            _ => self.fmul_round(fs[0], fs[1], Round::Nearest, true),
        };
        let mut res = self.as_bits(r);
        if k == Arith::Mul {
            let sm = self.dn_ku(0x8000_0000);
            let x = self.dn_ub(op::BITWISE_XOR, xs[0], xs[1]);
            let sign = self.dn_ub(op::BITWISE_AND, x, sm);
            let u = self.u32t();
            if fix & FIX_ROUND_AFTER != 0 {
                let up = self.m.c_f32(0x5f80_0000);
                let a = self.exact(op::F_MUL, fs[0], up);
                let q = self.exact(op::F_MUL, a, fs[1]);
                let f = self.f32t();
                let q = self.ext(f, glsl::F_ABS, &[q]);
                let lim = self.m.c_f32(0x2080_0000);
                let flush = self.fcmp(op::F_ORD_LESS_THAN, q, lim);
                res = self.select(u, flush, sign, res);
            }
        }
        self.canonical_nan_host(res, xs)
    }

    /// A round-to-nearest add, multiply or fused multiply-add without
    /// `.FTZ` on a host that may flush: the host's result on the operands
    /// as they are, unless an operand is a denormal or the result is tiny
    /// and not a sum or product of zeros, the only cases where a flushing
    /// host can differ; those branch to [`Ctx::arith_checked`], which gives
    /// the same result as this one everywhere else.
    fn arith_guarded(&mut self, k: Arith, xs: &[u32]) -> u32 {
        let fs: Vec<u32> = xs.iter().map(|&x| self.as_float(x)).collect();
        let r = match k {
            Arith::Add => self.fadd_round(fs[0], fs[1], Round::Nearest, false),
            Arith::Mul => self.fmul_round(fs[0], fs[1], Round::Nearest, false),
            Arith::Fma => self.ffma_round(fs[0], fs[1], fs[2], Round::Nearest, false),
        };
        let bits = self.as_bits(r);
        let b2 = self.dn_shl1(bits);
        let lim = self.dn_ku(0x0200_0000);
        let tiny = self.dn_bc(op::U_LESS_THAN, b2, lim);
        let trivial = self.zero_operands(k, xs);
        let nt = self.not(trivial);
        let mut pre = self.and(tiny, nt);
        for &x in xs {
            let d = self.is_denormal(x);
            pre = self.or(pre, d);
        }
        let check = self.m.id();
        let plain = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[pre, check, plain]);
        self.m.emit_void(op::LABEL, &[check]);
        let rc = self.arith_checked(k, xs, Round::Nearest, false, 0);
        let check_end = self.m.id();
        self.m.emit_void(op::BRANCH, &[check_end]);
        self.m.emit_void(op::LABEL, &[check_end]);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[plain]);
        let rp = self.canonical_nan_host(bits, xs);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
        let u = self.u32t();
        self.m.emit(op::PHI, u, &[rc, check_end, rp, plain])
    }

    /// Whether the result is a sum or product of zeros (`0 + 0`, `0 * x`,
    /// `0 * x + 0`), whose sign no denormal handling changes.
    fn zero_operands(&mut self, k: Arith, xs: &[u32]) -> u32 {
        let z: Vec<u32> = xs.iter().map(|&x| self.is_zero_bits(x)).collect();
        match k {
            Arith::Add => self.and(z[0], z[1]),
            Arith::Mul => self.or(z[0], z[1]),
            Arith::Fma => {
                let p = self.or(z[0], z[1]);
                self.and(p, z[2])
            }
        }
    }

    /// [`Ctx::arith_exact`] with every check: denormal operands and tiny
    /// results take the exact integer function.
    fn arith_checked(&mut self, k: Arith, xs: &[u32], mode: Round, ftz: bool, scale: i32) -> u32 {
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
            return self.canonical_nan_host(fast_bits, xs);
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
        let trivial = self.zero_operands(k, &xs);
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

    /// [`Ctx::canonical_nan`] for `r` the result of the host's own add,
    /// multiply or fused multiply-add on `xs`. Under SignedZeroInfNanPreserve
    /// the host keeps IEEE NaN propagation, so `r` is a NaN whenever an input
    /// is one and only `r` needs the test.
    fn canonical_nan_host(&mut self, r: u32, xs: &[u32]) -> u32 {
        if self.denorm.szinp {
            self.canonical_nan(r, &[])
        } else {
            self.canonical_nan(r, xs)
        }
    }

    /// `r`, or the guest's NaN `0x7fffffff` when `r` or an input is a NaN
    /// (the host may keep or make another payload). Under
    /// SignedZeroInfNanPreserve the host's `OpIsNan` is exact, one compare
    /// where the bit test takes two.
    fn canonical_nan(&mut self, r: u32, xs: &[u32]) -> u32 {
        if self.host.canonical_nan {
            return r;
        }
        let mut nan = if self.denorm.szinp {
            let f = self.as_float(r);
            let b = self.boolt();
            self.m.emit(op::IS_NAN, b, &[f])
        } else {
            self.is_nan_bits(r)
        };
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
        let saved_vars = self.m.suspend_vars();
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
        self.m.restore_vars(saved_vars);
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
        if k == Arith::Fma && mode == Round::Nearest && !ftz && scale == 0 {
            return self.exact_fma_nearest(ps);
        }
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

    /// `x = f * 2^e` for the bits `x` of a finite float: `f` in `[1, 2)` with
    /// the sign of `x` (a denormal is normalized through the exact
    /// conversion of its mantissa), or zero with `e` far below any other.
    fn dn_norm(&mut self, x: u32) -> (u32, u32) {
        let u = self.u32t();
        let i = self.i32t();
        let fl = self.f32t();
        let (k23, ff, mm, sm, one) = (
            self.dn_ku(23),
            self.dn_ku(0xff),
            self.dn_ku(0x7f_ffff),
            self.dn_ku(0x8000_0000),
            self.dn_ku(0x3f80_0000),
        );
        let sign = self.dn_ub(op::BITWISE_AND, x, sm);
        let mant = self.dn_ub(op::BITWISE_AND, x, mm);
        let sh = self.dn_ub(op::SHIFT_RIGHT_LOGICAL, x, k23);
        let field = self.dn_ub(op::BITWISE_AND, sh, ff);
        let z = self.dn_ku(0);
        let den = self.dn_bc(op::I_EQUAL, field, z);
        let sone = self.dn_ub(op::BITWISE_OR, sign, one);
        let nf = self.dn_ub(op::BITWISE_OR, sone, mant);
        let fi = self.m.emit(op::BITCAST, i, &[field]);
        let c127 = self.dn_ki(127);
        let ne = self.dn_ib(op::I_SUB, fi, c127);
        let md = self.m.emit(op::CONVERT_U_TO_F, fl, &[mant]);
        let fb = self.m.emit(op::BITCAST, u, &[md]);
        let dm = self.dn_ub(op::BITWISE_AND, fb, mm);
        let df = self.dn_ub(op::BITWISE_OR, sone, dm);
        let fsh = self.dn_ub(op::SHIFT_RIGHT_LOGICAL, fb, k23);
        let dfield = self.dn_ub(op::BITWISE_AND, fsh, ff);
        let dfi = self.m.emit(op::BITCAST, i, &[dfield]);
        let c276 = self.dn_ki(276);
        let de = self.dn_ib(op::I_SUB, dfi, c276);
        let fbits = self.select(u, den, df, nf);
        let e = self.select(i, den, de, ne);
        let zero = self.is_zero_bits(x);
        let zf = self.dn_ku(0);
        let fbits = self.select(u, zero, zf, fbits);
        let ze = self.dn_ki(ZERO_EXP);
        let e = self.select(i, zero, ze, e);
        (self.as_float(fbits), e)
    }

    /// `2^d` for an `i32` `d` from `-126` to `127`.
    fn dn_pow2(&mut self, d: u32) -> u32 {
        let u = self.u32t();
        let bias = self.dn_ki(127);
        let e = self.dn_ib(op::I_ADD, d, bias);
        let eu = self.m.emit(op::BITCAST, u, &[e]);
        let k23 = self.dn_ku(23);
        let b = self.dn_ub(op::SHIFT_LEFT_LOGICAL, eu, k23);
        self.as_float(b)
    }

    /// A round-to-nearest fused multiply-add of finite operands, one of
    /// which is denormal or whose result is tiny, with denormals kept, from
    /// a host that flushes them. The operands are normalized and scaled so
    /// that every float step is in the normal range: `r1`, the fused result
    /// of the scaled operands, is the correct 24-bit rounding of the scaled
    /// exact value; a result that stays normal is `r1` with its exponent
    /// moved back, and one that falls below the normal range is rounded to
    /// the denormal grid from `r1`'s bits, where only a `r1` exactly
    /// halfway between two grid values can have been rounded the wrong
    /// way, and the sign of the exact error of the fused result (Boldo and
    /// Muller's error of the FMA) says which way.
    fn exact_fma_nearest(&mut self, ps: &[u32]) -> u32 {
        let u = self.u32t();
        let i = self.i32t();
        let (fa, ea) = self.dn_norm(ps[0]);
        let (fb, eb) = self.dn_norm(ps[1]);
        let (fc, ec) = self.dn_norm(ps[2]);
        let ep = self.dn_ib(op::I_ADD, ea, eb);
        let r = self.dn_imax(ep, ec);
        let dp = self.dn_ib(op::I_SUB, ep, r);
        let st = self.dn_ki(STICKY);
        let dp = self.dn_imax(dp, st);
        let dc = self.dn_ib(op::I_SUB, ec, r);
        let dc = self.dn_imax(dc, st);
        let sp = self.dn_pow2(dp);
        let sc = self.dn_pow2(dc);
        let b1 = self.exact(op::F_MUL, fb, sp);
        let c1 = self.exact(op::F_MUL, fc, sc);
        let r1 = self.fma(fa, b1, c1);
        let u1 = self.exact(op::F_MUL, fa, b1);
        let nu1 = self.fneg(u1);
        let u2 = self.fma(fa, b1, nu1);
        let (al1, al2) = self.two_sum(c1, u2);
        let (be1, be2) = self.two_sum(u1, al1);
        let d = self.exact(op::F_SUB, be1, r1);
        let g = self.exact(op::F_ADD, d, be2);
        let (r2, _) = self.two_sum(g, al2);
        let (sm, mm, mag_mask) = (
            self.dn_ku(0x8000_0000),
            self.dn_ku(0x7f_ffff),
            self.dn_ku(0x7fff_ffff),
        );
        let r1b = self.as_bits(r1);
        let sign = self.dn_ub(op::BITWISE_AND, r1b, sm);
        let mag = self.dn_ub(op::BITWISE_AND, r1b, mag_mask);
        let k23 = self.dn_ku(23);
        let efield = self.dn_ub(op::SHIFT_RIGHT_LOGICAL, mag, k23);
        let ef = self.m.emit(op::BITCAST, i, &[efield]);
        let total = self.dn_ib(op::I_ADD, ef, r);
        let c1m = self.dn_ki(1);
        let sh = self.dn_ib(op::I_SUB, c1m, total);
        let ru = self.m.emit(op::BITCAST, u, &[r]);
        let radd = self.dn_ub(op::SHIFT_LEFT_LOGICAL, ru, k23);
        let normal = self.dn_ub(op::I_ADD, r1b, radd);
        let one_u = self.dn_ku(1);
        let shc = self.dn_iclamp(sh, 1, 31);
        let shu = self.m.emit(op::BITCAST, u, &[shc]);
        let hid = self.dn_ku(0x80_0000);
        let m24 = {
            let lo = self.dn_ub(op::BITWISE_AND, mag, mm);
            self.dn_ub(op::BITWISE_OR, lo, hid)
        };
        let q = self.dn_ub(op::SHIFT_RIGHT_LOGICAL, m24, shu);
        let bit = self.dn_ub(op::SHIFT_LEFT_LOGICAL, one_u, shu);
        let mask = self.dn_ub(op::I_SUB, bit, one_u);
        let dd = self.dn_ub(op::BITWISE_AND, m24, mask);
        let shm1 = self.dn_ub(op::I_SUB, shu, one_u);
        let half = self.dn_ub(op::SHIFT_LEFT_LOGICAL, one_u, shm1);
        let gt = self.dn_bc(op::U_GREATER_THAN, dd, half);
        let eq = self.dn_bc(op::I_EQUAL, dd, half);
        let zf = self.m.c_f32(0);
        let rpos = self.fcmp(op::F_ORD_GREATER_THAN, r2, zf);
        let rneg = self.fcmp(op::F_ORD_LESS_THAN, r2, zf);
        let z = self.dn_ku(0);
        let r1neg = self.dn_bc(op::I_NOT_EQUAL, sign, z);
        let bt = self.boolt();
        let toward = self.select(bt, r1neg, rneg, rpos);
        let away = self.select(bt, r1neg, rpos, rneg);
        let qb = self.dn_ub(op::BITWISE_AND, q, one_u);
        let odd = self.dn_bc(op::I_NOT_EQUAL, qb, z);
        let na = self.not(away);
        let evenup = self.and(na, odd);
        let tie_up = self.or(toward, evenup);
        let tie = self.and(eq, tie_up);
        let up = self.or(gt, tie);
        let inc = self.select(u, up, one_u, z);
        let n = self.dn_ub(op::I_ADD, q, inc);
        let dbits = self.dn_ub(op::BITWISE_OR, sign, n);
        let one_i = self.dn_ki(1);
        let grid = self.dn_bc(op::S_GREATER_THAN_EQUAL, sh, one_i);
        let res = self.select(u, grid, dbits, normal);
        let c255 = self.dn_ki(255);
        let ovf = self.dn_bc(op::S_GREATER_THAN_EQUAL, total, c255);
        let inf = self.dn_ku(0x7f80_0000);
        let sinf = self.dn_ub(op::BITWISE_OR, sign, inf);
        let res = self.select(u, ovf, sinf, res);
        let r1z = self.dn_bc(op::I_EQUAL, mag, z);
        self.select(u, r1z, z, res)
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

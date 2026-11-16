//! Double precision: DADD, DMUL, DFMA in every rounding mode, DSETP, and
//! F2F between F32 and F64.
//!
//! The arithmetic is integer code on the bit patterns, so the results do
//! not depend on the host's float64 support, denormal mode or fused
//! multiply-add. DADD and DMUL are DFMA with a factor of one and an addend
//! of `-0`, which round the same way. A NaN operand gives that NaN made
//! quiet, the second source winning over the third and the third over the
//! first; an invalid operation gives `0xfff8000000000000`. Negation and
//! absolute value leave a NaN operand as it is. Denormals are kept as the
//! hardware keeps them.

use super::{Ctx, Res, bad};
use crate::spirv::{glsl, op};
use crate::{Inst, Operand};

const CAP_INT64: u32 = 11;
const LOGICAL_EQUAL: u16 = 164;
const FUNCTION_PARAMETER: u16 = 55;
const FUNCTION_CALL: u16 = 57;
const RETURN_VALUE: u16 = 254;
const FMA_KEY: u32 = 0x5_0000;
const QUIET: u64 = 1 << 51;
const DEFAULT_NAN: u64 = 0xfff8_0000_0000_0000;
const NO_EXP: u32 = -100_000i32 as u32;

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "DADD" | "DMUL" | "DFMA" => arith(c, i),
        "DSETP" => dsetp(c, i),
        "F2F" if i.has("F64") => f2f(c, i),
        _ => return None,
    })
}

fn mode_of(i: &Inst) -> u32 {
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

/// A 64-bit source as one `u64` with its sign modifiers applied.
fn src64(c: &mut Ctx, o: &Operand) -> Res<u32> {
    let (lo, hi, neg, abs) = match o {
        Operand::F64Hi(h) => (c.m.c_u32(0), c.m.c_u32(*h), false, false),
        Operand::Reg(n, m) | Operand::UReg(n, m) => {
            if m.not || m.swz != crate::Swz::None {
                return bad(format!("F64 operand {o}"));
            }
            let plain = match o {
                Operand::Reg(..) => Operand::Reg(*n, Default::default()),
                _ => Operand::UReg(*n, Default::default()),
            };
            let (lo, hi) = c.src_pair(&plain)?;
            (lo, hi, m.neg, m.abs)
        }
        Operand::CBuf { mods, .. } => {
            let mut plain = o.clone();
            if let Operand::CBuf { mods, .. } = &mut plain {
                *mods = Default::default();
            }
            let (lo, hi) = c.src_pair(&plain)?;
            (lo, hi, mods.neg, mods.abs)
        }
        o => return bad(format!("F64 operand {o}")),
    };
    let raw = c.join64(lo, hi);
    if !abs && !neg {
        return Ok(raw);
    }
    let hm = if abs { c.and_k(hi, 0x7fff_ffff) } else { hi };
    let hm = if neg {
        c.sh(op::BITWISE_XOR, hm, 0x8000_0000)
    } else {
        hm
    };
    let v = c.join64(lo, hm);
    let nan = c.nan64(raw);
    Ok(c.qsel(nan, raw, v))
}

fn store64(c: &mut Ctx, o: &Operand, v: u32) -> Res<()> {
    let (lo, hi) = c.halves64(v);
    c.store_pair(o, lo, hi)
}

fn arith(c: &mut Ctx, i: &Inst) -> Res<()> {
    for m in &i.mods {
        if !matches!(*m, "RM" | "RP" | "RZ") {
            return bad(format!("{}.{m}", i.mnemonic));
        }
    }
    let mode = mode_of(i);
    let one = c.c64(0x3ff0_0000_0000_0000);
    let nzero = c.c64(0x8000_0000_0000_0000);
    let a = src64(c, &i.ops[1])?;
    let (a, b, cc) = match i.mnemonic {
        "DADD" => (a, one, src64(c, &i.ops[2])?),
        "DMUL" => (a, src64(c, &i.ops[2])?, nzero),
        _ => {
            let b = src64(c, &i.ops[2])?;
            (a, b, src64(c, &i.ops[3])?)
        }
    };
    let f = c.fma64_fn(mode);
    let t = c.m.t_u64();
    let r = c.m.emit(FUNCTION_CALL, t, &[f, a, b, cc]);
    store64(c, &i.ops[0], r)
}

/// DSETP: the comparison of two doubles combined with a predicate. MAX and
/// MIN say whether the first source is the result of `fmax` or `fmin`
/// (with `+0` above `-0`), and their second predicate whether both are NaN.
fn dsetp(c: &mut Ctx, i: &Inst) -> Res<()> {
    let cmp = i.mods.first().copied().unwrap_or("");
    let bop = i.mods.get(1).copied().unwrap_or("AND");
    let a = src64(c, &i.ops[2])?;
    let b = src64(c, &i.ops[3])?;
    let p = c.pred_operand(&i.ops[4])?;
    let na = c.nan64(a);
    let nb = c.nan64(b);
    let any = c.lor(na, nb);
    let lt = c.lt64(a, b);
    let gt = c.lt64(b, a);
    let ord = c.lnot(any);
    let eq = {
        let ka = c.dkey(a);
        let kb = c.dkey(b);
        let t = c.boolt();
        let e = c.m.emit(op::I_EQUAL, t, &[ka, kb]);
        c.land(e, ord)
    };
    let le = c.lor(lt, eq);
    let ge = c.lor(gt, eq);
    let (r, second) = match cmp {
        "LT" => (lt, None),
        "EQ" => (eq, None),
        "LE" => (le, None),
        "GT" => (gt, None),
        "NE" => {
            let e = c.lnot(eq);
            (c.land(e, ord), None)
        }
        "GE" => (ge, None),
        "NUM" => (ord, None),
        "NAN" => (any, None),
        "LTU" => (c.lor(lt, any), None),
        "EQU" => (c.lor(eq, any), None),
        "LEU" => (c.lor(le, any), None),
        "GTU" => (c.lor(gt, any), None),
        "NEU" => (c.lnot(eq), None),
        "GEU" => (c.lor(ge, any), None),
        "MAX" | "MIN" => {
            let first = c.minmax_first(a, b, cmp == "MAX");
            let either = c.lor(nb, first);
            let not_a = c.lnot(na);
            (c.land(not_a, either), Some(c.land(na, nb)))
        }
        m => return bad(format!("DSETP.{m}")),
    };
    let comb = |c: &mut Ctx, x: u32| match bop {
        "OR" => Ok(c.lor(x, p)),
        "XOR" => {
            let t = c.boolt();
            Ok(c.m.emit(op::LOGICAL_NOT_EQUAL, t, &[x, p]))
        }
        "AND" => Ok(c.land(x, p)),
        m => bad(format!("DSETP.{m}")),
    };
    let first = comb(c, r)?;
    let other = match second {
        Some(s) => comb(c, s)?,
        None => {
            let nr = c.lnot(r);
            comb(c, nr)?
        }
    };
    c.store_pdst(&i.ops[0], first)?;
    c.store_pdst(&i.ops[1], other)
}

fn f2f(c: &mut Ctx, i: &Inst) -> Res<()> {
    let tys: Vec<&str> = i
        .mods
        .iter()
        .copied()
        .filter(|m| matches!(*m, "F16" | "F32" | "F64"))
        .collect();
    let mode = mode_of(i);
    match tys.as_slice() {
        ["F64", "F32"] => {
            if i.has("FTZ") {
                return bad("F2F.F64.F32.FTZ");
            }
            let x = c.fsrc_bits(&i.ops[1])?;
            let r = c.widen(x);
            store64(c, &i.ops[0], r)
        }
        ["F32", "F64"] => {
            if i.has("FTZ") {
                return bad("F2F.F32.F64.FTZ");
            }
            let x = src64(c, &i.ops[1])?;
            let r = c.narrow(x, mode);
            c.store_dst(&i.ops[0], r)
        }
        t => bad(format!("F2F {t:?}")),
    }
}

impl Ctx {
    fn c64(&mut self, v: u64) -> u32 {
        self.cap(CAP_INT64);
        self.m.c_u64(v)
    }

    fn join64(&mut self, lo: u32, hi: u32) -> u32 {
        self.cap(CAP_INT64);
        let t = self.m.t_u64();
        let l = self.m.emit(op::U_CONVERT, t, &[lo]);
        let h = self.m.emit(op::U_CONVERT, t, &[hi]);
        let k = self.m.c_u32(32);
        let h = self.m.emit(op::SHIFT_LEFT_LOGICAL, t, &[h, k]);
        self.m.emit(op::BITWISE_OR, t, &[h, l])
    }

    fn halves64(&mut self, v: u32) -> (u32, u32) {
        let u = self.u32t();
        let t = self.m.t_u64();
        let lo = self.m.emit(op::U_CONVERT, u, &[v]);
        let k = self.m.c_u32(32);
        let h = self.m.emit(op::SHIFT_RIGHT_LOGICAL, t, &[v, k]);
        let hi = self.m.emit(op::U_CONVERT, u, &[h]);
        (lo, hi)
    }

    fn q(&mut self, opc: u16, a: u32, b: u32) -> u32 {
        let t = self.m.t_u64();
        self.m.emit(opc, t, &[a, b])
    }

    fn qk(&mut self, opc: u16, a: u32, k: u64) -> u32 {
        let kk = self.c64(k);
        self.q(opc, a, kk)
    }

    /// `a` shifted by the 32-bit count `n`, which must be below 64.
    fn qs(&mut self, opc: u16, a: u32, n: u32) -> u32 {
        let t = self.m.t_u64();
        self.m.emit(opc, t, &[a, n])
    }

    fn qsk(&mut self, opc: u16, a: u32, n: u32) -> u32 {
        let k = self.m.c_u32(n);
        self.qs(opc, a, k)
    }

    fn qb(&mut self, opc: u16, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(opc, t, &[a, b])
    }

    fn qbk(&mut self, opc: u16, a: u32, k: u64) -> u32 {
        let kk = self.c64(k);
        self.qb(opc, a, kk)
    }

    fn qsel(&mut self, p: u32, a: u32, b: u32) -> u32 {
        let t = self.m.t_u64();
        self.select(t, p, a, b)
    }

    fn usel(&mut self, p: u32, a: u32, b: u32) -> u32 {
        let u = self.u32t();
        self.select(u, p, a, b)
    }

    fn nan64(&mut self, x: u32) -> u32 {
        let m = self.qk(op::BITWISE_AND, x, !(1 << 63));
        self.qbk(op::U_GREATER_THAN, m, 0x7ff0_0000_0000_0000)
    }

    fn inf64(&mut self, x: u32) -> u32 {
        let m = self.qk(op::BITWISE_AND, x, !(1 << 63));
        self.qbk(op::I_EQUAL, m, 0x7ff0_0000_0000_0000)
    }

    fn zero64(&mut self, x: u32) -> u32 {
        let m = self.qk(op::BITWISE_AND, x, !(1 << 63));
        self.qbk(op::I_EQUAL, m, 0)
    }

    /// `a < b` for doubles that are not NaN, with `-0 == +0`.
    fn lt64(&mut self, a: u32, b: u32) -> u32 {
        let ka = self.dkey(a);
        let kb = self.dkey(b);
        let l = self.qb(op::S_LESS_THAN, ka, kb);
        let na = self.nan64(a);
        let nb = self.nan64(b);
        let n = self.lor(na, nb);
        let nn = self.lnot(n);
        self.land(l, nn)
    }

    /// A signed integer that orders doubles, both zeros mapping to 0.
    fn dkey(&mut self, x: u32) -> u32 {
        let mag = self.qk(op::BITWISE_AND, x, !(1 << 63));
        let t = self.m.t_u64();
        let neg = self.m.emit(op::S_NEGATE, t, &[mag]);
        let s = self.qbk(op::U_GREATER_THAN_EQUAL, x, 1 << 63);
        self.qsel(s, neg, mag)
    }

    /// Whether `fmax` (or `fmin`) of two non-NaN doubles is `a`, with
    /// `+0` above `-0`.
    fn minmax_first(&mut self, a: u32, b: u32, max: bool) -> u32 {
        let better = if max {
            self.lt64(b, a)
        } else {
            self.lt64(a, b)
        };
        let ka = self.dkey(a);
        let kb = self.dkey(b);
        let eq = self.qb(op::I_EQUAL, ka, kb);
        let r = self.lor(better, eq);
        let za = self.zero64(a);
        let zb = self.zero64(b);
        let both = self.land(za, zb);
        let sa = self.qbk(op::U_GREATER_THAN_EQUAL, a, 1 << 63);
        let zpick = if max { self.lnot(sa) } else { sa };
        let same = self.qb(op::I_EQUAL, a, b);
        let zpick = self.lor(zpick, same);
        self.lor_sel(both, zpick, r)
    }

    fn lor_sel(&mut self, p: u32, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.select(t, p, a, b)
    }

    /// F32 bits widened to F64 bits exactly.
    fn widen(&mut self, x: u32) -> u32 {
        let u = self.u32t();
        let s = self.and_k(x, 0x8000_0000);
        let e = self.sh(op::SHIFT_RIGHT_LOGICAL, x, 23);
        let e = self.and_k(e, 0xff);
        let m = self.and_k(x, 0x7f_ffff);
        let mz = self.cmp_k(op::I_EQUAL, m, 0);
        let e0 = self.cmp_k(op::I_EQUAL, e, 0);
        let lz = {
            let i = self.i32t();
            let msb = self.ext(i, glsl::FIND_U_MSB, &[m]);
            let k23 = self.m.c_u32(23);
            self.m.emit(op::I_SUB, u, &[k23, msb])
        };
        let mn = self.ib(op::SHIFT_LEFT_LOGICAL, m, lz);
        let mn = self.and_k(mn, 0x7f_ffff);
        let k1 = self.m.c_u32(1);
        let en = self.ib(op::I_SUB, k1, lz);
        let sub = {
            let nz = self.lnot(mz);
            self.land(e0, nz)
        };
        let m2 = self.usel(sub, mn, m);
        let e2 = self.usel(sub, en, e);
        let k896 = self.m.c_u32(896);
        let eb = self.ib(op::I_ADD, e2, k896);
        let k2047 = self.m.c_u32(2047);
        let spec = self.cmp_k(op::I_EQUAL, e, 0xff);
        let eb = self.usel(spec, k2047, eb);
        let zero = self.land(e0, mz);
        let z = self.m.c_u32(0);
        let eb = self.usel(zero, z, eb);
        let nan = {
            let nz = self.lnot(mz);
            self.land(spec, nz)
        };
        let m3 = {
            let qm = self.sh(op::BITWISE_OR, m2, 0x40_0000);
            self.usel(nan, qm, m2)
        };
        let hi = self.sh(op::SHIFT_LEFT_LOGICAL, eb, 20);
        let mh = self.sh(op::SHIFT_RIGHT_LOGICAL, m3, 3);
        let hi = self.ib(op::BITWISE_OR, hi, mh);
        let hi = self.ib(op::BITWISE_OR, hi, s);
        let lo = self.sh(op::SHIFT_LEFT_LOGICAL, m3, 29);
        self.join64(lo, hi)
    }

    /// F64 bits rounded to F32 bits in `mode`, keeping denormals.
    fn narrow(&mut self, x: u32, mode: u32) -> u32 {
        let u = self.u32t();
        let (_, hi) = self.halves64(x);
        let s = self.and_k(hi, 0x8000_0000);
        let neg = self.cmp_k(op::I_NOT_EQUAL, s, 0);
        let mag = self.qk(op::BITWISE_AND, x, !(1 << 63));
        let e = self.qsk(op::SHIFT_RIGHT_LOGICAL, mag, 52);
        let e = self.m.emit(op::U_CONVERT, u, &[e]);
        let f = self.qk(op::BITWISE_AND, x, (1 << 52) - 1);
        let e0 = self.cmp_k(op::I_EQUAL, e, 0);
        let im = self.qk(op::BITWISE_OR, f, 1 << 52);
        let sig = self.qsel(e0, f, im);
        let k1 = self.m.c_u32(1);
        let ee = self.usel(e0, k1, e);
        let msb = self.msb64(sig);
        let k23 = self.m.c_u32(23);
        let drop_n = self.m.emit(op::I_SUB, u, &[msb, k23]);
        let k926 = self.m.c_u32(926);
        let drop_d = self.m.emit(op::I_SUB, u, &[k926, ee]);
        let drop = self.ext(u, glsl::S_MAX, &[drop_n, drop_d]);
        let k63 = self.m.c_u32(63);
        let n = self.ext(u, glsl::U_MIN, &[drop, k63]);
        let q = self.qs(op::SHIFT_RIGHT_LOGICAL, sig, n);
        let one = self.c64(1);
        let p = self.qs(op::SHIFT_LEFT_LOGICAL, one, n);
        let low = self.q(op::I_SUB, p, one);
        let rem = self.q(op::BITWISE_AND, sig, low);
        let half = self.qsk(op::SHIFT_RIGHT_LOGICAL, p, 1);
        let inc = self.round_up(q, rem, half, neg, mode);
        let z64 = self.c64(0);
        let incv = self.qsel(inc, one, z64);
        let q = self.q(op::I_ADD, q, incv);
        let q = self.m.emit(op::U_CONVERT, u, &[q]);
        let be = {
            let t = self.m.emit(op::I_ADD, u, &[ee, drop]);
            let k = self.m.c_u32(1075 - 150);
            self.m.emit(op::I_SUB, u, &[t, k])
        };
        let field = self.m.emit(op::I_SUB, u, &[be, k1]);
        let k255 = self.m.c_u32(255);
        let field = self.ext(u, glsl::U_MIN, &[field, k255]);
        let fb = self.sh(op::SHIFT_LEFT_LOGICAL, field, 23);
        let r = self.ib(op::I_ADD, fb, q);
        let over = self.cmp_k(op::U_GREATER_THAN_EQUAL, r, 0x7f80_0000);
        let up = self.overflow_to_inf(neg, mode);
        let inf = self.m.c_u32(0x7f80_0000);
        let maxf = self.m.c_u32(0x7f7f_ffff);
        let ov = self.usel(up, inf, maxf);
        let r = self.usel(over, ov, r);
        let zero = self.zero64(x);
        let z = self.m.c_u32(0);
        let r = self.usel(zero, z, r);
        let r = self.ib(op::BITWISE_OR, r, s);
        let nan = self.nan64(x);
        let inf64 = self.inf64(x);
        let infv = self.ib(op::BITWISE_OR, inf, s);
        let r = self.usel(inf64, infv, r);
        let pay = self.qsk(op::SHIFT_RIGHT_LOGICAL, f, 29);
        let pay = self.m.emit(op::U_CONVERT, u, &[pay]);
        let qn = self.sh(op::BITWISE_OR, pay, 0x7fc0_0000);
        let qn = self.ib(op::BITWISE_OR, qn, s);
        self.usel(nan, qn, r)
    }

    /// Whether rounding in `mode` adds one to the cut value `q` with the
    /// remainder `rem` below the place value `2 * half`.
    fn round_up(&mut self, q: u32, rem: u32, half: u32, neg: u32, mode: u32) -> u32 {
        match mode {
            0 => {
                let gt = self.qb(op::U_GREATER_THAN, rem, half);
                let eq = self.qb(op::I_EQUAL, rem, half);
                let odd = self.qk(op::BITWISE_AND, q, 1);
                let oddb = self.qbk(op::I_NOT_EQUAL, odd, 0);
                let tie = self.land(eq, oddb);
                self.lor(gt, tie)
            }
            1 | 2 => {
                let lost = self.qbk(op::I_NOT_EQUAL, rem, 0);
                let side = if mode == 1 { neg } else { self.lnot(neg) };
                self.land(lost, side)
            }
            _ => self.m.c_bool(false),
        }
    }

    /// Whether an overflow in `mode` for a result of sign `neg` gives
    /// infinity rather than the largest finite value.
    fn overflow_to_inf(&mut self, neg: u32, mode: u32) -> u32 {
        match mode {
            0 => self.m.c_bool(true),
            1 => neg,
            2 => self.lnot(neg),
            _ => self.m.c_bool(false),
        }
    }

    /// The index of the highest set bit of a nonzero `u64`.
    fn msb64(&mut self, x: u32) -> u32 {
        let u = self.u32t();
        let i = self.i32t();
        let (lo, hi) = self.halves64(x);
        let mh = self.ext(i, glsl::FIND_U_MSB, &[hi]);
        let ml = self.ext(i, glsl::FIND_U_MSB, &[lo]);
        let k32 = self.m.c_u32(32);
        let mh = self.m.emit(op::BITCAST, u, &[mh]);
        let ml = self.m.emit(op::BITCAST, u, &[ml]);
        let mh = self.m.emit(op::I_ADD, u, &[mh, k32]);
        let hz = self.cmp_k(op::I_EQUAL, hi, 0);
        self.usel(hz, ml, mh)
    }

    fn fma64_fn(&mut self, mode: u32) -> u32 {
        let key = FMA_KEY + mode;
        if let Some(&f) = self.builtins.get(&key) {
            return f;
        }
        self.cap(CAP_INT64);
        let saved = std::mem::take(&mut self.m.body);
        let t = self.m.t_u64();
        let fnt = self.m.t_fn_args(t, &[t, t, t]);
        let id = self.m.id();
        self.m.emit_void(op::FUNCTION, &[t, id, 0, fnt]);
        let ps: Vec<u32> = (0..3)
            .map(|_| self.m.emit(FUNCTION_PARAMETER, t, &[]))
            .collect();
        let entry = self.m.id();
        self.m.emit_void(op::LABEL, &[entry]);
        let r = self.fma64_body(ps[0], ps[1], ps[2], mode);
        self.m.emit_void(RETURN_VALUE, &[r]);
        self.m.emit_void(op::FUNCTION_END, &[]);
        let body = std::mem::replace(&mut self.m.body, saved);
        self.funcs.extend(body);
        self.builtins.insert(key, id);
        id
    }

    /// Sign (bool), unbiased exponent of the significand's last bit (i32 in
    /// a u32, `NO_EXP` for zero) and 53-bit significand of a finite double.
    fn parts64(&mut self, x: u32) -> (u32, u32, u32) {
        let u = self.u32t();
        let s = self.qbk(op::U_GREATER_THAN_EQUAL, x, 1 << 63);
        let mag = self.qk(op::BITWISE_AND, x, !(1 << 63));
        let e = self.qsk(op::SHIFT_RIGHT_LOGICAL, mag, 52);
        let e = self.m.emit(op::U_CONVERT, u, &[e]);
        let f = self.qk(op::BITWISE_AND, x, (1 << 52) - 1);
        let e0 = self.cmp_k(op::I_EQUAL, e, 0);
        let im = self.qk(op::BITWISE_OR, f, 1 << 52);
        let sig = self.qsel(e0, f, im);
        let k1 = self.m.c_u32(1);
        let ee = self.usel(e0, k1, e);
        let k = self.m.c_u32(1075);
        let ex = self.m.emit(op::I_SUB, u, &[ee, k]);
        let z = self.zero64(x);
        let none = self.m.c_u32(NO_EXP);
        (s, self.usel(z, none, ex), sig)
    }

    fn fma64_body(&mut self, a: u32, b: u32, c: u32, mode: u32) -> u32 {
        let u = self.u32t();
        let (sa, ea, ma) = self.parts64(a);
        let (sb, eb, mb) = self.parts64(b);
        let (sc, ec, mc) = self.parts64(c);
        let sp = {
            let t = self.boolt();
            self.m.emit(op::LOGICAL_NOT_EQUAL, t, &[sa, sb])
        };
        let za = self.zero64(a);
        let zb = self.zero64(b);
        let pz = self.lor(za, zb);
        let ep = self.m.emit(op::I_ADD, u, &[ea, eb]);
        let none = self.m.c_u32(NO_EXP);
        let ep = self.usel(pz, none, ep);
        let (ph, pl) = self.mul53(ma, mb);
        let z64 = self.c64(0);
        let ph = self.qsel(pz, z64, ph);
        let pl = self.qsel(pz, z64, pl);
        let (ph, pl, ep) = self.norm128(ph, pl, ep);
        let (ch, cl, ec) = self.norm128(z64, mc, ec);
        let pbig = {
            let gt = self.qb(op::S_GREATER_THAN, ep, ec);
            let eq = self.qb(op::I_EQUAL, ep, ec);
            let hgt = self.qb(op::U_GREATER_THAN, ph, ch);
            let heq = self.qb(op::I_EQUAL, ph, ch);
            let lge = self.qb(op::U_GREATER_THAN_EQUAL, pl, cl);
            let tie = self.land(heq, lge);
            let mge = self.lor(hgt, tie);
            let e_mge = self.land(eq, mge);
            self.lor(gt, e_mge)
        };
        let xh = self.qsel(pbig, ph, ch);
        let xl = self.qsel(pbig, pl, cl);
        let yh = self.qsel(pbig, ch, ph);
        let yl = self.qsel(pbig, cl, pl);
        let ex = self.usel(pbig, ep, ec);
        let ey = self.usel(pbig, ec, ep);
        let sx = self.lor_sel(pbig, sp, sc);
        let d = self.m.emit(op::I_SUB, u, &[ex, ey]);
        let (yh, yl) = self.shr_jam128(yh, yl, d);
        let same = {
            let t = self.boolt();
            self.m.emit(LOGICAL_EQUAL, t, &[sp, sc])
        };
        let (ah, al) = self.add128(xh, xl, yh, yl);
        let (dh, dl) = self.sub128(xh, xl, yh, yl);
        let sh = self.qsel(same, ah, dh);
        let sl = self.qsel(same, al, dl);
        let sum_zero = {
            let o = self.q(op::BITWISE_OR, sh, sl);
            self.qbk(op::I_EQUAL, o, 0)
        };
        let r = self.round128(sh, sl, ex, sx, mode);
        let sbit = self.c64(1 << 63);
        let rs = self.q(op::BITWISE_OR, r, sbit);
        let r = self.qsel(sx, rs, r);
        let zc = self.zero64(c);
        let both_zero = self.land(pz, zc);
        let zsign = if mode == 1 {
            self.lor(sp, sc)
        } else {
            self.land(sp, sc)
        };
        let exact_sign = self.m.c_bool(mode == 1);
        let zs = self.lor_sel(both_zero, zsign, exact_sign);
        let zr = self.qsel(zs, sbit, z64);
        let r = self.qsel(sum_zero, zr, r);
        let na = self.nan64(a);
        let nb = self.nan64(b);
        let nc = self.nan64(c);
        let ia = self.inf64(a);
        let ib = self.inf64(b);
        let ic = self.inf64(c);
        let pinf = self.lor(ia, ib);
        let iz1 = self.land(ia, zb);
        let iz2 = self.land(za, ib);
        let invalid_p = self.lor(iz1, iz2);
        let opp = {
            let t = self.boolt();
            self.m.emit(op::LOGICAL_NOT_EQUAL, t, &[sp, sc])
        };
        let pic = self.land(pinf, ic);
        let invalid_s = self.land(pic, opp);
        let invalid = self.lor(invalid_p, invalid_s);
        let inf_bits = self.c64(0x7ff0_0000_0000_0000);
        let ninf = self.c64(0xfff0_0000_0000_0000);
        let pinf_v = self.qsel(sp, ninf, inf_bits);
        let r = self.qsel(ic, c, r);
        let r = self.qsel(pinf, pinf_v, r);
        let dn = self.c64(DEFAULT_NAN);
        let r = self.qsel(invalid, dn, r);
        let quiet = |cx: &mut Ctx, v: u32| cx.qk(op::BITWISE_OR, v, QUIET);
        let qa = quiet(self, a);
        let qb = quiet(self, b);
        let qc = quiet(self, c);
        let r = self.qsel(na, qa, r);
        let r = self.qsel(nc, qc, r);
        self.qsel(nb, qb, r)
    }

    /// The 106-bit product of two 53-bit significands as `(hi, lo)`.
    fn mul53(&mut self, a: u32, b: u32) -> (u32, u32) {
        let m32 = 0xffff_ffffu64;
        let al = self.qk(op::BITWISE_AND, a, m32);
        let ah = self.qsk(op::SHIFT_RIGHT_LOGICAL, a, 32);
        let bl = self.qk(op::BITWISE_AND, b, m32);
        let bh = self.qsk(op::SHIFT_RIGHT_LOGICAL, b, 32);
        let ll = self.q(op::I_MUL, al, bl);
        let lh = self.q(op::I_MUL, al, bh);
        let hl = self.q(op::I_MUL, ah, bl);
        let hh = self.q(op::I_MUL, ah, bh);
        let mid = self.q(op::I_ADD, lh, hl);
        let mid_lo = self.qsk(op::SHIFT_LEFT_LOGICAL, mid, 32);
        let lo = self.q(op::I_ADD, ll, mid_lo);
        let carry = {
            let c = self.qb(op::U_LESS_THAN, lo, ll);
            let one = self.c64(1);
            let z = self.c64(0);
            self.qsel(c, one, z)
        };
        let mid_hi = self.qsk(op::SHIFT_RIGHT_LOGICAL, mid, 32);
        let hi = self.q(op::I_ADD, hh, mid_hi);
        let hi = self.q(op::I_ADD, hi, carry);
        (hi, lo)
    }

    /// Shifts a 128-bit value so that its top bit is bit 125, adjusting the
    /// exponent of its last bit; zero is left alone.
    fn norm128(&mut self, h: u32, l: u32, e: u32) -> (u32, u32, u32) {
        let u = self.u32t();
        let hz = self.qbk(op::I_EQUAL, h, 0);
        let mh = self.msb64(h);
        let ml = self.msb64(l);
        let k64 = self.m.c_u32(64);
        let mh64 = self.m.emit(op::I_ADD, u, &[mh, k64]);
        let msb = self.usel(hz, ml, mh64);
        let k125 = self.m.c_u32(125);
        let s = self.m.emit(op::I_SUB, u, &[k125, msb]);
        let o = self.q(op::BITWISE_OR, h, l);
        let zero = self.qbk(op::I_EQUAL, o, 0);
        let z = self.m.c_u32(0);
        let s = self.usel(zero, z, s);
        let (nh, nl) = self.shl128(h, l, s);
        let ne = self.m.emit(op::I_SUB, u, &[e, s]);
        (nh, nl, ne)
    }

    /// `(h, l) << n` for `n` from 0 to 128.
    fn shl128(&mut self, h: u32, l: u32, n: u32) -> (u32, u32) {
        let u = self.u32t();
        let k63 = self.m.c_u32(63);
        let k64 = self.m.c_u32(64);
        let z = self.m.c_u32(0);
        let z64 = self.c64(0);
        let big = self.qb(op::U_GREATER_THAN_EQUAL, n, k64);
        let n1 = self.m.emit(op::BITWISE_AND, u, &[n, k63]);
        let inv = self.m.emit(op::I_SUB, u, &[k64, n1]);
        let inv = self.ext(u, glsl::U_MIN, &[inv, k63]);
        let nz = self.qb(op::I_EQUAL, n1, z);
        let carry = self.qs(op::SHIFT_RIGHT_LOGICAL, l, inv);
        let carry = self.qsel(nz, z64, carry);
        let hs = self.qs(op::SHIFT_LEFT_LOGICAL, h, n1);
        let hs = self.q(op::BITWISE_OR, hs, carry);
        let ls = self.qs(op::SHIFT_LEFT_LOGICAL, l, n1);
        let rh = self.qsel(big, ls, hs);
        let rl = self.qsel(big, z64, ls);
        let k128 = self.m.c_u32(128);
        let all = self.qb(op::U_GREATER_THAN_EQUAL, n, k128);
        (self.qsel(all, z64, rh), rl)
    }

    /// `(h, l) >> n` for any `n` (a negative `n` meaning a huge shift), the
    /// bits shifted out ORed into bit 0.
    fn shr_jam128(&mut self, h: u32, l: u32, n: u32) -> (u32, u32) {
        let u = self.u32t();
        let k63 = self.m.c_u32(63);
        let k64 = self.m.c_u32(64);
        let k128 = self.m.c_u32(128);
        let z = self.m.c_u32(0);
        let z64 = self.c64(0);
        let huge = self.qb(op::U_GREATER_THAN_EQUAL, n, k128);
        let big = self.qb(op::U_GREATER_THAN_EQUAL, n, k64);
        let n1 = self.m.emit(op::BITWISE_AND, u, &[n, k63]);
        let inv = self.m.emit(op::I_SUB, u, &[k64, n1]);
        let inv = self.ext(u, glsl::U_MIN, &[inv, k63]);
        let nz = self.qb(op::I_EQUAL, n1, z);
        let carry = self.qs(op::SHIFT_LEFT_LOGICAL, h, inv);
        let carry = self.qsel(nz, z64, carry);
        let ls = self.qs(op::SHIFT_RIGHT_LOGICAL, l, n1);
        let ls = self.q(op::BITWISE_OR, ls, carry);
        let hs = self.qs(op::SHIFT_RIGHT_LOGICAL, h, n1);
        let lost_small = self.qs(op::SHIFT_LEFT_LOGICAL, l, inv);
        let lost_small = self.qsel(nz, z64, lost_small);
        let lost_big = {
            let m = self.qs(op::SHIFT_LEFT_LOGICAL, h, inv);
            let m = self.qsel(nz, z64, m);
            self.q(op::BITWISE_OR, m, l)
        };
        let rh = self.qsel(big, z64, hs);
        let rl = self.qsel(big, hs, ls);
        let lost = self.qsel(big, lost_big, lost_small);
        let all = self.q(op::BITWISE_OR, h, l);
        let rh = self.qsel(huge, z64, rh);
        let rl = self.qsel(huge, z64, rl);
        let lost = self.qsel(huge, all, lost);
        let sticky = self.qbk(op::I_NOT_EQUAL, lost, 0);
        let one = self.c64(1);
        let sb = self.qsel(sticky, one, z64);
        (rh, self.q(op::BITWISE_OR, rl, sb))
    }

    fn add128(&mut self, ah: u32, al: u32, bh: u32, bl: u32) -> (u32, u32) {
        let l = self.q(op::I_ADD, al, bl);
        let c = self.qb(op::U_LESS_THAN, l, al);
        let one = self.c64(1);
        let z = self.c64(0);
        let cy = self.qsel(c, one, z);
        let h = self.q(op::I_ADD, ah, bh);
        (self.q(op::I_ADD, h, cy), l)
    }

    fn sub128(&mut self, ah: u32, al: u32, bh: u32, bl: u32) -> (u32, u32) {
        let l = self.q(op::I_SUB, al, bl);
        let c = self.qb(op::U_LESS_THAN, al, bl);
        let one = self.c64(1);
        let z = self.c64(0);
        let bw = self.qsel(c, one, z);
        let h = self.q(op::I_SUB, ah, bh);
        (self.q(op::I_SUB, h, bw), l)
    }

    /// Rounds a nonzero 128-bit magnitude times `2^e` to F64 magnitude bits.
    fn round128(&mut self, h: u32, l: u32, e: u32, neg: u32, mode: u32) -> u32 {
        let u = self.u32t();
        let hz = self.qbk(op::I_EQUAL, h, 0);
        let mh = self.msb64(h);
        let ml = self.msb64(l);
        let k64 = self.m.c_u32(64);
        let mh64 = self.m.emit(op::I_ADD, u, &[mh, k64]);
        let msb = self.usel(hz, ml, mh64);
        let k52 = self.m.c_u32(52);
        let drop_n = self.m.emit(op::I_SUB, u, &[msb, k52]);
        let m1074 = self.m.c_i32(-1074);
        let drop_d = self.m.emit(op::I_SUB, u, &[m1074, e]);
        let drop = self.ext(u, glsl::S_MAX, &[drop_n, drop_d]);
        let z = self.m.c_u32(0);
        let pos = self.qb(op::S_GREATER_THAN, drop, z);
        let k128 = self.m.c_u32(128);
        let n = self.ext(u, glsl::U_MIN, &[drop, k128]);
        let n = self.usel(pos, n, z);
        let (_, ql) = self.shr128(h, l, n);
        let (mh, ml) = self.mask128(n);
        let rh = self.q(op::BITWISE_AND, h, mh);
        let rl = self.q(op::BITWISE_AND, l, ml);
        let (hh, hl) = self.half128(n);
        let gt = {
            let gt_h = self.qb(op::U_GREATER_THAN, rh, hh);
            let eq_h = self.qb(op::I_EQUAL, rh, hh);
            let gt_l = self.qb(op::U_GREATER_THAN, rl, hl);
            let lo = self.land(eq_h, gt_l);
            self.lor(gt_h, lo)
        };
        let eq = {
            let eq_h = self.qb(op::I_EQUAL, rh, hh);
            let eq_l = self.qb(op::I_EQUAL, rl, hl);
            self.land(eq_h, eq_l)
        };
        let lost = {
            let o = self.q(op::BITWISE_OR, rh, rl);
            self.qbk(op::I_NOT_EQUAL, o, 0)
        };
        let inc = match mode {
            0 => {
                let odd = self.qk(op::BITWISE_AND, ql, 1);
                let oddb = self.qbk(op::I_NOT_EQUAL, odd, 0);
                let tie = self.land(eq, oddb);
                self.lor(gt, tie)
            }
            1 => self.land(lost, neg),
            2 => {
                let p = self.lnot(neg);
                self.land(lost, p)
            }
            _ => self.m.c_bool(false),
        };
        let one = self.c64(1);
        let z64 = self.c64(0);
        let incv = self.qsel(inc, one, z64);
        let left = {
            let nd = self.m.emit(op::S_NEGATE, u, &[drop]);
            let k63 = self.m.c_u32(63);
            let nd = self.ext(u, glsl::U_MIN, &[nd, k63]);
            let nd = self.usel(pos, z, nd);
            self.qs(op::SHIFT_LEFT_LOGICAL, l, nd)
        };
        let q = self.qsel(pos, ql, left);
        let q = self.q(op::I_ADD, q, incv);
        let field = {
            let k = self.m.c_u32(1074);
            let t = self.m.emit(op::I_ADD, u, &[e, drop]);
            self.m.emit(op::I_ADD, u, &[t, k])
        };
        let k2047 = self.m.c_u32(2047);
        let big = self.qb(op::S_GREATER_THAN_EQUAL, field, k2047);
        let field = self.ext(u, glsl::U_MIN, &[field, k2047]);
        let t = self.m.t_u64();
        let fv = self.m.emit(op::U_CONVERT, t, &[field]);
        let fv = self.qsk(op::SHIFT_LEFT_LOGICAL, fv, 52);
        let r = self.q(op::I_ADD, fv, q);
        let over = self.qbk(op::U_GREATER_THAN_EQUAL, r, 0x7ff0_0000_0000_0000);
        let over = self.lor(over, big);
        let up = self.overflow_to_inf(neg, mode);
        let inf = self.c64(0x7ff0_0000_0000_0000);
        let maxf = self.c64(0x7fef_ffff_ffff_ffff);
        let ov = self.qsel(up, inf, maxf);
        self.qsel(over, ov, r)
    }

    /// `(h, l) >> n` for `n` from 0 to 128.
    fn shr128(&mut self, h: u32, l: u32, n: u32) -> (u32, u32) {
        let u = self.u32t();
        let k63 = self.m.c_u32(63);
        let k64 = self.m.c_u32(64);
        let z = self.m.c_u32(0);
        let z64 = self.c64(0);
        let big = self.qb(op::U_GREATER_THAN_EQUAL, n, k64);
        let n1 = self.m.emit(op::BITWISE_AND, u, &[n, k63]);
        let inv = self.m.emit(op::I_SUB, u, &[k64, n1]);
        let inv = self.ext(u, glsl::U_MIN, &[inv, k63]);
        let nz = self.qb(op::I_EQUAL, n1, z);
        let carry = self.qs(op::SHIFT_LEFT_LOGICAL, h, inv);
        let carry = self.qsel(nz, z64, carry);
        let ls = self.qs(op::SHIFT_RIGHT_LOGICAL, l, n1);
        let ls = self.q(op::BITWISE_OR, ls, carry);
        let hs = self.qs(op::SHIFT_RIGHT_LOGICAL, h, n1);
        let k128 = self.m.c_u32(128);
        let all = self.qb(op::U_GREATER_THAN_EQUAL, n, k128);
        let rl = self.qsel(big, hs, ls);
        (self.qsel(big, z64, hs), self.qsel(all, z64, rl))
    }

    /// `2^n - 1` as `(hi, lo)`, `n` from 0 to 128.
    fn mask128(&mut self, n: u32) -> (u32, u32) {
        let one = self.c64(1);
        let z = self.c64(0);
        let (ph, pl) = self.shl128(z, one, n);
        self.sub128(ph, pl, z, one)
    }

    /// `2^(n-1)` as `(hi, lo)` (0 for `n` = 0), `n` from 0 to 128.
    fn half128(&mut self, n: u32) -> (u32, u32) {
        let u = self.u32t();
        let one = self.c64(1);
        let z = self.c64(0);
        let k1 = self.m.c_u32(1);
        let nz = {
            let zz = self.m.c_u32(0);
            self.qb(op::I_EQUAL, n, zz)
        };
        let nm1 = self.m.emit(op::I_SUB, u, &[n, k1]);
        let zz = self.m.c_u32(0);
        let nm1 = self.usel(nz, zz, nm1);
        let (h, l) = self.shl128(z, one, nm1);
        (self.qsel(nz, z, h), self.qsel(nz, z, l))
    }
}

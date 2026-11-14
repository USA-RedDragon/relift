//! What the generated templates call: the guest state as the instruction
//! sees it while it runs, branch exits, and the integer helpers their static
//! code uses. These are the hand-written side of the cut line.

use relift_ir::{Builder, CmpOp, Exit, Flow, FpOp, FpRound, Next, Reg, Value, Width};

use crate::regs;

/// Where a branch goes.
#[derive(Clone, Copy, Debug)]
pub enum Target {
    /// A known address.
    Const(u64),
    /// A computed address.
    Dynamic(Value),
}

/// The kind of a branch.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// A plain jump.
    Jump,
    /// A call returning to the next instruction.
    Call,
    /// A return.
    Return,
}

/// The guest state of one instruction while its template runs.
///
/// IR reads see the block's entry state and IR writes land at its end, but
/// Sail semantics read their own writes; `St` keeps the current value of
/// each slot the template wrote and emits the writes in [`St::finish`].
#[derive(Debug, Default)]
pub struct St {
    cur: Vec<Option<Value>>,
    exit: Option<(Next, Kind)>,
}

impl St {
    /// A state that has written nothing.
    pub fn new() -> St {
        St::default()
    }

    /// The current value of `r`.
    pub fn get(&mut self, b: &mut Builder, r: Reg) -> Value {
        if let Some(Some(v)) = self.cur.get(r.index()) {
            return *v;
        }
        b.get(r)
    }

    /// Writes `v` to `r`, or, under the condition `g`, keeps the old value
    /// when `g` is 0.
    pub fn set(&mut self, b: &mut Builder, r: Reg, v: Value, g: Option<Value>) {
        let v = match g {
            Some(g) => {
                let old = self.get(b, r);
                b.select(g, v, old)
            }
            None => v,
        };
        if self.cur.len() <= r.index() {
            self.cur.resize(r.index() + 1, None);
        }
        self.cur[r.index()] = Some(v);
    }

    /// `X[n]` truncated to `w` bits; 31 reads zero.
    pub fn x_read(&mut self, b: &mut Builder, n: u32, w: u32) -> Value {
        let w = Width::new(w);
        if n >= 31 {
            return b.konst(w, 0);
        }
        let v = self.get(b, regs::x(n));
        b.trunc(v, w)
    }

    /// Writes `X[n]` zero-extended; 31 discards the value.
    pub fn x_set(&mut self, b: &mut Builder, n: u32, v: Value, g: Option<Value>) {
        if n >= 31 {
            return;
        }
        let v = b.zext(v, Width::W64);
        self.set(b, regs::x(n), v, g);
    }

    /// `V[n]` truncated to `w` bits.
    pub fn v_read(&mut self, b: &mut Builder, n: u32, w: u32) -> Value {
        let v = self.get(b, regs::vreg(n));
        if w == 128 {
            return v;
        }
        b.trunc(v, Width::new(w))
    }

    /// Writes `V[n]` zero-extended to 128 bits.
    pub fn v_set(&mut self, b: &mut Builder, n: u32, v: Value, g: Option<Value>) {
        let v = if b.width(v) == Width::W128 {
            v
        } else {
            b.zext(v, Width::W128)
        };
        self.set(b, regs::vreg(n), v, g);
    }

    /// `SP`.
    pub fn sp_read(&mut self, b: &mut Builder) -> Value {
        self.get(b, regs::SP)
    }

    /// Writes `SP`.
    pub fn sp_set(&mut self, b: &mut Builder, v: Value, g: Option<Value>) {
        self.set(b, regs::SP, v, g);
    }

    /// Records a branch to `t`, taken when `g` is 1 (always without `g`).
    pub fn branch(&mut self, b: &mut Builder, t: Target, k: Kind, g: Option<Value>) {
        let fall = b.pc().wrapping_add(4);
        let next = match (t, g) {
            (Target::Const(c), None) => Next::Const(c),
            (Target::Dynamic(v), None) => Next::Dynamic(v),
            (Target::Const(c), Some(g)) => Next::Select {
                cond: g,
                taken: c,
                not_taken: fall,
            },
            (Target::Dynamic(v), Some(g)) => {
                let f = b.konst(Width::W64, u128::from(fall));
                Next::Dynamic(b.select(g, v, f))
            }
        };
        self.exit = Some((next, k));
    }

    /// Emits the writes and gives the instruction's exit.
    pub fn finish(self, b: &mut Builder) -> Exit {
        for (i, v) in self.cur.iter().enumerate() {
            if let Some(v) = v {
                b.set(Reg(i as u16), *v);
            }
        }
        let fall = b.pc().wrapping_add(4);
        match self.exit {
            None => Exit {
                next: Next::Const(fall),
                flow: Flow::Jump,
            },
            Some((next, k)) => Exit {
                next,
                flow: match k {
                    Kind::Jump => Flow::Jump,
                    Kind::Call => Flow::Call { ret: fall },
                    Kind::Return => Flow::Return,
                },
            },
        }
    }
}

/// The two's complement value of the low `w` bits of `x`.
pub fn sint(x: u128, w: u32) -> i128 {
    if w == 0 || w >= 128 {
        return x as i128;
    }
    let s = 128 - w;
    ((x << s) as i128) >> s
}

/// The low `w` bits of `x` sign-extended to `n` bits.
pub fn sext(x: u128, w: u32, n: u32) -> u128 {
    (sint(x, w) as u128) & mask(n)
}

/// The all-ones value of `w` bits.
pub fn mask(w: u32) -> u128 {
    if w >= 128 {
        u128::MAX
    } else {
        (1u128 << w) - 1
    }
}

fn amount(n: i128) -> u32 {
    n.clamp(0, 255) as u32
}

/// `x >> n`, 0 once `n` reaches 128.
pub fn lshr(x: u128, n: i128) -> u128 {
    x.checked_shr(amount(n)).unwrap_or(0)
}

/// `x << n` within `w` bits.
pub fn shl(x: u128, n: i128, w: u32) -> u128 {
    x.checked_shl(amount(n)).unwrap_or(0) & mask(w)
}

/// Arithmetic `x >> n` within `w` bits.
pub fn ashr(x: u128, n: i128, w: u32) -> u128 {
    let s = sint(x, w);
    (s >> amount(n).min(127)) as u128 & mask(w)
}

/// Count of leading zeros of the low `w` bits of `x`.
pub fn clz(x: u128, w: u32) -> i128 {
    i128::from(w) - i128::from(128 - (x & mask(w)).leading_zeros())
}

/// Truncating division; 0 for division by zero.
pub fn tdiv(a: i128, b: i128) -> i128 {
    if b == 0 { 0 } else { a.wrapping_div(b) }
}

/// Remainder of truncating division; `a` for division by zero.
pub fn tmod(a: i128, b: i128) -> i128 {
    if b == 0 { a } else { a.wrapping_rem(b) }
}

/// Floor (Euclidean) division; 0 for division by zero.
pub fn fdiv(a: i128, b: i128) -> i128 {
    if b == 0 { 0 } else { a.div_euclid(b) }
}

/// Euclidean remainder; `a` for division by zero.
pub fn fmod(a: i128, b: i128) -> i128 {
    if b == 0 { a } else { a.rem_euclid(b) }
}

/// `2^n`, 0 outside 0 to 126.
pub fn pow2(n: i128) -> i128 {
    if (0..127).contains(&n) { 1i128 << n } else { 0 }
}

/// `a << n` on integers.
pub fn shl_int(a: i128, n: i128) -> i128 {
    a.checked_shl(amount(n)).unwrap_or(0)
}

/// `a >> n` on integers, rounding toward minus infinity.
pub fn shr_int(a: i128, n: i128) -> i128 {
    a >> amount(n).min(127)
}

/// `x << n` on Sail integers held in 128 bits: exact when the result fits,
/// else the sign-saturated value with its low 64 bits clear (what the exact
/// product truncates to in 64 bits, and beyond any saturation bound).
pub fn shl_int_ir(b: &mut Builder, x: Value, n: Value) -> Value {
    let w = Width::W128;
    let exact = b.bin(relift_ir::BinOp::Shl, x, n);
    let back = b.bin(relift_ir::BinOp::AShr, exact, n);
    let k128 = b.konst(w, 128);
    let in_range = b.cmp(relift_ir::CmpOp::Ult, n, k128);
    let same = b.cmp(relift_ir::CmpOp::Eq, back, x);
    let small = b.and(in_range, same);
    let zero = b.konst(w, 0);
    let is_zero = b.cmp(relift_ir::CmpOp::Eq, x, zero);
    let neg = b.cmp(relift_ir::CmpOp::Slt, x, zero);
    let hi_pos = b.konst(w, (u128::MAX >> 1) & !u128::from(u64::MAX));
    let hi_neg = b.konst(w, 1u128 << 127);
    let sat = b.select(neg, hi_neg, hi_pos);
    let big = b.select(is_zero, zero, sat);
    b.select(small, exact, big)
}

/// `DecodeBitMasks` for `m`-bit masks: `(wmask, tmask, valid)`; `valid` is
/// false where the Arm pseudocode is UNDEFINED.
pub fn decode_bit_masks(
    immn: u128,
    imms: u128,
    immr: u128,
    immediate: bool,
    m: u32,
) -> (u128, u128, bool) {
    let v = ((immn & 1) << 6) | (!imms & 0x3f);
    if v == 0 {
        return (0, 0, false);
    }
    let len = 127 - v.leading_zeros();
    if len < 1 || (1u32 << len) > m {
        return (0, 0, false);
    }
    let levels = mask(len);
    if immediate && imms & levels == levels {
        return (0, 0, false);
    }
    let s = (imms & levels) as u32;
    let r = (immr & levels) as u32;
    let esize = 1u32 << len;
    let d = (s.wrapping_sub(r)) & (levels as u32);
    let welem = mask(s + 1);
    let telem = mask(d + 1);
    let rot = if r == 0 {
        welem
    } else {
        ((welem >> r) | (welem << (esize - r))) & mask(esize)
    };
    let (mut w, mut t) = (0u128, 0u128);
    let mut i = 0;
    while i < m {
        w |= rot << i;
        t |= telem << i;
        i += esize;
    }
    (w & mask(m), t & mask(m), true)
}

/// The floating-point cut: arithmetic, comparisons and min/max on 32- and
/// 64-bit values with FPCR zero (round to nearest, no flush, NaNs
/// propagated), as Arm's `FPProcessNaNs` and `FPDefaultNaN` describe.
/// Exception flags (FPSR) are not produced.
pub struct Fp {
    w: Width,
    sign: u128,
    inf: u128,
    quiet: u128,
}

impl Fp {
    /// The format of `w` bits (32 or 64).
    pub fn new(w: u32) -> Fp {
        let (e, f) = if w == 32 { (8, 23) } else { (11, 52) };
        Fp {
            w: Width::new(w),
            sign: 1u128 << (w - 1),
            inf: mask(e) << f,
            quiet: 1u128 << (f - 1),
        }
    }

    fn k(&self, b: &mut Builder, v: u128) -> Value {
        b.konst(self.w, v)
    }

    fn magnitude(&self, b: &mut Builder, x: Value) -> Value {
        let m = self.k(b, !self.sign & mask(self.w.bits()));
        b.and(x, m)
    }

    /// Whether `x` is a NaN.
    pub fn is_nan(&self, b: &mut Builder, x: Value) -> Value {
        let m = self.magnitude(b, x);
        let inf = self.k(b, self.inf);
        b.cmp(CmpOp::Ult, inf, m)
    }

    fn is_snan(&self, b: &mut Builder, x: Value) -> Value {
        let n = self.is_nan(b, x);
        let q = self.k(b, self.quiet);
        let qb = b.and(x, q);
        let z = self.k(b, 0);
        let s = b.cmp(CmpOp::Eq, qb, z);
        b.and(n, s)
    }

    fn is_inf(&self, b: &mut Builder, x: Value) -> Value {
        let m = self.magnitude(b, x);
        let inf = self.k(b, self.inf);
        b.cmp(CmpOp::Eq, m, inf)
    }

    fn is_zero(&self, b: &mut Builder, x: Value) -> Value {
        let m = self.magnitude(b, x);
        let z = self.k(b, 0);
        b.cmp(CmpOp::Eq, m, z)
    }

    fn quieted(&self, b: &mut Builder, x: Value) -> Value {
        let q = self.k(b, self.quiet);
        b.or(x, q)
    }

    fn default_nan(&self, b: &mut Builder) -> Value {
        self.k(b, self.inf | self.quiet)
    }

    /// `r` with any NaN replaced by the default NaN.
    fn canonical(&self, b: &mut Builder, r: Value) -> Value {
        let n = self.is_nan(b, r);
        let d = self.default_nan(b);
        b.select(n, d, r)
    }

    /// `FPProcessNaNs` over `ops` in order, else `r`.
    fn nans(&self, b: &mut Builder, ops: &[Value], r: Value) -> Value {
        let mut out = r;
        for x in ops.iter().rev() {
            let n = self.is_nan(b, *x);
            out = b.select(n, *x, out);
        }
        for x in ops.iter().rev() {
            let s = self.is_snan(b, *x);
            let q = self.quieted(b, *x);
            out = b.select(s, q, out);
        }
        out
    }

    /// A two-operand IEEE operation with Arm NaN handling.
    pub fn bin(&self, b: &mut Builder, op: FpOp, x: Value, y: Value) -> Value {
        let r = b.fp(op, &[x, y], self.w);
        let r = self.canonical(b, r);
        self.nans(b, &[x, y], r)
    }

    /// The square root with Arm NaN handling.
    pub fn sqrt(&self, b: &mut Builder, x: Value) -> Value {
        let r = b.fp(FpOp::Sqrt, &[x], self.w);
        let r = self.canonical(b, r);
        self.nans(b, &[x], r)
    }

    /// `FPMulAdd(addend, x, y)`: `addend + x * y` with one rounding.
    pub fn mul_add(&self, b: &mut Builder, addend: Value, x: Value, y: Value) -> Value {
        let r = b.fp(FpOp::MulAdd, &[x, y, addend], self.w);
        let r = self.canonical(b, r);
        let r = self.nans(b, &[addend, x, y], r);
        let an = self.is_nan(b, addend);
        let asn = self.is_snan(b, addend);
        let nasn = b.not(asn);
        let aq = b.and(an, nasn);
        let (ix, iy) = (self.is_inf(b, x), self.is_inf(b, y));
        let (zx, zy) = (self.is_zero(b, x), self.is_zero(b, y));
        let p = b.and(ix, zy);
        let q = b.and(zx, iy);
        let pq = b.or(p, q);
        let invalid = b.and(aq, pq);
        let d = self.default_nan(b);
        b.select(invalid, d, r)
    }

    fn key(&self, b: &mut Builder, x: Value) -> Value {
        let s = self.k(b, self.sign);
        let sb = b.and(x, s);
        let z = self.k(b, 0);
        let neg = b.cmp(CmpOp::Ne, sb, z);
        let nx = b.not(x);
        let px = b.or(x, s);
        b.select(neg, nx, px)
    }

    /// `(unordered, equal, less)` for `x` against `y`.
    fn order(&self, b: &mut Builder, x: Value, y: Value) -> (Value, Value, Value) {
        let nx = self.is_nan(b, x);
        let ny = self.is_nan(b, y);
        let un = b.or(nx, ny);
        let both = b.or(x, y);
        let bz = self.is_zero(b, both);
        let same = b.cmp(CmpOp::Eq, x, y);
        let eq0 = b.or(same, bz);
        let ord = b.not(un);
        let eq = b.and(ord, eq0);
        let (kx, ky) = (self.key(b, x), self.key(b, y));
        let lt0 = b.cmp(CmpOp::Ult, kx, ky);
        let ne = b.not(eq0);
        let lt1 = b.and(lt0, ne);
        let lt = b.and(ord, lt1);
        (un, eq, lt)
    }

    /// `FPCompare`: NZCV as four bits.
    pub fn compare(&self, b: &mut Builder, x: Value, y: Value) -> Value {
        let (un, eq, lt) = self.order(b, x, y);
        let w4 = Width::new(4);
        let gt = b.konst(w4, 0b0010);
        let l = b.konst(w4, 0b1000);
        let e = b.konst(w4, 0b0110);
        let u = b.konst(w4, 0b0011);
        let r = b.select(lt, l, gt);
        let r = b.select(eq, e, r);
        b.select(un, u, r)
    }

    /// `FPCompareEQ`, `FPCompareGE` or `FPCompareGT` (`kind` 0, 1, 2).
    pub fn compare_bool(&self, b: &mut Builder, x: Value, y: Value, kind: u32) -> Value {
        let (un, eq, lt) = self.order(b, x, y);
        let ord = b.not(un);
        match kind {
            0 => eq,
            1 => {
                let nlt = b.not(lt);
                b.and(ord, nlt)
            }
            _ => {
                let nlt = b.not(lt);
                let neq = b.not(eq);
                let g = b.and(nlt, neq);
                b.and(ord, g)
            }
        }
    }

    /// `FPMax` or `FPMin` without the alternative behaviors.
    pub fn max_min(&self, b: &mut Builder, x: Value, y: Value, max: bool) -> Value {
        let (_, _, lt) = self.order(b, x, y);
        let (_, eq, _) = self.order(b, x, y);
        let r = if max {
            let gt0 = b.not(lt);
            let neq = b.not(eq);
            let gt = b.and(gt0, neq);
            b.select(gt, x, y)
        } else {
            b.select(lt, x, y)
        };
        let both = b.or(x, y);
        let bz = self.is_zero(b, both);
        let z = if max { b.and(x, y) } else { b.or(x, y) };
        let r = b.select(bz, z, r);
        self.nans(b, &[x, y], r)
    }

    /// `FPMaxNum` or `FPMinNum`: a quiet NaN against a non-quiet-NaN operand
    /// becomes the infinity that loses.
    pub fn max_min_num(&self, b: &mut Builder, x: Value, y: Value, max: bool) -> Value {
        let ninf = self.k(b, self.inf | if max { self.sign } else { 0 });
        let (nx, ny) = (self.is_nan(b, x), self.is_nan(b, y));
        let (sx, sy) = (self.is_snan(b, x), self.is_snan(b, y));
        let (nsx, nsy) = (b.not(sx), b.not(sy));
        let qx = b.and(nx, nsx);
        let qy = b.and(ny, nsy);
        let (nqx, nqy) = (b.not(qx), b.not(qy));
        let rx = b.and(qx, nqy);
        let ry = b.and(nqx, qy);
        let x2 = b.select(rx, ninf, x);
        let y2 = b.select(ry, ninf, y);
        self.max_min(b, x2, y2, max)
    }

    /// `FPCompareEQ`.
    pub fn compare_eq(&self, b: &mut Builder, x: Value, y: Value) -> Value {
        self.compare_bool(b, x, y, 0)
    }

    /// `FPCompareGE`.
    pub fn compare_ge(&self, b: &mut Builder, x: Value, y: Value) -> Value {
        self.compare_bool(b, x, y, 1)
    }

    /// `FPCompareGT`.
    pub fn compare_gt(&self, b: &mut Builder, x: Value, y: Value) -> Value {
        self.compare_bool(b, x, y, 2)
    }

    /// `FPMax`.
    pub fn max(&self, b: &mut Builder, x: Value, y: Value) -> Value {
        self.max_min(b, x, y, true)
    }

    /// `FPMin`.
    pub fn min(&self, b: &mut Builder, x: Value, y: Value) -> Value {
        self.max_min(b, x, y, false)
    }

    /// `FPMaxNum`.
    pub fn max_num(&self, b: &mut Builder, x: Value, y: Value) -> Value {
        self.max_min_num(b, x, y, true)
    }

    /// `FPMinNum`.
    pub fn min_num(&self, b: &mut Builder, x: Value, y: Value) -> Value {
        self.max_min_num(b, x, y, false)
    }

    fn pow2(&self, b: &mut Builder, k: u32) -> Value {
        let (bias, f) = if self.w == Width::W32 {
            (127, 23)
        } else {
            (1023, 52)
        };
        self.k(b, u128::from(bias + k) << f)
    }

    /// `FPToFixed`: `x * 2^fbits` rounded by `r` to an `out`-bit integer,
    /// saturating, NaN giving 0.
    pub fn fp_to_fixed(
        &self,
        b: &mut Builder,
        x: Value,
        fbits: u32,
        signed: bool,
        r: FpRound,
        out: u32,
    ) -> Value {
        let x = if fbits == 0 {
            x
        } else {
            let k = self.pow2(b, fbits);
            b.fp(FpOp::Mul, &[x, k], self.w)
        };
        let op = if signed {
            FpOp::ToSInt(r)
        } else {
            FpOp::ToUInt(r)
        };
        b.fp(op, &[x], Width::new(out))
    }

    /// `FixedToFP`: the integer `x` divided by `2^fbits`, rounded to nearest
    /// even in this format.
    pub fn fixed_to_fp(&self, b: &mut Builder, x: Value, fbits: u32, signed: bool) -> Value {
        let op = if signed {
            FpOp::FromSInt
        } else {
            FpOp::FromUInt
        };
        let r = b.fp(op, &[x], self.w);
        if fbits == 0 {
            return r;
        }
        let k = self.pow2(b, fbits);
        b.fp(FpOp::Div, &[r, k], self.w)
    }

    /// `FPRoundInt`: `x` rounded by `r` to an integral value.
    pub fn round_int(&self, b: &mut Builder, x: Value, r: FpRound) -> Value {
        let v = b.fp(FpOp::RoundInt(r), &[x], self.w);
        self.nans(b, &[x], v)
    }

    /// `FPConvert` to the format `to`, rounding to nearest even; a NaN keeps
    /// its sign and the top of its payload and becomes quiet.
    pub fn convert(&self, b: &mut Builder, x: Value, to: &Fp) -> Value {
        let r = b.fp(FpOp::Convert, &[x], to.w);
        let n = self.is_nan(b, x);
        let (fw, tw) = (self.w.bits(), to.w.bits());
        let (ff, tf) = (
            self.quiet.trailing_zeros() + 1,
            to.quiet.trailing_zeros() + 1,
        );
        let frac = self.k(b, self.quiet * 2 - 1);
        let fx = b.and(x, frac);
        let s = self.k(b, self.sign);
        let sx = b.and(x, s);
        let (fx, sx) = if fw > tw {
            let fsh = self.k(b, u128::from(ff - tf));
            let ssh = self.k(b, u128::from(fw - tw));
            let fx = b.bin(relift_ir::BinOp::LShr, fx, fsh);
            let sx = b.bin(relift_ir::BinOp::LShr, sx, ssh);
            (b.trunc(fx, to.w), b.trunc(sx, to.w))
        } else {
            let fx = b.zext(fx, to.w);
            let sx = b.zext(sx, to.w);
            let fsh = to.k(b, u128::from(tf - ff));
            let ssh = to.k(b, u128::from(tw - fw));
            (
                b.bin(relift_ir::BinOp::Shl, fx, fsh),
                b.bin(relift_ir::BinOp::Shl, sx, ssh),
            )
        };
        let q = to.k(b, to.inf | to.quiet);
        let nan = b.or(sx, fx);
        let nan = b.or(nan, q);
        b.select(n, nan, r)
    }
}

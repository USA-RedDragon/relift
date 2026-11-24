//! The value semantics of every elementwise instruction: the integer,
//! logical, conversion and float operations of the core instruction set
//! and of GLSL.std.450, on one component. [`crate::reference`] defines
//! them the same way; the compiled interpreter calls these.

use crate::{
    Denorm, Env, GARBAGE, f16_to_f64, int_to_f32, mask, round_f16, round_f32, sext, two_sum,
};

/// What the float operations read of an [`Env`].
#[derive(Clone, Copy)]
pub(crate) struct Fx {
    pub denorm: Denorm,
    pub canonical_nan: bool,
    pub mul_zero_positive: bool,
    pub fold_identity: bool,
}

impl Fx {
    pub fn of(env: &Env) -> Fx {
        Fx {
            denorm: env.denorm,
            canonical_nan: env.canonical_nan,
            mul_zero_positive: env.mul_zero_positive,
            fold_identity: env.fold_identity,
        }
    }

    #[inline]
    pub fn flush_in(&self, v: u64) -> u64 {
        if self.denorm != Denorm::Preserve && v & 0x7f80_0000 == 0 {
            v & 0x8000_0000
        } else {
            v
        }
    }

    #[inline]
    pub fn round_arith(&self, s: f64, e: f64) -> u32 {
        if s.is_nan() && self.canonical_nan {
            return 0x7fff_ffff;
        }
        round_f32(s, e, self.denorm)
    }

    fn flush_out_exact(&self, v: f32) -> u32 {
        let b = v.to_bits();
        if self.denorm != Denorm::Preserve && b & 0x7f80_0000 == 0 {
            b & 0x8000_0000
        } else {
            b
        }
    }

    /// A 32-bit FAdd, FSub or FMul (129, 131, 133) after folding.
    #[inline]
    pub fn farith(&self, op: u16, x0: u64, x1: u64) -> u64 {
        let f = |v: u64| f32::from_bits(v as u32);
        let (p, q) = (f(self.flush_in(x0)), f(self.flush_in(x1)));
        let (s, e) = match op {
            129 => two_sum(f64::from(p), f64::from(q)),
            131 => two_sum(f64::from(p), -f64::from(q)),
            _ => (f64::from(p) * f64::from(q), 0.0),
        };
        let r = u64::from(self.round_arith(s, e));
        if op == 133 && self.mul_zero_positive && r & 0x7fff_ffff == 0 {
            0
        } else {
            r
        }
    }

    /// What a driver folds a 32-bit OpFMul (133) or OpFAdd (129) to, given
    /// which operands come from a constant (see [`Env::fold_identity`]).
    #[inline]
    pub fn folds(op: u16, known: [bool; 2], x: &[u64]) -> Option<u64> {
        for (k, o) in [(0, 1), (1, 0)] {
            if !known[k] {
                continue;
            }
            match (op, x[k]) {
                (133, 0x3f80_0000) => return Some(x[o]),
                (133, 0xbf80_0000) => return Some(x[o] ^ 0x8000_0000),
                (129, 0x8000_0000) => return Some(x[o]),
                _ => {}
            }
        }
        None
    }

    /// One component of core instruction `op` with result width `w`, the
    /// first operand's width `src_w` and operand values `x`.
    pub fn scalar(&self, op: u16, w: u32, src_w: u32, x: &[u64], known: [bool; 2]) -> u64 {
        let mk = mask(w);
        let f = |v: u64| f32::from_bits(v as u32);
        let b = |c: bool| u64::from(c);
        match op {
            128 => x[0].wrapping_add(x[1]) & mk,
            130 => x[0].wrapping_sub(x[1]) & mk,
            132 => x[0].wrapping_mul(x[1]) & mk,
            134 => x[0].checked_div(x[1]).unwrap_or(GARBAGE) & mk,
            137 => x[0].checked_rem(x[1]).unwrap_or(GARBAGE) & mk,
            126 => x[0].wrapping_neg() & mk,
            194..=196 => {
                if x[1] >= u64::from(w) {
                    return GARBAGE & mk;
                }
                match op {
                    194 => (x[0] >> x[1]) & mk,
                    195 => ((sext(x[0], w) >> x[1]) as u64) & mk,
                    _ => (x[0] << x[1]) & mk,
                }
            }
            197 => x[0] | x[1],
            198 => x[0] ^ x[1],
            199 => x[0] & x[1],
            200 => !x[0] & mk,
            201 => {
                let (off, cnt) = (x[2], x[3]);
                if off + cnt > u64::from(w) {
                    return GARBAGE & mk;
                }
                let fm = mask(cnt as u32) << off;
                (x[0] & !fm) | ((x[1] << off) & fm)
            }
            202 | 203 => {
                let (off, cnt) = (x[1], x[2]);
                if off + cnt > u64::from(w) {
                    return GARBAGE & mk;
                }
                if cnt == 0 {
                    return 0;
                }
                let v = (x[0] >> off) & mask(cnt as u32);
                if op == 202 {
                    (sext(v, cnt as u32) as u64) & mk
                } else {
                    v
                }
            }
            204 => (x[0].reverse_bits() >> (64 - w)) & mk,
            205 => u64::from(x[0].count_ones()),
            170 => b(x[0] == x[1]),
            171 => b(x[0] != x[1]),
            172 => b(x[0] > x[1]),
            174 => b(x[0] >= x[1]),
            176 => b(x[0] < x[1]),
            178 => b(x[0] <= x[1]),
            173 | 175 | 177 | 179 => {
                let (p, q) = (sext(x[0], src_w), sext(x[1], src_w));
                b(match op {
                    173 => p > q,
                    175 => p >= q,
                    177 => p < q,
                    _ => p <= q,
                })
            }
            164 => b(x[0] == x[1]),
            165 => b(x[0] != x[1]),
            166 => b(x[0] != 0 || x[1] != 0),
            167 => b(x[0] != 0 && x[1] != 0),
            168 => b(x[0] == 0),
            113 => x[0] & mk,
            114 => (sext(x[0], src_w) as u64) & mk,
            109 | 110 => {
                let v = self.flush_in(x[0]);
                let fv = f64::from(f(v)).trunc();
                let (lo, hi) = if op == 110 {
                    (-(2f64.powi(w as i32 - 1)), 2f64.powi(w as i32 - 1))
                } else {
                    (0.0, 2f64.powi(w as i32))
                };
                if fv.is_nan() || fv < lo || fv >= hi {
                    return GARBAGE & mk;
                }
                if op == 110 {
                    (fv as i64 as u64) & mk
                } else {
                    fv as u64 & mk
                }
            }
            111 | 112 => {
                let (neg, mag) = if op == 111 && sext(x[0], src_w) < 0 {
                    (true, sext(x[0], src_w).unsigned_abs())
                } else {
                    (false, x[0] & mask(src_w))
                };
                u64::from(int_to_f32(neg, mag))
            }
            129 | 131 | 133 | 4427 if w == 16 => {
                let h = |v: u64| f16_to_f64(v as u16);
                let (s, e) = match op {
                    129 => two_sum(h(x[0]), h(x[1])),
                    131 => two_sum(h(x[0]), -h(x[1])),
                    133 => (h(x[0]) * h(x[1]), 0.0),
                    _ => two_sum(h(x[0]) * h(x[1]), h(x[2])),
                };
                u64::from(round_f16(s, e))
            }
            115 if w == 16 => u64::from(round_f16(f64::from(f(x[0])), 0.0)),
            115 if w == 32 => u64::from((f16_to_f64(x[0] as u16) as f32).to_bits()),
            127 => x[0] ^ 0x8000_0000,
            133 | 129 if self.fold_identity && Fx::folds(op, known, x).is_some() => {
                Fx::folds(op, known, x).unwrap_or(0)
            }
            129 | 131 | 133 => self.farith(op, x[0], x[1]),
            4427 => {
                let (p, q, r) = (
                    f(self.flush_in(x[0])),
                    f(self.flush_in(x[1])),
                    f(self.flush_in(x[2])),
                );
                let (s, e) = two_sum(f64::from(p) * f64::from(q), f64::from(r));
                u64::from(self.round_arith(s, e))
            }
            180..=191 => {
                let (p, q) = (f(self.flush_in(x[0])), f(self.flush_in(x[1])));
                let un = p.is_nan() || q.is_nan();
                let r = match op {
                    180 | 181 => p == q,
                    182 | 183 => p != q,
                    184 | 185 => p < q,
                    186 | 187 => p > q,
                    188 | 189 => p <= q,
                    _ => p >= q,
                };
                let ordered = op.is_multiple_of(2);
                if ordered { b(!un && r) } else { b(un || r) }
            }
            156 => b(f(x[0]).is_nan()),
            157 => b(f(x[0]).is_infinite()),
            _ => panic!("unsupported op {op}"),
        }
    }

    /// One component of GLSL.std.450 instruction `inst` with result width
    /// `w` and operand values `x`.
    pub fn ext_scalar(&self, inst: u32, w: u32, x: &[u64]) -> u64 {
        let f = |v: u64| f32::from_bits(v as u32);
        let fi = |v: u64| f(self.flush_in(v));
        let out = |v: f32| -> u64 { u64::from(self.flush_out_exact(v)) };
        match inst {
            4 => x[0] & 0x7fff_ffff,
            8 => out(fi(x[0]).floor()),
            9 => out(fi(x[0]).ceil()),
            3 => out(fi(x[0]).trunc()),
            2 => out(fi(x[0]).round_ties_even()),
            50 => {
                let (p, q, r) = (fi(x[0]), fi(x[1]), fi(x[2]));
                let (s, e) = two_sum(f64::from(p) * f64::from(q), f64::from(r));
                u64::from(self.round_arith(s, e))
            }
            53 => {
                let v = fi(x[0]);
                let n = sext(x[1], 32).clamp(-400, 400) as i32;
                u64::from(round_f32(f64::from(v) * 2f64.powi(n), 0.0, self.denorm))
            }
            79 | 80 => {
                let (p, q) = (fi(x[0]), fi(x[1]));
                if p.is_nan() {
                    return u64::from(q.to_bits());
                }
                if q.is_nan() {
                    return u64::from(p.to_bits());
                }
                let r = if p == q {
                    q
                } else if (inst == 79) == (p < q) {
                    p
                } else {
                    q
                };
                u64::from(r.to_bits())
            }
            81 => {
                let (v, lo, hi) = (fi(x[0]), fi(x[1]), fi(x[2]));
                let mx = if v.is_nan() {
                    lo
                } else if lo.is_nan() || v > lo {
                    v
                } else {
                    lo
                };
                let r = if mx.is_nan() {
                    hi
                } else if hi.is_nan() || mx < hi {
                    mx
                } else {
                    hi
                };
                u64::from(r.to_bits())
            }
            38 => x[0].min(x[1]),
            41 => x[0].max(x[1]),
            39 => sext(x[0], w).min(sext(x[1], w)) as u64 & mask(w),
            42 => sext(x[0], w).max(sext(x[1], w)) as u64 & mask(w),
            44 => x[0].max(x[1]).min(x[2]),
            45 => sext(x[0], w).max(sext(x[1], w)).min(sext(x[2], w)) as u64 & mask(w),
            5 => (sext(x[0], w).unsigned_abs()) & mask(w),
            73 => {
                if x[0] == 0 {
                    mask(w)
                } else {
                    u64::from(x[0].trailing_zeros())
                }
            }
            74 => {
                let v = sext(x[0], w);
                let u = if v < 0 { !v as u64 } else { v as u64 };
                if u == 0 {
                    mask(w)
                } else {
                    u64::from(63 - u.leading_zeros())
                }
            }
            75 => {
                if x[0] == 0 {
                    mask(w)
                } else {
                    u64::from(63 - x[0].leading_zeros())
                }
            }
            13 | 14 | 29 | 30 | 31 | 32 => {
                let v = f64::from(fi(x[0]));
                let r = match inst {
                    13 => v.sin(),
                    14 => v.cos(),
                    29 => v.exp2(),
                    30 => v.log2(),
                    31 => v.sqrt(),
                    _ => 1.0 / v.sqrt(),
                };
                u64::from(round_f32(r, 0.0, self.denorm))
            }
            _ => panic!("unsupported GLSL.std.450 instruction {inst}"),
        }
    }
}

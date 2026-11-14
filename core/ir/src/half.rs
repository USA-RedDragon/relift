//! IEEE binary16 for the interpreter, by way of binary64. Sums, products,
//! quotients and square roots of binary16 values computed in binary64 and
//! rounded once more are correctly rounded (binary64 has more than twice
//! binary16's precision plus two bits); a fused multiply-add is computed
//! exactly in integers.

/// The binary64 value of a binary16 pattern (exact).
pub fn to_f64(h: u16) -> f64 {
    let sign = if h & 0x8000 != 0 { -1.0 } else { 1.0 };
    let e = i32::from((h >> 10) & 0x1f);
    let f = f64::from(h & 0x3ff);
    if e == 0x1f {
        if h & 0x3ff == 0 {
            return sign * f64::INFINITY;
        }
        let payload = u64::from(h & 0x3ff) << 42;
        return f64::from_bits((u64::from(h >> 15) << 63) | 0x7ff0_0000_0000_0000 | payload);
    }
    if e == 0 {
        return sign * f * (2.0f64).powi(-24);
    }
    sign * (1024.0 + f) * (2.0f64).powi(e - 25)
}

/// `mag * 2^exp` with the sign `neg`, rounded to nearest even binary16.
pub fn round(neg: bool, mag: u128, exp: i32) -> u16 {
    let s = if neg { 0x8000u16 } else { 0 };
    if mag == 0 {
        return s;
    }
    let top = 127 - mag.leading_zeros() as i32;
    let e = top + exp;
    if e > 15 {
        return s | 0x7c00;
    }
    // The unit of the last place: 2^(e - 10) for normals, 2^-24 below.
    let ulp = (e.max(-14)) - 10;
    let shift = ulp - exp;
    let q = if shift <= 0 {
        mag << (-shift) as u32
    } else if shift >= 128 {
        0
    } else {
        let sh = shift as u32;
        let q = mag >> sh;
        let rem = mag & ((1u128 << sh) - 1);
        let half = 1u128 << (sh - 1);
        if rem > half || (rem == half && q & 1 == 1) {
            q + 1
        } else {
            q
        }
    };
    // q is the significand at the unit `ulp`; rounding may carry into the
    // next binade, which the encoding absorbs.
    let bits = if ulp == -24 {
        q
    } else {
        (q - 1024) + ((u128::try_from(ulp + 25).unwrap_or(0)) << 10)
    };
    if bits >= 0x7c00 {
        return s | 0x7c00;
    }
    s | bits as u16
}

/// A binary64 value rounded to nearest even binary16; NaNs keep their sign
/// and the top of their payload, quieted.
pub fn from_f64(x: f64) -> u16 {
    let b = x.to_bits();
    let neg = b >> 63 == 1;
    let s = if neg { 0x8000u16 } else { 0 };
    if x.is_nan() {
        return s | 0x7e00 | ((b >> 42) & 0x3ff) as u16;
    }
    if x.is_infinite() {
        return s | 0x7c00;
    }
    let e = ((b >> 52) & 0x7ff) as i32;
    let f = b & ((1 << 52) - 1);
    if e == 0 {
        return round(neg, u128::from(f), -1074);
    }
    round(neg, u128::from(f | (1 << 52)), e - 1075)
}

fn parts(h: u16) -> (bool, u128, i32) {
    let e = i32::from((h >> 10) & 0x1f);
    let f = u128::from(h & 0x3ff);
    let neg = h & 0x8000 != 0;
    if e == 0 {
        (neg, f, -24)
    } else {
        (neg, f | 0x400, e - 25)
    }
}

/// `a * b + c` with one rounding.
pub fn mul_add(a: u16, b: u16, c: u16) -> u16 {
    let (fa, fb, fc) = (to_f64(a), to_f64(b), to_f64(c));
    if fa.is_nan()
        || fb.is_nan()
        || fc.is_nan()
        || fa.is_infinite()
        || fb.is_infinite()
        || fc.is_infinite()
    {
        return from_f64(fa.mul_add(fb, fc));
    }
    let (na, ma, ea) = parts(a);
    let (nb, mb, eb) = parts(b);
    let (nc, mc, ec) = parts(c);
    // Every product is a multiple of 2^-48 and every operand of 2^-24.
    let p = (ma * mb) << (ea + eb + 48) as u32;
    let q = mc << (ec + 48) as u32;
    let (np, nq) = (na != nb, nc);
    let (neg, mag) = if np == nq {
        (np, p + q)
    } else if p >= q {
        (np, p - q)
    } else {
        (nq, q - p)
    };
    if mag == 0 {
        let zero_neg = if p == 0 && q == 0 { np && nq } else { false };
        return if zero_neg { 0x8000 } else { 0 };
    }
    round(neg, mag, -48)
}

/// The integer `x` (signed when `signed`, of `w` bits) rounded to nearest
/// even binary16.
pub fn from_int(x: u128, w: u32, signed: bool) -> u16 {
    let m = if w >= 128 {
        u128::MAX
    } else {
        (1u128 << w) - 1
    };
    let x = x & m;
    let neg = signed && (x >> (w - 1)) & 1 == 1;
    let mag = if neg { (!x).wrapping_add(1) & m } else { x };
    round(neg, mag, 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_and_edges() {
        for h in 0..=u16::MAX {
            let x = to_f64(h);
            if x.is_nan() {
                assert_eq!(from_f64(x), h | 0x200);
            } else {
                assert_eq!(from_f64(x), h, "{h:#06x}");
            }
        }
        assert_eq!(from_f64(65504.0), 0x7bff);
        assert_eq!(from_f64(65520.0), 0x7c00);
        assert_eq!(from_f64(65519.99), 0x7bff);
        assert_eq!(from_f64(2.0f64.powi(-25)), 0);
        assert_eq!(from_f64(2.0f64.powi(-25) * 1.0001), 1);
        assert_eq!(from_f64(3.0 * 2.0f64.powi(-25)), 2);
        assert_eq!(from_int(65519, 32, false), 0x7bff);
        assert_eq!(from_int(u128::from(u32::MAX), 32, true), 0xbc00);
        assert_eq!(mul_add(0x3c00, 0x3c00, 0x8000 | 0x3c00), 0);
        assert_eq!(mul_add(0x8000, 0x3c00, 0x8000), 0x8000);
    }

    #[test]
    fn fused_multiply_add_keeps_the_sticky_bit() {
        let (a, b) = (from_f64(683.0), from_f64(3.0));
        assert_eq!(to_f64(mul_add(a, b, 0)), 2048.0);
        assert_eq!(to_f64(mul_add(a, b, 0x0001)), 2050.0);
        assert_eq!(to_f64(mul_add(a, b, 0x8001)), 2048.0);
        assert_eq!(mul_add(0x3c00, 0x3c00, 0x0001), 0x3c00);
    }
}

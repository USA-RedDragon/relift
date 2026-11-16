//! What the runtime's slow path must implement for the cases the lifted code guards out. Inputs: op (funct),
//! fmt (0x10 S, 0x11 D, 0x14 W, 0x15 L), FCR31 before, source bits (x, y) in the format's own width.
//! Rules (VR4300 User's Manual ch. 7, FPU exceptions; legacy MIPS NaN encoding: mantissa MSB set = signalling):
//!  - QNaN or denormal input, or (float->int) any NaN/Inf input or |x| out of range: Unimplemented Operation
//!    (FCR31 cause E, exception, destination unchanged);
//!  - SNaN input: Invalid Operation (V; enabled -> exception, else flag + default QNaN 0x7FBFFFFF / 0x7FF7FFFFFFFFFFFF);
//!  - denormal result (tiny, also tiny->0): with FS=0 -> Unimplemented Operation; with FS=1 -> U|I flags, result flushed
//!    to a signed zero (round to nearest);
//!  - overflow -> O|I, +-Inf; x/0 -> Z, +-Inf; 0/0, Inf-Inf, 0*Inf, Inf/Inf, sqrt(<0) -> V, default QNaN.

use super::cases::Case;
use super::run::Out;

#[derive(Debug, Clone, PartialEq)]
pub enum Rule {
    Unimplemented,
    Done { cause: u32, result: u64 },
    Skip(&'static str),
}
fn classify32(b: u32) -> u8 {
    // 0 zero, 1 denormal, 2 normal, 3 inf, 4 QNaN, 5 SNaN
    let (e, m) = (b >> 23 & 255, b & 0x7F_FFFF);
    match (e, m) {
        (0, 0) => 0,
        (0, _) => 1,
        (255, 0) => 3,
        (255, _) => {
            if m >> 22 & 1 == 1 {
                5
            } else {
                4
            }
        }
        _ => 2,
    }
}
fn classify64(b: u64) -> u8 {
    let (e, m) = (b >> 52 & 2047, b & 0xF_FFFF_FFFF_FFFF);
    match (e, m) {
        (0, 0) => 0,
        (0, _) => 1,
        (2047, 0) => 3,
        (2047, _) => {
            if m >> 51 & 1 == 1 {
                5
            } else {
                4
            }
        }
        _ => 2,
    }
}
/// The rule for one operation: `op` is the funct field, `fmt` the format, `fcr` FCR31 before, `x` and `y` the operands.
pub fn fp_rule(op: u32, fmt: u32, fcr: u32, x: u64, y: u64) -> Rule {
    let single = fmt == 0x10;
    let fs = fcr >> 24 & 1 == 1;
    if fcr >> 7 & 31 != 0 || fcr & 3 != 0 {
        return Rule::Skip("enables or rounding mode");
    }
    let v: f64 = if single {
        f64::from(f32::from_bits(x as u32))
    } else {
        f64::from_bits(x)
    };
    let w: f64 = if single {
        f64::from(f32::from_bits(y as u32))
    } else {
        f64::from_bits(y)
    };
    let cl = |b: u64| {
        if single {
            classify32(b as u32)
        } else {
            classify64(b)
        }
    };
    let (cx, cy) = (cl(x), cl(y));
    let to_int = matches!(op, 8..=15 | 36 | 37);
    let binary = matches!(op, 0..=3);
    let cvt_fp = (op == 32 && fmt == 0x11) || (op == 33 && fmt == 0x10);
    if !(binary || to_int || cvt_fp || matches!(op, 4 | 5 | 7)) {
        return Rule::Skip("op");
    }
    let ins: Vec<u8> = if binary { vec![cx, cy] } else { vec![cx] };
    let qnan = |op: u32, fmt: u32| -> u64 {
        let dest_single = match op {
            32 => true,
            33 => false,
            0..=5 | 7 => fmt == 0x10,
            _ => true,
        };
        if dest_single {
            0x7FBF_FFFF
        } else {
            0x7FF7_FFFF_FFFF_FFFF
        }
    };
    if ins.iter().any(|c| *c == 4 || *c == 1) {
        return Rule::Unimplemented;
    }
    if to_int {
        if ins[0] == 3 || ins[0] == 5 {
            return Rule::Unimplemented;
        }
        let wide = matches!(op, 8..=11 | 37);
        let lim = if wide {
            9007199254740992.0f64
        } else {
            2147483648.0
        };
        // .W accepts [-2^31, 2^31), .L only (-2^53, 2^53): the asymmetry is taken from ares (no independent source)
        if v >= lim || v < -lim || (wide && v == -lim) {
            return Rule::Unimplemented;
        }
        let r = match op {
            8 | 12 | 36 | 37 => v.round_ties_even(),
            9 | 13 => v.trunc(),
            10 | 14 => v.ceil(),
            _ => v.floor(),
        };
        let r = if op >= 36 { v.round_ties_even() } else { r };
        let res = if wide {
            r as i64 as u64
        } else {
            u64::from(r as i64 as i32 as u32)
        };
        return Rule::Done {
            cause: u32::from(r != v),
            result: res,
        };
    }
    if ins.contains(&5) {
        return Rule::Done {
            cause: 16,
            result: qnan(op, fmt),
        };
    }
    let dest_single = if op == 32 {
        true
    } else if op == 33 {
        false
    } else {
        single
    };
    let inf = [cx == 3, cy == 3];
    let (xz, yz) = (cx == 0, cy == 0);
    let nan_r = Rule::Done {
        cause: 16,
        result: qnan(op, fmt),
    };
    let exact: f64 = match op {
        0 => v + w,
        1 => v - w,
        2 => v * w,
        3 => {
            if yz {
                return if xz || inf[0] && false {
                    nan_r
                } else {
                    let neg = (x >> if single { 31 } else { 63 } & 1)
                        ^ (y >> if single { 31 } else { 63 } & 1)
                        == 1;
                    let infb: u64 = match (single, neg) {
                        (true, false) => 0x7F80_0000,
                        (true, true) => 0xFF80_0000,
                        (false, false) => 0x7FF0_0000_0000_0000,
                        (false, true) => 0xFFF0_0000_0000_0000,
                    };
                    if inf[0] {
                        Rule::Done {
                            cause: 0,
                            result: infb,
                        }
                    } else {
                        Rule::Done {
                            cause: 8,
                            result: infb,
                        }
                    }
                };
            }
            v / w
        }
        4 => {
            if v < 0.0 {
                return nan_r;
            } else {
                v.sqrt()
            }
        }
        5 => v.abs(),
        7 => -v,
        _ => v,
    };
    if exact.is_nan() {
        return nan_r;
    }
    if matches!(op, 5 | 7) {
        let sign = 1u64 << if single { 31 } else { 63 };
        return Rule::Done {
            cause: 0,
            result: if op == 5 { x & !sign } else { x ^ sign },
        };
    }
    // the result in the destination format with IEEE round-to-nearest-even (single ops rounded from the exact f64
    // value only when that is exact: add/sub/mul of f32 operands are exact in f64 except for huge exponent gaps, so
    // single arithmetic uses native f32 operations)
    let r_bits: u64 = if dest_single {
        let r: f32 = if single {
            let (a, b) = (f32::from_bits(x as u32), f32::from_bits(y as u32));
            match op {
                0 => a + b,
                1 => a - b,
                2 => a * b,
                3 => a / b,
                4 => a.sqrt(),
                _ => unreachable!(),
            }
        } else {
            f64::from_bits(x) as f32
        };
        u64::from(r.to_bits())
    } else if op == 33 {
        v.to_bits()
    } else {
        let (a, b) = (f64::from_bits(x), f64::from_bits(y));
        (match op {
            0 => a + b,
            1 => a - b,
            2 => a * b,
            3 => a / b,
            4 => a.sqrt(),
            _ => unreachable!(),
        })
        .to_bits()
    };
    let rc = if dest_single {
        classify32(r_bits as u32)
    } else {
        classify64(r_bits)
    };
    let finite_in = !inf[0] && !(binary && inf[1]);
    if rc == 3 && finite_in {
        return Rule::Done {
            cause: 4 | 1,
            result: r_bits,
        };
    }
    // tiny: denormal result, or zero from a non-zero exact result (underflow to zero)
    let tiny = rc == 1
        || (rc == 0 && exact != 0.0)
        || (rc == 0 && op == 2 && !xz && !yz)
        || (rc == 0 && op == 3 && !xz && !inf[1]);
    if tiny {
        if !fs {
            return Rule::Unimplemented;
        }
        let neg = r_bits >> if dest_single { 31 } else { 63 } & 1;
        let z = neg << if dest_single { 31 } else { 63 };
        return Rule::Done {
            cause: 2 | 1,
            result: z,
        };
    }
    // normal / zero / inf result: inexact detection through the exact f64 (valid for these ranges when the
    // operation is not a wide-gap add; cases reaching here from the guard are overflow/specials only)
    let back = if dest_single {
        f64::from(f32::from_bits(r_bits as u32))
    } else {
        f64::from_bits(r_bits)
    };
    let inexact = back != exact && !back.is_nan();
    Rule::Done {
        cause: u32::from(inexact),
        result: r_bits,
    }
}

/// Compares the rule with the oracle's outcome for a guarded case; `None` is agreement.
pub fn slow_check(c: &Case, or: &Out) -> Option<String> {
    let w = c.words[0];
    let (fmt, op) = (w >> 21 & 31, w & 63);
    let (fs, ft, fd) = (
        (w >> 11 & 31) as usize,
        (w >> 16 & 31) as usize,
        (w >> 6 & 31) as usize,
    );
    // FR=0 with an odd register: ares reads fs from the even register's low half, ft and fd from the odd
    // register's own low half (not the even/odd pair of the manual); the rule does not model that quirk
    let fr1 = c.status >> 26 & 1 == 1;
    if !fr1 && (fs | ft | fd) & 1 != 0 {
        return Some("skip: FR=0 with an odd register (ares view quirk)".into());
    }
    let wide_src = fmt == 0x11 || fmt == 0x15;
    let view = |r: &[u64; 32], n: usize, wide: bool| -> u64 {
        if wide { r[n] } else { r[n] & 0xFFFF_FFFF }
    };
    let (x, y) = (view(&c.fpr, fs, wide_src), view(&c.fpr, ft, wide_src));
    let rule = fp_rule(op, fmt, c.fcr31, x, y);
    let dest_wide = match op {
        0..=7 => fmt == 0x11,
        8..=11 | 37 => true,
        12..=15 | 36 => false,
        32 => false,
        33 => true,
        _ => false,
    };
    let cause_mask = 0x3Fu32 << 12;
    match rule {
        Rule::Skip(r) => Some(format!("skip: {r}")),
        Rule::Unimplemented => {
            let want = (c.fcr31 & !cause_mask) | 1 << 17;
            if or.exc == Some(15) && or.fcr31 == want && or.fpr == c.fpr {
                None
            } else {
                Some(format!(
                    "rule: unimplemented (fcr31 {want:08x}); ares exc {:?} fcr31 {:08x}",
                    or.exc, or.fcr31
                ))
            }
        }
        Rule::Done { cause, result } => {
            let fcr = (c.fcr31 & !cause_mask) | cause << 12 | cause << 2 | (c.fcr31 & 0x7C);
            let got = view(&or.fpr, fd, dest_wide);
            if or.exc.is_none() && or.fcr31 == fcr && got == result {
                None
            } else {
                Some(format!(
                    "rule: cause {cause:#x} result {result:#x}; ares exc {:?} fcr31 {:08x} (want {fcr:08x}) result {got:#x}",
                    or.exc, or.fcr31
                ))
            }
        }
    }
}

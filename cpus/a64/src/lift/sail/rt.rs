//! What the generated templates call: the guest state as the instruction
//! sees it while it runs, branch exits, and the integer helpers their static
//! code uses. These are the hand-written side of the cut line.

use relift_ir::{Builder, Exit, Flow, Next, Reg, Value, Width};

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

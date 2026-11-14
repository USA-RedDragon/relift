//! Operations on staged values: each folds concrete operands, emits a Rust
//! expression for static ones and an IR builder call for dynamic ones.

use std::rc::Rc;

use crate::eval::{Gen, R, fail};
use crate::value::{K, V, mask};

/// A two-operand bit vector operation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BOp {
    Add,
    Sub,
    Mul,
    And,
    Or,
    Xor,
}

/// A two-operand integer operation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IOp {
    Add,
    Sub,
    Mul,
    TDiv,
    TMod,
    FDiv,
    FMod,
    Max,
    Min,
}

/// A comparison.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Cmp {
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
}

fn ty_of(k: K) -> &'static str {
    match k {
        K::Bool => "bool",
        K::Int | K::Enum(_) => "i128",
        K::Bits(_) => "u128",
    }
}

impl Gen<'_> {
    /// Emits `let sN: T = expr;` and gives the static value.
    pub fn st(&mut self, k: K, expr: String, deps: u32) -> V {
        let name = format!("s{}", self.fresh());
        self.line(format!("let {name}: {} = {expr};", ty_of(k)));
        V::S(k, name.into(), deps)
    }

    /// Emits `let vN = expr;` and gives the dynamic value.
    pub fn dy(&mut self, k: K, expr: String) -> V {
        let name = format!("v{}", self.fresh());
        self.line(format!("let {name} = {expr};"));
        V::D(k, name.into())
    }

    /// The Rust expression of a concrete or static value.
    pub fn sx(&self, v: &V) -> R<String> {
        Ok(match v {
            V::Bool(b) => b.to_string(),
            V::Int(i) => format!("({i}i128)"),
            V::Bits(_, x) => format!("{x:#x}u128"),
            V::Undef(Some(_)) => "0u128".into(),
            V::S(_, e, _) => e.to_string(),
            V::Enum(_, i) => format!("({i}i128)"),
            _ => return fail(format!("no static form of {v:?}")),
        })
    }

    /// The IR value of `v`, emitting a constant for a concrete or static one.
    pub fn dx(&mut self, v: &V) -> R<Rc<str>> {
        let (w, e) = match v {
            V::D(_, n) => return Ok(n.clone()),
            V::Bool(b) => (1, u128::from(*b).to_string()),
            V::Int(i) => (128, format!("{}u128", *i as u128)),
            V::Bits(w, x) => (*w, format!("{x:#x}")),
            V::Undef(Some(w)) => (*w, "0".into()),
            V::S(K::Bool, e, _) => (1, format!("u128::from({e})")),
            V::S(K::Int | K::Enum(_), e, _) => (128, format!("({e}) as u128")),
            V::S(K::Bits(w), e, _) => (*w, e.to_string()),
            _ => return fail(format!("no IR form of {v:?}")),
        };
        let name = format!("v{}", self.fresh());
        self.line(format!("let {name} = b.konst(Width::new({w}), {e});"));
        Ok(name.into())
    }

    fn level(a: &V) -> u8 {
        match a {
            V::S(..) => 1,
            V::D(..) => 2,
            _ => 0,
        }
    }

    /// Normalizes `undefined` of a known width to zero bits.
    pub fn defd(&self, v: &V) -> V {
        match v {
            V::Undef(Some(w)) => V::Bits(*w, 0),
            V::Undef(None) => V::Int(0),
            _ => v.clone(),
        }
    }

    /// A bit vector operation on two operands of one width.
    pub fn bits_op(&mut self, op: BOp, a: &V, b: &V) -> R<V> {
        let (a, b) = (self.defd(a), self.defd(b));
        if a.kind() != crate::value::Kind::Bits || b.kind() != crate::value::Kind::Bits {
            return Err(crate::eval::E::NoMatch);
        }
        let w = a
            .width()
            .ok_or_else(|| crate::eval::err(format!("{op:?} of {a:?}")))?;
        if b.width() != Some(w) {
            return fail(format!("{op:?} width mismatch {a:?} {b:?}"));
        }
        let m = mask(w);
        if let (V::Bits(_, x), V::Bits(_, y)) = (&a, &b) {
            let r = match op {
                BOp::Add => x.wrapping_add(*y),
                BOp::Sub => x.wrapping_sub(*y),
                BOp::Mul => x.wrapping_mul(*y),
                BOp::And => x & y,
                BOp::Or => x | y,
                BOp::Xor => x ^ y,
            };
            return Ok(V::Bits(w, r & m));
        }
        if Self::level(&a).max(Self::level(&b)) == 1 {
            let (x, y) = (self.sx(&a)?, self.sx(&b)?);
            let e = match op {
                BOp::Add => format!("{x}.wrapping_add({y}) & {m:#x}"),
                BOp::Sub => format!("{x}.wrapping_sub({y}) & {m:#x}"),
                BOp::Mul => format!("{x}.wrapping_mul({y}) & {m:#x}"),
                BOp::And => format!("{x} & {y}"),
                BOp::Or => format!("{x} | {y}"),
                BOp::Xor => format!("{x} ^ {y}"),
            };
            let d = a.deps() | b.deps();
            return Ok(self.st(K::Bits(w), e, d));
        }
        let (x, y) = (self.dx(&a)?, self.dx(&b)?);
        let f = match op {
            BOp::Add => "Add",
            BOp::Sub => "Sub",
            BOp::Mul => "Mul",
            BOp::And => "And",
            BOp::Or => "Or",
            BOp::Xor => "Xor",
        };
        Ok(self.dy(K::Bits(w), format!("b.bin(BinOp::{f}, {x}, {y})")))
    }

    /// Bitwise complement.
    pub fn bits_not(&mut self, a: &V) -> R<V> {
        let a = self.defd(a);
        let w = a
            .width()
            .ok_or_else(|| crate::eval::err(format!("not of {a:?}")))?;
        Ok(match &a {
            V::Bits(_, x) => V::Bits(w, !x & mask(w)),
            V::S(_, e, d) => self.st(K::Bits(w), format!("!{e} & {:#x}", mask(w)), *d),
            V::D(_, n) => self.dy(K::Bits(w), format!("b.not({n})")),
            _ => return fail("not"),
        })
    }

    /// Integer arithmetic.
    pub fn int_op(&mut self, op: IOp, a: &V, b: &V) -> R<V> {
        let (a, b) = (self.defd(a), self.defd(b));
        if let (V::Int(x), V::Int(y)) = (&a, &b) {
            let (x, y) = (*x, *y);
            let r = match op {
                IOp::Add => x.wrapping_add(y),
                IOp::Sub => x.wrapping_sub(y),
                IOp::Mul => x.wrapping_mul(y),
                IOp::TDiv => {
                    if y == 0 {
                        0
                    } else {
                        x / y
                    }
                }
                IOp::TMod => {
                    if y == 0 {
                        x
                    } else {
                        x % y
                    }
                }
                IOp::FDiv => {
                    if y == 0 {
                        0
                    } else {
                        x.div_euclid(y)
                    }
                }
                IOp::FMod => {
                    if y == 0 {
                        x
                    } else {
                        x.rem_euclid(y)
                    }
                }
                IOp::Max => x.max(y),
                IOp::Min => x.min(y),
            };
            return Ok(V::Int(r));
        }
        if !matches!(a.kind(), crate::value::Kind::Int)
            || !matches!(b.kind(), crate::value::Kind::Int)
        {
            return Err(crate::eval::E::NoMatch);
        }
        if Self::level(&a).max(Self::level(&b)) == 1 {
            let (x, y) = (self.sx(&a)?, self.sx(&b)?);
            let e = match op {
                IOp::Add => format!("{x}.wrapping_add({y})"),
                IOp::Sub => format!("{x}.wrapping_sub({y})"),
                IOp::Mul => format!("{x}.wrapping_mul({y})"),
                IOp::TDiv => format!("rt::tdiv({x}, {y})"),
                IOp::TMod => format!("rt::tmod({x}, {y})"),
                IOp::FDiv => format!("rt::fdiv({x}, {y})"),
                IOp::FMod => format!("rt::fmod({x}, {y})"),
                IOp::Max => format!("{x}.max({y})"),
                IOp::Min => format!("{x}.min({y})"),
            };
            let d = a.deps() | b.deps();
            return Ok(self.st(K::Int, e, d));
        }
        let (x, y) = (self.dx(&a)?, self.dx(&b)?);
        let e = match op {
            IOp::Add => format!("b.bin(BinOp::Add, {x}, {y})"),
            IOp::Sub => format!("b.bin(BinOp::Sub, {x}, {y})"),
            IOp::Mul => format!("b.bin(BinOp::Mul, {x}, {y})"),
            IOp::TDiv => format!("b.bin(BinOp::SDiv, {x}, {y})"),
            IOp::TMod => format!("b.bin(BinOp::SRem, {x}, {y})"),
            IOp::Max | IOp::Min => {
                let c = if op == IOp::Max { "Slt" } else { "Sle" };
                let t = format!("v{}", self.fresh());
                self.line(format!("let {t} = b.cmp(CmpOp::{c}, {x}, {y});"));
                if op == IOp::Max {
                    format!("b.select({t}, {y}, {x})")
                } else {
                    format!("b.select({t}, {x}, {y})")
                }
            }
            IOp::FDiv | IOp::FMod => {
                let V::Int(m) = b else {
                    return fail("dynamic floor division");
                };
                if m <= 0 || m.count_ones() != 1 {
                    return fail("dynamic floor division");
                }
                if op == IOp::FMod {
                    let k = self.dx(&V::Int(m - 1))?;
                    format!("b.bin(BinOp::And, {x}, {k})")
                } else {
                    let k = self.dx(&V::Int(i128::from(m.trailing_zeros())))?;
                    format!("b.bin(BinOp::AShr, {x}, {k})")
                }
            }
        };
        Ok(self.dy(K::Int, e))
    }

    /// A comparison of two integers, bit vectors, booleans or enums.
    pub fn cmp(&mut self, op: Cmp, a: &V, b: &V) -> R<V> {
        let (a, b) = (self.defd(a), self.defd(b));
        let c = |o: std::cmp::Ordering| match op {
            Cmp::Eq => o.is_eq(),
            Cmp::Ne => o.is_ne(),
            Cmp::Lt => o.is_lt(),
            Cmp::Le => o.is_le(),
            Cmp::Gt => o.is_gt(),
            Cmp::Ge => o.is_ge(),
        };
        match (&a, &b) {
            (V::Int(x), V::Int(y)) => return Ok(V::Bool(c(x.cmp(y)))),
            (V::Bits(_, x), V::Bits(_, y)) => return Ok(V::Bool(c(x.cmp(y)))),
            (V::Bool(x), V::Bool(y)) => return Ok(V::Bool(c(x.cmp(y)))),
            (V::Enum(_, x), V::Enum(_, y)) => return Ok(V::Bool(c(x.cmp(y)))),
            (V::Str(x), V::Str(y)) => return Ok(V::Bool(c(x.cmp(y)))),
            (V::Unit, V::Unit) => return Ok(V::Bool(c(std::cmp::Ordering::Equal))),
            _ => {}
        }
        let ka = a.kind();
        if ka != b.kind() {
            return fail(format!("compare {a:?} with {b:?}"));
        }
        if let (Some(x), Some(y)) = (a.width(), b.width())
            && x != y
        {
            return fail(format!("compare widths {x} {y}"));
        }
        let int = ka == crate::value::Kind::Int;
        if Self::level(&a).max(Self::level(&b)) == 1 {
            let (x, y) = (self.sx(&a)?, self.sx(&b)?);
            let o = match op {
                Cmp::Eq => "==",
                Cmp::Ne => "!=",
                Cmp::Lt => "<",
                Cmp::Le => "<=",
                Cmp::Gt => ">",
                Cmp::Ge => ">=",
            };
            let d = a.deps() | b.deps();
            return Ok(self.st(K::Bool, format!("{x} {o} {y}"), d));
        }
        let (x, y) = (self.dx(&a)?, self.dx(&b)?);
        let (lt, le) = if int { ("Slt", "Sle") } else { ("Ult", "Ule") };
        let e = match op {
            Cmp::Eq => format!("b.cmp(CmpOp::Eq, {x}, {y})"),
            Cmp::Ne => format!("b.cmp(CmpOp::Ne, {x}, {y})"),
            Cmp::Lt => format!("b.cmp(CmpOp::{lt}, {x}, {y})"),
            Cmp::Le => format!("b.cmp(CmpOp::{le}, {x}, {y})"),
            Cmp::Gt => format!("b.cmp(CmpOp::{lt}, {y}, {x})"),
            Cmp::Ge => format!("b.cmp(CmpOp::{le}, {y}, {x})"),
        };
        Ok(self.dy(K::Bool, e))
    }

    /// A boolean of any time from a value that may be a one-bit vector.
    pub fn truth(&mut self, v: &V) -> R<V> {
        Ok(match v {
            V::Bool(_) | V::S(K::Bool, ..) | V::D(K::Bool, _) => v.clone(),
            V::Bits(1, x) => V::Bool(*x == 1),
            V::S(K::Bits(1), e, d) => self.st(K::Bool, format!("{e} == 1"), *d),
            V::D(K::Bits(1), n) => V::D(K::Bool, n.clone()),
            _ => return fail(format!("not a bool: {v:?}")),
        })
    }

    /// Boolean and.
    pub fn and(&mut self, a: &V, b: &V) -> R<V> {
        let (a, b) = (self.truth(a)?, self.truth(b)?);
        Ok(match (&a, &b) {
            (V::Bool(false), _) | (_, V::Bool(false)) => V::Bool(false),
            (V::Bool(true), _) => b,
            (_, V::Bool(true)) => a,
            _ if a == b => a,
            (V::S(_, x, d), V::S(_, y, e)) => self.st(K::Bool, format!("{x} && {y}"), d | e),
            _ => {
                let (x, y) = (self.dx(&a)?, self.dx(&b)?);
                self.dy(K::Bool, format!("b.and({x}, {y})"))
            }
        })
    }

    /// Boolean or.
    pub fn or(&mut self, a: &V, b: &V) -> R<V> {
        let (a, b) = (self.truth(a)?, self.truth(b)?);
        Ok(match (&a, &b) {
            (V::Bool(true), _) | (_, V::Bool(true)) => V::Bool(true),
            (V::Bool(false), _) => b,
            (_, V::Bool(false)) => a,
            _ if a == b => a,
            (V::S(_, x, d), V::S(_, y, e)) => self.st(K::Bool, format!("{x} || {y}"), d | e),
            _ => {
                let (x, y) = (self.dx(&a)?, self.dx(&b)?);
                self.dy(K::Bool, format!("b.or({x}, {y})"))
            }
        })
    }

    /// Boolean not.
    pub fn not(&mut self, a: &V) -> R<V> {
        let a = self.truth(a)?;
        Ok(match &a {
            V::Bool(x) => V::Bool(!x),
            V::S(_, e, d) => self.st(K::Bool, format!("!{e}"), *d),
            V::D(_, n) => self.dy(K::Bool, format!("b.not({n})")),
            _ => return fail("not"),
        })
    }

    /// `UInt` or `SInt` of a bit vector.
    pub fn int_of(&mut self, a: &V, signed: bool) -> R<V> {
        let a = self.defd(a);
        let w = a
            .width()
            .ok_or_else(|| crate::eval::err(format!("int of {a:?}")))?;
        Ok(match &a {
            V::Bits(_, x) => {
                if signed && w > 0 && (x >> (w - 1)) & 1 == 1 {
                    if w >= 128 {
                        V::Int(*x as i128)
                    } else {
                        V::Int(*x as i128 - (1i128 << w))
                    }
                } else {
                    V::Int(*x as i128)
                }
            }
            V::S(_, e, d) => {
                if signed {
                    self.st(K::Int, format!("rt::sint({e}, {w})"), *d)
                } else {
                    self.st(K::Int, format!("{e} as i128"), *d)
                }
            }
            V::D(_, n) => {
                if w == 128 {
                    V::D(K::Int, n.clone())
                } else if signed {
                    self.dy(K::Int, format!("b.sext({n}, Width::W128)"))
                } else {
                    self.dy(K::Int, format!("b.zext({n}, Width::W128)"))
                }
            }
            _ => return fail("int"),
        })
    }

    /// Bits `lo .. lo + w` of an integer, two's complement.
    pub fn int_bits(&mut self, a: &V, lo: u32, w: u32) -> R<V> {
        let a = self.defd(a);
        Ok(match &a {
            V::Int(x) => V::Bits(w, ((*x >> lo.min(127)) as u128) & mask(w)),
            V::S(K::Int, e, d) => self.st(
                K::Bits(w),
                format!("(({e} >> {lo}) as u128) & {:#x}", mask(w)),
                *d,
            ),
            V::D(K::Int, n) => {
                if lo + w > 128 {
                    return fail("int slice above 128 bits");
                }
                if lo == 0 && w == 128 {
                    V::D(K::Bits(128), n.clone())
                } else {
                    self.dy(K::Bits(w), format!("b.extract({n}, {lo}, Width::new({w}))"))
                }
            }
            _ => return fail(format!("int bits of {a:?}")),
        })
    }

    /// Bits `lo .. lo + w` of a bit vector; `lo` may be static or dynamic.
    pub fn extract(&mut self, a: &V, lo: &V, w: u32) -> R<V> {
        let a = self.defd(a);
        if let V::Int(_) | V::S(K::Int, ..) | V::D(K::Int, _) = a {
            if let V::Int(l) = lo {
                return self.int_bits(&a, *l as u32, w);
            }
            return fail("int slice at a non-concrete position");
        }
        let aw = a
            .width()
            .ok_or_else(|| crate::eval::err(format!("extract from {a:?}")))?;
        if let V::Wide(ps) = &a {
            let V::Int(l) = lo else {
                if let V::S(_, _, d) = lo
                    && *d != 0
                {
                    return Err(crate::eval::E::Split(*d));
                }
                return fail("wide extract at a non-concrete position");
            };
            let (l, hi) = (*l as u32, *l as u32 + w);
            let mut out: Option<V> = None;
            let mut base = 0;
            for p in ps.iter() {
                let pw = p.width().unwrap_or(0);
                let (s, e) = (l.max(base), hi.min(base + pw));
                if s < e {
                    let piece = self.extract(p, &V::Int(i128::from(s - base)), e - s)?;
                    out = Some(match out {
                        None => piece,
                        Some(o) => self.concat(&piece, &o)?,
                    });
                }
                base += pw;
            }
            return out.ok_or_else(|| crate::eval::err("empty wide extract"));
        }
        if let V::Int(l) = lo {
            let l = *l as u32;
            if l + w > aw {
                return fail(format!("extract {l}+{w} from {aw} bits"));
            }
            return Ok(match &a {
                V::Bits(_, x) => V::Bits(w, x.checked_shr(l).unwrap_or(0) & mask(w)),
                V::S(_, e, d) => {
                    if self.is_word(e) {
                        return Ok(self.word_bits(l, w));
                    }
                    if l == 0 && w == aw {
                        a.clone()
                    } else {
                        self.st(K::Bits(w), format!("({e} >> {l}) & {:#x}", mask(w)), *d)
                    }
                }
                V::D(_, n) => {
                    if l == 0 && w == aw {
                        a.clone()
                    } else {
                        self.dy(K::Bits(w), format!("b.extract({n}, {l}, Width::new({w}))"))
                    }
                }
                _ => return fail("extract"),
            });
        }
        if !lo.is_dyn() && !a.is_dyn() {
            let (x, l) = (self.sx(&a)?, self.sx(lo)?);
            let d = a.deps() | lo.deps();
            return Ok(self.st(
                K::Bits(w),
                format!("rt::lshr({x}, {l}) & {:#x}", mask(w)),
                d,
            ));
        }
        let x = self.dx(&a)?;
        let l = self.dx(lo)?;
        let s = format!("v{}", self.fresh());
        self.line(format!("let {s} = b.bin(BinOp::LShr, {x}, {l});"));
        if w == aw {
            return Ok(V::D(K::Bits(w), s.into()));
        }
        Ok(self.dy(K::Bits(w), format!("b.extract({s}, 0, Width::new({w}))")))
    }

    /// `hi @ lo`.
    pub fn concat(&mut self, hi: &V, lo: &V) -> R<V> {
        let hi = self.bitify(hi)?;
        let lo = self.bitify(lo)?;
        let (wh, wl) = (
            hi.width()
                .ok_or_else(|| crate::eval::err(format!("concat {hi:?}")))?,
            lo.width()
                .ok_or_else(|| crate::eval::err(format!("concat {lo:?}")))?,
        );
        if wh == 0 {
            return Ok(lo);
        }
        if wl == 0 {
            return Ok(hi);
        }
        if wh + wl > 128 || matches!(hi, V::Wide(_)) || matches!(lo, V::Wide(_)) {
            let mut parts = Vec::new();
            for v in [&lo, &hi] {
                match v {
                    V::Wide(ps) => parts.extend(ps.iter().cloned()),
                    v => parts.push(v.clone()),
                }
            }
            return Ok(V::Wide(Rc::new(parts)));
        }
        let w = wh + wl;
        if let (V::Bits(_, x), V::Bits(_, y)) = (&hi, &lo) {
            return Ok(V::Bits(w, (x << wl) | y));
        }
        if !hi.is_dyn() && !lo.is_dyn() {
            let (x, y) = (self.sx(&hi)?, self.sx(&lo)?);
            let d = hi.deps() | lo.deps();
            return Ok(self.st(K::Bits(w), format!("({x} << {wl}) | {y}"), d));
        }
        let (x, y) = (self.dx(&hi)?, self.dx(&lo)?);
        Ok(self.dy(K::Bits(w), format!("b.concat({x}, {y})")))
    }

    /// A bit vector from a boolean or `undefined`; other values unchanged.
    pub fn bitify(&mut self, v: &V) -> R<V> {
        Ok(match v {
            V::Bool(b) => V::Bits(1, u128::from(*b)),
            V::S(K::Bool, e, d) => self.st(K::Bits(1), format!("u128::from({e})"), *d),
            V::D(K::Bool, n) => V::D(K::Bits(1), n.clone()),
            V::Undef(Some(w)) => V::Bits(*w, 0),
            _ => v.clone(),
        })
    }

    /// Zero or sign extension of `a` to `n` bits.
    pub fn extend(&mut self, a: &V, n: u32, signed: bool) -> R<V> {
        let a = self.defd(a);
        let w = a
            .width()
            .ok_or_else(|| crate::eval::err(format!("extend {a:?}")))?;
        if n < w {
            return fail(format!("extend {w} to {n}"));
        }
        if n == w {
            return Ok(a);
        }
        Ok(match &a {
            V::Bits(_, x) => {
                let neg = signed && (x >> (w - 1)) & 1 == 1;
                V::Bits(n, if neg { (x | !mask(w)) & mask(n) } else { *x })
            }
            V::S(_, e, d) => {
                if signed {
                    self.st(K::Bits(n), format!("rt::sext({e}, {w}, {n})"), *d)
                } else {
                    a.clone().with_kind(K::Bits(n))
                }
            }
            V::D(_, x) => {
                let f = if signed { "sext" } else { "zext" };
                self.dy(K::Bits(n), format!("b.{f}({x}, Width::new({n}))"))
            }
            _ => return fail("extend"),
        })
    }

    /// Shifts of a bit vector by an integer amount: `kind` is `shl`,
    /// `lshr` or `ashr`.
    pub fn shift(&mut self, a: &V, n: &V, kind: &str) -> R<V> {
        let a = self.defd(a);
        let w = a
            .width()
            .ok_or_else(|| crate::eval::err(format!("shift {a:?}")))?;
        if let (V::Bits(_, x), V::Int(s)) = (&a, n) {
            let s = (*s).clamp(0, 255) as u32;
            let r = match kind {
                "shl" => {
                    if s >= w {
                        0
                    } else {
                        (x << s) & mask(w)
                    }
                }
                "lshr" => {
                    if s >= w {
                        0
                    } else {
                        x >> s
                    }
                }
                _ => {
                    let neg = (x >> (w - 1)) & 1 == 1;
                    let fill = if neg { mask(w) } else { 0 };
                    if s >= w {
                        fill
                    } else {
                        ((x >> s) | (fill & !(mask(w) >> s))) & mask(w)
                    }
                }
            };
            return Ok(V::Bits(w, r));
        }
        if !a.is_dyn() && !n.is_dyn() {
            let (x, s) = (self.sx(&a)?, self.sx(n)?);
            let d = a.deps() | n.deps();
            return Ok(self.st(K::Bits(w), format!("rt::{kind}({x}, {s}, {w})"), d));
        }
        let x = self.dx(&a)?;
        let s = self.dx(n)?;
        let op = match kind {
            "shl" => "Shl",
            "lshr" => "LShr",
            _ => "AShr",
        };
        Ok(self.dy(K::Bits(w), format!("b.bin(BinOp::{op}, {x}, {s})")))
    }

    /// `a` repeated `n` times.
    pub fn replicate(&mut self, a: &V, n: u32) -> R<V> {
        let a = self.bitify(a)?;
        let w = a.width().ok_or_else(|| crate::eval::err("replicate"))?;
        if n == 0 {
            return fail("replicate zero times");
        }
        let mut out = a.clone();
        for _ in 1..n {
            if out.width().unwrap_or(0) + w > 128 {
                return fail("replicate above 128 bits");
            }
            out = self.concat(&out, &a)?;
        }
        Ok(out)
    }

    /// One bit, `a[i]`.
    pub fn bit(&mut self, a: &V, i: &V) -> R<V> {
        self.extract(a, i, 1)
    }

    /// `c ? a : b` for values of any shape.
    pub fn select(&mut self, c: &V, a: &V, b: &V) -> R<V> {
        let c = self.truth(c)?;
        if let V::Bool(x) = c {
            return Ok(if x { a.clone() } else { b.clone() });
        }
        if a == b {
            return Ok(a.clone());
        }
        match (a, b) {
            (V::Undef(_), _) => return Ok(b.clone()),
            (_, V::Undef(_)) => return Ok(a.clone()),
            (V::Tuple(x), V::Tuple(y)) if x.len() == y.len() => {
                let mut out = Vec::new();
                for (p, q) in x.iter().zip(y.iter()) {
                    out.push(self.select(&c, p, q)?);
                }
                return Ok(V::tuple(out));
            }
            (V::Struct(t, x), V::Struct(_, y)) => {
                let mut out = Vec::new();
                for (k, p) in x.iter() {
                    let q = y
                        .iter()
                        .find(|(n, _)| n == k)
                        .map(|(_, v)| v.clone())
                        .unwrap_or(V::Undef(None));
                    out.push((k.clone(), self.select(&c, p, &q)?));
                }
                return Ok(V::Struct(t.clone(), Rc::new(out)));
            }
            (V::Bf(t, x), V::Bf(_, y)) => {
                let v = self.select(&c, x, y)?;
                return Ok(V::Bf(t.clone(), Box::new(v)));
            }
            (V::Str(_), V::Str(_)) => return Ok(a.clone()),
            (V::Wide(x), V::Wide(y))
                if x.len() == y.len()
                    && x.iter().zip(y.iter()).all(|(p, q)| p.width() == q.width()) =>
            {
                let mut out = Vec::new();
                for (p, q) in x.iter().zip(y.iter()) {
                    out.push(self.select(&c, p, q)?);
                }
                return Ok(V::Wide(Rc::new(out)));
            }
            _ => {}
        }
        let (a, b) = (self.defd(a), self.defd(b));
        let k = match (a.kind(), b.kind()) {
            (crate::value::Kind::Bool, crate::value::Kind::Bool) => K::Bool,
            (crate::value::Kind::Int, crate::value::Kind::Int) => K::Int,
            (crate::value::Kind::Enum(t), crate::value::Kind::Enum(_)) => {
                K::Enum(Box::leak(t.to_string().into_boxed_str()))
            }
            (crate::value::Kind::Bits, crate::value::Kind::Bits) => {
                let (w, v) = (a.width(), b.width());
                if w != v {
                    return fail(format!("select widths {w:?} {v:?}"));
                }
                K::Bits(w.unwrap_or(0))
            }
            (x, y) => return fail(format!("select of {x:?} and {y:?}")),
        };
        if !c.is_dyn() && !a.is_dyn() && !b.is_dyn() {
            let (x, y, z) = (self.sx(&c)?, self.sx(&a)?, self.sx(&b)?);
            let d = c.deps() | a.deps() | b.deps();
            return Ok(self.st(k, format!("if {x} {{ {y} }} else {{ {z} }}"), d));
        }
        if !c.is_dyn() {
            let x = self.sx(&c)?;
            let (y, z) = (self.dx(&a)?, self.dx(&b)?);
            return Ok(self.dy(k, format!("if {x} {{ {y} }} else {{ {z} }}")));
        }
        let (x, y, z) = (self.dx(&c)?, self.dx(&a)?, self.dx(&b)?);
        Ok(self.dy(k, format!("b.select({x}, {y}, {z})")))
    }
}

impl V {
    /// The same static value read as another type.
    pub fn with_kind(self, k: K) -> V {
        match self {
            V::S(_, e, d) => V::S(k, e, d),
            V::D(_, n) => V::D(k, n),
            v => v,
        }
    }
}

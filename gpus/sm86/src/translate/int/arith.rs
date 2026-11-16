//! Integer arithmetic: IADD3, IMAD, LEA, SHF, comparisons, selects and
//! minimum and maximum, with their uniform-datapath twins.
//!
//! Carries follow the hardware: a three-input add counts how many times the
//! 32-bit sum wrapped (0 to 3) and its two carry predicates are the unary
//! encoding of that count (first: at least one wrap, second: at least two).
//! A negated input of a plain add is inverted with a one added to the sum,
//! so the carry out of a subtraction is "no borrow".

use crate::spirv::{glsl, op};
use crate::translate::{Ctx, Res, bad};
use crate::{Inst, Operand, RegMods, Swz};

pub(super) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "IADD3" | "UIADD3" => iadd3(c, i),
        "IMAD" | "UIMAD" => imad(c, i),
        "LEA" | "ULEA" => lea(c, i),
        "SHF" | "USHF" => shf(c, i),
        "ISETP" | "UISETP" => isetp(c, i),
        "IMNMX" => imnmx(c, i),
        "IABS" => iabs(c, i),
        "SEL" | "USEL" => sel(c, i),
        _ => return None,
    })
}

impl Ctx {
    pub(crate) fn kc(&mut self, v: u32) -> u32 {
        self.m.c_u32(v)
    }

    pub(crate) fn ib(&mut self, opc: u16, a: u32, b: u32) -> u32 {
        let u = self.u32t();
        self.m.emit(opc, u, &[a, b])
    }

    pub(crate) fn icmp_op(&mut self, opc: u16, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(opc, t, &[a, b])
    }

    pub(crate) fn inot(&mut self, a: u32) -> u32 {
        let u = self.u32t();
        self.m.emit(op::NOT, u, &[a])
    }

    pub(crate) fn bpick(&mut self, p: u32, a: u32, b: u32) -> u32 {
        let u = self.u32t();
        self.select(u, p, a, b)
    }

    /// 0 or 1 from a predicate.
    pub(crate) fn b2u(&mut self, p: u32) -> u32 {
        self.bool_to_u32(p, 1)
    }

    /// Sum and carry (0 or 1) of two words.
    pub(crate) fn addc(&mut self, a: u32, b: u32) -> (u32, u32) {
        let u = self.u32t();
        let st = self.m.t_struct_cached(&[u, u]);
        let r = self.m.emit(op::I_ADD_CARRY, st, &[a, b]);
        let s = self.m.emit(op::COMPOSITE_EXTRACT, u, &[r, 0]);
        let cy = self.m.emit(op::COMPOSITE_EXTRACT, u, &[r, 1]);
        (s, cy)
    }

    /// The full 64-bit product of two words as (low, high).
    pub(crate) fn mul_wide(&mut self, a: u32, b: u32, signed: bool) -> (u32, u32) {
        let u = self.u32t();
        let st = self.m.t_struct_cached(&[u, u]);
        let opc = if signed {
            op::S_MUL_EXTENDED
        } else {
            op::U_MUL_EXTENDED
        };
        let r = self.m.emit(opc, st, &[a, b]);
        let lo = self.m.emit(op::COMPOSITE_EXTRACT, u, &[r, 0]);
        let hi = self.m.emit(op::COMPOSITE_EXTRACT, u, &[r, 1]);
        (lo, hi)
    }

    /// A 64-bit add with an optional carry in (a word, 0 or 1); returns
    /// (low, high, carry out as 0 or 1).
    pub(crate) fn add64(
        &mut self,
        a: (u32, u32),
        b: (u32, u32),
        cin: Option<u32>,
    ) -> (u32, u32, u32) {
        let (l, c1) = self.addc(a.0, b.0);
        let (l, cl) = match cin {
            Some(ci) => {
                let (l, c2) = self.addc(l, ci);
                (l, self.ib(op::I_ADD, c1, c2))
            }
            None => (l, c1),
        };
        let (h, c3) = self.addc(a.1, b.1);
        let (h, c4) = self.addc(h, cl);
        let carry = self.ib(op::BITWISE_OR, c3, c4);
        (l, h, carry)
    }

    pub(crate) fn store_p(&mut self, uni: bool, n: u8, v: u32) {
        if uni {
            self.store_upred(n, v);
        } else {
            self.store_pred(n, v);
        }
    }

    /// An integer operand with its inversion and negation applied; the flag
    /// says a one must be added to finish a two's-complement negation.
    pub(crate) fn term(&mut self, o: &Operand) -> Res<(u32, bool)> {
        let (v, m) = self.raw(o)?;
        if m.abs || m.swz != Swz::None {
            return bad(format!("int operand mods {o}"));
        }
        let mut v = v;
        let mut plus = false;
        if m.not {
            v = self.inot(v);
        }
        if m.neg {
            v = self.inot(v);
            plus = true;
        }
        Ok((v, plus))
    }

    /// An integer operand that may carry a negation (two's complement).
    pub(crate) fn isrc_neg(&mut self, o: &Operand) -> Res<u32> {
        let (v, plus) = self.term(o)?;
        if plus {
            let one = self.kc(1);
            Ok(self.ib(op::I_ADD, v, one))
        } else {
            Ok(v)
        }
    }

    /// Sums words and extra carry bits; when `carries` is set also returns
    /// how many times the sum wrapped.
    fn wrapping_sum(&mut self, terms: &[u32], carries: bool) -> (u32, Option<u32>) {
        let mut acc = terms[0];
        if !carries {
            for &t in &terms[1..] {
                acc = self.ib(op::I_ADD, acc, t);
            }
            return (acc, None);
        }
        let mut wraps = self.kc(0);
        for &t in &terms[1..] {
            let (s, cy) = self.addc(acc, t);
            acc = s;
            wraps = self.ib(op::I_ADD, wraps, cy);
        }
        (acc, Some(wraps))
    }

    /// Writes the two carry predicates of a sum that wrapped `wraps` times.
    fn store_wraps(&mut self, uni: bool, p0: u32, p1: u32, wraps: u32) {
        let zero = self.kc(0);
        let one = self.kc(1);
        if p0 != 7 {
            let b = self.icmp_op(op::U_GREATER_THAN, wraps, zero);
            self.store_p(uni, p0 as u8, b);
        }
        if p1 != 7 {
            let b = self.icmp_op(op::U_GREATER_THAN, wraps, one);
            self.store_p(uni, p1 as u8, b);
        }
    }
}

fn is_uni(i: &Inst) -> bool {
    i.mnemonic.starts_with('U')
}

fn iadd3(c: &mut Ctx, i: &Inst) -> Res<()> {
    let uni = is_uni(i);
    let w = c.word;
    let x = i.has("X");
    for m in &i.mods {
        if *m != "X" {
            return bad(format!("{}.{m}", i.mnemonic));
        }
    }
    let n = i.ops.len();
    let first = if x {
        n.wrapping_sub(5)
    } else {
        n.wrapping_sub(3)
    };
    if first == 0 || first > n {
        return bad(format!("{} form", i.mnemonic));
    }
    let mut terms = Vec::new();
    let mut ones = 0u32;
    for o in &i.ops[first..first + 3] {
        let (v, plus) = c.term(o)?;
        terms.push(v);
        ones += u32::from(plus);
    }
    if ones > 0 {
        terms.push(c.kc(ones));
    }
    if x {
        for o in &i.ops[n - 2..] {
            let p = c.pred_operand(o)?;
            terms.push(c.b2u(p));
        }
    }
    let (p0, p1) = (w.get(81, 3), w.get(84, 3));
    let carries = p0 != 7 || p1 != 7;
    let (sum, wraps) = c.wrapping_sum(&terms, carries);
    if let Some(wr) = wraps {
        c.store_wraps(uni, p0, p1, wr);
    }
    c.store_dst(&i.ops[0], sum)
}

fn imad(c: &mut Ctx, i: &Inst) -> Res<()> {
    let uni = is_uni(i);
    let w = c.word;
    let signed = !i.has("U32");
    let x = i.has("X");
    let wide = i.has("WIDE");
    let hi = i.has("HI");
    for m in &i.mods {
        if !matches!(*m, "U32" | "WIDE" | "MOV" | "IADD" | "SHL" | "HI" | "X") {
            return bad(format!("{}.{m}", i.mnemonic));
        }
    }
    let n = i.ops.len();
    let first = n.wrapping_sub(if x { 4 } else { 3 });
    if first == 0 || first > n {
        return bad(format!("{} form", i.mnemonic));
    }
    let a = c.usrc(&i.ops[first])?;
    let b = c.usrc(&i.ops[first + 1])?;
    let cin = if x {
        let p = c.pred_operand(&i.ops[n - 1])?;
        Some(c.b2u(p))
    } else {
        None
    };
    if !wide && !hi {
        let (cv, plus) = c.term(&i.ops[first + 2])?;
        let p = c.ib(op::I_MUL, a, b);
        let mut terms = vec![p, cv];
        if plus {
            terms.push(c.kc(1));
        }
        terms.extend(cin);
        let (sum, _) = c.wrapping_sum(&terms, false);
        return c.store_dst(&i.ops[0], sum);
    }
    let (co, flip) = match &i.ops[first + 2] {
        Operand::Reg(_, m) | Operand::UReg(_, m) if m.neg || m.abs => {
            return bad("wide IMAD with a negated addend");
        }
        Operand::Reg(n, m) => (Operand::Reg(*n, RegMods { not: false, ..*m }), m.not),
        Operand::UReg(n, m) => (Operand::UReg(*n, RegMods { not: false, ..*m }), m.not),
        o => (o.clone(), false),
    };
    let (mut clo, mut chi) = c.src_pair(&co)?;
    if flip {
        clo = c.inot(clo);
        chi = c.inot(chi);
    }
    let (plo, phi) = c.mul_wide(a, b, signed);
    let (lo, hi_w, carry) = c.add64((plo, phi), (clo, chi), cin);
    let p0 = w.get(81, 3);
    if p0 != 7 {
        let zero = c.kc(0);
        let b = c.icmp_op(op::I_NOT_EQUAL, carry, zero);
        c.store_p(uni, p0 as u8, b);
    }
    if wide {
        c.store_pair(&i.ops[0], lo, hi_w)
    } else {
        c.store_dst(&i.ops[0], hi_w)
    }
}

impl Ctx {
    /// The high word of `(c:a) << s` for a constant `0 <= s < 32`.
    fn funnel_hi_const(&mut self, a: u32, c: u32, s: u32) -> u32 {
        if s == 0 {
            return c;
        }
        let k = self.kc(s);
        let k2 = self.kc(32 - s);
        let l = self.ib(op::SHIFT_LEFT_LOGICAL, c, k);
        let r = self.ib(op::SHIFT_RIGHT_LOGICAL, a, k2);
        self.ib(op::BITWISE_OR, l, r)
    }

    /// A 64-bit shift of `(c:a)` by `s` (0 to 64) as (low, high). Left shifts
    /// move toward the high word; right shifts may fill with the sign.
    pub(crate) fn shift64(
        &mut self,
        a: u32,
        c: u32,
        s: u32,
        left: bool,
        arith: bool,
    ) -> (u32, u32) {
        let zero = self.kc(0);
        let k32 = self.kc(32);
        let k64 = self.kc(64);
        let k31 = self.kc(31);
        let k1 = self.kc(1);
        let lt32 = self.icmp_op(op::U_LESS_THAN, s, k32);
        let lt64 = self.icmp_op(op::U_LESS_THAN, s, k64);
        let sm = self.ib(op::BITWISE_AND, s, k31);
        let inv = self.ib(op::I_SUB, k31, sm);
        if left {
            let alo = self.ib(op::SHIFT_LEFT_LOGICAL, a, sm);
            let lo = self.bpick(lt32, alo, zero);
            let ch = self.ib(op::SHIFT_LEFT_LOGICAL, c, sm);
            let a1 = self.ib(op::SHIFT_RIGHT_LOGICAL, a, k1);
            let al = self.ib(op::SHIFT_RIGHT_LOGICAL, a1, inv);
            let h32 = self.ib(op::BITWISE_OR, ch, al);
            let h64 = self.bpick(lt64, alo, zero);
            let hi = self.bpick(lt32, h32, h64);
            return (lo, hi);
        }
        let sh = if arith {
            op::SHIFT_RIGHT_ARITHMETIC
        } else {
            op::SHIFT_RIGHT_LOGICAL
        };
        let fill = if arith { self.ib(sh, c, k31) } else { zero };
        let ch = self.ib(sh, c, sm);
        let hi = self.bpick(lt32, ch, fill);
        let al = self.ib(op::SHIFT_RIGHT_LOGICAL, a, sm);
        let c1 = self.ib(op::SHIFT_LEFT_LOGICAL, c, k1);
        let cl = self.ib(op::SHIFT_LEFT_LOGICAL, c1, inv);
        let l32 = self.ib(op::BITWISE_OR, al, cl);
        let l64 = self.bpick(lt64, ch, fill);
        let lo = self.bpick(lt32, l32, l64);
        (lo, hi)
    }
}

fn lea(c: &mut Ctx, i: &Inst) -> Res<()> {
    let uni = is_uni(i);
    let w = c.word;
    let hi = i.has("HI");
    let x = i.has("X");
    let sx = i.has("SX32");
    for m in &i.mods {
        if !matches!(*m, "HI" | "X" | "SX32") {
            return bad(format!("{}.{m}", i.mnemonic));
        }
    }
    let n = i.ops.len();
    let sh_at = n.wrapping_sub(if x { 2 } else { 1 });
    let ops_in = if hi && !sx { 3 } else { 2 };
    if sh_at < ops_in + 1 || sh_at >= n {
        return bad(format!("{} form", i.mnemonic));
    }
    let Operand::Hex(sh) = i.ops[sh_at] else {
        return bad("LEA shift");
    };
    let sh = sh as u32;
    if sh > 31 {
        return bad("LEA shift");
    }
    let (a, am) = c.raw(&i.ops[sh_at - ops_in])?;
    if am.abs || am.swz != Swz::None {
        return bad(format!("int operand mods {}", i.ops[sh_at - ops_in]));
    }
    let (bv, plus) = c.term(&i.ops[sh_at - ops_in + 1])?;
    let shifted = if hi {
        let cv = if sx {
            let k = c.kc(31);
            c.ib(op::SHIFT_RIGHT_ARITHMETIC, a, k)
        } else {
            c.usrc(&i.ops[sh_at - 1])?
        };
        c.funnel_hi_const(a, cv, sh)
    } else if sh == 0 {
        a
    } else {
        let k = c.kc(sh);
        c.ib(op::SHIFT_LEFT_LOGICAL, a, k)
    };
    let shifted = if am.neg || am.not {
        c.inot(shifted)
    } else {
        shifted
    };
    let mut terms = vec![shifted, bv];
    for _ in 0..u32::from(plus) + u32::from(am.neg) {
        terms.push(c.kc(1));
    }
    if x {
        let p = c.pred_operand(&i.ops[n - 1])?;
        terms.push(c.b2u(p));
    }
    let p0 = w.get(81, 3);
    let (sum, wraps) = c.wrapping_sum(&terms, p0 != 7);
    if let Some(wr) = wraps {
        c.store_wraps(uni, p0, 7, wr);
    }
    c.store_dst(&i.ops[0], sum)
}

fn shf(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut left = true;
    let mut wrap = false;
    let mut is64 = false;
    let mut arith = false;
    let mut high = false;
    for m in &i.mods {
        match *m {
            "L" => left = true,
            "R" => left = false,
            "W" => wrap = true,
            "S64" => (is64, arith) = (true, true),
            "U64" => is64 = true,
            "S32" => arith = true,
            "U32" => {}
            "HI" => high = true,
            m => return bad(format!("{}.{m}", i.mnemonic)),
        }
    }
    if i.ops.len() != 4 {
        return bad("SHF form");
    }
    let a = c.usrc(&i.ops[1])?;
    let s = c.usrc(&i.ops[2])?;
    let cv = c.usrc(&i.ops[3])?;
    let s = if wrap {
        let k = c.kc(if is64 { 63 } else { 31 });
        c.ib(op::BITWISE_AND, s, k)
    } else {
        let k = c.kc(if is64 { 64 } else { 32 });
        let u = c.u32t();
        c.ext(u, glsl::U_MIN, &[s, k])
    };
    let (lo, hi) = c.shift64(a, cv, s, left, arith);
    c.store_dst(&i.ops[0], if high { hi } else { lo })
}

pub(crate) fn icmp(c: &mut Ctx, cmp: &str, signed: bool, a: u32, b: u32) -> Res<u32> {
    let opc = match (cmp, signed) {
        ("F", _) => return Ok(c.m.c_bool(false)),
        ("T", _) => return Ok(c.m.c_bool(true)),
        ("EQ", _) => op::I_EQUAL,
        ("NE", _) => op::I_NOT_EQUAL,
        ("LT", true) => op::S_LESS_THAN,
        ("LE", true) => op::S_LESS_THAN_EQUAL,
        ("GT", true) => op::S_GREATER_THAN,
        ("GE", true) => op::S_GREATER_THAN_EQUAL,
        ("LT", false) => op::U_LESS_THAN,
        ("LE", false) => op::U_LESS_THAN_EQUAL,
        ("GT", false) => op::U_GREATER_THAN,
        ("GE", false) => op::U_GREATER_THAN_EQUAL,
        (c, _) => return bad(format!("int comparison {c}")),
    };
    Ok(c.icmp_op(opc, a, b))
}

/// The extended compare of the high words of a wide comparison: the
/// low-word result `prev` breaks ties.
fn icmp_ex(c: &mut Ctx, cmp: &str, signed: bool, a: u32, b: u32, prev: u32) -> Res<u32> {
    let eq = c.icmp_op(op::I_EQUAL, a, b);
    let t = c.boolt();
    let tie = c.m.emit(op::LOGICAL_AND, t, &[eq, prev]);
    let strict = match cmp {
        "LT" | "LE" => "LT",
        "GT" | "GE" => "GT",
        "EQ" => return Ok(tie),
        "NE" => {
            let ne = c.icmp_op(op::I_NOT_EQUAL, a, b);
            return Ok(c.m.emit(op::LOGICAL_OR, t, &[ne, prev]));
        }
        other => return icmp(c, other, signed, a, b),
    };
    let s = icmp(c, strict, signed, a, b)?;
    Ok(c.m.emit(op::LOGICAL_OR, t, &[s, tie]))
}

fn isetp(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut cmp = None;
    let mut bop = None;
    let mut signed = true;
    let mut ex = false;
    for m in &i.mods {
        match *m {
            "U32" => signed = false,
            "AND" | "OR" | "XOR" => bop = Some(*m),
            "EX" => ex = true,
            m => cmp = Some(m),
        }
    }
    let (Some(cmp), Some(bop)) = (cmp, bop) else {
        return bad("ISETP form");
    };
    if i.ops.len() != if ex { 6 } else { 5 } {
        return bad("ISETP operands");
    }
    let a = c.usrc(&i.ops[2])?;
    let b = c.usrc(&i.ops[3])?;
    let pc = c.pred_operand(&i.ops[4])?;
    let r = if ex {
        let prev = c.pred_operand(&i.ops[5])?;
        icmp_ex(c, cmp, signed, a, b, prev)?
    } else {
        icmp(c, cmp, signed, a, b)?
    };
    let nr = c.not_if(r, true);
    let p = c.bool_op(bop, r, pc)?;
    let q = c.bool_op(bop, nr, pc)?;
    c.store_pdst(&i.ops[0], p)?;
    c.store_pdst(&i.ops[1], q)
}

fn imnmx(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["U32"])?;
    let signed = !i.has("U32");
    let a = c.usrc(&i.ops[1])?;
    let b = c.usrc(&i.ops[2])?;
    let p = c.pred_operand(&i.ops[3])?;
    let u = c.u32t();
    let (mn, mx) = if signed {
        (glsl::S_MIN, glsl::S_MAX)
    } else {
        (glsl::U_MIN, glsl::U_MAX)
    };
    let lo = c.ext(u, mn, &[a, b]);
    let hi = c.ext(u, mx, &[a, b]);
    let r = c.bpick(p, lo, hi);
    c.store_dst(&i.ops[0], r)
}

fn iabs(c: &mut Ctx, i: &Inst) -> Res<()> {
    let v = c.usrc(&i.ops[1])?;
    let zero = c.kc(0);
    let neg = c.ib(op::I_SUB, zero, v);
    let k = c.kc(31);
    let sign = c.ib(op::SHIFT_RIGHT_LOGICAL, v, k);
    let one = c.kc(1);
    let is_neg = c.icmp_op(op::I_EQUAL, sign, one);
    let r = c.bpick(is_neg, neg, v);
    c.store_dst(&i.ops[0], r)
}

fn sel(c: &mut Ctx, i: &Inst) -> Res<()> {
    let a = c.usrc(&i.ops[1])?;
    let b = c.usrc(&i.ops[2])?;
    let p = c.pred_operand(&i.ops[3])?;
    let r = c.bpick(p, a, b);
    c.store_dst(&i.ops[0], r)
}

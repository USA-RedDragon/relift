//! The run-time side of the generated templates: `St` holds the lift-time
//! state of one block (pending register writes, the delay-slot and branch
//! state, the effects the runtime carries out) and turns the cut calls of
//! the templates into IR.
//!
//! - Register writes are pending until [`St::finish`]; a write under a
//!   guard becomes a select of the new and the old value.
//! - DMEM is the 4 KiB at IR addresses 0 to 0xFFF, big-endian. Every access
//!   wraps at 4 KiB byte by byte, as the RSP's does: a scalar access is
//!   lifted byte by byte, and a vector access reads or writes whole 16-byte
//!   lines (aligned 128-bit loads and stores), one line when the access
//!   stays in its line and two otherwise.
//! - The COP0 registers (the SP and DP interfaces) and BREAK are the
//!   runtime's: the intrinsics `rsp_mfc0(rd)` and `rsp_mtc0(rd, value,
//!   run)` and `rsp_break(run)`, where `run` is 1 when the instruction is not
//!   annulled by its guard.
//! - VRCP and VRSQ read the RSP's reciprocal and inverse square root ROMs.
//!   [`St::rcp_rom`] and [`St::rsq_rom`] compute an entry in IR from the
//!   formula that defines the ROM ([`rcp_rom`] and [`rsq_rom`] give the same
//!   tables at lift time); no table is stored.

use relift_ir::{BinOp, Builder, CmpOp, Endian, FpOp, FpRound, Order, Reg, UnOp, Value, Width};

use crate::regs;

/// Where a branch goes when it is taken.
#[derive(Clone, Copy, Debug)]
pub enum Target {
    /// A known address.
    Const(u64),
    /// An address computed in the block.
    Dynamic(Value),
}

/// The kind of a memory access (the model's `MemAccessType`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Access {
    /// A load.
    LoadData,
    /// A store.
    StoreData,
}

/// An effect the runtime carries out after the block.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Sys {
    /// A write to COP0 register `n` (an SP or DP register).
    Cop0Write(u8),
    /// BREAK: the RSP halts.
    Break,
}

/// The mask of the 12-bit IMEM and DMEM addresses.
pub const ADDR_MASK: u64 = 0xFFF;

/// The lift-time state of one block.
pub struct St {
    pc: u64,
    in_delay: bool,
    pending: Vec<(Reg, Value)>,
    branch: Option<(Target, Option<Value>)>,
    has_slot: bool,
    sys: Vec<Sys>,
}

impl St {
    /// The state for the instruction at `pc`.
    pub fn new(pc: u64) -> St {
        St {
            pc,
            in_delay: false,
            pending: Vec::new(),
            branch: None,
            has_slot: false,
            sys: Vec::new(),
        }
    }

    /// Moves on to the delay slot after the branch lifted so far.
    pub fn enter_delay_slot(&mut self) {
        self.in_delay = true;
        self.pc = (self.pc + 4) & ADDR_MASK;
    }

    /// Whether the instruction lifted so far has a delay slot.
    pub fn has_delay_slot(&self) -> bool {
        self.has_slot
    }

    /// The branch target and the condition under which it is taken (`None`:
    /// always).
    pub fn branch_target(&self) -> Option<(Target, Option<Value>)> {
        self.branch
    }

    /// The effects the runtime carries out, in order.
    pub fn sys_effects(&self) -> &[Sys] {
        &self.sys
    }

    fn get(&mut self, b: &mut Builder, r: Reg) -> Value {
        match self.pending.iter().find(|(p, _)| *p == r) {
            Some((_, v)) => *v,
            None => b.get(r),
        }
    }

    fn set(&mut self, b: &mut Builder, r: Reg, v: Value, g: Option<Value>) {
        let v = match g {
            Some(g) => {
                let old = self.get(b, r);
                b.select(g, v, old)
            }
            None => v,
        };
        match self.pending.iter_mut().find(|(p, _)| *p == r) {
            Some(p) => p.1 = v,
            None => self.pending.push((r, v)),
        }
    }

    fn named(name: &str) -> Reg {
        regs::slot(name).unwrap_or_else(|| panic!("the register file has no {name}"))
    }

    /// The instruction's address.
    pub fn pc(&self) -> u128 {
        u128::from(self.pc)
    }

    /// Whether the instruction is a delay slot.
    pub fn in_delay(&self) -> bool {
        self.in_delay
    }

    /// Scalar register `n`, sign-extended to the model's 64 bits.
    pub fn gpr(&mut self, b: &mut Builder, n: u8) -> Value {
        if n == 0 {
            return b.konst(Width::W64, 0);
        }
        let v = self.get(b, regs::gpr(n));
        b.sext(v, Width::W64)
    }

    /// Writes the low 32 bits of `v` to scalar register `n` (a write to `r0`
    /// is dropped).
    pub fn set_gpr(&mut self, b: &mut Builder, n: u8, v: Value, g: Option<Value>) {
        if n != 0 {
            let lo = b.extract(v, 0, Width::W32);
            self.set(b, regs::gpr(n), lo, g);
        }
    }

    /// Vector register `n`.
    pub fn vpr(&mut self, b: &mut Builder, n: u8) -> Value {
        self.get(b, regs::vpr(n))
    }

    /// Writes vector register `n`.
    pub fn set_vpr(&mut self, b: &mut Builder, n: u8, v: Value, g: Option<Value>) {
        self.set(b, regs::vpr(n), v, g);
    }

    /// The model register `name`.
    pub fn reg(&mut self, b: &mut Builder, name: &str) -> Value {
        self.get(b, St::named(name))
    }

    /// Writes the model register `name`.
    pub fn set_reg(&mut self, b: &mut Builder, name: &str, v: Value, g: Option<Value>) {
        self.set(b, St::named(name), v, g);
    }

    /// A branch to `t` under `g`, after the delay slot. IMEM addresses are
    /// 12 bits and word aligned.
    pub fn branch(&mut self, b: &mut Builder, t: Target, g: Option<Value>) {
        let t = match t {
            Target::Const(x) => Target::Const(x & 0xFFC),
            Target::Dynamic(v) => {
                let v = b.extract(v, 0, Width::W32);
                let m = b.konst(Width::W32, 0xFFC);
                Target::Dynamic(b.bin(BinOp::And, v, m))
            }
        };
        self.branch = Some((t, g));
        self.has_slot = true;
    }

    /// A branch not taken: the slot runs, then the next instruction.
    pub fn delay_slot(&mut self, _b: &mut Builder, _g: Option<Value>) {
        self.has_slot = true;
    }

    /// The DMEM address of `va`: its low 12 bits.
    pub fn translate(
        &mut self,
        b: &mut Builder,
        va: Value,
        _a: Access,
        _g: Option<Value>,
    ) -> Value {
        let m = b.konst(Width::W64, u128::from(ADDR_MASK));
        b.bin(BinOp::And, va, m)
    }

    /// The 32-bit DMEM address `a + k`, wrapped to 12 bits.
    fn addr(b: &mut Builder, a: Value, k: u32, mask: u32) -> Value {
        let w = b.width(a);
        let a = match w.bits() {
            32 => a,
            n if n < 32 => b.zext(a, Width::W32),
            _ => b.extract(a, 0, Width::W32),
        };
        let a = if k == 0 {
            a
        } else {
            let k = b.konst(Width::W32, u128::from(k));
            b.add(a, k)
        };
        let m = b.konst(Width::W32, u128::from(mask));
        b.bin(BinOp::And, a, m)
    }

    fn byte_addr(b: &mut Builder, a: Value, i: u32) -> Value {
        St::addr(b, a, i, 0xFFF)
    }

    /// A big-endian load of `n` bytes at `pa`, byte by byte.
    pub fn load(
        &mut self,
        b: &mut Builder,
        pa: Value,
        n: u32,
        _ll: bool,
        _g: Option<Value>,
    ) -> Value {
        let mut r: Option<Value> = None;
        for i in 0..n {
            let a = St::byte_addr(b, pa, i);
            let x = b.load(a, Width::W8, Endian::Big, Order::Plain);
            r = Some(match r {
                None => x,
                Some(h) => b.concat(h, x),
            });
        }
        r.expect("a load reads at least one byte")
    }

    /// A big-endian store of `n` bytes of `v` at `pa` under `g`.
    pub fn store(&mut self, b: &mut Builder, pa: Value, n: u32, v: Value, g: Option<Value>) {
        for i in 0..n {
            let x = b.extract(v, 8 * (n - 1 - i), Width::W8);
            let a = St::byte_addr(b, pa, i);
            let x = match g {
                Some(g) => {
                    let old = b.load(a, Width::W8, Endian::Big, Order::Plain);
                    b.select(g, x, old)
                }
                None => x,
            };
            b.store(a, x, Endian::Big, Order::Plain);
        }
    }

    fn line(b: &mut Builder, a: Value) -> Value {
        b.load(a, Width::W128, Endian::Big, Order::Plain)
    }

    /// `a & 15` times 8: the bit offset of `a` in its line, as 32 bits.
    fn line_bits(b: &mut Builder, a: Value) -> Value {
        let a = St::addr(b, a, 0, 15);
        let k = b.konst(Width::W32, 3);
        b.bin(BinOp::Shl, a, k)
    }

    /// The 16-byte DMEM line that holds `a`; its first byte is the most
    /// significant.
    pub fn dmem_line(&mut self, b: &mut Builder, a: Value) -> Value {
        let base = St::addr(b, a, 0, 0xFF0);
        St::line(b, base)
    }

    /// The 16 bytes at `a`, `a + 1`, ... `a + 15`, wrapping at 4 KiB; the
    /// byte at `a` is the most significant.
    pub fn dmem16(&mut self, b: &mut Builder, a: Value) -> Value {
        let base0 = St::addr(b, a, 0, 0xFF0);
        let base1 = St::addr(b, a, 16, 0xFF0);
        let l0 = St::line(b, base0);
        let l1 = St::line(b, base1);
        let sh = St::line_bits(b, a);
        let k128 = b.konst(Width::W32, 128);
        let rsh = b.bin(BinOp::Sub, k128, sh);
        let hi = b.bin(BinOp::Shl, l0, sh);
        let lo = b.bin(BinOp::LShr, l1, rsh);
        b.bin(BinOp::Or, hi, lo)
    }

    fn masked(b: &mut Builder, mask: Value, g: Option<Value>) -> Value {
        match g {
            Some(g) => {
                let z = b.konst(Width::W128, 0);
                b.select(g, mask, z)
            }
            None => mask,
        }
    }

    fn merge_line(b: &mut Builder, base: Value, data: Value, mask: Value) {
        let old = St::line(b, base);
        let keep = b.not(mask);
        let new = b.bin(BinOp::And, data, mask);
        let old = b.bin(BinOp::And, old, keep);
        let v = b.bin(BinOp::Or, new, old);
        b.store(base, v, Endian::Big, Order::Plain);
    }

    /// Writes the bytes of `data` where `mask` has a byte of ones to the
    /// line that holds `a`, under `g`.
    pub fn write_line(
        &mut self,
        b: &mut Builder,
        a: Value,
        data: Value,
        mask: Value,
        g: Option<Value>,
    ) {
        let mask = St::masked(b, mask, g);
        let base = St::addr(b, a, 0, 0xFF0);
        St::merge_line(b, base, data, mask);
    }

    /// Writes the bytes of `data` where `mask` has a byte of ones to `a`,
    /// `a + 1`, ... `a + 15` (wrapping at 4 KiB), the most significant byte
    /// at `a`, under `g`.
    pub fn write16(
        &mut self,
        b: &mut Builder,
        a: Value,
        data: Value,
        mask: Value,
        g: Option<Value>,
    ) {
        let mask = St::masked(b, mask, g);
        let base0 = St::addr(b, a, 0, 0xFF0);
        let base1 = St::addr(b, a, 16, 0xFF0);
        let sh = St::line_bits(b, a);
        let k128 = b.konst(Width::W32, 128);
        let lsh = b.bin(BinOp::Sub, k128, sh);
        let d0 = b.bin(BinOp::LShr, data, sh);
        let m0 = b.bin(BinOp::LShr, mask, sh);
        St::merge_line(b, base0, d0, m0);
        let d1 = b.bin(BinOp::Shl, data, lsh);
        let m1 = b.bin(BinOp::Shl, mask, lsh);
        St::merge_line(b, base1, d1, m1);
    }

    /// COP0 register `rd` (an SP or DP register).
    pub fn cop0_read(&mut self, b: &mut Builder, rd: u8) -> Value {
        let k = b.konst(Width::W32, u128::from(rd));
        b.intrinsic("rsp_mfc0", &[k], Some(Width::W32))
    }

    /// The value 1 when the instruction is not annulled by `g`.
    fn runs(b: &mut Builder, g: Option<Value>) -> Value {
        match g {
            Some(c) => c,
            None => b.konst(Width::W1, 1),
        }
    }

    /// Writes `v` to COP0 register `rd` under `g`.
    pub fn cop0_write(&mut self, b: &mut Builder, rd: u8, v: Value, g: Option<Value>) {
        let k = b.konst(Width::W32, u128::from(rd));
        let r = St::runs(b, g);
        b.intrinsic("rsp_mtc0", &[k, v, r], None);
        self.sys.push(Sys::Cop0Write(rd));
    }

    /// BREAK under `g`.
    pub fn brk(&mut self, b: &mut Builder, g: Option<Value>) {
        let r = St::runs(b, g);
        b.intrinsic("rsp_break", &[r], None);
        self.sys.push(Sys::Break);
    }

    /// Entry `index` (9 bits) of the reciprocal ROM: [`rcp_rom`] in IR.
    pub fn rcp_rom(&mut self, b: &mut Builder, index: Value) -> Value {
        let i = b.zext(index, Width::W64);
        let k512 = b.konst(Width::W64, 512);
        let a = b.add(i, k512);
        let num = b.konst(Width::W64, 1 << 34);
        let q = b.bin(BinOp::UDiv, num, a);
        let one = b.konst(Width::W64, 1);
        let q1 = b.add(q, one);
        let k8 = b.konst(Width::W64, 8);
        let r = b.bin(BinOp::LShr, q1, k8);
        let r = b.extract(r, 0, Width::W16);
        let z = b.konst(Width::new(9), 0);
        let is0 = b.cmp(CmpOp::Eq, index, z);
        let ones = b.konst(Width::W16, 0xFFFF);
        b.select(is0, ones, r)
    }

    /// Entry `index` (9 bits) of the inverse square root ROM: [`rsq_rom`]
    /// in IR. The entry is `(c - 1) >> 1` for the least `c` with
    /// `a * c * c >= 2^44`, that is `c = ceil(sqrt(ceil(2^44 / a)))`; the
    /// square root is a correctly rounded `f64` one, corrected by one either
    /// way with exact integer checks.
    pub fn rsq_rom(&mut self, b: &mut Builder, index: Value) -> Value {
        let w = Width::W64;
        let i = b.zext(index, w);
        let k512 = b.konst(w, 512);
        let i512 = b.add(i, k512);
        let one = b.konst(w, 1);
        let odd = b.bin(BinOp::And, i, one);
        let a = b.bin(BinOp::LShr, i512, odd);
        let num = b.konst(w, (1 << 44) - 1);
        let num = b.add(num, a);
        let num = b.bin(BinOp::Sub, num, one);
        let q = b.bin(BinOp::UDiv, num, a);
        let f = b.fp(FpOp::FromSInt, &[q], w);
        let s = b.fp(FpOp::Sqrt, &[f], w);
        let c0 = b.fp(FpOp::ToSInt(FpRound::Ceil), &[s], w);
        let sq = b.bin(BinOp::Mul, c0, c0);
        let low = b.cmp(CmpOp::Ult, sq, q);
        let up = b.add(c0, one);
        let c1 = b.select(low, up, c0);
        let d = b.bin(BinOp::Sub, c1, one);
        let dsq = b.bin(BinOp::Mul, d, d);
        let high = b.cmp(CmpOp::Ule, q, dsq);
        let c = b.select(high, d, c1);
        let r = b.bin(BinOp::Sub, c, one);
        let r = b.bin(BinOp::LShr, r, one);
        b.extract(r, 0, Width::W16)
    }

    /// `x` with the byte permutation `pattern` (a [`relift_ir::Permute`]
    /// pattern as a value); the identity gives `x` itself.
    pub fn permute(&mut self, b: &mut Builder, x: Value, pattern: u128) -> Value {
        if pattern == IDENTITY {
            return x;
        }
        b.permute(x, pattern.to_le_bytes())
    }

    /// Writes the pending registers, in slot order.
    pub fn finish(&mut self, b: &mut Builder) {
        let mut p = std::mem::take(&mut self.pending);
        p.sort_by_key(|(r, _)| r.index());
        for (r, v) in p {
            b.set(r, v);
        }
    }
}

/// The identity byte permutation.
pub const IDENTITY: u128 = 0x0F0E_0D0C_0B0A_0908_0706_0504_0302_0100;

/// The element an element selector `e` gives RSP element `n` (0 is the most
/// significant): all elements, the quarters, the halves or one element.
pub fn selected(e: u32, n: u32) -> u32 {
    match e {
        0 | 1 => n,
        2 | 3 => (n & !1) + (e - 2),
        4..=7 => (n & !3) + (e - 4),
        _ => e - 8,
    }
}

/// The byte permutation of the element selector `e` (4 bits): IR lane
/// `7 - n` takes IR lane `7 - selected(e, n)`.
pub fn elem_pattern(e: u128) -> u128 {
    let mut p = [0u8; 16];
    for n in 0..8u32 {
        let l = 7 - n;
        let s = 7 - selected((e & 15) as u32, n);
        p[2 * l as usize] = (2 * s) as u8;
        p[2 * l as usize + 1] = (2 * s + 1) as u8;
    }
    u128::from_le_bytes(p)
}

/// `x` rotated left by `n` bytes (taken modulo 16), as one IR rotate.
pub fn rotl_bytes(b: &mut Builder, x: Value, n: i128) -> Value {
    let k = b.konst(Width::W32, ((16 - n.rem_euclid(16)) * 8 % 128) as u128);
    b.bin(BinOp::RotR, x, k)
}

/// `x` rotated left by the dynamic `n` bytes (a Sail integer; taken modulo
/// 16).
pub fn rotl_bytes_ir(b: &mut Builder, x: Value, n: Value) -> Value {
    let n = b.extract(n, 0, Width::W32);
    let neg = b.un(UnOp::Neg, n);
    let k3 = b.konst(Width::W32, 3);
    let bits = b.bin(BinOp::Shl, neg, k3);
    let m = b.konst(Width::W32, 127);
    let r = b.bin(BinOp::And, bits, m);
    b.bin(BinOp::RotR, x, r)
}

/// The reciprocal ROM as the RSP defines it: entry 0 is 0xFFFF and entry
/// `i` is `(2^34 / (i + 512) + 1) >> 8`.
pub fn rcp_rom() -> [u16; 512] {
    let mut t = [0u16; 512];
    t[0] = 0xFFFF;
    for (i, e) in t.iter_mut().enumerate().skip(1) {
        let a = i as u64 + 512;
        *e = (((1u64 << 34) / a + 1) >> 8) as u16;
    }
    t
}

/// The inverse square root ROM as the RSP defines it: with `a = (i + 512)`
/// shifted right once for odd `i`, entry `i` is `b >> 1` for the largest
/// `b` (from 2^17) with `a * (b + 1)^2 < 2^44`.
pub fn rsq_rom() -> [u16; 512] {
    let mut t = [0u16; 512];
    for (i, e) in t.iter_mut().enumerate() {
        let a = (i as u64 + 512) >> (i % 2);
        let mut b = 1u64 << 17;
        while a * (b + 1) * (b + 1) < (1u64 << 44) {
            b += 1;
        }
        *e = (b >> 1) as u16;
    }
    t
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

/// Remainder of truncating division.
pub fn tmod(a: i128, b: i128) -> i128 {
    if b == 0 { a } else { a.wrapping_rem(b) }
}

/// Floor division.
pub fn fdiv(a: i128, b: i128) -> i128 {
    if b == 0 { 0 } else { a.div_euclid(b) }
}

/// Euclidean remainder.
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

/// `a >> n` on integers.
pub fn shr_int(a: i128, n: i128) -> i128 {
    a >> amount(n).min(127)
}

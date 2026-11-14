//! The WebAssembly for each IR operation.
//!
//! A value of width `w` up to 64 bits lives in one `i64` local holding its
//! value modulo 2^w (zero-extended); a wider one in a low and a high `i64`,
//! the high half masked to `w - 64` bits.

use relift_ir::{
    AtomicOp, BinOp, Block, CmpOp, Endian, ExtKind, FpOp, FpRound, IoKind, Op, UnOp, Value, Width,
};
use wasm_encoder::{BlockType, Instruction as I};

use crate::func::{Em, L_BUDGET, L_RC, V, ma, mask64, nat};
use crate::{Helper, Intrinsic, ctx, rt};

const T_ADDR: u32 = 6;
const T_VAL: u32 = 8;
/// The new memory value of an atomic read-modify-write.
const T_NEW: u32 = 10;

/// The pieces `(offset, size)` an access of `n` bytes splits into, in
/// address order, each of 1, 2, 4 or 8 bytes and none crossing an 8-byte
/// boundary of the access.
fn pieces(n: u32) -> Vec<(u32, u32)> {
    let mut out = Vec::new();
    let mut off = 0;
    while off < n {
        let left = n - off;
        let room = 8 - off % 8;
        let size = [8, 4, 2, 1]
            .into_iter()
            .find(|s| *s <= left && *s <= room)
            .unwrap_or(1);
        out.push((off, size));
        off += size;
    }
    out
}

fn bytes(w: Width) -> Result<u32, String> {
    match w.bytes() {
        Some(n) if (1..=16).contains(&n) => Ok(n),
        _ => Err(format!("a {}-bit memory access", w.bits())),
    }
}

impl Em<'_> {
    /// Pushes the `i32` memory offset of guest address `a`.
    fn addr(&mut self, a: V) {
        self.push_lo(a);
        self.i(I::I32WrapI64);
        self.get(1);
        self.i(I::I32Add);
    }

    fn load_piece(&mut self, off: u32, size: u32) {
        let m = ma(u64::from(off));
        self.i(match size {
            1 => I::I64Load8U(m),
            2 => I::I64Load16U(m),
            4 => I::I64Load32U(m),
            _ => I::I64Load(m),
        });
    }

    fn store_piece(&mut self, off: u32, size: u32) {
        let m = ma(u64::from(off));
        self.i(match size {
            1 => I::I64Store8(m),
            2 => I::I64Store16(m),
            4 => I::I64Store32(m),
            _ => I::I64Store(m),
        });
    }

    /// Loads `n` bytes at `a`, little-endian, into `dst`.
    fn load_le(&mut self, a: V, n: u32, dst: V) {
        let mut first_lo = true;
        let mut first_hi = true;
        for (off, size) in pieces(n) {
            self.addr(a);
            self.load_piece(off, size);
            let (shift, first) = if off < 8 {
                (off, &mut first_lo)
            } else {
                (off - 8, &mut first_hi)
            };
            if shift > 0 {
                self.c64(i64::from(8 * shift));
                self.i(I::I64Shl);
            }
            if *first {
                *first = false;
            } else {
                self.i(I::I64Or);
            }
            if off < 8 && off + size == n.min(8) {
                self.set(dst.lo());
            }
            if off >= 8
                && off + size == n
                && let V::P(_, h) = dst
            {
                self.set(h);
            }
        }
        if let V::P(_, h) = dst
            && n <= 8
        {
            self.c64(0);
            self.set(h);
        }
    }

    /// Stores the low `n` bytes of `v` at `a`, little-endian.
    fn store_le(&mut self, a: V, v: V, n: u32) {
        for (off, size) in pieces(n) {
            self.addr(a);
            let shift = if off < 8 {
                self.push_lo(v);
                off
            } else {
                self.push_hi(v);
                off - 8
            };
            if shift > 0 {
                self.c64(i64::from(8 * shift));
                self.i(I::I64ShrU);
            }
            self.store_piece(off, size);
        }
    }

    /// Writes the `n`-byte value `x` with its bytes reversed into `into`.
    fn bswap_val(&mut self, x: V, n: u32, into: V) {
        let bswap = self.cx.helper(Helper::ByteSwap64);
        if n <= 8 {
            self.push_lo(x);
            if n > 1 {
                self.i(I::Call(bswap));
                self.c64(i64::from(64 - 8 * n));
                self.i(I::I64ShrU);
            }
            self.set(into.lo());
            if let V::P(_, h) = into {
                self.c64(0);
                self.set(h);
            }
            return;
        }
        self.push_hi(x);
        self.i(I::Call(bswap));
        self.push_lo(x);
        self.i(I::Call(bswap));
        self.c64(i64::from(128 - 8 * n));
        self.i(I::Call(self.cx.helper(Helper::LShr128)));
        self.set_pair(into, 8 * n);
    }

    /// The temps that hold a value of width `w`.
    fn temps(w: Width) -> V {
        if nat(w) {
            V::S(Self::t(T_VAL))
        } else {
            V::P(Self::t(T_VAL), Self::t(T_VAL + 1))
        }
    }

    /// Pushes an `i32`: whether an access of `n` bytes at `a` goes to a
    /// device.
    fn io_cond(&mut self, a: V, n: u32, kind: IoKind, write: bool) {
        if kind == IoKind::Device {
            self.c32(1);
            return;
        }
        let iv = if write {
            self.dev_w.clone()
        } else {
            self.dev_r.clone()
        };
        if iv.is_empty() {
            self.c32(0);
            return;
        }
        self.push_lo(a);
        self.set(Self::t(T_ADDR));
        for (k, (first, last)) in iv.iter().enumerate() {
            self.get(Self::t(T_ADDR));
            self.c64(*last as i64);
            self.i(I::I64LeU);
            let lo = first.saturating_sub(u64::from(n) - 1);
            if lo > 0 {
                self.get(Self::t(T_ADDR));
                self.c64(lo as i64);
                self.i(I::I64GeU);
                self.i(I::I32And);
            }
            if k > 0 {
                self.i(I::I32Or);
            }
        }
    }

    fn monitor_arm(&mut self, a: V, n: u32) {
        self.get(0);
        self.push_lo(a);
        self.i(I::I64Store(ma(u64::from(ctx::MONITOR_ADDR))));
        self.get(0);
        self.c32(n as i32);
        self.i(I::I32Store(ma(u64::from(ctx::MONITOR_SIZE))));
        self.get(0);
        self.c32(1);
        self.i(I::I32Store(ma(u64::from(ctx::MONITOR_ARMED))));
    }

    fn monitor_clear(&mut self) {
        self.get(0);
        self.c32(0);
        self.i(I::I32Store(ma(u64::from(ctx::MONITOR_ARMED))));
    }

    /// Writes `(lo, hi)` on the stack into `dst`, masking hi to `bits`.
    pub(crate) fn set_pair(&mut self, dst: V, bits: u32) {
        match dst {
            V::P(l, h) => {
                self.and_mask(bits - 64);
                self.set(h);
                self.set(l);
            }
            V::S(l) => {
                self.i(I::Drop);
                self.and_mask(bits);
                self.set(l);
            }
        }
    }

    /// Pushes a shift amount saturated to `i64` (all ones when it does not
    /// fit 64 bits).
    fn amount(&mut self, y: V) {
        match y {
            V::S(l) => self.get(l),
            V::P(l, h) => {
                self.get(l);
                self.c64(-1);
                self.get(h);
                self.i(I::I64Eqz);
                self.i(I::Select);
            }
        }
    }

    /// Sets temp 0 to `y` modulo `n`.
    fn amount_mod(&mut self, y: V, n: u32) {
        let n64 = i64::from(n);
        match y {
            V::S(l) => {
                self.get(l);
                self.c64(n64);
                self.i(I::I64RemU);
            }
            V::P(l, h) => {
                let c = ((1u128 << 64) % u128::from(n)) as i64;
                self.get(h);
                self.c64(n64);
                self.i(I::I64RemU);
                self.c64(c);
                self.i(I::I64Mul);
                self.get(l);
                self.c64(n64);
                self.i(I::I64RemU);
                self.i(I::I64Add);
                self.c64(n64);
                self.i(I::I64RemU);
            }
        }
        self.set(Self::t(0));
    }

    /// Pushes the pair `x` sign-extended from `bits` to 128.
    fn sx128(&mut self, x: V, bits: u32) {
        match x {
            V::S(l) => {
                self.get(l);
                self.sx64(bits);
                self.tee(Self::t(7));
                self.get(Self::t(7));
                self.c64(63);
                self.i(I::I64ShrS);
            }
            V::P(l, h) => {
                self.get(l);
                self.get(h);
                self.sx64(bits - 64);
            }
        }
    }

    pub(crate) fn op(
        &mut self,
        b: &Block,
        op: &Op,
        width: Option<Width>,
        dst: V,
        wid: &dyn Fn(Value) -> Width,
    ) -> Result<(), String> {
        let w = width.unwrap_or(Width::W1);
        let n = w.bits();
        let loc = |s: &Self, v: Value| s.vloc(v, wid(v));
        if !nat(b.addr_width) {
            return Err("an address wider than 64 bits".into());
        }
        match op {
            Op::Const(c) => {
                self.c64(*c as u64 as i64);
                self.set(dst.lo());
                if let V::P(_, h) = dst {
                    self.c64((*c >> 64) as u64 as i64);
                    self.set(h);
                }
            }
            Op::Get(r) => {
                let src = self.reg_locals[r];
                self.push_lo(src);
                self.set(dst.lo());
                if let V::P(_, h) = dst {
                    self.push_hi(src);
                    self.set(h);
                }
            }
            Op::Un(u, x) => {
                let x = loc(self, *x);
                self.un(*u, x, n, dst);
            }
            Op::Bin(o, x, y) => {
                let (x, y) = (loc(self, *x), loc(self, *y));
                if nat(w) {
                    self.bin_nat(*o, x, y, n);
                    self.set(dst.lo());
                } else {
                    self.bin_big(*o, x, y, n, dst);
                }
            }
            Op::Cmp(o, x, y) => {
                let ww = wid(*x);
                let (x, y) = (loc(self, *x), loc(self, *y));
                self.cmp(*o, x, y, ww.bits());
                self.i(I::I64ExtendI32U);
                self.set(dst.lo());
            }
            Op::Select(c, x, y) => {
                let (c, x, y) = (loc(self, *c), loc(self, *x), loc(self, *y));
                self.push_lo(x);
                self.push_lo(y);
                self.push_lo(c);
                self.i(I::I32WrapI64);
                self.i(I::Select);
                self.set(dst.lo());
                if let V::P(_, h) = dst {
                    self.push_hi(x);
                    self.push_hi(y);
                    self.push_lo(c);
                    self.i(I::I32WrapI64);
                    self.i(I::Select);
                    self.set(h);
                }
            }
            Op::Ext(k, x) => {
                let ws = wid(*x).bits();
                let x = loc(self, *x);
                match (k, dst) {
                    (ExtKind::Zero, V::S(l)) => {
                        self.push_lo(x);
                        self.set(l);
                    }
                    (ExtKind::Zero, V::P(l, h)) => {
                        self.push_lo(x);
                        self.set(l);
                        self.push_hi(x);
                        self.set(h);
                    }
                    (ExtKind::Sign, V::S(l)) => {
                        self.push_lo(x);
                        self.sx64(ws);
                        self.and_mask(n);
                        self.set(l);
                    }
                    (ExtKind::Sign, d) => {
                        self.sx128(x, ws);
                        self.set_pair(d, n);
                    }
                }
            }
            Op::Extract(x, lo) => {
                let k = u32::from(*lo);
                let x = loc(self, *x);
                self.extract(x, k, n, dst);
            }
            Op::Concat(hv, lv) => {
                let wl = wid(*lv).bits();
                let (hx, lx) = (loc(self, *hv), loc(self, *lv));
                self.concat(hx, lx, wl, n, dst);
            }
            Op::Load { addr, endian, .. } | Op::LoadExclusive { addr, endian, .. } => {
                let nb = bytes(w)?;
                let a = loc(self, *addr);
                self.load_le(a, nb, dst);
                if *endian == Endian::Big {
                    self.bswap_val(dst, nb, dst);
                }
                if matches!(op, Op::LoadExclusive { .. }) {
                    self.monitor_arm(a, nb);
                }
            }
            Op::Store {
                addr,
                value,
                endian,
                ..
            } => {
                let wv = wid(*value);
                let nb = bytes(wv)?;
                let (a, v) = (loc(self, *addr), loc(self, *value));
                let v = self.ordered(v, wv, nb, *endian);
                self.store_le(a, v, nb);
            }
            Op::StoreExclusive {
                addr,
                value,
                endian,
                ..
            } => {
                let wv = wid(*value);
                let nb = bytes(wv)?;
                let (a, v) = (loc(self, *addr), loc(self, *value));
                self.get(0);
                self.i(I::I32Load(ma(u64::from(ctx::MONITOR_ARMED))));
                self.get(0);
                self.i(I::I64Load(ma(u64::from(ctx::MONITOR_ADDR))));
                self.push_lo(a);
                self.i(I::I64Eq);
                self.i(I::I32And);
                self.get(0);
                self.i(I::I32Load(ma(u64::from(ctx::MONITOR_SIZE))));
                self.c32(nb as i32);
                self.i(I::I32Eq);
                self.i(I::I32And);
                self.set(L_RC);
                self.monitor_clear();
                self.get(L_RC);
                self.i(I::If(BlockType::Empty));
                let v = self.ordered(v, wv, nb, *endian);
                self.store_le(a, v, nb);
                self.i(I::End);
                self.get(L_RC);
                self.i(I::I64ExtendI32U);
                self.set(dst.lo());
            }
            Op::ClearExclusive => self.monitor_clear(),
            Op::Fence(_) => {}
            Op::Fp(f, args) => {
                let Some(first) = args.first() else {
                    return Err("a floating-point operation without operands".into());
                };
                let aw = wid(*first).bits();
                let float = |b: u32| b == 16 || b == 32 || b == 64;
                let same = f.converts() || aw == n;
                if !float(n) || !float(aw) || !same || args.len() != f.arity() {
                    return Err(format!(
                        "a {aw}-bit to {n}-bit floating-point operation {f:?}"
                    ));
                }
                let a: Vec<V> = args.iter().map(|x| loc(self, *x)).collect();
                if n == 16 || aw == 16 {
                    self.fp16(*f, &a, aw, n);
                } else {
                    self.fp(*f, &a, aw, n);
                }
                self.set(dst.lo());
            }
            Op::Intrinsic(name, args) => {
                let a: Vec<V> = args.iter().map(|x| loc(self, *x)).collect();
                self.intrinsic(name, &a, width, dst)?;
            }
            Op::Tick(c) => {
                self.get(L_BUDGET);
                self.c64(i64::from(*c));
                self.i(I::I64Sub);
                self.set(L_BUDGET);
            }
            Op::TickIf(c, k) => {
                let c = loc(self, *c);
                self.get(L_BUDGET);
                self.push_lo(c);
                self.c64(i64::from(*k));
                self.i(I::I64Mul);
                self.i(I::I64Sub);
                self.set(L_BUDGET);
            }
            Op::Sync => {}
            Op::IoLoad { addr, endian, kind } => {
                let nb = bytes(w)?;
                let a = loc(self, *addr);
                self.io_cond(a, nb, *kind, false);
                self.i(I::If(BlockType::Empty));
                self.store_budget();
                let mut first_lo = true;
                let mut first_hi = true;
                for (off, size) in pieces(nb) {
                    self.get(0);
                    self.get(1);
                    self.push_lo(a);
                    if off > 0 {
                        self.c64(i64::from(off));
                        self.i(I::I64Add);
                    }
                    self.c32(size as i32);
                    self.i(I::Call(rt::IO_READ));
                    self.and_mask(8 * size);
                    let (shift, first) = if off < 8 {
                        (off, &mut first_lo)
                    } else {
                        (off - 8, &mut first_hi)
                    };
                    if shift > 0 {
                        self.c64(i64::from(8 * shift));
                        self.i(I::I64Shl);
                    }
                    if *first {
                        *first = false;
                    } else {
                        self.i(I::I64Or);
                    }
                    if off < 8 && off + size == nb.min(8) {
                        self.set(dst.lo());
                    }
                    if off >= 8
                        && off + size == nb
                        && let V::P(_, h) = dst
                    {
                        self.set(h);
                    }
                }
                if let V::P(_, h) = dst
                    && nb <= 8
                {
                    self.c64(0);
                    self.set(h);
                }
                self.load_budget();
                self.i(I::Else);
                self.load_le(a, nb, dst);
                self.i(I::End);
                if *endian == Endian::Big {
                    self.bswap_val(dst, nb, dst);
                }
            }
            Op::IoStore {
                addr,
                value,
                endian,
                kind,
            } => {
                let wv = wid(*value);
                let nb = bytes(wv)?;
                let (a, v) = (loc(self, *addr), loc(self, *value));
                let v = self.ordered(v, wv, nb, *endian);
                self.io_cond(a, nb, *kind, true);
                self.i(I::If(BlockType::Empty));
                self.store_budget();
                for (off, size) in pieces(nb) {
                    self.get(0);
                    self.get(1);
                    self.push_lo(a);
                    if off > 0 {
                        self.c64(i64::from(off));
                        self.i(I::I64Add);
                    }
                    self.c32(size as i32);
                    let shift = if off < 8 {
                        self.push_lo(v);
                        off
                    } else {
                        self.push_hi(v);
                        off - 8
                    };
                    if shift > 0 {
                        self.c64(i64::from(8 * shift));
                        self.i(I::I64ShrU);
                    }
                    self.and_mask(8 * size);
                    self.i(I::Call(rt::IO_WRITE));
                }
                self.load_budget();
                self.i(I::Else);
                self.store_le(a, v, nb);
                self.i(I::End);
            }
            Op::Crypto(..) => return Err("a cipher step".into()),
            Op::AtomicRmw {
                op: k,
                addr,
                value,
                endian,
                ..
            } => {
                let nb = bytes(w)?;
                let (a, v) = (loc(self, *addr), loc(self, *value));
                self.load_le(a, nb, dst);
                if *endian == Endian::Big {
                    self.bswap_val(dst, nb, dst);
                }
                let new = self.rmw(*k, dst, v, n);
                let new = self.ordered(new, w, nb, *endian);
                self.store_le(a, new, nb);
            }
            Op::CompareSwap {
                addr,
                expected,
                value,
                endian,
                ..
            } => {
                let nb = bytes(w)?;
                let (a, e, v) = (loc(self, *addr), loc(self, *expected), loc(self, *value));
                self.load_le(a, nb, dst);
                if *endian == Endian::Big {
                    self.bswap_val(dst, nb, dst);
                }
                self.cmp(CmpOp::Eq, dst, e, n);
                self.i(I::If(BlockType::Empty));
                let v = self.ordered(v, w, nb, *endian);
                self.store_le(a, v, nb);
                self.i(I::End);
            }
            Op::Set(..) | Op::Guard(..) => {
                return Err("an operation out of place".into());
            }
        }
        Ok(())
    }

    /// The new memory value `op(old, v)` of an atomic read-modify-write of
    /// `n` bits, in the [`T_NEW`] temps (or `v` itself for a swap).
    fn rmw(&mut self, op: AtomicOp, old: V, v: V, n: u32) -> V {
        let out = if nat(Width::new(n)) {
            V::S(Self::t(T_NEW))
        } else {
            V::P(Self::t(T_NEW), Self::t(T_NEW + 1))
        };
        match op {
            AtomicOp::Swap => return v,
            AtomicOp::Add => {
                if nat(Width::new(n)) {
                    self.bin_nat(BinOp::Add, old, v, n);
                    self.set(out.lo());
                } else {
                    self.bin_big(BinOp::Add, old, v, n, out);
                }
            }
            AtomicOp::Clear | AtomicOp::Xor | AtomicOp::Or => {
                let mut halves = vec![(false, out.lo())];
                if let V::P(_, h) = out {
                    halves.push((true, h));
                }
                for (hi, to) in halves {
                    for x in [old, v] {
                        if hi {
                            self.push_hi(x);
                        } else {
                            self.push_lo(x);
                        }
                    }
                    if op == AtomicOp::Clear {
                        // Both operands are already masked, so `old & !v`
                        // stays masked.
                        self.c64(-1);
                        self.i(I::I64Xor);
                    }
                    self.i(match op {
                        AtomicOp::Clear => I::I64And,
                        AtomicOp::Xor => I::I64Xor,
                        _ => I::I64Or,
                    });
                    self.set(to);
                }
            }
            AtomicOp::SMax | AtomicOp::SMin | AtomicOp::UMax | AtomicOp::UMin => {
                let signed = matches!(op, AtomicOp::SMax | AtomicOp::SMin);
                let max = matches!(op, AtomicOp::SMax | AtomicOp::UMax);
                self.cmp(if signed { CmpOp::Slt } else { CmpOp::Ult }, old, v, n);
                self.set(L_RC);
                let (a, b) = if max { (v, old) } else { (old, v) };
                self.push_lo(a);
                self.push_lo(b);
                self.get(L_RC);
                self.i(I::Select);
                self.set(out.lo());
                if let V::P(_, h) = out {
                    self.push_hi(a);
                    self.push_hi(b);
                    self.get(L_RC);
                    self.i(I::Select);
                    self.set(h);
                }
            }
        }
        out
    }

    /// `v` in memory order: itself when little-endian, else byte-swapped
    /// into the value temps.
    fn ordered(&mut self, v: V, w: Width, nb: u32, endian: Endian) -> V {
        if endian == Endian::Little || nb == 1 {
            return v;
        }
        let t = Self::temps(w);
        self.bswap_val(v, nb, t);
        t
    }

    fn intrinsic(
        &mut self,
        name: &'static str,
        args: &[V],
        width: Option<Width>,
        dst: V,
    ) -> Result<(), String> {
        let kind = self
            .cx
            .opts
            .intrinsics
            .get(name)
            .copied()
            .ok_or_else(|| format!("intrinsic {name} has no WebAssembly form"))?;
        match kind {
            Intrinsic::Nop if width.is_none() => {}
            Intrinsic::Nop => return Err(format!("intrinsic {name} gives a value but is a no-op")),
            Intrinsic::Const(c) => {
                let Some(w) = width else {
                    return Err(format!("intrinsic {name} gives no value but is a constant"));
                };
                let c = c & w.mask();
                self.c64(c as u64 as i64);
                self.set(dst.lo());
                if let V::P(_, h) = dst {
                    self.c64((c >> 64) as u64 as i64);
                    self.set(h);
                }
            }
            Intrinsic::ZeroBlock(size) => {
                if size == 0 || !size.is_power_of_two() || size % 8 != 0 {
                    return Err(format!("intrinsic {name} zeroes {size} bytes"));
                }
                let [a] = args else {
                    return Err(format!("intrinsic {name} takes one address"));
                };
                self.push_lo(*a);
                self.c64(!(i64::from(size) - 1));
                self.i(I::I64And);
                self.set(Self::t(T_ADDR));
                let at = V::S(Self::t(T_ADDR));
                for off in (0..size).step_by(8) {
                    self.addr(at);
                    self.c64(0);
                    self.i(I::I64Store(ma(u64::from(off))));
                }
            }
            Intrinsic::Import => {
                if self.clash.contains(name) {
                    return Err(format!("intrinsic {name} is used with two signatures"));
                }
                let Some(&f) = self.cx.intrinsics.get(name) else {
                    return Err(format!("intrinsic {name} was not imported"));
                };
                self.store_budget();
                self.get(0);
                self.get(1);
                for a in args {
                    self.push_lo(*a);
                    if let V::P(_, h) = a {
                        self.get(*h);
                    }
                }
                self.i(I::Call(f));
                if let Some(w) = width {
                    if nat(w) {
                        self.and_mask(w.bits());
                        self.set(dst.lo());
                    } else {
                        self.set_pair(dst, w.bits());
                    }
                }
                self.load_budget();
            }
        }
        Ok(())
    }

    fn extract(&mut self, x: V, k: u32, n: u32, dst: V) {
        match x {
            V::S(l) => {
                self.get(l);
                if k > 0 {
                    self.c64(i64::from(k));
                    self.i(I::I64ShrU);
                }
                self.and_mask(n);
                self.set(dst.lo());
                if let V::P(_, h) = dst {
                    self.c64(0);
                    self.set(h);
                }
            }
            V::P(l, h) => {
                if k == 0 {
                    self.get(l);
                } else if k < 64 {
                    self.get(l);
                    self.c64(i64::from(k));
                    self.i(I::I64ShrU);
                    self.get(h);
                    self.c64(64 - i64::from(k));
                    self.i(I::I64Shl);
                    self.i(I::I64Or);
                } else {
                    self.get(h);
                    self.c64(i64::from(k) - 64);
                    self.i(I::I64ShrU);
                }
                match dst {
                    V::S(d) => {
                        self.and_mask(n);
                        self.set(d);
                    }
                    V::P(dl, dh) => {
                        self.set(Self::t(0));
                        if k < 64 {
                            self.get(h);
                            self.c64(i64::from(k));
                            self.i(I::I64ShrU);
                        } else {
                            self.c64(0);
                        }
                        self.and_mask(n - 64);
                        self.set(dh);
                        self.get(Self::t(0));
                        self.set(dl);
                    }
                }
            }
        }
    }

    fn concat(&mut self, hx: V, lx: V, wl: u32, n: u32, dst: V) {
        if n <= 64 {
            self.push_lo(hx);
            self.c64(i64::from(wl));
            self.i(I::I64Shl);
            self.push_lo(lx);
            self.i(I::I64Or);
            self.set(dst.lo());
            return;
        }
        let V::P(dl, dh) = dst else {
            return;
        };
        if wl < 64 {
            self.push_lo(hx);
            self.c64(i64::from(wl));
            self.i(I::I64Shl);
            self.push_lo(lx);
            self.i(I::I64Or);
            self.set(Self::t(0));
            self.push_hi(hx);
            self.c64(i64::from(wl));
            self.i(I::I64Shl);
            if wl > 0 {
                self.push_lo(hx);
                self.c64(64 - i64::from(wl));
                self.i(I::I64ShrU);
                self.i(I::I64Or);
            }
            self.push_hi(lx);
            self.i(I::I64Or);
        } else {
            self.push_lo(lx);
            self.set(Self::t(0));
            self.push_lo(hx);
            if wl > 64 {
                self.c64(i64::from(wl) - 64);
                self.i(I::I64Shl);
            }
            self.push_hi(lx);
            self.i(I::I64Or);
        }
        self.and_mask(n - 64);
        self.set(dh);
        self.get(Self::t(0));
        self.set(dl);
    }

    fn un(&mut self, u: UnOp, x: V, n: u32, dst: V) {
        let rbit = self.cx.helper(Helper::BitReverse64);
        let bswap = self.cx.helper(Helper::ByteSwap64);
        if n <= 64 {
            self.push_lo(x);
            match u {
                UnOp::Not => {
                    self.c64(mask64(n).unwrap_or(-1));
                    self.i(I::I64Xor);
                }
                UnOp::Neg => {
                    self.set(Self::t(0));
                    self.c64(0);
                    self.get(Self::t(0));
                    self.i(I::I64Sub);
                    self.and_mask(n);
                }
                UnOp::Clz => {
                    self.i(I::I64Clz);
                    self.c64(64 - i64::from(n));
                    self.i(I::I64Sub);
                }
                UnOp::Ctz => {
                    self.i(I::I64Ctz);
                    self.set(Self::t(0));
                    self.c64(i64::from(n));
                    self.get(Self::t(0));
                    self.push_lo(x);
                    self.i(I::I64Eqz);
                    self.i(I::Select);
                }
                UnOp::Popcount => self.i(I::I64Popcnt),
                UnOp::BitReverse | UnOp::ByteSwap => {
                    if !(u == UnOp::ByteSwap && n == 8) {
                        self.i(I::Call(if u == UnOp::BitReverse { rbit } else { bswap }));
                        if n < 64 {
                            self.c64(64 - i64::from(n));
                            self.i(I::I64ShrU);
                        }
                    }
                }
            }
            self.set(dst.lo());
            return;
        }
        match u {
            UnOp::Not => {
                self.push_lo(x);
                self.c64(-1);
                self.i(I::I64Xor);
                self.push_hi(x);
                self.c64(-1);
                self.i(I::I64Xor);
                self.set_pair(dst, n);
            }
            UnOp::Neg => {
                self.c64(0);
                self.push_lo(x);
                self.i(I::I64Sub);
                self.c64(0);
                self.push_hi(x);
                self.i(I::I64Sub);
                self.push_lo(x);
                self.i(I::I64Eqz);
                self.i(I::I64ExtendI32U);
                self.c64(1);
                self.i(I::I64Xor);
                self.i(I::I64Sub);
                self.set_pair(dst, n);
            }
            UnOp::Clz => {
                self.push_lo(x);
                self.i(I::I64Clz);
                self.c64(64);
                self.i(I::I64Add);
                self.push_hi(x);
                self.i(I::I64Clz);
                self.push_hi(x);
                self.i(I::I64Eqz);
                self.i(I::Select);
                self.c64(128 - i64::from(n));
                self.i(I::I64Sub);
                self.c64(0);
                self.set_pair(dst, n);
            }
            UnOp::Ctz => {
                self.push_hi(x);
                self.i(I::I64Ctz);
                self.c64(64);
                self.i(I::I64Add);
                self.push_lo(x);
                self.i(I::I64Ctz);
                self.push_lo(x);
                self.i(I::I64Eqz);
                self.i(I::Select);
                self.set(Self::t(0));
                self.c64(i64::from(n));
                self.get(Self::t(0));
                self.get(Self::t(0));
                self.c64(i64::from(n));
                self.i(I::I64GtU);
                self.i(I::Select);
                self.c64(0);
                self.set_pair(dst, n);
            }
            UnOp::Popcount => {
                self.push_lo(x);
                self.i(I::I64Popcnt);
                self.push_hi(x);
                self.i(I::I64Popcnt);
                self.i(I::I64Add);
                self.c64(0);
                self.set_pair(dst, n);
            }
            UnOp::BitReverse | UnOp::ByteSwap => {
                let f = if u == UnOp::BitReverse { rbit } else { bswap };
                self.push_hi(x);
                self.i(I::Call(f));
                self.push_lo(x);
                self.i(I::Call(f));
                self.c64(128 - i64::from(n));
                self.i(I::Call(self.cx.helper(Helper::LShr128)));
                self.set_pair(dst, n);
            }
        }
    }

    fn bin_nat(&mut self, o: BinOp, x: V, y: V, n: u32) {
        let (xl, yl) = (x.lo(), y.lo());
        let n64 = i64::from(n);
        match o {
            BinOp::Add | BinOp::Sub | BinOp::Mul => {
                self.get(xl);
                self.get(yl);
                self.i(match o {
                    BinOp::Add => I::I64Add,
                    BinOp::Sub => I::I64Sub,
                    _ => I::I64Mul,
                });
                self.and_mask(n);
            }
            BinOp::And | BinOp::Or | BinOp::Xor => {
                self.get(xl);
                self.get(yl);
                self.i(match o {
                    BinOp::And => I::I64And,
                    BinOp::Or => I::I64Or,
                    _ => I::I64Xor,
                });
            }
            BinOp::UDiv | BinOp::URem => {
                self.get(yl);
                self.i(I::I64Eqz);
                self.i(I::If(BlockType::Result(wasm_encoder::ValType::I64)));
                if o == BinOp::UDiv {
                    self.c64(0);
                } else {
                    self.get(xl);
                }
                self.i(I::Else);
                self.get(xl);
                self.get(yl);
                self.i(if o == BinOp::UDiv {
                    I::I64DivU
                } else {
                    I::I64RemU
                });
                self.i(I::End);
            }
            BinOp::SDiv | BinOp::SRem => {
                self.get(xl);
                self.sx64(n);
                self.set(Self::t(0));
                self.get(yl);
                self.sx64(n);
                self.set(Self::t(1));
                self.get(Self::t(1));
                self.i(I::I64Eqz);
                self.i(I::If(BlockType::Result(wasm_encoder::ValType::I64)));
                if o == BinOp::SDiv {
                    self.c64(0);
                } else {
                    self.get(xl);
                }
                self.i(I::Else);
                self.get(Self::t(1));
                self.c64(-1);
                self.i(I::I64Eq);
                self.i(I::If(BlockType::Result(wasm_encoder::ValType::I64)));
                if o == BinOp::SDiv {
                    self.c64(0);
                    self.get(Self::t(0));
                    self.i(I::I64Sub);
                } else {
                    self.c64(0);
                }
                self.i(I::Else);
                self.get(Self::t(0));
                self.get(Self::t(1));
                self.i(if o == BinOp::SDiv {
                    I::I64DivS
                } else {
                    I::I64RemS
                });
                self.i(I::End);
                self.i(I::End);
                self.and_mask(n);
            }
            BinOp::Shl | BinOp::LShr => {
                self.c64(0);
                self.get(xl);
                self.get(yl);
                self.i(if o == BinOp::Shl {
                    I::I64Shl
                } else {
                    I::I64ShrU
                });
                if o == BinOp::Shl {
                    self.and_mask(n);
                }
                self.amount(y);
                self.c64(n64);
                self.i(I::I64GeU);
                self.i(I::Select);
            }
            BinOp::AShr => {
                self.get(xl);
                self.sx64(n);
                self.amount(y);
                self.set(Self::t(0));
                self.get(Self::t(0));
                self.c64(n64 - 1);
                self.get(Self::t(0));
                self.c64(n64 - 1);
                self.i(I::I64LtU);
                self.i(I::Select);
                self.i(I::I64ShrS);
                self.and_mask(n);
            }
            BinOp::RotR => {
                self.amount_mod(y, n);
                self.get(xl);
                self.get(Self::t(0));
                self.i(I::I64ShrU);
                self.get(xl);
                self.c64(n64);
                self.get(Self::t(0));
                self.i(I::I64Sub);
                self.i(I::I64Shl);
                self.i(I::I64Or);
                self.and_mask(n);
            }
        }
    }

    fn bin_big(&mut self, o: BinOp, x: V, y: V, n: u32, dst: V) {
        let n64 = i64::from(n);
        match o {
            BinOp::And | BinOp::Or | BinOp::Xor => {
                let ins = match o {
                    BinOp::And => I::I64And,
                    BinOp::Or => I::I64Or,
                    _ => I::I64Xor,
                };
                self.push_lo(x);
                self.push_lo(y);
                self.i(ins.clone());
                self.push_hi(x);
                self.push_hi(y);
                self.i(ins);
                self.set_pair(dst, n);
            }
            BinOp::Add => {
                self.push_lo(x);
                self.push_lo(y);
                self.i(I::I64Add);
                self.tee(Self::t(0));
                self.push_hi(x);
                self.push_hi(y);
                self.i(I::I64Add);
                self.get(Self::t(0));
                self.push_lo(x);
                self.i(I::I64LtU);
                self.i(I::I64ExtendI32U);
                self.i(I::I64Add);
                self.set_pair(dst, n);
            }
            BinOp::Sub => {
                self.push_lo(x);
                self.push_lo(y);
                self.i(I::I64Sub);
                self.push_hi(x);
                self.push_hi(y);
                self.i(I::I64Sub);
                self.push_lo(x);
                self.push_lo(y);
                self.i(I::I64LtU);
                self.i(I::I64ExtendI32U);
                self.i(I::I64Sub);
                self.set_pair(dst, n);
            }
            BinOp::Mul => {
                self.push_lo(x);
                self.push_lo(y);
                self.i(I::I64Mul);
                self.push_lo(x);
                self.push_lo(y);
                self.i(I::Call(self.cx.helper(Helper::MulHi)));
                self.push_lo(x);
                self.push_hi(y);
                self.i(I::I64Mul);
                self.i(I::I64Add);
                self.push_hi(x);
                self.push_lo(y);
                self.i(I::I64Mul);
                self.i(I::I64Add);
                self.set_pair(dst, n);
            }
            BinOp::Shl | BinOp::LShr => {
                self.push_lo(x);
                self.push_hi(x);
                self.amount(y);
                self.tee(Self::t(0));
                self.i(I::Call(self.cx.helper(if o == BinOp::Shl {
                    Helper::Shl128
                } else {
                    Helper::LShr128
                })));
                self.set(Self::t(2));
                self.set(Self::t(1));
                for t in [1, 2] {
                    self.get(Self::t(t));
                    self.c64(0);
                    self.get(Self::t(0));
                    self.c64(n64);
                    self.i(I::I64LtU);
                    self.i(I::Select);
                }
                self.set_pair(dst, n);
            }
            BinOp::AShr => {
                self.sx128(x, n);
                self.amount(y);
                self.set(Self::t(0));
                self.get(Self::t(0));
                self.c64(n64 - 1);
                self.get(Self::t(0));
                self.c64(n64 - 1);
                self.i(I::I64LtU);
                self.i(I::Select);
                self.i(I::Call(self.cx.helper(Helper::AShr128)));
                self.set_pair(dst, n);
            }
            BinOp::UDiv | BinOp::SDiv | BinOp::URem | BinOp::SRem => {
                self.push_lo(x);
                self.push_hi(x);
                self.push_lo(y);
                self.push_hi(y);
                self.c32(match o {
                    BinOp::UDiv => 0,
                    BinOp::SDiv => 1,
                    BinOp::URem => 2,
                    _ => 3,
                });
                self.c32(n as i32);
                self.i(I::Call(rt::DIV128));
                self.set_pair(dst, n);
            }
            BinOp::RotR => {
                self.amount_mod(y, n);
                self.push_lo(x);
                self.push_hi(x);
                self.get(Self::t(0));
                self.i(I::Call(self.cx.helper(Helper::LShr128)));
                self.set(Self::t(3));
                self.set(Self::t(2));
                self.push_lo(x);
                self.push_hi(x);
                self.c64(n64);
                self.get(Self::t(0));
                self.i(I::I64Sub);
                self.i(I::Call(self.cx.helper(Helper::Shl128)));
                self.set(Self::t(5));
                self.set(Self::t(4));
                self.push_lo(x);
                self.get(Self::t(2));
                self.get(Self::t(4));
                self.i(I::I64Or);
                self.get(Self::t(0));
                self.i(I::I64Eqz);
                self.i(I::Select);
                self.push_hi(x);
                self.get(Self::t(3));
                self.get(Self::t(5));
                self.i(I::I64Or);
                self.get(Self::t(0));
                self.i(I::I64Eqz);
                self.i(I::Select);
                self.set_pair(dst, n);
            }
        }
    }

    /// Pushes an `i32` 0 or 1.
    fn cmp(&mut self, o: CmpOp, x: V, y: V, n: u32) {
        let signed = matches!(o, CmpOp::Slt | CmpOp::Sle);
        if n <= 64 {
            self.push_lo(x);
            if signed {
                self.sx64(n);
            }
            self.push_lo(y);
            if signed {
                self.sx64(n);
            }
            self.i(match o {
                CmpOp::Eq => I::I64Eq,
                CmpOp::Ne => I::I64Ne,
                CmpOp::Ult => I::I64LtU,
                CmpOp::Ule => I::I64LeU,
                CmpOp::Slt => I::I64LtS,
                CmpOp::Sle => I::I64LeS,
            });
            return;
        }
        match o {
            CmpOp::Eq | CmpOp::Ne => {
                self.push_lo(x);
                self.push_lo(y);
                self.i(I::I64Eq);
                self.push_hi(x);
                self.push_hi(y);
                self.i(I::I64Eq);
                self.i(I::I32And);
                if o == CmpOp::Ne {
                    self.i(I::I32Eqz);
                }
            }
            _ => {
                let hb = n - 64;
                let push_h = |s: &mut Self, v: V| {
                    s.push_hi(v);
                    if signed {
                        s.sx64(hb);
                    }
                };
                push_h(self, x);
                push_h(self, y);
                self.i(if signed { I::I64LtS } else { I::I64LtU });
                push_h(self, x);
                push_h(self, y);
                self.i(I::I64Eq);
                self.push_lo(x);
                self.push_lo(y);
                self.i(if matches!(o, CmpOp::Ult | CmpOp::Slt) {
                    I::I64LtU
                } else {
                    I::I64LeU
                });
                self.i(I::I32And);
                self.i(I::I32Or);
            }
        }
    }

    fn push_f(&mut self, x: V, w: u32) {
        self.push_lo(x);
        if w == 32 {
            self.i(I::I32WrapI64);
            self.i(I::F32ReinterpretI32);
        } else {
            self.i(I::F64ReinterpretI64);
        }
    }

    fn pop_f(&mut self, w: u32) {
        if w == 32 {
            self.i(I::I32ReinterpretF32);
            self.i(I::I64ExtendI32U);
        } else {
            self.i(I::I64ReinterpretF64);
        }
    }

    fn round(&mut self, r: FpRound, w: u32) {
        let f32 = w == 32;
        self.i(match (r, f32) {
            (FpRound::TiesEven, true) => I::F32Nearest,
            (FpRound::TiesEven, false) => I::F64Nearest,
            (FpRound::Floor, true) => I::F32Floor,
            (FpRound::Floor, false) => I::F64Floor,
            (FpRound::Ceil, true) => I::F32Ceil,
            (FpRound::Ceil, false) => I::F64Ceil,
            (FpRound::Zero, true) => I::F32Trunc,
            (FpRound::Zero, false) => I::F64Trunc,
            (FpRound::TiesAway, true) => I::Call(self.cx.helper(Helper::RoundAway32)),
            (FpRound::TiesAway, false) => I::Call(self.cx.helper(Helper::RoundAway64)),
        });
    }

    /// Pushes the `i64` result bits.
    fn fp(&mut self, f: FpOp, a: &[V], aw: u32, w: u32) {
        let f32 = w == 32;
        match f {
            FpOp::Neg | FpOp::Abs => {
                self.push_lo(a[0]);
                let sign = 1i64 << (w - 1);
                if f == FpOp::Neg {
                    self.c64(sign);
                    self.i(I::I64Xor);
                } else {
                    self.c64(!sign & mask64(w).unwrap_or(-1));
                    self.i(I::I64And);
                }
                return;
            }
            FpOp::Add | FpOp::Sub | FpOp::Mul | FpOp::Div => {
                self.push_f(a[0], w);
                self.push_f(a[1], w);
                self.i(match (f, f32) {
                    (FpOp::Add, true) => I::F32Add,
                    (FpOp::Add, false) => I::F64Add,
                    (FpOp::Sub, true) => I::F32Sub,
                    (FpOp::Sub, false) => I::F64Sub,
                    (FpOp::Mul, true) => I::F32Mul,
                    (FpOp::Mul, false) => I::F64Mul,
                    (_, true) => I::F32Div,
                    _ => I::F64Div,
                });
            }
            FpOp::Sqrt => {
                self.push_f(a[0], w);
                self.i(if f32 { I::F32Sqrt } else { I::F64Sqrt });
            }
            FpOp::MulAdd => {
                for x in a {
                    self.push_f(*x, w);
                }
                self.i(I::Call(if f32 { rt::FMA32 } else { rt::FMA64 }));
            }
            FpOp::RoundInt(r) => {
                self.push_f(a[0], w);
                self.round(r, w);
            }
            FpOp::ToSInt(r) | FpOp::ToUInt(r) => {
                self.push_f(a[0], aw);
                if aw == 32 {
                    self.i(I::F64PromoteF32);
                }
                self.round(r, 64);
                let s = matches!(f, FpOp::ToSInt(_));
                if w == 32 {
                    self.i(if s {
                        I::I32TruncSatF64S
                    } else {
                        I::I32TruncSatF64U
                    });
                    self.i(I::I64ExtendI32U);
                } else {
                    self.i(if s {
                        I::I64TruncSatF64S
                    } else {
                        I::I64TruncSatF64U
                    });
                }
                return;
            }
            FpOp::FromSInt | FpOp::FromUInt => {
                let s = f == FpOp::FromSInt;
                self.push_lo(a[0]);
                if aw == 32 {
                    self.i(I::I32WrapI64);
                    self.i(match (s, f32) {
                        (true, true) => I::F32ConvertI32S,
                        (true, false) => I::F64ConvertI32S,
                        (false, true) => I::F32ConvertI32U,
                        (false, false) => I::F64ConvertI32U,
                    });
                } else {
                    self.i(match (s, f32) {
                        (true, true) => I::F32ConvertI64S,
                        (true, false) => I::F64ConvertI64S,
                        (false, true) => I::F32ConvertI64U,
                        (false, false) => I::F64ConvertI64U,
                    });
                }
            }
            FpOp::Convert => {
                self.push_f(a[0], aw);
                if aw == 32 {
                    self.i(I::F64PromoteF32);
                }
                if f32 {
                    self.i(I::F32DemoteF64);
                }
            }
        }
        self.pop_f(w);
    }

    /// Pushes the `f64` value of the `bits`-bit float `x` (exact).
    fn push_d(&mut self, x: V, bits: u32) {
        match bits {
            16 => {
                self.push_lo(x);
                self.i(I::Call(self.cx.helper(Helper::F16ToF64)));
            }
            32 => {
                self.push_f(x, 32);
                self.i(I::F64PromoteF32);
            }
            _ => self.push_f(x, 64),
        }
    }

    /// Rounds the `f64` on the stack to a `bits`-bit float and pushes its
    /// `i64` bits.
    fn pop_d(&mut self, bits: u32) {
        match bits {
            16 => self.i(I::Call(self.cx.helper(Helper::F64ToF16))),
            32 => {
                self.i(I::F32DemoteF64);
                self.pop_f(32);
            }
            _ => self.pop_f(64),
        }
    }

    /// Converts the integral `f64` on the stack to a `bits`-bit integer,
    /// saturating, with 0 for a NaN; pushes the `i64` bits.
    fn sat_int(&mut self, bits: u32, signed: bool) {
        match (bits, signed) {
            (64, true) => self.i(I::I64TruncSatF64S),
            (64, false) => self.i(I::I64TruncSatF64U),
            (32, _) => {
                self.i(if signed {
                    I::I32TruncSatF64S
                } else {
                    I::I32TruncSatF64U
                });
                self.i(I::I64ExtendI32U);
            }
            _ => {
                let t = Self::t(0);
                let (lo, hi) = if signed {
                    (-(1i64 << (bits - 1)), (1i64 << (bits - 1)) - 1)
                } else {
                    (0, (1i64 << bits) - 1)
                };
                self.i(if signed {
                    I::I64TruncSatF64S
                } else {
                    I::I64TruncSatF64U
                });
                self.set(t);
                self.get(t);
                self.c64(hi);
                self.get(t);
                self.c64(hi);
                self.i(if signed { I::I64LtS } else { I::I64LtU });
                self.i(I::Select);
                if signed {
                    self.set(t);
                    self.get(t);
                    self.c64(lo);
                    self.get(t);
                    self.c64(lo);
                    self.i(I::I64GtS);
                    self.i(I::Select);
                }
                self.and_mask(bits);
            }
        }
    }

    /// Pushes the `i64` result bits of an operation where an operand or
    /// the result is binary16 (or, for the integer conversions, a 16-bit
    /// integer). Operands are widened to binary64 exactly and the result is
    /// rounded once, which is how `relift_ir` defines binary16: binary64
    /// has more than twice binary16's precision plus two bits, so a sum,
    /// difference, product, quotient or square root rounded once more is
    /// correctly rounded; a product of two binary16 values is exact in
    /// binary64, and adding the third operand is either exact or, when the
    /// exponents are 33 or more apart, off by less than the distance to the
    /// nearest binary16 rounding boundary, so the fused multiply-add is too.
    fn fp16(&mut self, f: FpOp, a: &[V], aw: u32, w: u32) {
        match f {
            FpOp::Neg | FpOp::Abs => self.fp(f, a, aw, w),
            FpOp::Add | FpOp::Sub | FpOp::Mul | FpOp::Div => {
                self.push_d(a[0], aw);
                self.push_d(a[1], aw);
                self.i(match f {
                    FpOp::Add => I::F64Add,
                    FpOp::Sub => I::F64Sub,
                    FpOp::Mul => I::F64Mul,
                    _ => I::F64Div,
                });
                self.pop_d(w);
            }
            FpOp::Sqrt => {
                self.push_d(a[0], aw);
                self.i(I::F64Sqrt);
                self.pop_d(w);
            }
            FpOp::MulAdd => {
                self.push_d(a[0], aw);
                self.push_d(a[1], aw);
                self.i(I::F64Mul);
                self.push_d(a[2], aw);
                self.i(I::F64Add);
                self.pop_d(w);
            }
            FpOp::RoundInt(r) => {
                self.push_d(a[0], aw);
                self.round(r, 64);
                self.pop_d(w);
            }
            FpOp::Convert => {
                self.push_d(a[0], aw);
                self.pop_d(w);
            }
            FpOp::ToSInt(r) | FpOp::ToUInt(r) => {
                self.push_d(a[0], aw);
                self.round(r, 64);
                self.sat_int(w, matches!(f, FpOp::ToSInt(_)));
            }
            FpOp::FromSInt | FpOp::FromUInt => {
                let s = f == FpOp::FromSInt;
                self.push_lo(a[0]);
                if s {
                    self.sx64(aw);
                }
                if w == 16 {
                    self.i(if s {
                        I::F64ConvertI64S
                    } else {
                        I::F64ConvertI64U
                    });
                    self.pop_d(16);
                } else {
                    self.i(match (s, w == 32) {
                        (true, true) => I::F32ConvertI64S,
                        (true, false) => I::F64ConvertI64S,
                        (false, true) => I::F32ConvertI64U,
                        (false, false) => I::F64ConvertI64U,
                    });
                    self.pop_f(w);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::pieces;

    #[test]
    fn accesses_split_in_address_order() {
        assert_eq!(pieces(1), vec![(0, 1)]);
        assert_eq!(pieces(3), vec![(0, 2), (2, 1)]);
        assert_eq!(pieces(7), vec![(0, 4), (4, 2), (6, 1)]);
        assert_eq!(pieces(8), vec![(0, 8)]);
        assert_eq!(pieces(11), vec![(0, 8), (8, 2), (10, 1)]);
        assert_eq!(pieces(16), vec![(0, 8), (8, 8)]);
        for n in 1..=16 {
            let p = pieces(n);
            assert_eq!(p.iter().map(|x| x.1).sum::<u32>(), n);
            assert!(p.iter().all(|(o, s)| o / 8 == (o + s - 1) / 8));
        }
    }
}

//! Runs one lifted block (an instruction, or a branch with its delay slot)
//! over the abstract state.

use relift_cpu_mips::lift::{EXC_GUARD, IN_DELAY};
use relift_ir::interp::{eval_bin, eval_cmp, eval_un};
use relift_ir::{BinOp, Block, CmpOp, ExtKind, Next, Op, Value, Width};

use crate::val::{
    Abs, Base, PHYS, PI_CART, PI_DRAM, SLOTS, SP_DRAM, SP_MEM, State, Sym, UNBOUNDED, frame_bits,
    known, mask, slot_of,
};

/// The PI registers (physical addresses).
pub(crate) const PI_DRAM_ADDR: u32 = 0x0460_0000;
pub(crate) const PI_CART_ADDR: u32 = 0x0460_0004;
pub(crate) const PI_WR_LEN: u32 = 0x0460_000C;
/// The SP registers.
pub(crate) const SP_MEM_ADDR: u32 = 0x0404_0000;
pub(crate) const SP_DRAM_ADDR: u32 = 0x0404_0004;
pub(crate) const SP_RD_LEN: u32 = 0x0404_0008;

/// The exception codes of a TLB miss on a load and on a store.
const TLB_CODES: [u32; 2] = [2, 3];
/// The exception code of SYSCALL.
const SYS_CODE: u32 = 8;

/// A DMA the code starts, with the register values at that point.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum Dma {
    /// Cartridge to RDRAM: RDRAM address, cartridge address, length
    /// minus one.
    Pi { dram: Abs, cart: Abs, len: Abs },
    /// RDRAM to SP memory: SP address, RDRAM address, the length word.
    Sp { mem: Abs, dram: Abs, len: Abs },
}

/// How the block ends besides its exit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum End {
    /// It runs to its exit.
    Exit,
    /// It always raises an exception that does not come back.
    Raise,
    /// It is a system call: the handler comes back after it.
    Syscall,
}

/// What one block does.
pub(crate) struct Out {
    pub end: End,
    /// The state after it.
    pub st: State,
    /// The candidate next program counters.
    pub targets: Vec<Abs>,
    /// The slot written with the address after the block (a link).
    pub link: Option<usize>,
    /// DMAs it starts.
    pub dma: Vec<Dma>,
    /// Constant reads: physical address and size.
    pub data: Vec<(u32, u8)>,
    /// Code-like constants the block stores to memory.
    pub consts: Vec<u32>,
    /// Physical base addresses of loads and stores through a constant or
    /// a table index: data, not code.
    pub bases: Vec<u32>,
}

/// Reads `n` bytes of the image at a physical address, recording the read.
pub(crate) type Reader<'a> = dyn FnMut(u32, u32) -> Option<u64> + 'a;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Ev {
    A(Abs),
    /// The bits from `lo` up of a symbol.
    Hi(Sym, u8),
}

fn wid(w: u32) -> Width {
    Width::new(w)
}

/// The value with known bits `m` = `v` at width `w`.
fn bits(m: u128, v: u128, w: u32) -> Abs {
    let m = m & mask(w);
    if m == mask(w) {
        Abs::K(v & m)
    } else if m == 0 {
        Abs::Top
    } else {
        Abs::B {
            mask: m,
            val: v & m,
        }
    }
}

fn bin_bits(op: BinOp, a: Abs, b: Abs, w: u32) -> Abs {
    let (ma, va) = known(a, w);
    let (mb, vb) = known(b, w);
    let full = mask(w);
    match (op, b) {
        (BinOp::And, _) => bits((ma & mb) | (ma & !va) | (mb & !vb), va & vb, w),
        (BinOp::Or, _) => bits((ma & mb) | (ma & va) | (mb & vb), va | vb, w),
        (BinOp::Xor, _) => bits(ma & mb, va ^ vb, w),
        (BinOp::Shl, Abs::K(s)) if s < u128::from(w) => {
            bits((ma << s) | ((1u128 << s) - 1), va << s, w)
        }
        (BinOp::LShr, Abs::K(s)) if s < u128::from(w) => {
            let hi = full & !(full >> s);
            bits((ma >> s) | hi, va >> s, w)
        }
        _ => Abs::Top,
    }
}

/// The power of two an unknown value is a multiple of, from its known low
/// zero bits.
fn stride_of(a: Abs) -> Option<u32> {
    match a {
        Abs::Top => Some(1),
        Abs::B { mask: m, val } => {
            let tz = (!m).trailing_zeros().min(val.trailing_zeros()).min(12);
            Some(1 << tz)
        }
        Abs::Idx {
            base: 0, stride, ..
        } => Some(stride),
        _ => None,
    }
}

/// How many multiples of `stride` from zero `x` can be, by its known bits.
fn count(x: Abs, stride: u32) -> u32 {
    match x {
        Abs::B { mask: m, val } => match u32::try_from((val | !m) & 0xFFFF_FFFF) {
            Ok(max) if max != u32::MAX && (val | !m) >> 32 & 0xFFFF_FFFF == 0 => max / stride + 1,
            _ => UNBOUNDED,
        },
        Abs::Idx { base: 0, n, .. } => n,
        _ => UNBOUNDED,
    }
}

fn is_sym(x: Abs) -> bool {
    matches!(x, Abs::S(_) | Abs::Idx { .. } | Abs::Tab { .. })
}

/// A constant plus an unknown multiple of four or more: a table index.
fn index(op: BinOp, a: Abs, b: Abs, w: u32) -> Option<Abs> {
    if w < 32 || op != BinOp::Add {
        return None;
    }
    let (k, x) = match (a, b) {
        (Abs::K(k), x) | (x, Abs::K(k)) => (k as u32, x),
        _ => return None,
    };
    match x {
        Abs::Idx { base, stride, n } => Some(Abs::Idx {
            base: base.wrapping_add(k),
            stride,
            n,
        }),
        Abs::B { .. } | Abs::Top if k >> 30 == 2 || (0x400..0x0080_0000).contains(&k) => {
            let stride = stride_of(x)?;
            (stride >= 4).then_some(Abs::Idx {
                base: k,
                stride,
                n: count(x, stride),
            })
        }
        _ => None,
    }
}

/// A value not known times a constant from 4 to 4 KiB: a multiple of it.
fn scaled(op: BinOp, a: Abs, b: Abs, w: u32) -> Option<Abs> {
    if w < 32 || op != BinOp::Mul {
        return None;
    }
    let (k, x) = match (a, b) {
        (Abs::K(k), x) | (x, Abs::K(k)) => (u32::try_from(k).ok()?, x),
        _ => return None,
    };
    if !(4..=0x1000).contains(&k) || is_sym(x) && !matches!(x, Abs::Idx { base: 0, .. }) {
        return None;
    }
    let s = match x {
        Abs::B { .. } | Abs::Idx { base: 0, .. } => stride_of(x)?,
        _ => 1,
    };
    let stride = k.checked_mul(s).filter(|s| *s <= 0x1000)?;
    Some(Abs::Idx {
        base: 0,
        stride,
        n: count(x, s),
    })
}

fn bin(op: BinOp, a: Abs, b: Abs, w: u32) -> Abs {
    use Abs::{K, S};
    if let (K(x), K(y)) = (a, b) {
        return K(eval_bin(op, x, y, wid(w)) & mask(w));
    }
    if let Some(i) = index(op, a, b, w) {
        return i;
    }
    if let Some(i) = scaled(op, a, b, w) {
        return i;
    }
    if !is_sym(a) && !is_sym(b) {
        return bin_bits(op, a, b, w);
    }
    if w < 32 {
        return Abs::Top;
    }
    match (op, a, b) {
        (BinOp::Add | BinOp::Or | BinOp::Xor | BinOp::Sub, x, K(0))
        | (BinOp::Add | BinOp::Or | BinOp::Xor, K(0), x) => x,
        (BinOp::Shl | BinOp::LShr | BinOp::AShr, x, K(0)) => x,
        (BinOp::Add, S(s), K(k)) | (BinOp::Add, K(k), S(s)) if !s.phys => S(Sym {
            off: s.off.wrapping_add(k as u32),
            ..s
        }),
        (BinOp::Sub, S(s), K(k)) if !s.phys => S(Sym {
            off: s.off.wrapping_sub(k as u32),
            ..s
        }),
        (BinOp::Sub, S(s), S(t)) if s.base == t.base && s.phys == t.phys => {
            let d = s.off.wrapping_sub(t.off);
            K(if w == 32 {
                u128::from(d)
            } else {
                crate::val::sext32(d) & mask(w)
            })
        }
        (BinOp::And, S(s), K(m)) | (BinOp::And, K(m), S(s)) if m as u32 == PHYS => {
            S(Sym { phys: true, ..s })
        }
        (BinOp::And, Abs::Idx { base, stride, n }, K(m))
        | (BinOp::And, K(m), Abs::Idx { base, stride, n })
            if m as u32 == PHYS && base >> 30 == 2 =>
        {
            Abs::Idx {
                base: base & PHYS,
                stride,
                n,
            }
        }
        (BinOp::And, S(s), K(m)) | (BinOp::And, K(m), S(s)) if m & 3 == 0 && m as u32 >> 2 != 0 => {
            if s.base == Base::Entry(crate::val::SP as u8) && m as u32 | 7 == u32::MAX {
                S(s)
            } else {
                bits(!m, 0, w)
            }
        }
        (BinOp::And, _, K(m)) | (BinOp::And, K(m), _) => bits(!m, 0, w),
        (BinOp::Shl, _, K(k)) if k < u128::from(w) => bits((1 << k) - 1, 0, w),
        _ => Abs::Top,
    }
}

fn cmp(op: CmpOp, a: Abs, b: Abs, w: u32) -> Abs {
    match (a, b) {
        (Abs::K(x), Abs::K(y)) => Abs::K(u128::from(eval_cmp(op, x, y, wid(w)))),
        (Abs::S(s), Abs::S(t)) if s.base == t.base && s.phys == t.phys => match op {
            CmpOp::Eq => Abs::K(u128::from(s.off == t.off)),
            CmpOp::Ne => Abs::K(u128::from(s.off != t.off)),
            _ => Abs::Top,
        },
        (Abs::K(_) | Abs::B { .. }, Abs::K(_) | Abs::B { .. })
            if matches!(op, CmpOp::Eq | CmpOp::Ne) =>
        {
            let (ma, va) = known(a, w);
            let (mb, vb) = known(b, w);
            if (va ^ vb) & ma & mb != 0 {
                Abs::K(u128::from(op == CmpOp::Ne))
            } else {
                Abs::Top
            }
        }
        _ => Abs::Top,
    }
}

/// The value of an extract of `lo..lo+w` from a symbol the stack pointer
/// is relative to: its low bits are those of the offset, since the entry
/// stack pointer is doubleword aligned.
fn sp_low(s: Sym, lo: u8, w: u32) -> Option<Abs> {
    (s.base == Base::Entry(crate::val::SP as u8) && u32::from(lo) + w <= 3)
        .then(|| Abs::K(u128::from(s.off >> lo) & mask(w)))
}

struct Ctx<'a> {
    b: &'a Block,
    vals: Vec<Ev>,
}

impl Ctx<'_> {
    fn get(&self, v: Value) -> Abs {
        match self.vals.get(v.index()) {
            Some(Ev::A(a)) => *a,
            _ => Abs::Top,
        }
    }

    fn width(&self, v: Value) -> u32 {
        self.b
            .insts
            .get(v.index())
            .and_then(|i| i.width)
            .map_or(0, Width::bits)
    }
}

/// Runs `b` from `st`. `read` gives image bytes at constant physical
/// addresses.
pub(crate) fn eval(b: &Block, st: &State, read: &mut Reader<'_>) -> Out {
    let mut out = Out {
        end: End::Exit,
        st: st.clone(),
        targets: Vec::new(),
        link: None,
        dma: Vec::new(),
        data: Vec::new(),
        consts: Vec::new(),
        bases: Vec::new(),
    };
    let mut cx = Ctx {
        b,
        vals: Vec::with_capacity(b.insts.len()),
    };
    let mut pend: Vec<(usize, Abs)> = Vec::new();
    let mut unmapped = false;
    for inst in &b.insts {
        let w = inst.width.map_or(0, Width::bits);
        let r = match &inst.op {
            Op::Const(k) => Ev::A(Abs::K(k.get() & mask(w))),
            Op::Get(r) => Ev::A(match slot_of(*r) {
                Some(0) => Abs::K(0),
                Some(i) => st.regs[i],
                None => Abs::Top,
            }),
            Op::Set(r, v) => {
                if let Some(i) = slot_of(*r) {
                    pend.push((i, cx.get(*v)));
                }
                Ev::A(Abs::Top)
            }
            Op::Un(op, x) => Ev::A(match cx.get(*x) {
                Abs::K(a) => Abs::K(eval_un(*op, a, wid(w)) & mask(w)),
                _ => Abs::Top,
            }),
            Op::Bin(op, x, y) => Ev::A(bin(*op, cx.get(*x), cx.get(*y), w)),
            Op::Cmp(op, x, y) => Ev::A(cmp(*op, cx.get(*x), cx.get(*y), cx.width(*x))),
            Op::Select(c, x, y) => {
                let (a, bb) = (cx.get(*x), cx.get(*y));
                Ev::A(match cx.get(*c) {
                    Abs::K(1) => a,
                    Abs::K(_) => bb,
                    _ if a == bb => a,
                    _ => {
                        let (ma, va) = known(a, w);
                        let (mb, vb) = known(bb, w);
                        bits(ma & mb & !(va ^ vb), va, w)
                    }
                })
            }
            Op::Ext(kind, x) => {
                let wx = cx.width(*x);
                Ev::A(match cx.get(*x) {
                    Abs::K(a) => match kind {
                        ExtKind::Zero => Abs::K(a),
                        ExtKind::Sign => {
                            let sh = 128 - wx.min(128);
                            Abs::K((((a << sh) as i128 >> sh) as u128) & mask(w))
                        }
                    },
                    a @ (Abs::S(_) | Abs::Idx { .. } | Abs::Tab { .. })
                        if wx == 32 && *kind == ExtKind::Sign || wx >= 64 =>
                    {
                        a
                    }
                    Abs::S(s) if wx == 32 && s.phys => Abs::S(s),
                    a @ Abs::B { .. } => {
                        let (m, v) = known(a, wx);
                        let top = mask(w) & !mask(wx);
                        match kind {
                            ExtKind::Zero => bits(m | top, v, w),
                            ExtKind::Sign if m >> (wx - 1) & 1 == 1 => {
                                let s = if v >> (wx - 1) & 1 == 1 { top } else { 0 };
                                bits(m | top, v | s, w)
                            }
                            ExtKind::Sign => bits(m, v, w),
                        }
                    }
                    _ if *kind == ExtKind::Zero && wx < w => bits(mask(w) & !mask(wx), 0, w),
                    _ => Abs::Top,
                })
            }
            Op::Extract(x, lo) => {
                let wx = cx.width(*x);
                match cx.get(*x) {
                    Abs::K(a) => Ev::A(Abs::K((a >> lo) & mask(w))),
                    a @ (Abs::S(_) | Abs::Idx { .. } | Abs::Tab { .. }) if *lo == 0 && w == 32 => {
                        Ev::A(a)
                    }
                    Abs::S(s) => match sp_low(s, *lo, w) {
                        Some(v) => Ev::A(v),
                        None if w + u32::from(*lo) == 32 && wx >= 32 => Ev::Hi(s, *lo),
                        None => Ev::A(Abs::Top),
                    },
                    a @ Abs::B { .. } => {
                        let (m, v) = known(a, wx);
                        Ev::A(bits(m >> lo, v >> lo, w))
                    }
                    _ => Ev::A(Abs::Top),
                }
            }
            Op::Concat(h, l) => {
                let wl = cx.width(*l);
                let hv = cx.vals.get(h.index()).copied();
                Ev::A(match (hv, cx.get(*l)) {
                    (Some(Ev::A(Abs::K(x))), Abs::K(y)) if wl < 128 => Abs::K((x << wl) | y),
                    (Some(Ev::Hi(s, lo)), Abs::K(0)) if u32::from(lo) == wl => Abs::S(s),
                    (Some(Ev::A(hv)), lv) if wl < 128 => {
                        let (mh, vh) = known(hv, w - wl);
                        let (ml, vl) = known(lv, wl);
                        bits((mh << wl) | ml, (vh << wl) | vl, w)
                    }
                    _ => Abs::Top,
                })
            }
            Op::Load { addr, .. } | Op::IoLoad { addr, .. } => {
                let a = cx.get(*addr);
                base_of(&mut out, a);
                Ev::A(if unmapped {
                    Abs::Top
                } else {
                    load(&mut out, a, w / 8, read)
                })
            }
            Op::Store { addr, value, .. } | Op::IoStore { addr, value, .. } => {
                base_of(&mut out, cx.get(*addr));
                if !unmapped {
                    let n = (cx.width(*value) / 8) as u8;
                    store(&mut out, cx.get(*addr), n, cx.get(*value));
                }
                Ev::A(Abs::Top)
            }
            Op::Guard(c, code) => {
                let exc = (code & !IN_DELAY).checked_sub(EXC_GUARD);
                match cx.get(*c) {
                    Abs::K(0) => {}
                    Abs::K(_) => match exc {
                        Some(e) if TLB_CODES.contains(&e) => unmapped = true,
                        Some(SYS_CODE) => {
                            out.end = End::Syscall;
                            return out;
                        }
                        Some(_) => {
                            out.end = End::Raise;
                            return out;
                        }
                        None => {}
                    },
                    _ => {
                        if let Some(Ev::A(slot)) = cx.vals.get_mut(c.index()) {
                            *slot = Abs::K(0);
                        }
                    }
                }
                Ev::A(Abs::Top)
            }
            _ => Ev::A(Abs::Top),
        };
        cx.vals.push(r);
    }
    let after = sext(b.pc.wrapping_add(u64::from(b.len)));
    for (i, v) in pend {
        if i < SLOTS {
            if v == Abs::K(after) && b.len == 8 {
                out.link = Some(i);
            }
            out.st.regs[i] = v;
        }
    }
    out.targets = match b.exit.next {
        Next::Const(c) => vec![Abs::K(u128::from(c))],
        Next::Select {
            cond,
            taken,
            not_taken,
        } => match cx.get(cond) {
            Abs::K(1) => vec![Abs::K(u128::from(taken))],
            Abs::K(_) => vec![Abs::K(u128::from(not_taken))],
            _ => vec![Abs::K(u128::from(taken)), Abs::K(u128::from(not_taken))],
        },
        Next::Dynamic(v) => {
            let a = cx.get(v);
            match (a, b.insts.get(v.index()).map(|i| &i.op)) {
                (Abs::K(_), _) => vec![a],
                (_, Some(Op::Select(c, x, y))) if cx.get(*c).k().is_none() => {
                    vec![cx.get(*x), cx.get(*y)]
                }
                _ => vec![a],
            }
        }
    };
    out
}

fn sext(x: u64) -> u128 {
    u128::from(x)
}

/// A constant that is a word-aligned kseg0 or kseg1 address, as 32 bits
/// or sign-extended to 64.
pub(crate) fn code_like(k: u128) -> Option<u32> {
    let ok = k >> 32 == 0 || k >> 32 == 0xFFFF_FFFF;
    (ok && (k as u32) >> 30 == 2 && k & 3 == 0).then_some(k as u32)
}

fn base_of(out: &mut Out, a: Abs) {
    match a {
        Abs::K(x) => out.bases.push(x as u32 & PHYS),
        Abs::Idx { base, .. } => out.bases.push(base & PHYS),
        _ => {}
    }
}

fn load(out: &mut Out, addr: Abs, n: u32, read: &mut Reader<'_>) -> Abs {
    if let Abs::Idx { base, stride, n: c } = addr
        && n == 4
        && base % 4 == 0
        && stride % 4 == 0
    {
        return Abs::Tab { base, stride, n: c };
    }
    match addr {
        Abs::K(a) => {
            let a = a as u32 & PHYS;
            match read(a, n) {
                Some(v) => {
                    out.data.push((a, n as u8));
                    Abs::K(u128::from(v))
                }
                None => Abs::Top,
            }
        }
        _ => match addr.stack_off() {
            Some(o) => stack_load(&out.st, o, n),
            None => Abs::Top,
        },
    }
}

/// The word at stack offset `o` of `st`.
pub(crate) fn stack_value(st: &State, o: i32) -> Abs {
    stack_load(st, o, 4)
}

fn stack_load(st: &State, o: i32, n: u32) -> Abs {
    for &(eo, es, v) in &st.stack {
        if eo == o && u32::from(es) == n {
            return v;
        }
        if es == 8 && n == 4 && eo + 4 == o {
            return match v {
                Abs::K(x) => Abs::K(x & 0xFFFF_FFFF),
                Abs::S(_) => v,
                _ => Abs::Top,
            };
        }
        if eo < o + n as i32 && o < eo + i32::from(es) {
            return Abs::Top;
        }
    }
    if o >= 0 && n == 4 && o % 4 == 0 && st.frame_dirty & frame_bits(o, 4) == 0 && o < 256 {
        return Abs::sym(Base::Frame(o), 0);
    }
    Abs::Top
}

fn store(out: &mut Out, addr: Abs, n: u8, v: Abs) {
    if let Abs::K(k) = v
        && n >= 4
        && let Some(c) = code_like(k)
    {
        out.consts.push(c);
    }
    if let Some(o) = addr.stack_off() {
        let end = o + i32::from(n);
        out.st
            .stack
            .retain(|e| e.0 + i32::from(e.1) <= o || e.0 >= end);
        if o >= 0 {
            out.st.frame_dirty |= frame_bits(o, n);
        }
        if (n == 4 || n == 8) && o % i32::from(n) == 0 {
            let at = out.st.stack.partition_point(|e| e.0 < o);
            out.st.stack.insert(at, (o, n, v));
        }
        return;
    }
    let Abs::K(a) = addr else {
        return;
    };
    let a = a as u32 & PHYS;
    let st = &mut out.st;
    let v32 = match v {
        Abs::K(x) => Abs::K(x & 0xFFFF_FFFF),
        other => other,
    };
    match a {
        PI_DRAM_ADDR => st.regs[PI_DRAM] = v32,
        PI_CART_ADDR => st.regs[PI_CART] = v32,
        PI_WR_LEN => out.dma.push(crate::eval::Dma::Pi {
            dram: st.regs[PI_DRAM],
            cart: st.regs[PI_CART],
            len: v32,
        }),
        SP_MEM_ADDR => st.regs[SP_MEM] = v32,
        SP_DRAM_ADDR => st.regs[SP_DRAM] = v32,
        SP_RD_LEN => out.dma.push(crate::eval::Dma::Sp {
            mem: st.regs[SP_MEM],
            dram: st.regs[SP_DRAM],
            len: v32,
        }),
        _ => {}
    }
}

//! Runs one lifted instruction over the abstract state.

use relift_cpu_arm32::decode::Encoding;
use relift_cpu_arm32::lift::COND_FAIL;
use relift_ir::interp::{eval_bin, eval_cmp, eval_un};
use relift_ir::{BinOp, Block, CmpOp, ExtKind, Flow, Next, Op, Value, Width};

use crate::image::Image;
use crate::loc::{Loc, Region};
use crate::val::{Abs, Base, DMA_BASE, DMA_HALVES, SP, State, Sym};

/// A DMA channel enabled with the register values at that point.
#[derive(Clone, Copy, Debug)]
pub(crate) struct DmaStart {
    pub channel: u8,
    pub src: Abs,
    pub dst: Abs,
    pub count: Abs,
    pub control: u16,
}

/// What one instruction does.
pub(crate) struct Out {
    /// The condition may fail: the instruction may do nothing.
    pub may_skip: bool,
    /// The condition may hold.
    pub may_run: bool,
    /// The state after it, when it runs.
    pub st: State,
    /// The candidate next program counters.
    pub targets: Vec<Abs>,
    /// The exit kind.
    pub flow: Flow,
    /// Stores outside the stack frame: address, size, value.
    pub stores: Vec<(Abs, u8, Abs)>,
    /// Constant reads of ROM or of the code's own copy: address, size.
    pub data: Vec<(u32, u8)>,
    /// DMA channels it enables.
    pub dma: Vec<DmaStart>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Ev {
    A(Abs),
    /// The bits from `lo` up of a symbol.
    Hi(Sym, u8),
    /// The bits from `lo` up of a table index.
    HiIdx(u32, u32, u8),
    /// The bits from `lo` up of a table word.
    HiTab(u32, u32, u8),
}

fn mask(w: u32) -> u128 {
    if w >= 128 {
        u128::MAX
    } else {
        (1u128 << w) - 1
    }
}

fn wid(w: u32) -> Width {
    Width::new(w)
}

fn align(s: Sym, k: u8) -> Abs {
    if s.base == Base::Entry(SP) && k <= 2 {
        Abs::S(Sym {
            off: s.off & !((1u32 << k) - 1),
            ..s
        })
    } else {
        Abs::S(Sym {
            align: s.align.max(k),
            ..s
        })
    }
}

/// The known bits of `a` at width `w`: mask and values.
fn known(a: Abs, w: u32) -> (u128, u128) {
    match a {
        Abs::K(x) => (mask(w), x & mask(w)),
        Abs::B { mask: m, val } => (m & mask(w), val & m & mask(w)),
        _ => (0, 0),
    }
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

/// The power of two an unknown value is a multiple of, from its known
/// low zero bits.
fn stride_of(a: Abs) -> Option<u32> {
    match a {
        Abs::Top => Some(1),
        Abs::B { mask: m, val } => {
            let tz = (!m).trailing_zeros().min(val.trailing_zeros()).min(12);
            Some(1 << tz)
        }
        Abs::Idx { base: 0, stride } => Some(stride),
        _ => None,
    }
}

fn index(op: BinOp, a: Abs, b: Abs, w: u32) -> Option<Abs> {
    if w < 32 || op != BinOp::Add {
        return None;
    }
    let (k, x) = match (a, b) {
        (Abs::K(k), x) | (x, Abs::K(k)) => (u32::try_from(k).ok()?, x),
        _ => return None,
    };
    if let Abs::B { mask: m, .. } = x
        && w > 32
        && m >> 32 != mask(w) >> 32
    {
        return None;
    }
    match x {
        Abs::Idx { base, stride } => Some(Abs::Idx {
            base: base.wrapping_add(k),
            stride,
        }),
        _ if Region::of(k) == Region::Rom || Region::of(k).is_ram() => {
            let stride = stride_of(x)?;
            (stride >= 4).then_some(Abs::Idx { base: k, stride })
        }
        _ => None,
    }
}

/// A value not known times a constant from 4 to 4 KiB: a multiple of the
/// constant (and of the value's own known power of two), the index of a
/// table with that stride.
fn scaled(op: BinOp, a: Abs, b: Abs, w: u32) -> Option<Abs> {
    if w < 32 || op != BinOp::Mul {
        return None;
    }
    let (k, x) = match (a, b) {
        (Abs::K(k), x) | (x, Abs::K(k)) => (u32::try_from(k).ok()?, x),
        _ => return None,
    };
    if !(4..=0x1000).contains(&k) {
        return None;
    }
    let s = match x {
        Abs::B { .. } | Abs::Idx { base: 0, .. } => stride_of(x)?,
        _ => 1,
    };
    let stride = k.checked_mul(s).filter(|s| *s <= 0x1000)?;
    Some(Abs::Idx { base: 0, stride })
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
    let sym = |x: Abs| matches!(x, S(_) | Abs::Idx { .. } | Abs::Tab { .. });
    if !sym(a) && !sym(b) {
        return bin_bits(op, a, b, w);
    }
    if w < 32 {
        return Abs::Top;
    }
    match (op, a, b) {
        (BinOp::Add | BinOp::Or | BinOp::Xor | BinOp::Sub, x, K(0))
        | (BinOp::Add | BinOp::Or | BinOp::Xor, K(0), x) => x,
        (BinOp::Shl | BinOp::LShr | BinOp::AShr | BinOp::RotR, x, K(0)) => x,
        (BinOp::Add, S(s), K(k)) | (BinOp::Add, K(k), S(s)) if s.align == 0 => S(Sym {
            off: s.off.wrapping_add(k as u32),
            ..s
        }),
        (BinOp::Sub, S(s), K(k)) if s.align == 0 => S(Sym {
            off: s.off.wrapping_sub(k as u32),
            ..s
        }),
        (BinOp::Sub, S(s), S(t)) if w == 32 && s.base == t.base && s.align == 0 && t.align == 0 => {
            K(u128::from(s.off.wrapping_sub(t.off)))
        }
        (BinOp::And, S(s), K(m)) | (BinOp::And, K(m), S(s)) if w == 32 => match m {
            0xFFFF_FFFF => S(s),
            0xFFFF_FFFE => align(s, 1),
            0xFFFF_FFFC => align(s, 2),
            _ => Abs::Top,
        },
        _ => Abs::Top,
    }
}

fn cmp(op: CmpOp, a: Abs, b: Abs, w: u32) -> Abs {
    match (a, b) {
        (Abs::K(x), Abs::K(y)) => Abs::K(u128::from(eval_cmp(op, x, y, wid(w)))),
        (Abs::S(s), Abs::S(t)) if w == 32 && s.base == t.base && s.align == t.align => match op {
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

fn dma_store(st: &mut State, addr: u32, n: u8, v: Abs, out: &mut Vec<DmaStart>) {
    let Some(off) = addr.checked_sub(DMA_BASE) else {
        return;
    };
    let first = (off / 2) as usize;
    let halves = usize::from(n).div_ceil(2);
    if first >= DMA_HALVES {
        return;
    }
    for h in 0..halves {
        let i = first + h;
        if i >= DMA_HALVES {
            break;
        }
        let part = match (v, n) {
            (Abs::K(x), 2 | 4) => Abs::K((x >> (16 * h)) & 0xFFFF),
            _ => Abs::Top,
        };
        st.dma[i] = part;
        if i % 6 == 5
            && let Abs::K(c) = part
            && c & 0x8000 != 0
        {
            let base = i - 5;
            let word = |j: usize| match (st.dma[base + j], st.dma[base + j + 1]) {
                (Abs::K(lo), Abs::K(hi)) => Abs::K(lo | hi << 16),
                _ => Abs::Top,
            };
            out.push(DmaStart {
                channel: (i / 6) as u8,
                src: word(0),
                dst: word(2),
                count: st.dma[base + 4],
                control: c as u16,
            });
        }
    }
}

/// Memory known byte by byte, for a run with constant inputs: work RAM
/// and the run's own stack.
pub(crate) type Scratch = std::collections::HashMap<u32, u8>;

/// The run's private stack, below this address.
pub(crate) const SCRATCH_STACK: u32 = 0x0F01_0000;

pub(crate) fn in_scratch(a: u32) -> bool {
    Region::of(a).is_ram() || (SCRATCH_STACK - 0x1_0000..SCRATCH_STACK).contains(&a)
}

/// Whether the instruction `word` loads relative to the program counter
/// (a literal): the only reads of code's own RAM copy taken as constants,
/// since the rest of a copy may be variables the program changes.
pub(crate) fn pc_relative(enc: Encoding, word: u32, thumb: bool) -> bool {
    if thumb {
        return enc == Encoding::LDR_l_T1;
    }
    let load = word >> 20 & 1 == 1;
    let rn_pc = word >> 16 & 0xF == 15;
    let single = word >> 26 & 3 == 1;
    let half = word >> 25 & 7 == 0 && word >> 7 & 1 == 1 && word >> 4 & 1 == 1;
    load && rn_pc && (single || half)
}

/// Runs `b`, the instruction at `loc`, from state `st`; with `scratch`,
/// constant addresses in work RAM and the private stack read and write it.
/// `literal` tells that the instruction is a [`pc_relative`] load.
pub(crate) fn eval(
    b: &Block,
    loc: Loc,
    st: &State,
    image: &Image<'_>,
    literal: bool,
    mut scratch: Option<&mut Scratch>,
) -> Out {
    let mut out = Out {
        may_skip: false,
        may_run: true,
        st: st.clone(),
        targets: Vec::new(),
        flow: b.exit.flow,
        stores: Vec::new(),
        data: Vec::new(),
        dma: Vec::new(),
    };
    let mut cx = Ctx {
        b,
        vals: Vec::with_capacity(b.insts.len()),
    };
    let mut pend: Vec<(usize, Abs)> = Vec::new();
    for inst in &b.insts {
        let w = inst.width.map_or(0, Width::bits);
        let r = match &inst.op {
            Op::Const(k) => Ev::A(Abs::K(k.get() & mask(w))),
            Op::Get(r) => Ev::A(st.regs.get(r.index()).copied().unwrap_or(Abs::Top)),
            Op::Set(r, v) => {
                pend.push((r.index(), cx.get(*v)));
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
                    a @ (Abs::S(_) | Abs::Idx { .. } | Abs::Tab { .. }) if wx >= 32 && w >= wx => a,
                    a @ Abs::B { .. } if *kind == ExtKind::Zero => {
                        let (m, v) = known(a, wx);
                        bits(m | (mask(w) & !mask(wx)), v, w)
                    }
                    _ => Abs::Top,
                })
            }
            Op::Extract(x, lo) => match cx.get(*x) {
                Abs::K(a) => Ev::A(Abs::K((a >> lo) & mask(w))),
                Abs::S(s) if *lo == 0 && w == 32 => Ev::A(Abs::S(s)),
                a @ (Abs::Idx { .. } | Abs::Tab { .. }) if *lo == 0 && w == 32 => Ev::A(a),
                Abs::S(s) if cx.width(*x) == 32 && w + u32::from(*lo) == 32 => Ev::Hi(s, *lo),
                Abs::Idx { base, stride } if w + u32::from(*lo) <= stride.trailing_zeros() => {
                    Ev::A(Abs::K(u128::from(base >> lo) & mask(w)))
                }
                Abs::Tab { base, stride }
                    if cx.width(*x) == 32 && w + u32::from(*lo) == 32 && *lo > 0 =>
                {
                    Ev::HiTab(base, stride, *lo)
                }
                Abs::Idx { base, stride } if cx.width(*x) == 32 && w + u32::from(*lo) == 32 => {
                    Ev::HiIdx(base, stride, *lo)
                }
                a @ Abs::B { .. } => {
                    let (m, v) = known(a, cx.width(*x));
                    Ev::A(bits(m >> lo, v >> lo, w))
                }
                _ => Ev::A(Abs::Top),
            },
            Op::Concat(h, l) => {
                let wl = cx.width(*l);
                let hv = cx.vals.get(h.index()).copied();
                Ev::A(match (hv, cx.get(*l)) {
                    (Some(Ev::A(Abs::K(x))), Abs::K(y)) if wl < 128 => Abs::K((x << wl) | y),
                    (Some(Ev::Hi(s, lo)), Abs::K(0)) if u32::from(lo) == wl => align(s, lo),
                    (Some(Ev::HiTab(base, stride, lo)), Abs::K(0)) if u32::from(lo) == wl => {
                        Abs::Tab { base, stride }
                    }
                    (Some(Ev::HiIdx(base, stride, lo)), Abs::K(0))
                        if u32::from(lo) == wl && (base | stride) & ((1 << lo) - 1) == 0 =>
                    {
                        Abs::Idx { base, stride }
                    }
                    (Some(Ev::A(hv)), lv) if wl < 128 => {
                        let (mh, vh) = known(hv, w - wl);
                        let (ml, vl) = known(lv, wl);
                        bits((mh << wl) | ml, (vh << wl) | vl, w)
                    }
                    _ => Abs::Top,
                })
            }
            Op::Load { addr, .. } | Op::IoLoad { addr, .. } => Ev::A(load(
                &mut out,
                loc,
                image,
                cx.get(*addr),
                w / 8,
                literal,
                scratch.as_deref(),
            )),
            Op::Store { addr, value, .. } | Op::IoStore { addr, value, .. } => {
                let n = (cx.width(*value) / 8) as u8;
                store(
                    &mut out,
                    cx.get(*addr),
                    n,
                    cx.get(*value),
                    scratch.as_deref_mut(),
                );
                Ev::A(Abs::Top)
            }
            Op::Guard(c, code) => {
                match (cx.get(*c), *code) {
                    (Abs::K(0), _) => {}
                    (Abs::K(_), COND_FAIL) => {
                        out.may_skip = true;
                        out.may_run = false;
                        return out;
                    }
                    (_, COND_FAIL) => out.may_skip = true,
                    _ => {
                        out.may_run = false;
                        return out;
                    }
                }
                Ev::A(Abs::Top)
            }
            _ => Ev::A(Abs::Top),
        };
        cx.vals.push(r);
    }
    for (i, v) in pend {
        if let Some(slot) = out.st.regs.get_mut(i) {
            *slot = v;
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
                (Abs::Top, Some(Op::Select(c, x, y))) if cx.get(*c) == Abs::Top => {
                    vec![cx.get(*x), cx.get(*y)]
                }
                _ => vec![a],
            }
        }
    };
    out
}

fn load(
    out: &mut Out,
    loc: Loc,
    image: &Image<'_>,
    addr: Abs,
    n: u32,
    literal: bool,
    scratch: Option<&Scratch>,
) -> Abs {
    if let Abs::Idx { base, stride } = addr
        && n == 4
        && base % 4 == 0
        && stride % 4 == 0
        && (image.in_image(base) || !image.copies_at(base).is_empty())
    {
        return Abs::Tab { base, stride };
    }
    match addr {
        Abs::K(a) => {
            let a = a as u32;
            if let Some(m) = scratch
                && in_scratch(a)
            {
                let a = a & !(n - 1);
                let mut v = 0u128;
                for i in (0..n).rev() {
                    let Some(b) = m.get(&(a + i)) else {
                        return Abs::Top;
                    };
                    v = v << 8 | u128::from(*b);
                }
                return Abs::K(v);
            }
            let r = Region::of(a);
            let own = literal && loc.copy.is_some() && r.is_ram();
            if (r == Region::Rom || own)
                && let Some(v) = image.read(loc.copy, a, n)
            {
                out.data.push((a, n as u8));
                return Abs::K(u128::from(v));
            }
            if r.is_ram() && n == 4 && a.is_multiple_of(4) {
                return Abs::sym(Base::Mem(a), 0);
            }
            Abs::Top
        }
        _ => match addr.stack_off() {
            Some(o) if n == 4 && o % 4 == 0 => match out.st.stack.get(&o) {
                Some(v) => *v,
                None if o >= 0 && !out.st.frame_dirty.contains(&o) => Abs::sym(Base::Frame(o), 0),
                None => Abs::Top,
            },
            _ => Abs::Top,
        },
    }
}

fn store(out: &mut Out, addr: Abs, n: u8, v: Abs, scratch: Option<&mut Scratch>) {
    if addr.stack_off().is_none() {
        out.stores.push((addr, n, v));
    }
    if let Abs::K(a) = addr {
        let a = a as u32;
        if let Some(m) = scratch
            && in_scratch(a)
        {
            let a = a & !(u32::from(n) - 1);
            for i in 0..u32::from(n) {
                match v {
                    Abs::K(x) => m.insert(a + i, (x >> (8 * i)) as u8),
                    _ => m.remove(&(a + i)),
                };
            }
        }
        let mut ev = Vec::new();
        dma_store(&mut out.st, a, n, v, &mut ev);
        out.dma.extend(ev);
        return;
    }
    if let Some(o) = addr.stack_off() {
        let lo = o & !3;
        let hi = o + i32::from(n);
        let mut w = lo;
        while w < hi {
            out.st.stack.remove(&w);
            if w >= 0 {
                out.st.frame_dirty.insert(w);
            }
            w += 4;
        }
        if n == 4 && o % 4 == 0 {
            out.st.stack.insert(o, v);
        }
    }
}

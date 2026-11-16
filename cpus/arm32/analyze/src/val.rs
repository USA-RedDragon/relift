//! The abstract values and the machine state one function run tracks.
//!
//! A value is a constant, a symbol plus an offset, or unknown. The symbols
//! are the value a register had when the function was entered (the stack
//! pointer, the return address, an argument) and the word loaded from a
//! constant RAM address. The stack is modelled relative to the stack
//! pointer at entry, which is word aligned: the BIOS and crt0 set aligned
//! stacks and every Thumb stack-pointer change is a multiple of four; an
//! ARM change that is not leaves the stack unknown. A store to an unknown
//! address is taken not to hit the modelled stack frame.

use std::collections::BTreeMap;

use relift_cpu_arm32::regs;

/// What a symbolic value is relative to.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) enum Base {
    /// The value of `r0`-`r14` at entry.
    Entry(u8),
    /// The word at this RAM address, loaded in this run.
    Mem(u32),
    /// The word the caller left at this offset from the entry stack
    /// pointer (zero or above), loaded before this run stored there.
    Frame(i32),
    /// The word at RAM address `addr` with the bits outside `keep` (one
    /// field of at most [`FIELD_BITS`] bits above bit 0) cleared and then
    /// set to some value: a read-modify-write of one field of the word.
    Field { addr: u32, keep: u32 },
}

/// The widest field a [`Base::Field`] tracks.
pub(crate) const FIELD_BITS: u32 = 6;

/// The field `!keep` when it is one run of at most [`FIELD_BITS`] bits
/// that leaves bit 0: its lowest bit and width.
pub(crate) fn field(keep: u32) -> Option<(u32, u32)> {
    let f = !keep;
    let lo = f.trailing_zeros();
    let n = f.count_ones();
    (f != 0 && lo > 0 && n <= FIELD_BITS && f >> lo == (1 << n) - 1).then_some((lo, n))
}

/// `base + off` with the low `align` bits cleared after the addition.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct Sym {
    pub base: Base,
    pub off: u32,
    pub align: u8,
}

/// An abstract value.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum Abs {
    /// A constant, of the value's width.
    K(u128),
    /// A symbol, 32 bits wide.
    S(Sym),
    /// Some bits known: the bits set in `mask` have the values in `val`.
    B { mask: u128, val: u128 },
    /// `base + stride * i` for an unknown `i` below `n` ([`UNBOUNDED`]:
    /// any), 32 bits.
    Idx { base: u32, stride: u32, n: u32 },
    /// The word loaded from `base + stride * i`, `i` below `n`, in the ROM
    /// image.
    Tab { base: u32, stride: u32, n: u32 },
    /// Unknown.
    Top,
}

/// The count of an index no check bounds.
pub(crate) const UNBOUNDED: u32 = u32::MAX;

pub(crate) const SP: u8 = 13;
pub(crate) const LR: u8 = 14;

impl Abs {
    pub(crate) const fn sym(base: Base, off: u32) -> Abs {
        Abs::S(Sym {
            base,
            off,
            align: 0,
        })
    }

    pub(crate) fn k(self) -> Option<u128> {
        match self {
            Abs::K(v) => Some(v),
            _ => None,
        }
    }

    pub(crate) fn k32(self) -> Option<u32> {
        self.k().and_then(|v| u32::try_from(v).ok())
    }

    /// The least value that covers both. Two addresses in one memory
    /// region, a multiple of four apart and at most 4 KiB apart, join to
    /// a table index from the lower one: of the two entries for two
    /// constants, not bounded once an index is involved (a loop).
    pub(crate) fn join(self, o: Abs) -> Abs {
        if self == o {
            return self;
        }
        let span = |x: Abs| match x {
            Abs::K(v) => u32::try_from(v).ok().map(|v| (v, 0, false)),
            Abs::Idx { base, stride, .. } => Some((base, stride, false)),
            Abs::S(Sym {
                base: Base::Mem(m),
                off: 0,
                align: 0,
            }) => Some((m, 0, true)),
            Abs::Tab { base, stride, .. } => Some((base, stride, true)),
            _ => None,
        };
        let (Some((a, sa, la)), Some((b, sb, lb))) = (span(self), span(o)) else {
            return Abs::Top;
        };
        if la != lb {
            return Abs::Top;
        }
        let (lo, hi) = (a.min(b), a.max(b));
        if lo < 0x0200_0000 || a >> 24 != b >> 24 || hi - lo > 0x1000 {
            return Abs::Top;
        }
        let g = gcd(gcd(hi - lo, sa), sb);
        if g == 0 || !g.is_multiple_of(4) {
            return Abs::Top;
        }
        let n = if sa == 0 && sb == 0 {
            (hi - lo) / g + 1
        } else {
            UNBOUNDED
        };
        if la {
            Abs::Tab {
                base: lo,
                stride: g,
                n,
            }
        } else {
            Abs::Idx {
                base: lo,
                stride: g,
                n,
            }
        }
    }

    /// The stack offset of an address relative to the entry stack pointer.
    pub(crate) fn stack_off(self) -> Option<i32> {
        match self {
            Abs::S(Sym {
                base: Base::Entry(SP),
                off,
                align: 0,
            }) => Some(off as i32),
            _ => None,
        }
    }
}

/// The DMA registers tracked, as halfwords from `0x040000B0`.
pub(crate) const DMA_BASE: u32 = 0x0400_00B0;
pub(crate) const DMA_HALVES: usize = 24;

/// The abstract machine state at one instruction.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct State {
    pub regs: Vec<Abs>,
    pub stack: BTreeMap<i32, Abs>,
    /// Caller frame slots this run may have written.
    pub frame_dirty: std::collections::BTreeSet<i32>,
    pub dma: [Abs; DMA_HALVES],
    /// Words this run stored at addresses relative to an entry register
    /// other than the stack pointer, while no store could have overwritten
    /// them: address and value.
    pub held: Vec<(Sym, Abs)>,
}

/// The mode, the masks, the reserved CPSR bits, the SPSRs and the banked
/// registers: the slots from `regs::M` on.
pub(crate) type Psr = Vec<Abs>;

impl State {
    /// The slots from `regs::M` on.
    pub(crate) fn psr(&self) -> Psr {
        self.regs[regs::M.index()..].to_vec()
    }

    /// Sets the slots from `regs::M` on.
    pub(crate) fn set_psr(&mut self, p: &[Abs]) {
        self.regs[regs::M.index()..].copy_from_slice(p);
    }

    /// The state at a function entry in `thumb` state.
    pub(crate) fn entry(thumb: bool) -> State {
        let n = regs::regs().len();
        let mut r = vec![Abs::Top; n];
        for (i, v) in r.iter_mut().enumerate().take(15) {
            *v = Abs::sym(Base::Entry(i as u8), 0);
        }
        r[regs::T.index()] = Abs::K(u128::from(thumb));
        State {
            regs: r,
            stack: BTreeMap::new(),
            frame_dirty: std::collections::BTreeSet::new(),
            dma: [Abs::Top; DMA_HALVES],
            held: Vec::new(),
        }
    }

    pub(crate) fn reg(&self, n: u8) -> Abs {
        self.regs[usize::from(n)]
    }

    pub(crate) fn thumb(&self) -> Option<bool> {
        self.regs[regs::T.index()].k().map(|t| t & 1 == 1)
    }

    /// Joins `o` into `self`; whether anything changed.
    pub(crate) fn merge(&mut self, o: &State) -> bool {
        let mut changed = false;
        for (a, b) in self.regs.iter_mut().zip(&o.regs) {
            let j = a.join(*b);
            if j != *a {
                *a = j;
                changed = true;
            }
        }
        for (a, b) in self.dma.iter_mut().zip(&o.dma) {
            let j = a.join(*b);
            if j != *a {
                *a = j;
                changed = true;
            }
        }
        let before = self.stack.len();
        let dirty_before = self.frame_dirty.len();
        for k in self.stack.keys().chain(o.stack.keys()) {
            if *k >= 0 && self.stack.get(k) != o.stack.get(k) {
                self.frame_dirty.insert(*k);
            }
        }
        self.frame_dirty.extend(o.frame_dirty.iter().copied());
        let held = self.held.len();
        self.held.retain(|h| o.held.contains(h));
        self.stack.retain(|k, v| o.stack.get(k) == Some(v));
        changed
            || self.held.len() != held
            || self.stack.len() != before
            || self.frame_dirty.len() != dirty_before
    }

    /// Forgets every register but those in `keep` (bits 0-14), the flags
    /// and the DMA registers, as after a call; with `psr` the mode, the
    /// masks, the SPSRs and the banked registers stay.
    pub(crate) fn clobber(&mut self, keep: u16, psr: bool) {
        for (i, v) in self.regs.iter_mut().enumerate() {
            if i == regs::T.index()
                || (i < 15 && keep >> i & 1 == 1)
                || (psr && i >= regs::M.index())
            {
                continue;
            }
            *v = Abs::Top;
        }
        self.dma = [Abs::Top; DMA_HALVES];
        self.held.clear();
    }

    /// Forgets the frame words a callee can reach: each local whose
    /// address a register or a stack word holds, from that address up to
    /// the registers the function saved (the words that hold an entry
    /// register) or the entry stack pointer.
    pub(crate) fn forget_reachable_locals(&mut self) {
        let shown: Vec<i32> = self
            .regs
            .iter()
            .take(15)
            .enumerate()
            .filter(|(i, _)| *i != usize::from(SP))
            .map(|(_, v)| *v)
            .chain(self.stack.values().copied())
            .filter_map(Abs::stack_off)
            .filter(|o| *o < 0)
            .collect();
        for o in shown {
            let end = self
                .stack
                .range(o..0)
                .find(|(_, v)| matches!(v, Abs::S(Sym { base: Base::Entry(r), off: 0, .. }) if (4..15).contains(r)))
                .map_or(0, |(k, _)| *k);
            let gone: Vec<i32> = self.stack.range(o & !3..end).map(|(k, _)| *k).collect();
            for k in gone {
                self.stack.remove(&k);
            }
        }
    }
}

/// `a + off` at 32 bits.
pub(crate) fn add(a: Abs, off: u32) -> Abs {
    match a {
        Abs::K(x) => Abs::K(u128::from((x as u32).wrapping_add(off))),
        Abs::S(s) if s.align == 0 || off == 0 => Abs::S(Sym {
            off: s.off.wrapping_add(off),
            ..s
        }),
        _ => Abs::Top,
    }
}

fn gcd(a: u32, b: u32) -> u32 {
    if b == 0 { a } else { gcd(b, a % b) }
}

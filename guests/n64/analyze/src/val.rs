//! The abstract values and the machine state one function run tracks.
//!
//! A value is a constant, a symbol plus an offset, some known bits, a
//! table index (`base + stride * i`), a word read from such a table, or
//! unknown. The symbols are the values the tracked slots had when the
//! function was entered and the words the caller left in its frame. A
//! symbol is a 32-bit quantity, sign-extended where the IR widens it, which
//! is how the VR4300 keeps addresses in its 64-bit registers. The stack is
//! modelled relative to the stack pointer at entry; a store to an address
//! not known is taken not to hit it.

use relift_cpu_mips::regs;
use relift_ir::Reg;

/// The slots a state tracks: the general registers, `HI` and `LO`, the two
/// exception return addresses and the PI and SP DMA address registers as
/// the code last wrote them.
pub(crate) const SLOTS: usize = 40;
pub(crate) const EPC: usize = 34;
pub(crate) const ERROR_EPC: usize = 35;
pub(crate) const PI_DRAM: usize = 36;
pub(crate) const PI_CART: usize = 37;
pub(crate) const SP_MEM: usize = 38;
pub(crate) const SP_DRAM: usize = 39;

pub(crate) const SP: usize = 29;
pub(crate) const RA: usize = 31;

/// The slots the MIPS calling convention keeps across a call: `s0`-`s7`,
/// `gp`, `sp`, `fp` and `ra`, and the DMA registers. A callee not analyzed
/// yet is taken to keep these.
pub(crate) const ABI_KEEP: u64 = 0xFF << 16 | 0xF << 28 | 0xF << PI_DRAM;

/// The state slot of IR register `r`, if it is tracked.
pub(crate) fn slot_of(r: Reg) -> Option<usize> {
    match r {
        Reg(n @ 0..=33) => Some(usize::from(n)),
        regs::CP0_EPC => Some(EPC),
        regs::CP0_ERROR_EPC => Some(ERROR_EPC),
        _ => None,
    }
}

/// What a symbolic value is relative to.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) enum Base {
    /// The value of a slot at entry.
    Entry(u8),
    /// The word the caller left at this offset from the entry stack
    /// pointer (zero or above), loaded before this run stored there.
    Frame(i32),
}

/// `base + off`, taken to its low 29 bits when `phys` (the physical
/// address of a kseg0 or kseg1 address).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct Sym {
    pub base: Base,
    pub off: u32,
    pub phys: bool,
}

/// An abstract value of up to 128 bits.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum Abs {
    /// A constant, of the value's width.
    K(u128),
    /// A symbol.
    S(Sym),
    /// Some bits known: the bits set in `mask` have the values in `val`.
    B { mask: u128, val: u128 },
    /// `base + stride * i` for an unknown `i` below `n` ([`UNBOUNDED`]:
    /// any), a 32-bit address.
    Idx { base: u32, stride: u32, n: u32 },
    /// The word loaded from `base + stride * i`, `i` below `n`.
    Tab { base: u32, stride: u32, n: u32 },
    /// Unknown.
    Top,
}

/// The count of an index no check bounds.
pub(crate) const UNBOUNDED: u32 = u32::MAX;

/// The low 29 bits of a kseg0 or kseg1 address.
pub(crate) const PHYS: u32 = 0x1FFF_FFFF;

pub(crate) fn sext32(x: u32) -> u128 {
    u128::from(i64::from(x as i32) as u64)
}

impl Abs {
    pub(crate) const fn sym(base: Base, off: u32) -> Abs {
        Abs::S(Sym {
            base,
            off,
            phys: false,
        })
    }

    pub(crate) const fn entry(slot: usize) -> Abs {
        Abs::sym(Base::Entry(slot as u8), 0)
    }

    pub(crate) fn k(self) -> Option<u128> {
        match self {
            Abs::K(v) => Some(v),
            _ => None,
        }
    }

    /// The low 32 bits of a constant.
    pub(crate) fn k32(self) -> Option<u32> {
        self.k().map(|v| v as u32)
    }

    /// The least value that covers both. Two addresses in one 16 MiB
    /// area, a multiple of four apart and at most 4 KiB apart, join to a
    /// table index from the lower one: of the two entries for two
    /// constants, not bounded once an index is involved (a loop).
    pub(crate) fn join(self, o: Abs) -> Abs {
        if self == o {
            return self;
        }
        let span = |x: Abs| match x {
            Abs::K(v) => Some((v as u32, 0, false)),
            Abs::Idx { base, stride, .. } => Some((base, stride, false)),
            Abs::Tab { base, stride, .. } => Some((base, stride, true)),
            _ => None,
        };
        if let (Some((a, sa, la)), Some((b, sb, lb))) = (span(self), span(o))
            && la == lb
            && a >> 24 == b >> 24
            && a.abs_diff(b) <= 0x1000
            && (la || is_addr(self) && is_addr(o))
        {
            let (lo, hi) = (a.min(b), a.max(b));
            let g = gcd(gcd(hi - lo, sa), sb);
            if g != 0 && g.is_multiple_of(4) {
                let n = if sa == 0 && sb == 0 {
                    (hi - lo) / g + 1
                } else {
                    UNBOUNDED
                };
                return if la {
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
                };
            }
        }
        let (ma, va) = known(self, 128);
        let (mb, vb) = known(o, 128);
        let m = ma & mb & !(va ^ vb);
        if m == 0 {
            Abs::Top
        } else {
            Abs::B {
                mask: m,
                val: va & m,
            }
        }
    }

    /// The stack offset of an address relative to the entry stack pointer.
    pub(crate) fn stack_off(self) -> Option<i32> {
        match self {
            Abs::S(Sym {
                base: Base::Entry(b),
                off,
                ..
            }) if usize::from(b) == SP => Some(off as i32),
            _ => None,
        }
    }
}

/// Whether `a` looks like a code or data address: a constant in kseg0 or
/// kseg1 or its physical form, or an index.
fn is_addr(a: Abs) -> bool {
    match a {
        Abs::K(v) => {
            let lo = v as u32;
            let hi_ok = v >> 32 == 0 || v >> 32 == 0xFFFF_FFFF;
            hi_ok && (lo >> 30 == 2 || (0x400..0x0080_0000).contains(&lo))
        }
        Abs::Idx { .. } => true,
        _ => false,
    }
}

/// The known bits of `a` at width `w`: mask and values.
pub(crate) fn known(a: Abs, w: u32) -> (u128, u128) {
    let m = mask(w);
    match a {
        Abs::K(x) => (m, x & m),
        Abs::B { mask: k, val } => (k & m, val & k & m),
        _ => (0, 0),
    }
}

pub(crate) fn mask(w: u32) -> u128 {
    if w >= 128 {
        u128::MAX
    } else {
        (1u128 << w) - 1
    }
}

/// Words of the caller's frame (from the entry stack pointer up) that a
/// run tracks for writes.
pub(crate) const FRAME_WORDS: i32 = 64;

/// The abstract machine state at one instruction.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct State {
    pub regs: [Abs; SLOTS],
    /// Stack words and doublewords by offset from the entry stack pointer:
    /// offset, size, value; sorted, never overlapping.
    pub stack: Vec<(i32, u8, Abs)>,
    /// Words of the caller's frame this run may have written: bit `i` for
    /// offset `4 * i`.
    pub frame_dirty: u64,
    /// Zero words run through in a row.
    pub zeros: u8,
}

impl State {
    /// The state at a function entry: every slot its entry symbol.
    pub(crate) fn entry() -> State {
        let mut r = [Abs::Top; SLOTS];
        for (i, v) in r.iter_mut().enumerate() {
            *v = Abs::entry(i);
        }
        r[0] = Abs::K(0);
        State {
            regs: r,
            stack: Vec::new(),
            frame_dirty: 0,
            zeros: 0,
        }
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
        let before = self.stack.len();
        let mut dirty = self.frame_dirty | o.frame_dirty;
        for e in self.stack.iter().chain(&o.stack) {
            if e.0 >= 0 && !(self.stack.contains(e) && o.stack.contains(e)) {
                dirty |= frame_bits(e.0, e.1);
            }
        }
        self.stack.retain(|e| o.stack.contains(e));
        changed |= self.stack.len() != before || dirty != self.frame_dirty;
        self.frame_dirty = dirty;
        if o.zeros < self.zeros {
            self.zeros = o.zeros;
            changed = true;
        }
        changed
    }

    /// Forgets every slot not in `keep`, as after a call.
    pub(crate) fn clobber(&mut self, keep: u64) {
        for (i, v) in self.regs.iter_mut().enumerate().skip(1) {
            if keep >> i & 1 == 0 {
                *v = Abs::Top;
            }
        }
    }
}

/// The frame-dirty bits of `size` bytes at offset `off`.
pub(crate) fn frame_bits(off: i32, size: u8) -> u64 {
    let mut bits = 0;
    let mut w = off & !3;
    while w < off + i32::from(size) {
        if (0..FRAME_WORDS * 4).contains(&w) {
            bits |= 1 << (w / 4);
        }
        w += 4;
    }
    bits
}

pub(crate) fn gcd(a: u32, b: u32) -> u32 {
    if b == 0 { a } else { gcd(b, a % b) }
}

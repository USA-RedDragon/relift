//! The abstract values and memory of the FPCR proof.

use std::collections::{BTreeMap, BTreeSet};

use relift_ir::Width;

/// Where a tracked pointer points: the stack pointer at the root's entry,
/// or a stack the code switched to (a stack pointer set from a value the
/// analysis does not know), named by the instruction and context that set
/// it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(super) enum Base {
    Root,
    Fresh(u64, u32),
}

/// An abstract value: bits known to be zero and one (both clear means the
/// bit is unknown), a pointer at a known offset from a base, or a function
/// address that a loader-filled word holds (an index into the analysis's
/// target sets).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum V {
    Bits { zero: u128, one: u128 },
    Ptr(Base, i64),
    Code(u32),
}

pub(super) const TOP: V = V::Bits { zero: 0, one: 0 };

impl V {
    pub(super) fn konst(c: u128, w: Width) -> V {
        let m = w.mask();
        V::Bits {
            zero: !c & m,
            one: c & m,
        }
    }

    pub(super) fn known(self, w: Width) -> Option<u128> {
        match self {
            V::Bits { zero, one } if (zero | one) & w.mask() == w.mask() => Some(one & w.mask()),
            _ => None,
        }
    }

    /// The known-bits form, a pointer or a code value being unknown bits.
    pub(super) fn bits(self, w: Width) -> (u128, u128) {
        match self {
            V::Bits { zero, one } => (zero & w.mask(), one & w.mask()),
            _ => (0, 0),
        }
    }

    pub(super) fn join(self, o: V) -> V {
        match (self, o) {
            (a, b) if a == b => a,
            (V::Bits { zero: z1, one: o1 }, V::Bits { zero: z2, one: o2 }) => V::Bits {
                zero: z1 & z2,
                one: o1 & o2,
            },
            _ => TOP,
        }
    }
}

/// One byte of tracked memory: known bits, or byte `k` of a pointer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Byte {
    Bits(u8, u8),
    Ptr(Base, i64, u8),
}

/// The tracked stack memory, the addresses whose objects other code may
/// write (escaped), and the addresses the code takes as values, which
/// start objects (taken).
///
/// The model is that of a program that writes an object only through
/// addresses derived from that object's own address: an object that starts
/// at a taken address ends before the next one, and a load that overlaps
/// an object whose address escaped (to code the analysis does not follow,
/// to memory, or to other threads through either) gives an unknown value.
/// Stores through unknown addresses therefore only reach escaped objects,
/// and calls the analysis does not follow change only those. An address
/// computed from a stack pointer with an unknown offset may reach any
/// object of that stack, so it makes all of them escaped (wild).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(super) struct Mem {
    bytes: BTreeMap<(Base, i64), Byte>,
    escaped: BTreeSet<(Base, i64)>,
    taken: BTreeSet<(Base, i64)>,
    wild: BTreeSet<Base>,
}

impl Mem {
    pub(super) fn take(&mut self, b: Base, off: i64) {
        self.taken.insert((b, off));
    }

    pub(super) fn escape(&mut self, b: Base, off: i64) {
        self.taken.insert((b, off));
        self.escaped.insert((b, off));
    }

    /// Marks every object of base `b` as escaped: the code computed an
    /// address from one of them that the analysis does not know, which may
    /// reach any of them.
    pub(super) fn wild(&mut self, b: Base) {
        self.wild.insert(b);
    }

    /// Makes the pointers stored in escaped objects escape, until no more
    /// do.
    pub(super) fn escape_stored(&mut self) {
        loop {
            let found: Vec<(Base, i64)> = self
                .bytes
                .iter()
                .filter_map(|(&(b, off), byte)| match *byte {
                    Byte::Ptr(pb, po, 0) if self.tainted(b, off, 8) => Some((pb, po)),
                    _ => None,
                })
                .filter(|p| !self.escaped.contains(p))
                .collect();
            if found.is_empty() {
                return;
            }
            for (pb, po) in found {
                self.escape(pb, po);
            }
        }
    }

    /// Forgets everything about base `b`, a stack the code switched to anew.
    pub(super) fn reset(&mut self, b: Base) {
        self.bytes.retain(|k, _| k.0 != b);
        self.escaped.retain(|k| k.0 != b);
        self.taken.retain(|k| k.0 != b);
        self.wild.remove(&b);
    }

    /// Whether a load of `n` bytes at `off` may read an object whose
    /// address escaped: one that starts at the last taken address at or
    /// below an escaped one and ends at the next taken address above it.
    fn tainted(&self, b: Base, off: i64, n: i64) -> bool {
        if self.wild.contains(&b) {
            return true;
        }
        let end = off.saturating_add(n);
        self.escaped.iter().filter(|e| e.0 == b).any(|&(_, e)| {
            let start = self
                .taken
                .range((b, i64::MIN)..=(b, e))
                .next_back()
                .map_or(i64::MIN, |t| t.1);
            let stop = self
                .taken
                .range((b, e.saturating_add(1))..=(b, i64::MAX))
                .next()
                .map_or(i64::MAX, |t| t.1);
            start < end && stop > off
        })
    }

    pub(super) fn forget(&mut self, b: Base, off: i64, n: i64) {
        let end = off.saturating_add(n);
        let keys: Vec<(Base, i64)> = self
            .bytes
            .range((b, off)..(b, end))
            .map(|(k, _)| *k)
            .collect();
        for k in keys {
            self.bytes.remove(&k);
        }
    }

    pub(super) fn store(&mut self, b: Base, off: i64, n: u32, v: V) {
        self.forget(b, off, i64::from(n));
        for k in 0..n {
            let at = (b, off.wrapping_add(i64::from(k)));
            let byte = match v {
                V::Bits { zero, one } => {
                    let z = (zero >> (8 * k)) as u8;
                    let o = (one >> (8 * k)) as u8;
                    if z | o == 0 {
                        continue;
                    }
                    Byte::Bits(z, o)
                }
                V::Ptr(pb, po) if n == 8 => Byte::Ptr(pb, po, k as u8),
                _ => continue,
            };
            self.bytes.insert(at, byte);
        }
    }

    pub(super) fn load(&self, b: Base, off: i64, n: u32) -> V {
        if self.tainted(b, off, i64::from(n)) {
            return TOP;
        }
        let at = |k: u32| self.bytes.get(&(b, off.wrapping_add(i64::from(k))));
        if n == 8
            && let Some(&Byte::Ptr(pb, po, 0)) = at(0)
            && (1..8).all(|k| at(k) == Some(&Byte::Ptr(pb, po, k as u8)))
        {
            return V::Ptr(pb, po);
        }
        let (mut zero, mut one) = (0u128, 0u128);
        for k in 0..n {
            if let Some(&Byte::Bits(z, o)) = at(k) {
                zero |= u128::from(z) << (8 * k);
                one |= u128::from(o) << (8 * k);
            }
        }
        V::Bits { zero, one }
    }

    pub(super) fn join(&self, o: &Mem) -> Mem {
        let mut bytes = BTreeMap::new();
        for (k, a) in &self.bytes {
            let Some(c) = o.bytes.get(k) else {
                continue;
            };
            let j = match (*a, *c) {
                (x, y) if x == y => x,
                (Byte::Bits(z1, o1), Byte::Bits(z2, o2)) if (z1 & z2) | (o1 & o2) != 0 => {
                    Byte::Bits(z1 & z2, o1 & o2)
                }
                _ => continue,
            };
            bytes.insert(*k, j);
        }
        Mem {
            bytes,
            escaped: self.escaped.union(&o.escaped).copied().collect(),
            taken: self.taken.union(&o.taken).copied().collect(),
            wild: self.wild.union(&o.wild).copied().collect(),
        }
    }
}

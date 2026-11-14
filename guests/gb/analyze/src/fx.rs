//! A fast non-cryptographic hasher for the analysis' maps, whose keys are
//! large states hashed many times.

use std::collections::{HashMap, HashSet};
use std::hash::{BuildHasherDefault, Hasher};

/// A multiply-rotate hasher in the style of the Firefox hasher.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct Fx(u64);

const K: u64 = 0x517c_c1b7_2722_0a95;

impl Fx {
    fn add(&mut self, w: u64) {
        self.0 = (self.0.rotate_left(5) ^ w).wrapping_mul(K);
    }
}

impl Hasher for Fx {
    fn write(&mut self, bytes: &[u8]) {
        let (chunks, rest) = bytes.as_chunks::<8>();
        for c in chunks {
            self.add(u64::from_le_bytes(*c));
        }
        for b in rest {
            self.add(u64::from(*b));
        }
    }

    fn write_u8(&mut self, i: u8) {
        self.add(u64::from(i));
    }

    fn write_u16(&mut self, i: u16) {
        self.add(u64::from(i));
    }

    fn write_u32(&mut self, i: u32) {
        self.add(u64::from(i));
    }

    fn write_u64(&mut self, i: u64) {
        self.add(i);
    }

    fn write_usize(&mut self, i: usize) {
        self.add(i as u64);
    }

    fn finish(&self) -> u64 {
        self.0
    }
}

pub(crate) type FxMap<K, V> = HashMap<K, V, BuildHasherDefault<Fx>>;
pub(crate) type FxSet<K> = HashSet<K, BuildHasherDefault<Fx>>;

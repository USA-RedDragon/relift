//! What the lifter needs to know about the CPU a guest's code runs on: its
//! architecture features and the virtual address layout its OS gives the
//! process. The guest picks both per program.

use std::fmt;

use crate::decode::Features;

/// The virtual address layout of EL0 code, as `TCR_EL1` sets it: the size
/// of each half of the address space and whether loads, stores and
/// branches ignore the top byte. Pointer authentication codes sit in the
/// bits above the address, so the layout decides where they go.
///
/// Both halves have the same size and the same top-byte setting, and
/// `TBID` is zero.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Va {
    /// The address bits, `64 - TxSZ`.
    pub bits: u8,
    /// Top-byte ignore (`TBI0` and `TBI1`).
    pub tbi: bool,
}

impl Va {
    /// A 39-bit address space without top-byte ignore.
    pub const BITS39: Va = Va {
        bits: 39,
        tbi: false,
    };

    /// The `TCR_EL1` fields of the layout: `T0SZ`, `T1SZ`, `TBI0` and
    /// `TBI1`.
    pub const fn tcr_el1(self) -> u64 {
        let sz = 64 - self.bits as u64;
        let tbi = self.tbi as u64;
        sz | (sz << 16) | (tbi << 37) | (tbi << 38)
    }
}

impl fmt::Display for Va {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let tbi = if self.tbi { "with" } else { "without" };
        write!(f, "a {}-bit address space {tbi} top-byte ignore", self.bits)
    }
}

/// A guest CPU: what the decoder and the lifter take.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Cpu {
    /// The architecture features: a word of an encoding the CPU lacks is
    /// unallocated, and templates take the CPU's branch of feature tests.
    pub features: Features,
    /// The address layout the OS gives the process.
    pub va: Va,
}

impl Cpu {
    /// A CPU with `features` running code with the layout `va`.
    pub const fn new(features: Features, va: Va) -> Cpu {
        Cpu { features, va }
    }
}

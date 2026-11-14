//! Code locations: a ROM bank and a CPU address.

use core::fmt;

/// Where an instruction lives. `$0000-$3FFF` is always bank 0; an address
/// in `$4000-$7FFF` carries the bank mapped there.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Loc {
    /// The ROM bank: 0 for the fixed window.
    pub bank: u16,
    /// The CPU address.
    pub addr: u16,
}

impl Loc {
    /// A location in the fixed window.
    #[must_use]
    pub const fn fixed(addr: u16) -> Loc {
        Loc { bank: 0, addr }
    }

    /// Whether the address is in the switchable window.
    #[must_use]
    pub const fn is_window(self) -> bool {
        self.addr >= 0x4000 && self.addr < 0x8000
    }
}

impl fmt::Display for Loc {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:02X}:{:04X}", self.bank, self.addr)
    }
}

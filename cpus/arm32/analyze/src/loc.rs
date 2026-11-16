//! Code locations: an address, the CPU state the code runs in and, for code
//! in work RAM, the copy that put it there.

use std::fmt;

/// Where an instruction is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Loc {
    /// The address the instruction runs at.
    pub addr: u32,
    /// Whether it runs in Thumb state.
    pub thumb: bool,
    /// For code in work RAM, the index of the copy (in
    /// [`crate::Report::copies`]) whose bytes are there.
    pub copy: Option<u16>,
}

impl Loc {
    /// A location in the image.
    #[must_use]
    pub const fn rom(addr: u32, thumb: bool) -> Loc {
        Loc {
            addr,
            thumb,
            copy: None,
        }
    }

    /// The instruction size in bytes.
    #[must_use]
    pub const fn size(self) -> u32 {
        if self.thumb { 2 } else { 4 }
    }
}

impl fmt::Display for Loc {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = if self.thumb { 'T' } else { 'A' };
        write!(f, "{s}:{:08X}", self.addr)?;
        if let Some(c) = self.copy {
            write!(f, "@{c}")?;
        }
        Ok(())
    }
}

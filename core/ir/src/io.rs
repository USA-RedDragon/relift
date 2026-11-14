use crate::Width;

/// What a guest address holds, for one direction of access.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum AddrClass {
    /// Ordinary memory: an access reads or writes the bytes of the guest
    /// reservation and nothing else happens.
    Memory,
    /// A device register or a bank-switch control: an access goes to the
    /// guest's device hook.
    Device,
}

/// One range of the [`IoMap`]: the bytes from `first` to `last` inclusive,
/// with a class for reads and one for writes (a Game Boy cartridge ROM is
/// memory to read and a bank controller to write).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct IoRange {
    /// The first address.
    pub first: u64,
    /// The last address, inclusive.
    pub last: u64,
    /// The class of a read.
    pub read: AddrClass,
    /// The class of a write.
    pub write: AddrClass,
}

/// A guest's address-range classifier: which addresses an I/O access
/// ([`crate::Op::IoLoad`], [`crate::Op::IoStore`]) sends to the device hook.
///
/// The guest provides it; [`crate::opt::classify_io`] uses it on accesses
/// with a constant address, the interpreter on every access, and the
/// emitter compiles it into the range check of accesses of kind
/// [`crate::IoKind::Any`]. The first range holding a byte gives its class;
/// a byte in no range has the class `default`.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct IoMap {
    /// The ranges, first match wins.
    pub ranges: Vec<IoRange>,
    /// The class of a byte in no range.
    pub default: AddrClass,
}

impl IoMap {
    /// A map with no ranges, where everything is [`AddrClass::Memory`].
    pub const fn memory() -> IoMap {
        IoMap {
            ranges: Vec::new(),
            default: AddrClass::Memory,
        }
    }

    /// The class of the byte at `addr`.
    pub fn class_of(&self, addr: u64, write: bool) -> AddrClass {
        self.ranges
            .iter()
            .find(|r| r.first <= addr && addr <= r.last)
            .map_or(self.default, |r| if write { r.write } else { r.read })
    }

    /// The class of an access of `size` bytes at `addr`, the address width
    /// `addr_width` wide: [`AddrClass::Device`] when any byte of it is a
    /// device byte, so an access that straddles a boundary goes to the
    /// hook as a whole. The bytes run upward from `addr` without wrapping,
    /// like [`crate::interp::GuestMemory`] accesses.
    pub fn classify(&self, addr: u64, size: u32, write: bool, addr_width: Width) -> AddrClass {
        let addr = addr & (addr_width.mask() as u64);
        let device = (0..u64::from(size))
            .filter_map(|i| addr.checked_add(i))
            .any(|a| self.class_of(a, write) == AddrClass::Device);
        if device {
            AddrClass::Device
        } else {
            AddrClass::Memory
        }
    }
}

impl Default for IoMap {
    fn default() -> IoMap {
        IoMap::memory()
    }
}

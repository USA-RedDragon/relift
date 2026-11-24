//! The guest ABIs the generator emits thunks for: the data model (sizes and
//! alignments of scalars, pointers and handles) and where arguments live.
//!
//! The host is always LP64 (x86-64 or AArch64), so the host layout of a
//! registry type is its [`A64`] layout. A guest whose layout differs (i386)
//! converts every struct member by member; one whose layout is the host's
//! (AArch64) passes plain structs in place.

/// How a guest passes arguments to a called function.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Args {
    /// AAPCS64: integers and pointers in x0 to x7, floats in s0 to s7, the
    /// rest in 8-byte stack slots.
    Registers,
    /// i386 cdecl: every argument in 4-byte stack slots after the return
    /// address, 8-byte scalars in two.
    Stack,
}

/// The data model and calling convention of one guest.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Abi {
    /// The name of the target in file names and messages (`a64`, `i386`).
    pub name: &'static str,
    /// Size and alignment of pointers, dispatchable handles, `size_t`,
    /// `long` and function pointers.
    pub word: u64,
    /// Alignment of 8-byte scalars (`uint64_t`, `double`, non-dispatchable
    /// handles) inside a struct.
    pub align8: u64,
    /// The calling convention.
    pub args: Args,
}

/// AArch64 LP64 (the Switch): every scalar aligned to its size.
pub const A64: Abi = Abi {
    name: "a64",
    word: 8,
    align8: 8,
    args: Args::Registers,
};

/// i386 System V (Linux): ILP32, 8-byte scalars aligned to 4 in structs.
pub const I386: Abi = Abi {
    name: "i386",
    word: 4,
    align8: 4,
    args: Args::Stack,
};

impl Abi {
    /// The target of a name, as the command line gives it.
    pub fn by_name(name: &str) -> Option<Abi> {
        [A64, I386].into_iter().find(|a| a.name == name)
    }

    /// True when guest and host lay out every type the same way.
    pub fn is_host(&self) -> bool {
        *self == A64
    }

    /// Size and alignment of a C scalar of `size` bytes inside a struct.
    pub fn scalar(&self, size: u64) -> (u64, u64) {
        (size, if size == 8 { self.align8 } else { size })
    }
}

#[cfg(test)]
mod tests {
    use super::{A64, Abi, I386};

    #[test]
    fn names_and_scalars() {
        assert_eq!(Abi::by_name("i386"), Some(I386));
        assert_eq!(Abi::by_name("x"), None);
        assert_eq!(I386.scalar(8), (8, 4));
        assert_eq!(A64.scalar(8), (8, 8));
        assert!(A64.is_host() && !I386.is_host());
    }
}

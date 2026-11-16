//! What the analysis found.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt;

use crate::loc::{Loc, Region};

/// How a ROM-to-RAM copy was made.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum CopyKind {
    /// BIOS `CpuSet` (SWI 0x0B).
    CpuSet,
    /// BIOS `CpuFastSet` (SWI 0x0C).
    CpuFastSet,
    /// An immediate DMA transfer on this channel.
    Dma(u8),
    /// A call with constant arguments, run to its return.
    Run,
    /// A multiboot image where the BIOS loads it.
    Load,
    /// The bytes of this other copy with a constant the program stores into
    /// its code.
    Rewrite(u16),
}

impl fmt::Display for CopyKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CopyKind::CpuSet => f.write_str("CpuSet"),
            CopyKind::CpuFastSet => f.write_str("CpuFastSet"),
            CopyKind::Dma(n) => write!(f, "DMA{n}"),
            CopyKind::Run => f.write_str("run"),
            CopyKind::Load => f.write_str("load"),
            CopyKind::Rewrite(c) => write!(f, "rewrite of copy {c}"),
        }
    }
}

/// Bytes written to work RAM by code whose inputs are all constants: a
/// copy from ROM with constant source, destination and length, or a call
/// with constant arguments run to its return. Code reached in the
/// destination runs from these bytes.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct RamCopy {
    /// The instruction that starts it.
    pub site: Loc,
    /// How.
    pub kind: CopyKind,
    /// The ROM source address, for a plain copy.
    pub src: Option<u32>,
    /// The RAM destination address.
    pub dst: u32,
    /// The bytes written.
    pub bytes: std::rc::Rc<[u8]>,
}

impl RamCopy {
    /// The length in bytes.
    #[must_use]
    pub fn len(&self) -> u32 {
        self.bytes.len() as u32
    }

    /// Whether no byte was written.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }
}

/// Why a location is a function entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Origin {
    /// The reset entry at `0x08000000`.
    Entry,
    /// A constant stored to the IRQ vector at `0x03007FFC`.
    Irq,
    /// The target of a call.
    Called,
    /// The target of a jump that leaves another function.
    Jumped,
    /// A Thumb code address discovered code reads as a constant, or an
    /// entry of a table of them.
    AddressTaken,
    /// An instruction in RAM that the program may rewrite, in each copy
    /// that may hold it: code that finds other bytes there goes on here.
    Rewritten,
    /// A code address the project's hints name: a run reached it and
    /// discovery did not (`relift report` adds it from a stop report).
    Hint,
}

/// How control reaches another function.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum EdgeKind {
    /// `BL`, or a Thumb `BL` pair.
    Call,
    /// A jump with the return address set up before it (`MOV LR,PC`
    /// then `BX Rn` or `LDR PC`).
    LinkedJump,
    /// A call through a function that jumps to an address its caller
    /// passes in a register (a `BX Rn` veneer).
    Via,
    /// A call or jump through a pointer stored in RAM.
    Pointer,
    /// A jump to another function's entry.
    Tail,
}

/// A transfer to another function.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Edge {
    /// The instruction.
    pub site: Loc,
    /// The function reached.
    pub target: Loc,
    /// How.
    pub kind: EdgeKind,
}

/// Why a transfer was not followed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum UnresolvedKind {
    /// A jump to a computed address.
    IndirectJump,
    /// A call to a computed address.
    IndirectCall,
    /// A jump through a RAM word no discovered code stores a constant to.
    PointerNoStore,
    /// A transfer into work RAM where no proven copy put code.
    RamNoCopy,
    /// A transfer into the BIOS.
    Bios,
    /// Code outside the ROM image and the copies.
    OutsideCode,
    /// An instruction that does not lift (undefined, unpredictable).
    Undefined,
    /// A transfer whose target state is not known.
    UnknownState,
}

impl UnresolvedKind {
    /// The kebab-case name.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            UnresolvedKind::IndirectJump => "indirect-jump",
            UnresolvedKind::IndirectCall => "indirect-call",
            UnresolvedKind::PointerNoStore => "pointer-no-store",
            UnresolvedKind::RamNoCopy => "ram-no-copy",
            UnresolvedKind::Bios => "bios",
            UnresolvedKind::OutsideCode => "outside-code",
            UnresolvedKind::Undefined => "undefined",
            UnresolvedKind::UnknownState => "unknown-state",
        }
    }
}

/// A transfer the analysis could not follow.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Unresolved {
    /// The instruction.
    pub at: Loc,
    /// Why.
    pub kind: UnresolvedKind,
    /// Detail for a reader.
    pub detail: String,
}

/// A jump table: the targets of a computed jump for every index the
/// bounds check before it lets through.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Table {
    /// The computed jump.
    pub site: Loc,
    /// The bounds check.
    pub check: Loc,
    /// The highest index that reaches the jump.
    pub max: u32,
    /// The targets, by index.
    pub targets: Vec<Loc>,
}

/// One function.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Function {
    /// The entry.
    pub entry: Loc,
    /// Why it is one.
    pub origin: Origin,
    /// Its instructions and their sizes.
    pub instructions: BTreeMap<Loc, u8>,
    /// Its transfers to other functions.
    pub edges: Vec<Edge>,
    /// Its jump tables.
    pub tables: Vec<Table>,
    /// Whether some path returns.
    pub returns: bool,
    /// Its computed jumps that are calls: the link register holds the
    /// address of the next instruction.
    pub linked: BTreeSet<Loc>,
}

/// A BIOS call.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Swi {
    /// The instruction.
    pub site: Loc,
    /// The SWI number.
    pub number: u8,
}

/// Instructions found in one region.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RegionCoverage {
    /// ARM instructions.
    pub arm: usize,
    /// Thumb instructions.
    pub thumb: usize,
    /// Bytes of code.
    pub bytes: usize,
}

/// The analysis of one ROM.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Report {
    /// The functions by entry.
    pub functions: BTreeMap<Loc, Function>,
    /// The proven ROM-to-RAM copies.
    pub copies: Vec<RamCopy>,
    /// The transfers not followed.
    pub unresolved: Vec<Unresolved>,
    /// The BIOS calls.
    pub swis: Vec<Swi>,
    /// The IRQ handlers stored to the vector.
    pub irq_handlers: Vec<Loc>,
    /// ROM and copied bytes read as constants: address and size.
    pub data: BTreeSet<(u32, u8)>,
    /// Instructions that overlap a constant read as data.
    pub conflicts: Vec<Loc>,
}

impl Report {
    /// Every instruction found, with its size.
    #[must_use]
    pub fn instructions(&self) -> BTreeMap<Loc, u8> {
        self.functions
            .values()
            .flat_map(|f| f.instructions.iter().map(|(l, n)| (*l, *n)))
            .collect()
    }

    /// The instructions found per region.
    #[must_use]
    pub fn coverage(&self) -> BTreeMap<Region, RegionCoverage> {
        let mut m: BTreeMap<Region, RegionCoverage> = BTreeMap::new();
        for (l, n) in self.instructions() {
            let c = m.entry(l.region()).or_default();
            if l.thumb {
                c.thumb += 1;
            } else {
                c.arm += 1;
            }
            c.bytes += usize::from(n);
        }
        m
    }

    /// The unresolved transfers counted by kind.
    #[must_use]
    pub fn unresolved_by_kind(&self) -> BTreeMap<UnresolvedKind, usize> {
        let mut m = BTreeMap::new();
        for u in &self.unresolved {
            *m.entry(u.kind).or_insert(0) += 1;
        }
        m
    }
}

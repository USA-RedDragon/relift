//! What the analysis reports.

use relift_n64_formats::{Cic, Stream};

use crate::image::{Segment, Source};

/// How the boot code loads the game, as the analysis models it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Boot {
    /// A known boot variant: `ROM[0x1000..]`, 1 MiB, at the entry point
    /// the variant computes from the header word.
    Standard(Cic),
    /// An unknown boot variant with an ELF image at this ROM offset: its
    /// loadable segments at their addresses, its entry point.
    Elf(u32),
    /// An unknown boot variant without one: as [`Boot::Standard`] with
    /// the header's entry point unchanged.
    Unknown,
}

/// Why an address is a function entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Origin {
    /// The entry point.
    Entry,
    /// The target of an exception vector trampoline (a jump through
    /// `k0` or `k1` to a constant).
    Exception,
    /// A call target.
    Call,
    /// A jump target that is also a function entry.
    Tail,
    /// A constant code address the code computes, whose run lifts.
    AddressTaken,
    /// A word of a code region that allocates a stack frame or follows a
    /// return, whose run lifts.
    Prologue,
}

impl Origin {
    /// A short name.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Origin::Entry => "entry",
            Origin::Exception => "exception",
            Origin::Call => "call",
            Origin::Tail => "tail",
            Origin::AddressTaken => "address-taken",
            Origin::Prologue => "prologue",
        }
    }
}

/// A function entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Function {
    /// The segment, an index into [`Report::segments`].
    pub segment: u32,
    /// The address.
    pub addr: u32,
    /// Why it is one.
    pub origin: Origin,
}

/// What a transfer or DMA left unknown is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum UnresolvedKind {
    /// A jump whose target is not known.
    Jump,
    /// A call whose target is not known.
    Call,
    /// A DMA whose operands are not known.
    Dma,
    /// A known target that no segment holds.
    Outside,
    /// A path that runs past the end of its segment.
    NoCode,
}

impl UnresolvedKind {
    /// A short name.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            UnresolvedKind::Jump => "jump",
            UnresolvedKind::Call => "call",
            UnresolvedKind::Dma => "dma",
            UnresolvedKind::Outside => "outside",
            UnresolvedKind::NoCode => "no-code",
        }
    }
}

/// A transfer or DMA the analysis could not resolve.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Unresolved {
    /// The segment of the site.
    pub segment: u32,
    /// The address of the instruction.
    pub site: u32,
    /// What it is.
    pub kind: UnresolvedKind,
    /// The value it depends on, or the target.
    pub detail: String,
}

/// A run of code words of one segment: the unit the runtime binds when it
/// finds these bytes at their link address.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Unit {
    /// The segment.
    pub segment: u32,
    /// Where the bytes come from.
    pub source: Source,
    /// Their offset in the source.
    pub offset: u32,
    /// The length in bytes.
    pub len: u32,
    /// The address of the first word, when the segment has one.
    pub link: Option<u32>,
    /// FNV-1a (64-bit) of the words, big-endian, with the relocation
    /// fields cleared.
    pub hash: u64,
    /// Words with a relocation field.
    pub reloc: u32,
    /// Words found by the sweep only, not by a run.
    pub swept: u32,
}

/// A PI DMA from the cartridge with constant operands.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PiDma {
    /// The segment of the code that starts it.
    pub segment: u32,
    /// The address of the store that starts it.
    pub site: u32,
    /// The RDRAM physical address.
    pub dram: u32,
    /// The cartridge address.
    pub cart: u32,
    /// The length in bytes.
    pub len: u32,
}

/// Why an RSP image is listed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RspOrigin {
    /// An SP DMA to IMEM with constant operands, started at this address.
    SpDma {
        /// The address of the store that starts it.
        site: u32,
    },
    /// A run of words that decode as RSP code with vector instructions
    /// and do not decode as VR4300 code.
    Scan,
}

/// RSP code bytes, for the RSP frontend.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RspImage {
    /// Where the bytes come from.
    pub source: Source,
    /// Their offset in the source.
    pub offset: u32,
    /// The length in bytes.
    pub len: u32,
    /// The IMEM offset they are loaded at, when known.
    pub imem: Option<u32>,
    /// FNV-1a (64-bit) of the bytes.
    pub hash: u64,
    /// Why it is listed.
    pub origin: RspOrigin,
}

/// Counts of the analysis.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Stats {
    /// Code words reached by function runs.
    pub run_words: u64,
    /// Code words added by the sweep.
    pub swept_words: u64,
    /// Code regions the scan found.
    pub regions: u32,
    /// Regions linked at a voted address.
    pub linked_regions: u32,
    /// Function runs made.
    pub runs: u64,
    /// Rounds of the PI DMA fixed point.
    pub rounds: u32,
    /// Roots rejected because their run reached a word that does not lift
    /// or overlaps data.
    pub rejected: u32,
}

/// The result of the analysis.
#[derive(Clone, Debug)]
pub struct Report {
    /// How the boot was modelled.
    pub boot: Boot,
    /// The entry point.
    pub entry: u32,
    /// The compressed streams of the ROM, decoded as sources.
    pub streams: Vec<Stream>,
    /// The segments.
    pub segments: Vec<Segment>,
    /// The code units, by segment and offset.
    pub units: Vec<Unit>,
    /// The function entries, by segment and address.
    pub functions: Vec<Function>,
    /// What was left unknown.
    pub unresolved: Vec<Unresolved>,
    /// The PI DMAs with constant operands.
    pub pi_dma: Vec<PiDma>,
    /// The RSP images.
    pub rsp: Vec<RspImage>,
    /// Counts.
    pub stats: Stats,
}

/// FNV-1a, 64-bit.
pub(crate) fn fnv(bytes: impl IntoIterator<Item = u8>) -> u64 {
    let mut h: u64 = 0xCBF2_9CE4_8422_2325;
    for b in bytes {
        h ^= u64::from(b);
        h = h.wrapping_mul(0x0100_0000_01B3);
    }
    h
}

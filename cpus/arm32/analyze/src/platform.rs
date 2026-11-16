//! What discovery needs to know about the machine the code runs on.

use relift_cpu_arm32::decode::Profile;

/// What a memory address holds, as discovery sees it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Kind {
    /// The code image: bytes that never change, read as constants.
    Image,
    /// Work RAM: code there runs from the copies discovery proves.
    Ram,
    /// The BIOS: calls into it are left unresolved.
    Bios,
    /// Anything else (I/O, video memory, save memory, unmapped).
    Other,
}

/// What a BIOS call (`SWI`/`SVC`) does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BiosCall {
    /// It returns and leaves `r4`-`r14` as they were.
    Returns,
    /// It never returns (a reset).
    NoReturn,
    /// `CpuSet`: `r0` source, `r1` destination, `r2` count and mode.
    CpuSet,
    /// `CpuFastSet`: `r0` source, `r1` destination, `r2` word count.
    CpuFastSet,
}

/// A DMA channel enabled with the register values at that point: the
/// values not known are `None`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Dma {
    /// The channel.
    pub channel: u8,
    /// The source address.
    pub src: Option<u32>,
    /// The destination address.
    pub dst: Option<u32>,
    /// The count register.
    pub count: Option<u32>,
    /// The control register.
    pub control: u16,
}

/// The machine the code runs on: its CPU profile, memory map, code image
/// and the meaning of its BIOS calls, DMA and IRQ vector.
pub trait Platform {
    /// The CPU profile instructions are lifted for.
    fn profile(&self) -> Profile;

    /// What `addr` holds.
    fn kind(&self, addr: u32) -> Kind;

    /// The image bytes at `addr..addr + n`, when they are all in the image.
    fn image(&self, addr: u32, n: u32) -> Option<&[u8]>;

    /// Whether a word holding `addr` names code in the image: the entries
    /// of tables of code addresses and the code addresses taken.
    fn code_addr(&self, addr: u32) -> bool;

    /// Whether a word stored at the RAM address `addr` installs the IRQ
    /// handler.
    fn irq_vector(&self, addr: u32) -> bool;

    /// What BIOS call `number` does.
    fn bios_call(&self, number: u8) -> BiosCall;

    /// The copy an immediate DMA transfer makes: source, destination and
    /// length in bytes; `None` when it makes none discovery can prove.
    fn dma(&self, d: &Dma) -> Option<(u32, u32, u32)>;

    /// The image as segments, base and bytes, for the scan of every image
    /// word ([`crate::Options::scan_words`]); where segments overlap, the
    /// last one holding an address is the one it is in.
    fn segments(&self) -> Vec<(u32, &[u8])> {
        Vec::new()
    }

    /// Whether `addr` is code of another unit, analyzed in a run of its
    /// own: a call there returns and keeps what the procedure call
    /// standard keeps.
    fn external(&self, addr: u32) -> bool {
        let _ = addr;
        false
    }

    /// Whether code at `addr` is this run's to find through the code
    /// addresses it takes: in a run of one unit over others, the
    /// candidates in the others are left to their own runs.
    fn owns(&self, addr: u32) -> bool {
        let _ = addr;
        true
    }
}

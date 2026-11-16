//! Function discovery for Game Boy Advance ROMs across ARM and Thumb code.
//!
//! The analysis is `relift-cpu-arm32-analyze` on the GBA machine: the
//! ARMv4T profile, the memory map ([`Region`]), the cartridge ROM in its
//! three wait regions as the image, the IRQ vector at `0x03007FFC` (and its
//! mirrors in internal work RAM), the BIOS copy calls `CpuSet` (`SWI 0x0B`)
//! and `CpuFastSet` (`SWI 0x0C`), the resets `SWI 0x00` and `SWI 0x26` and
//! immediate DMA on channels 1-3. Discovery starts at the reset entry
//! ([`analyze`]) or, for a multiboot image loaded to work RAM, at its entry
//! ([`analyze_multiboot`]).

use std::fmt;
use std::rc::Rc;

use relift_cpu_arm32_analyze::{BiosCall, Dma, Kind, Platform, Profile, discover, discover_with};
use relift_gba_formats::header::{MAX_ROM, MULTIBOOT_BASE, MULTIBOOT_ENTRY};
use relift_gba_formats::rom::OFFSET_MASK;
use relift_gba_formats::{Error as FormatError, Header};

pub use relift_cpu_arm32_analyze::{
    CopyKind, Edge, EdgeKind, Function, Loc, Origin, RamCopy, RegionCoverage, Report, Swi, Table,
    Unresolved, UnresolvedKind,
};

/// The memory regions code can run from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Region {
    /// The BIOS, `0x00000000-0x00003FFF`.
    Bios,
    /// External work RAM, `0x02xxxxxx`.
    Ewram,
    /// Internal work RAM, `0x03xxxxxx`.
    Iwram,
    /// The cartridge ROM in its three wait regions, `0x08-0x0D`.
    Rom,
    /// Anything else (I/O, palette, VRAM, OAM, SRAM, unmapped).
    Other,
}

impl Region {
    /// The region of `addr`.
    #[must_use]
    pub fn of(addr: u32) -> Region {
        match addr >> 24 {
            0x00 if addr < 0x4000 => Region::Bios,
            0x02 => Region::Ewram,
            0x03 => Region::Iwram,
            0x08..=0x0D => Region::Rom,
            _ => Region::Other,
        }
    }

    /// Whether the region is work RAM.
    #[must_use]
    pub fn is_ram(self) -> bool {
        matches!(self, Region::Ewram | Region::Iwram)
    }

    /// The lowercase name.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Region::Bios => "bios",
            Region::Ewram => "ewram",
            Region::Iwram => "iwram",
            Region::Rom => "rom",
            Region::Other => "other",
        }
    }
}

/// Why an image cannot be analyzed.
#[derive(Debug)]
pub enum AnalyzeError {
    /// The header does not parse.
    Format(FormatError),
    /// The image is larger than the cartridge space.
    TooLarge(usize),
    /// The image has no multiboot entry, or is larger than work RAM.
    NotMultiboot,
}

impl fmt::Display for AnalyzeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AnalyzeError::Format(e) => write!(f, "{e}"),
            AnalyzeError::TooLarge(n) => {
                write!(f, "image of {n} bytes exceeds the cartridge space")
            }
            AnalyzeError::NotMultiboot => {
                f.write_str("no multiboot entry branch at 0xC0, or larger than work RAM")
            }
        }
    }
}

impl std::error::Error for AnalyzeError {}

/// The reset entry.
pub const RESET: u32 = 0x0800_0000;

/// The IRQ vector in internal work RAM.
const IRQ_VECTOR: u32 = 0x0300_7FFC;

/// The Game Boy Advance with the cartridge image `rom` (empty for a
/// multiboot image, which is a load copy instead).
struct Gba<'a> {
    rom: &'a [u8],
}

impl Platform for Gba<'_> {
    fn profile(&self) -> Profile {
        Profile::V4t
    }

    fn kind(&self, addr: u32) -> Kind {
        match Region::of(addr) {
            Region::Bios => Kind::Bios,
            Region::Ewram | Region::Iwram => Kind::Ram,
            Region::Rom => Kind::Image,
            Region::Other => Kind::Other,
        }
    }

    fn image(&self, addr: u32, n: u32) -> Option<&[u8]> {
        if !(0x08..=0x0C).contains(&(addr >> 24)) {
            return None;
        }
        let off = (addr & OFFSET_MASK) as usize;
        self.rom.get(off..off + n as usize)
    }

    fn code_addr(&self, addr: u32) -> bool {
        addr >> 24 == 0x08 && self.image(addr, 1).is_some()
    }

    fn irq_vector(&self, addr: u32) -> bool {
        addr & 0x00FF_7FFF == IRQ_VECTOR & 0x00FF_7FFF && Region::of(addr) == Region::Iwram
    }

    fn bios_call(&self, number: u8) -> BiosCall {
        match number {
            0x00 | 0x26 => BiosCall::NoReturn,
            0x0B => BiosCall::CpuSet,
            0x0C => BiosCall::CpuFastSet,
            _ => BiosCall::Returns,
        }
    }

    fn dma(&self, d: &Dma) -> Option<(u32, u32, u32)> {
        let c = u32::from(d.control);
        let timing = (c >> 12) & 3;
        let src_ctl = (c >> 7) & 3;
        let dst_ctl = (c >> 5) & 3;
        if timing != 0 || src_ctl != 0 || !(dst_ctl == 0 || dst_ctl == 3) || d.channel == 0 {
            return None;
        }
        let (src, dst, count) = (d.src?, d.dst?, d.count?);
        let count = match (count, d.channel) {
            (0, 3) => 0x1_0000,
            (0, _) => 0x4000,
            (n, _) => n,
        };
        let unit = if c & 0x400 != 0 { 4 } else { 2 };
        Some((src, dst, count * unit))
    }
}

/// Find the functions of the image `rom` run as a cartridge, from the
/// reset entry at `0x08000000`.
///
/// # Errors
///
/// The header does not parse, or the image is larger than 32 MiB.
pub fn analyze(rom: &[u8]) -> Result<Report, AnalyzeError> {
    analyze_with(rom, &[])
}

/// As [`analyze`], with `hints` as roots too: code addresses a run reached
/// that discovery did not (the hints of a stop report, [`Origin::Hint`]).
///
/// # Errors
///
/// As [`analyze`].
pub fn analyze_with(rom: &[u8], hints: &[Loc]) -> Result<Report, AnalyzeError> {
    if rom.len() > MAX_ROM {
        return Err(AnalyzeError::TooLarge(rom.len()));
    }
    Header::parse(rom).map_err(AnalyzeError::Format)?;
    Ok(discover_with(
        &Gba { rom },
        Vec::new(),
        &[Loc::rom(RESET, false)],
        hints,
    ))
}

/// Find the functions of the multiboot image `rom` as the BIOS runs it:
/// loaded to `0x02000000` in external work RAM and entered at its
/// entry word at `0xC0`, with no cartridge. The image is copy 0 of the
/// report, of kind [`CopyKind::Load`].
///
/// # Errors
///
/// The header does not parse, or the image has no multiboot entry branch
/// or does not fit work RAM.
pub fn analyze_multiboot(rom: &[u8]) -> Result<Report, AnalyzeError> {
    let h = Header::parse(rom).map_err(AnalyzeError::Format)?;
    if h.multiboot_entry.is_none() {
        return Err(AnalyzeError::NotMultiboot);
    }
    let site = Loc {
        addr: MULTIBOOT_BASE + MULTIBOOT_ENTRY as u32,
        thumb: false,
        copy: Some(0),
    };
    let load = RamCopy {
        site,
        kind: CopyKind::Load,
        src: None,
        dst: MULTIBOOT_BASE,
        bytes: Rc::from(rom),
    };
    Ok(discover(&Gba { rom: &[] }, vec![load], &[site]))
}

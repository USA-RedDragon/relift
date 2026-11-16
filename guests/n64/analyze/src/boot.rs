//! The boot code and how it places the game.
//!
//! The PIF copies the first 4 KiB of the ROM to SP DMEM and starts the CPU
//! at `0xA4000040`, the boot code there. For a boot variant known by its
//! hash, that code is lifted like any other, rooted at that address, and
//! runs at startup. Where it loads the game is also given as a segment, so
//! the game's code is found without following the boot code's transfers:
//! the boot variant's rule, or the loadable segments of an ELF image that
//! a boot code not known by its hash loads (such a boot code is not
//! lifted; the runtime then boots the game at a high level).

use relift_n64_formats::{Cic, Rom};

use crate::image::{Segment, SegmentOrigin, Source};
use crate::report::Boot;

/// The bytes the PIF copies to SP DMEM.
pub(crate) const CODE_LEN: u32 = 0x1000;
/// Where the PIF starts the CPU: the boot code in SP DMEM.
pub const CODE_ENTRY: u32 = 0xA400_0040;

/// The ROM offset of the boot image.
pub(crate) const BOOT_OFFSET: u32 = 0x1000;
/// The length of the boot image.
pub(crate) const BOOT_LEN: u32 = 0x10_0000;
/// Where an ELF image is looked for: every 256 bytes from the boot image
/// offset up to this one.
const ELF_SEARCH_END: usize = 0x10_0000;

/// The boot model, the entry point and the segments it loads, with the
/// boot code's segment last.
pub(crate) fn boot(rom: &Rom, cic: Option<Cic>) -> (Boot, u32, Vec<Segment>) {
    let (kind, entry, mut segs) = game(rom, cic);
    if matches!(kind, Boot::Standard(_)) && rom.bytes.len() >= CODE_LEN as usize {
        segs.push(Segment {
            source: Source::Rom,
            offset: 0,
            len: CODE_LEN,
            link: Some(CODE_ENTRY & !0xFFF),
            origin: SegmentOrigin::BootCode,
        });
    }
    (kind, entry, segs)
}

fn game(rom: &Rom, cic: Option<Cic>) -> (Boot, u32, Vec<Segment>) {
    let bytes = &rom.bytes;
    let header_entry = rom.header().entry;
    let standard = |entry: u32| {
        let len = (bytes.len() as u32)
            .saturating_sub(BOOT_OFFSET)
            .min(BOOT_LEN)
            & !3;
        vec![Segment {
            source: Source::Rom,
            offset: BOOT_OFFSET,
            len,
            link: Some(entry),
            origin: SegmentOrigin::Boot,
        }]
    };
    if let Some(c) = cic {
        let entry = c.entry(header_entry);
        return (Boot::Standard(c), entry, standard(entry));
    }
    let end = bytes.len().min(ELF_SEARCH_END);
    for off in (BOOT_OFFSET as usize..end).step_by(0x100) {
        if let Some((entry, segs)) = elf(bytes, off) {
            return (Boot::Elf(off as u32), entry, segs);
        }
    }
    (Boot::Unknown, header_entry, standard(header_entry))
}

fn be32(b: &[u8], at: usize) -> Option<u32> {
    let w = b.get(at..at + 4)?;
    Some(u32::from_be_bytes([w[0], w[1], w[2], w[3]]))
}

fn be16(b: &[u8], at: usize) -> Option<u16> {
    let w = b.get(at..at + 2)?;
    Some(u16::from_be_bytes([w[0], w[1]]))
}

/// A 32-bit big-endian MIPS ELF executable at `off`: its entry point and
/// a segment for each loadable program header with file bytes at a kseg0
/// or kseg1 address.
fn elf(b: &[u8], off: usize) -> Option<(u32, Vec<Segment>)> {
    let h = b.get(off..off + 0x34)?;
    if h[..4] != *b"\x7fELF" || h[4] != 1 || h[5] != 2 || be16(h, 0x12)? != 8 {
        return None;
    }
    let entry = be32(h, 0x18)?;
    let phoff = be32(h, 0x1C)? as usize;
    let phentsize = usize::from(be16(h, 0x2A)?);
    let phnum = usize::from(be16(h, 0x2C)?);
    if phentsize < 0x20 || phnum == 0 || phnum > 64 {
        return None;
    }
    let mut segs = Vec::new();
    for i in 0..phnum {
        let p = off + phoff + i * phentsize;
        if be32(b, p)? != 1 {
            continue;
        }
        let file = be32(b, p + 4)? as usize;
        let vaddr = be32(b, p + 8)?;
        let filesz = be32(b, p + 0x10)? & !3;
        let at = off.checked_add(file)?;
        if filesz == 0 || vaddr >> 30 != 2 || at + filesz as usize > b.len() {
            continue;
        }
        segs.push(Segment {
            source: Source::Rom,
            offset: at as u32,
            len: filesz,
            link: Some(vaddr),
            origin: SegmentOrigin::Elf,
        });
    }
    let holds = segs.iter().any(|s| s.holds(entry));
    holds.then_some((entry, segs))
}

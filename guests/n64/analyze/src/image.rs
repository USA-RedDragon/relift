//! Where code bytes come from and where they are linked: the sources (the
//! ROM and every compressed stream decoded from it) and the segments that
//! place a run of a source's bytes at a link address.

use relift_n64_formats::Stream;

/// Where a run of bytes comes from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Source {
    /// The ROM, by its big-endian file offset.
    Rom,
    /// The decoded bytes of [`crate::Report::streams`] entry `n`.
    Stream(u32),
}

/// The ROM and its decoded streams.
pub(crate) struct Sources<'a> {
    pub rom: &'a [u8],
    pub streams: Vec<(Stream, Vec<u8>)>,
}

impl Sources<'_> {
    pub(crate) fn bytes(&self, s: Source) -> &[u8] {
        match s {
            Source::Rom => self.rom,
            Source::Stream(i) => &self.streams[i as usize].1,
        }
    }

    pub(crate) fn word(&self, s: Source, off: u32) -> Option<u32> {
        let b = self.bytes(s);
        let o = off as usize;
        let w = b.get(o..o.checked_add(4)?)?;
        Some(u32::from_be_bytes([w[0], w[1], w[2], w[3]]))
    }

    pub(crate) fn all(&self) -> impl Iterator<Item = Source> + '_ {
        std::iter::once(Source::Rom).chain((0..self.streams.len() as u32).map(Source::Stream))
    }
}

/// Why a segment exists.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SegmentOrigin {
    /// The boot image: `ROM[0x1000..]` at the entry point, 1 MiB.
    Boot,
    /// A loadable segment of an ELF image the boot code loads.
    Elf,
    /// A PI DMA from the cartridge with constant operands, made by the
    /// code at this address.
    PiDma {
        /// The address of the store that starts the DMA.
        site: u32,
    },
    /// A code region of a source found by the scan, linked at the address
    /// its own `JAL` targets vote for, or not linked.
    Region {
        /// The votes for the link address (zero when not linked).
        votes: u32,
    },
    /// A ROM range and its link address from a segment table in a source:
    /// a word that is the range's start, the next its end, and the next a
    /// kseg0 address, the shape a build tool's segment table gives each
    /// segment a loader moves.
    Table {
        /// The offset of the table entry in its source.
        at: u32,
    },
    /// An exception vector trampoline placed at a vector address, where a
    /// program copies it.
    Vector {
        /// The trampoline's address in the segment it was found in.
        from: u32,
    },
}

/// A run of source bytes placed at a link address.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Segment {
    /// Where the bytes come from.
    pub source: Source,
    /// Their offset in the source.
    pub offset: u32,
    /// Their length.
    pub len: u32,
    /// The kseg0 address the first byte is linked at; `None` for a region
    /// whose link address is not known, which is followed only by its own
    /// relative branches.
    pub link: Option<u32>,
    /// Why it exists.
    pub origin: SegmentOrigin,
}

/// Where a segment with no link address is placed for the analysis.
pub(crate) const FLAT_BASE: u32 = 0x8800_0000;

impl Segment {
    pub(crate) fn base(&self) -> u32 {
        self.link.unwrap_or(FLAT_BASE)
    }

    /// The offset into the segment of a kseg0 or kseg1 address, or of a
    /// physical address when `phys`.
    pub(crate) fn offset_of(&self, addr: u32, phys: bool) -> Option<u32> {
        let a = if phys {
            addr
        } else if addr >> 30 == 2 {
            addr & crate::val::PHYS
        } else {
            return None;
        };
        let rel = a.wrapping_sub(self.base() & crate::val::PHYS);
        (rel < self.len).then_some(rel)
    }

    /// The source offset of the kseg0 or kseg1 address `addr`, when the
    /// segment holds it.
    #[must_use]
    pub fn source_offset(&self, addr: u32) -> Option<u32> {
        self.offset_of(addr, false).map(|r| self.offset + r)
    }

    /// Whether the segment holds the kseg0 or kseg1 address `addr`.
    pub(crate) fn holds(&self, addr: u32) -> bool {
        self.offset_of(addr, false).is_some()
    }

    /// The kseg0 address of segment offset `rel`.
    pub(crate) fn addr(&self, rel: u32) -> u32 {
        self.base().wrapping_add(rel)
    }

    /// Whether it is linked at a known address that any code may reach.
    pub(crate) fn shared(&self) -> bool {
        !matches!(self.origin, SegmentOrigin::Region { .. })
    }
}

/// The segments of one analysis.
pub(crate) struct Image<'a> {
    pub src: &'a Sources<'a>,
    pub segs: Vec<Segment>,
}

impl Image<'_> {
    /// The instruction word at `addr` of segment `seg`.
    pub(crate) fn word(&self, seg: u32, addr: u32) -> Option<u32> {
        let s = &self.segs[seg as usize];
        let rel = s.offset_of(addr, false)?;
        if !rel.is_multiple_of(4) || rel + 4 > s.len {
            return None;
        }
        self.src.word(s.source, s.offset + rel)
    }

    /// `n` bytes (1, 2, 4 or 8) at physical address `pa`, read from
    /// segment `seg` when it holds them, else from the one shared segment
    /// that does; `None` when no segment or more than one other does.
    /// Gives the segment and offset read too.
    pub(crate) fn read(&self, seg: u32, pa: u32, n: u32) -> Option<(u64, u32, u32)> {
        let here = &self.segs[seg as usize];
        let pick = if here.offset_of(pa, true).is_some() {
            seg
        } else {
            let mut hit = None;
            for (i, s) in self.segs.iter().enumerate() {
                if s.shared() && s.offset_of(pa, true).is_some() {
                    if hit.is_some() {
                        return None;
                    }
                    hit = Some(i as u32);
                }
            }
            hit?
        };
        let s = &self.segs[pick as usize];
        let rel = s.offset_of(pa, true)?;
        if rel + n > s.len {
            return None;
        }
        let b = self.src.bytes(s.source);
        let at = (s.offset + rel) as usize;
        let bytes = b.get(at..at + n as usize)?;
        let v = bytes.iter().fold(0u64, |a, x| a << 8 | u64::from(*x));
        Some((v, pick, rel))
    }

    /// The segments a transfer from segment `from` to `addr` may reach:
    /// `from` itself when it holds the address; otherwise, from a shared
    /// segment, every shared segment that holds it, or every region that
    /// does when none does.
    pub(crate) fn targets(&self, from: u32, addr: u32) -> Vec<u32> {
        let here = &self.segs[from as usize];
        if here.holds(addr) {
            return vec![from];
        }
        if !here.shared() {
            return Vec::new();
        }
        let hits: Vec<u32> = (0..self.segs.len() as u32)
            .filter(|i| {
                let s = &self.segs[*i as usize];
                s.shared() && s.holds(addr)
            })
            .collect();
        if !hits.is_empty() {
            return hits;
        }
        (0..self.segs.len() as u32)
            .filter(|i| {
                let s = &self.segs[*i as usize];
                s.link.is_some() && s.holds(addr)
            })
            .collect()
    }
}

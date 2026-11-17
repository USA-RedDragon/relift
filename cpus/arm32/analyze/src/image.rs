//! The bytes code and constants come from: the platform's code image and
//! the proven copies to RAM.

use std::collections::HashSet;

use crate::config::Options;
use crate::loc::Loc;
use crate::platform::{Kind, Platform};
use crate::report::{CopyKind, RamCopy};

#[derive(Clone)]
pub(crate) struct Image<'a> {
    pub platform: &'a dyn Platform,
    pub options: Options,
    pub copies: Vec<RamCopy>,
    /// The image bytes discovered code stores to at constant addresses:
    /// variables, whose image bytes are only their first values.
    pub written: HashSet<u32>,
    /// Seeds that open no stack frame: candidates that become entries as
    /// code addresses taken do, once their own run checks out.
    pub seeds: Vec<Loc>,
}

fn le(b: &[u8]) -> u32 {
    b.iter()
        .rev()
        .fold(0u32, |acc, x| (acc << 8) | u32::from(*x))
}

impl<'a> Image<'a> {
    pub(crate) fn new(platform: &'a dyn Platform, options: Options) -> Image<'a> {
        Image {
            platform,
            options,
            copies: Vec::new(),
            written: HashSet::new(),
            seeds: Vec::new(),
        }
    }

    /// Whether code stores to one of the image bytes `addr..addr + n`.
    pub(crate) fn variable(&self, addr: u32, n: u32) -> bool {
        self.platform.image_writable()
            || (0..n).any(|i| self.written.contains(&addr.wrapping_add(i)))
    }

    /// What `addr` holds.
    pub(crate) fn kind(&self, addr: u32) -> Kind {
        self.platform.kind(addr)
    }

    /// Whether `addr` is in work RAM.
    pub(crate) fn is_ram(&self, addr: u32) -> bool {
        self.kind(addr) == Kind::Ram
    }

    /// The image bytes at `addr`.
    pub(crate) fn rom_bytes(&self, addr: u32, n: u32) -> Option<&'a [u8]> {
        self.platform.image(addr, n)
    }

    /// Reads `n` bytes (1, 2 or 4) at `addr` as the code of `copy` sees
    /// them, when they are in the image or in that copy.
    pub(crate) fn read(&self, copy: Option<u16>, addr: u32, n: u32) -> Option<u32> {
        if let Some(i) = copy
            && self.is_ram(addr)
        {
            let c = self.copies.get(usize::from(i))?;
            let off = addr.checked_sub(c.dst)? as usize;
            return c.bytes.get(off..off + n as usize).map(le);
        }
        self.rom_bytes(addr, n).map(le)
    }

    /// The instruction word or halfword at `loc`.
    pub(crate) fn fetch(&self, loc: Loc) -> Option<u32> {
        if !loc.addr.is_multiple_of(loc.size()) {
            return None;
        }
        match (self.kind(loc.addr), loc.copy) {
            (Kind::Image, None) => self.read(None, loc.addr, loc.size()),
            (Kind::Ram, Some(_)) => self.read(loc.copy, loc.addr, loc.size()),
            _ => None,
        }
    }

    /// The copies whose destination holds `addr`, but for rewrites, which
    /// code reaches only from inside them.
    pub(crate) fn copies_at(&self, addr: u32) -> Vec<u16> {
        self.copies
            .iter()
            .enumerate()
            .filter(|(_, c)| {
                addr.wrapping_sub(c.dst) < c.len() && !matches!(c.kind, CopyKind::Rewrite(_))
            })
            .filter_map(|(i, _)| u16::try_from(i).ok())
            .collect()
    }

    /// Whether copy `c` holds `addr`.
    pub(crate) fn holds(&self, c: u16, addr: u32) -> bool {
        self.copies
            .get(usize::from(c))
            .is_some_and(|c| addr.wrapping_sub(c.dst) < c.len())
    }

    /// The copy a rewrite `c` was made from, or `c`.
    /// Whether copy `c` is one function of a table a writer copies
    /// ([`CopyKind::Table`]): an alternative to the other copies at its
    /// address, not a rewrite of them.
    pub(crate) fn is_table(&self, c: u16) -> bool {
        matches!(
            self.copies.get(usize::from(c)).map(|c| c.kind),
            Some(CopyKind::Table(_))
        )
    }

    pub(crate) fn base(&self, c: u16) -> u16 {
        match self.copies.get(usize::from(c)).map(|c| c.kind) {
            Some(CopyKind::Rewrite(b)) => b,
            _ => c,
        }
    }

    /// The bytes a copy can read at `src`: the image, or a loaded image
    /// where the loader put it.
    pub(crate) fn source(&self, src: u32, n: u32) -> Option<&[u8]> {
        if let Some(b) = self.rom_bytes(src, n) {
            return Some(b);
        }
        let c = self.copies.iter().find(|c| c.kind == CopyKind::Load)?;
        let off = src.checked_sub(c.dst)? as usize;
        c.bytes.get(off..off + n as usize)
    }

    /// The `n` bytes at `src` that code copies from there: the image, a
    /// loaded image, or a copy in RAM that holds them (a program that runs
    /// from a copy and copies its own code on).
    pub(crate) fn code_bytes(&self, src: u32, n: u32) -> Option<&[u8]> {
        self.source(src, n).or_else(|| {
            self.copies_at(src).into_iter().find_map(|i| {
                let c = self.copies.get(usize::from(i))?;
                let off = src.checked_sub(c.dst)? as usize;
                c.bytes.get(off..off.checked_add(n as usize)?)
            })
        })
    }

    /// Whether `addr` is in the image or in a loaded image.
    pub(crate) fn is_source(&self, addr: u32) -> bool {
        self.source(addr, 1).is_some()
    }

    /// Whether `addr` is inside the image.
    pub(crate) fn in_image(&self, addr: u32) -> bool {
        self.rom_bytes(addr, 1).is_some()
    }
}

//! A processor's static binary: decompressed, with its autoload blocks.

use std::ops::Range;

use crate::blz;
use crate::bytes::{slice, u32_at};
use crate::error::{Error, Result};
use crate::header::Binary;
use crate::params::{self, ModuleParams};

/// Which processor a binary runs on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Cpu {
    /// The ARM946E-S.
    Arm9,
    /// The ARM7TDMI.
    Arm7,
}

impl Cpu {
    /// The lowercase name used in file names: `arm9` or `arm7`.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Cpu::Arm9 => "arm9",
            Cpu::Arm7 => "arm7",
        }
    }
}

/// One autoload block: a range the start code copies to its address and
/// then follows with zeroed bss.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Autoload {
    /// The address the block loads to.
    pub addr: u32,
    /// Size of the data.
    pub size: u32,
    /// Size of the bss that follows the data.
    pub bss: u32,
    /// Where the data lies in [`StaticUnit::image`].
    pub range: Range<usize>,
}

/// A static binary as the program sees it once the start code has run its
/// decompression.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StaticUnit<'a> {
    /// The processor.
    pub cpu: Cpu,
    /// The address the binary loads to.
    pub base: u32,
    /// The entry address.
    pub entry: u32,
    /// The binary as stored in the image.
    pub raw: &'a [u8],
    /// The module parameters, when found.
    pub params: Option<ModuleParams>,
    /// Whether the static was BLZ-compressed.
    pub compressed: bool,
    /// The decompressed binary; with the compressed static end cleared, as
    /// the start code leaves it.
    pub image: Vec<u8>,
    /// The autoload blocks, in list order.
    pub autoloads: Vec<Autoload>,
}

impl<'a> StaticUnit<'a> {
    /// Load the static binary of `cpu` that `binary` describes out of
    /// the image `rom`.
    ///
    /// # Errors
    ///
    /// The binary or its parts lie outside the image, or the compressed
    /// static does not decode.
    pub fn load(rom: &'a [u8], cpu: Cpu, binary: &Binary) -> Result<StaticUnit<'a>> {
        let off = u64::from(binary.rom_offset);
        let size = u64::from(binary.size);
        let raw = slice(rom, "static binary", off, size)?;
        let footer = (cpu == Cpu::Arm9)
            .then(|| slice(rom, "static footer", off + size, params::FOOTER_LEN as u64).ok())
            .flatten();
        let params = params::find(raw, binary.ram, footer);
        let mut unit = StaticUnit {
            cpu,
            base: binary.ram,
            entry: binary.entry,
            raw,
            params,
            compressed: false,
            image: raw.to_vec(),
            autoloads: Vec::new(),
        };
        if let Some(p) = params {
            unit.decode(&p)?;
        }
        Ok(unit)
    }

    fn decode(&mut self, p: &ModuleParams) -> Result<()> {
        let ram = self.base;
        if p.compressed_end != 0 {
            let end = p
                .compressed_end
                .checked_sub(ram)
                .map(|e| e as usize)
                .filter(|e| *e <= self.raw.len())
                .ok_or(Error::Malformed("compressed static end outside the binary"))?;
            let mut image = blz::decompress(&self.raw[..end]).map_err(|source| Error::Blz {
                what: "static binary",
                source,
            })?;
            image.extend_from_slice(&self.raw[end..]);
            let field = p.offset + params::COMPRESSED_END;
            image
                .get_mut(field..field + 4)
                .ok_or(Error::Malformed("module parameters outside the image"))?
                .fill(0);
            self.image = image;
            self.compressed = true;
        }
        let rel = |a: u32, what: &'static str| {
            a.checked_sub(ram)
                .map(|v| v as usize)
                .ok_or(Error::Malformed(what))
        };
        let start = rel(p.autoload_start, "autoload list start below the binary")?;
        let end = rel(p.autoload_end, "autoload list end below the binary")?;
        let mut data = rel(p.autoload_data, "autoload data start below the binary")?;
        let mut at = start;
        while at < end {
            let w = |i: usize| {
                u32_at(&self.image, at + i)
                    .ok_or(Error::Malformed("autoload list outside the image"))
            };
            let (addr, size, bss) = (w(0)?, w(4)?, w(8)?);
            let range = data..data + size as usize;
            if range.end > self.image.len() {
                return Err(Error::Malformed("autoload data outside the image"));
            }
            data = range.end;
            self.autoloads.push(Autoload {
                addr,
                size,
                bss,
                range,
            });
            at += 12;
        }
        Ok(())
    }

    /// The data of autoload block `a` of this unit.
    #[must_use]
    pub fn autoload_data(&self, a: &Autoload) -> &[u8] {
        &self.image[a.range.clone()]
    }
}

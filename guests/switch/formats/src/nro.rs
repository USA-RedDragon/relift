//! NRO executables: homebrew and shared objects, stored flat (a segment's
//! file offset is its memory offset), with an optional asset section after
//! the module (icon, NACP, romfs).
//!
//! The layout is the one the switchbrew wiki documents (`NRO0`, the asset
//! header `ASET`). The module header is `0x80` bytes: a start word, the MOD0
//! offset, `NRO0`, version, size, flags, text, rodata and data extents, the
//! .bss size, the module ID, the DSO handle offset, the embedded (API info)
//! extent and the .dynstr and .dynsym extents. Those extents are image
//! offsets, unlike an NSO's, which count from .rodata.
//!
//! Round trips: [`Nro::header_bytes`] and the file keep every reserved
//! field as read, so a writer can reproduce an NRO from the parsed parts.
//! The compressed form (flag bit 1, recent firmware) is rejected as
//! unsupported rather than misread.

use std::sync::Arc;

use crate::bytes::{array, hex, slice, u32_at, u64_at};
use crate::error::{Error, Result, malformed};
use crate::nso::{Mod0, Segment, Symbol, parse_mod0, read_symbols};
use crate::romfs::Romfs;
use crate::storage::{Shared, SubStorage, read_vec};

/// The size of the NRO header.
pub const HEADER_SIZE: usize = 0x80;
/// The size of the asset header.
pub const ASSET_HEADER_SIZE: u64 = 0x38;

/// A byte range inside the asset section, relative to the asset header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AssetSection {
    /// The offset from the asset header.
    pub offset: u64,
    /// The size; 0 means the section is absent.
    pub size: u64,
}

impl AssetSection {
    /// Whether the section holds data.
    pub fn is_present(&self) -> bool {
        self.size != 0
    }
}

/// The `ASET` header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Assets {
    /// Where the asset header sits in the file (the NRO's `size`).
    pub header_offset: u64,
    /// The format version, currently 0.
    pub version: u32,
    /// The icon (a 256x256 JPEG).
    pub icon: AssetSection,
    /// The NACP.
    pub nacp: AssetSection,
    /// The romfs the application can mount.
    pub romfs: AssetSection,
}

/// A parsed NRO.
#[derive(Clone)]
pub struct Nro {
    storage: Shared,
    header: [u8; HEADER_SIZE],
    /// The format version.
    pub version: u32,
    /// The module size the header records (where assets start).
    pub size: u32,
    /// The flags.
    pub flags: u32,
    /// text, rodata, data; file offset equals memory offset.
    pub segments: [Segment; 3],
    /// The .bss size after data.
    pub bss_size: u32,
    /// The module ID (build ID), 32 bytes.
    pub build_id: [u8; 32],
    /// The DSO handle offset.
    pub dso_handle_offset: u32,
    /// The embedded (API info) extent, offset and size.
    pub embedded: (u32, u32),
    /// The .dynstr extent, offset and size.
    pub dynstr: (u32, u32),
    /// The .dynsym extent, offset and size.
    pub dynsym: (u32, u32),
    /// The module image: the file from byte 0 to the end of data.
    pub image: Vec<u8>,
    /// The asset header, if the file has one.
    pub assets: Option<Assets>,
}

impl Nro {
    /// Parse the NRO in `storage`: the header, the segment extents and the
    /// asset header. The image is read into memory; assets are read on
    /// request.
    ///
    /// # Errors
    ///
    /// A short file, bad magic, segments that overlap or leave the file, a
    /// size past the file, an `ASET` section outside the file, or a
    /// compressed NRO ([`Error::Unsupported`]).
    pub fn open(storage: Shared) -> Result<Nro> {
        let fsize = storage.size();
        let header_raw = read_vec(&*storage, 0, (HEADER_SIZE as u64).min(fsize))?;
        if header_raw.len() < HEADER_SIZE {
            return malformed("NRO: shorter than its header");
        }
        if &header_raw[0x10..0x14] != b"NRO0" {
            return malformed("NRO: bad magic");
        }
        let header: [u8; HEADER_SIZE] = array(&header_raw, 0, "NRO header")?;
        let size = u32_at(&header, 0x18, "NRO")?;
        let flags = u32_at(&header, 0x1C, "NRO")?;
        if flags & 2 != 0 {
            return Err(Error::Unsupported("compressed NRO (flag bit 1)".into()));
        }
        if u64::from(size) > fsize {
            return malformed(format!(
                "NRO: header size {size:#x} is past the file ({fsize:#x})"
            ));
        }
        let mut segments = [Segment {
            memory_offset: 0,
            size: 0,
        }; 3];
        let mut prev_end = 0u64;
        for (i, seg) in segments.iter_mut().enumerate() {
            let offset = u32_at(&header, 0x20 + i * 8, "NRO")?;
            let len = u32_at(&header, 0x24 + i * 8, "NRO")?;
            let name = ["text", "rodata", "data"][i];
            let end = u64::from(offset) + u64::from(len);
            if u64::from(offset) < prev_end {
                return malformed(format!("NRO {name}: overlaps the previous segment"));
            }
            if end > u64::from(size) {
                return malformed(format!("NRO {name}: runs past the module size"));
            }
            *seg = Segment {
                memory_offset: offset,
                size: len,
            };
            prev_end = end;
        }
        if prev_end < HEADER_SIZE as u64 {
            return malformed("NRO: the segments end inside the header");
        }
        let image = read_vec(&*storage, 0, prev_end)?;
        let assets = if fsize > u64::from(size) {
            read_assets(&*storage, u64::from(size))?
        } else {
            None
        };
        Ok(Nro {
            storage,
            version: u32_at(&header, 0x14, "NRO")?,
            size,
            flags,
            segments,
            bss_size: u32_at(&header, 0x38, "NRO")?,
            build_id: array(&header, 0x40, "NRO")?,
            dso_handle_offset: u32_at(&header, 0x60, "NRO")?,
            embedded: (u32_at(&header, 0x68, "NRO")?, u32_at(&header, 0x6C, "NRO")?),
            dynstr: (u32_at(&header, 0x70, "NRO")?, u32_at(&header, 0x74, "NRO")?),
            dynsym: (u32_at(&header, 0x78, "NRO")?, u32_at(&header, 0x7C, "NRO")?),
            header,
            image,
            assets,
        })
    }

    /// The raw 0x80-byte header.
    pub fn header_bytes(&self) -> &[u8; HEADER_SIZE] {
        &self.header
    }

    /// The build ID as lowercase hex (all 32 bytes).
    pub fn build_id_hex(&self) -> String {
        hex(&self.build_id)
    }

    /// The bytes of segment `i` (0 text, 1 rodata, 2 data).
    ///
    /// # Errors
    ///
    /// `i` out of range.
    pub fn segment_bytes(&self, i: usize) -> Result<&[u8]> {
        let s = self
            .segments
            .get(i)
            .ok_or_else(|| Error::Format(format!("NRO has no segment {i}")))?;
        slice(
            &self.image,
            s.memory_offset as usize,
            s.size as usize,
            "NRO segment",
        )
    }

    /// The flat module image with .bss zero-filled.
    pub fn flat_image(&self) -> Vec<u8> {
        let mut v = self.image.clone();
        v.resize(v.len() + self.bss_size as usize, 0);
        v
    }

    /// The MOD0 header the start word at 4 points at, if there is one.
    ///
    /// # Errors
    ///
    /// A MOD0 offset past the image.
    pub fn mod0(&self) -> Result<Option<Mod0>> {
        parse_mod0(&self.image)
    }

    /// The dynamic symbols, from the .dynstr and .dynsym extents (image
    /// offsets). Empty extents give no symbols.
    ///
    /// # Errors
    ///
    /// Extents past the image or a name past .dynstr.
    pub fn symbols(&self) -> Result<Vec<Symbol>> {
        if self.dynsym.1 == 0 {
            return Ok(Vec::new());
        }
        let strtab = slice(
            &self.image,
            self.dynstr.0 as usize,
            self.dynstr.1 as usize,
            ".dynstr",
        )?;
        let symtab = slice(
            &self.image,
            self.dynsym.0 as usize,
            self.dynsym.1 as usize,
            ".dynsym",
        )?;
        read_symbols(strtab, symtab)
    }

    fn asset_bytes(&self, section: Option<AssetSection>) -> Result<Option<Vec<u8>>> {
        let (Some(a), Some(s)) = (self.assets, section) else {
            return Ok(None);
        };
        if !s.is_present() {
            return Ok(None);
        }
        Ok(Some(read_vec(
            &*self.storage,
            a.header_offset + s.offset,
            s.size,
        )?))
    }

    /// The icon bytes, if present.
    ///
    /// # Errors
    ///
    /// A read fails.
    pub fn icon(&self) -> Result<Option<Vec<u8>>> {
        self.asset_bytes(self.assets.map(|a| a.icon))
    }

    /// The raw NACP, if present.
    ///
    /// # Errors
    ///
    /// A read fails.
    pub fn nacp(&self) -> Result<Option<Vec<u8>>> {
        self.asset_bytes(self.assets.map(|a| a.nacp))
    }

    /// The romfs asset as a storage, if present.
    ///
    /// # Errors
    ///
    /// The window is outside the file.
    pub fn romfs_storage(&self) -> Result<Option<Shared>> {
        let Some(a) = self.assets else {
            return Ok(None);
        };
        if !a.romfs.is_present() {
            return Ok(None);
        }
        Ok(Some(SubStorage::shared(
            self.storage.clone(),
            a.header_offset + a.romfs.offset,
            a.romfs.size,
        )?))
    }

    /// The romfs asset, opened.
    ///
    /// # Errors
    ///
    /// The romfs is malformed.
    pub fn romfs(&self) -> Result<Option<Romfs>> {
        self.romfs_storage()?.map(Romfs::open).transpose()
    }
}

fn read_assets(storage: &dyn crate::storage::Storage, at: u64) -> Result<Option<Assets>> {
    let left = storage.size() - at;
    if left < 4 || read_vec(storage, at, 4)? != b"ASET" {
        return Ok(None);
    }
    if left < ASSET_HEADER_SIZE {
        return malformed("NRO: the asset header is cut short");
    }
    let h = read_vec(storage, at, ASSET_HEADER_SIZE)?;
    let section = |o: usize, name: &str| -> Result<AssetSection> {
        let s = AssetSection {
            offset: u64_at(&h, o, "ASET")?,
            size: u64_at(&h, o + 8, "ASET")?,
        };
        let end = s.offset.checked_add(s.size);
        if s.is_present() && end.is_none_or(|e| e > left) {
            return malformed(format!("NRO asset {name}: runs past the end of the file"));
        }
        Ok(s)
    };
    Ok(Some(Assets {
        header_offset: at,
        version: u32_at(&h, 4, "ASET")?,
        icon: section(8, "icon")?,
        nacp: section(0x18, "nacp")?,
        romfs: section(0x28, "romfs")?,
    }))
}

/// Inputs of a fixture NRO.
#[derive(Clone, Debug, Default)]
pub struct NroSpec {
    /// Bytes of .text after the header and the MOD0 block.
    pub text: Vec<u8>,
    /// Bytes of .rodata after the dynamic string and symbol tables.
    pub rodata: Vec<u8>,
    /// Bytes of .data.
    pub data: Vec<u8>,
    /// The .bss size.
    pub bss: u32,
    /// The module ID.
    pub build_id: [u8; 32],
    /// Defined dynamic symbols: name, value, size.
    pub symbols: Vec<(String, u64, u64)>,
    /// The icon asset.
    pub icon: Option<Vec<u8>>,
    /// The NACP asset.
    pub nacp: Option<Vec<u8>>,
    /// The romfs asset (a raw romfs image).
    pub romfs: Option<Vec<u8>>,
}

fn align(v: &mut Vec<u8>, to: usize) {
    v.resize(v.len().div_ceil(to) * to, 0);
}

fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

/// Build an NRO (fixtures): the header and a MOD0 block at the start of
/// .text, the dynamic tables at the start of .rodata, page-aligned
/// segments and, when any asset is given, an `ASET` section.
pub fn build(spec: &NroSpec) -> Vec<u8> {
    let page = |n: usize| n.div_ceil(0x1000) * 0x1000;
    let to32 = |n: usize| u32::try_from(n).unwrap_or(0);
    let mut text = vec![0u8; HEADER_SIZE + 0x20];
    text.extend_from_slice(&spec.text);
    text.resize(page(text.len()), 0);
    let ro_at = text.len();
    let mut strtab = vec![0u8];
    let mut symtab = vec![0u8; 24];
    for (name, value, size) in &spec.symbols {
        let mut e = [0u8; 24];
        e[..4].copy_from_slice(&to32(strtab.len()).to_le_bytes());
        e[4] = 0x12;
        e[6..8].copy_from_slice(&1u16.to_le_bytes());
        e[8..16].copy_from_slice(&value.to_le_bytes());
        e[16..24].copy_from_slice(&size.to_le_bytes());
        symtab.extend_from_slice(&e);
        strtab.extend_from_slice(name.as_bytes());
        strtab.push(0);
    }
    let mut ro = strtab.clone();
    align(&mut ro, 8);
    let symtab_at = ro.len();
    ro.extend_from_slice(&symtab);
    ro.extend_from_slice(&spec.rodata);
    ro.resize(page(ro.len()), 0);
    let data_at = ro_at + ro.len();
    let mut data = spec.data.clone();
    data.resize(page(data.len()), 0);
    let data_end = data_at + data.len();
    let rel = |to: usize| to32(to).wrapping_sub(HEADER_SIZE as u32);
    put32(&mut text, 4, HEADER_SIZE as u32);
    let m = HEADER_SIZE;
    text[m..m + 4].copy_from_slice(b"MOD0");
    put32(&mut text, m + 4, rel(data_at));
    put32(&mut text, m + 8, rel(data_end));
    put32(&mut text, m + 0xC, rel(data_end + spec.bss as usize));
    put32(&mut text, m + 0x10, rel(ro_at));
    put32(&mut text, m + 0x14, rel(ro_at));
    put32(&mut text, m + 0x18, rel(data_end));
    let mut file = text;
    file.extend_from_slice(&ro);
    file.extend_from_slice(&data);
    let size = file.len();
    file[0x10..0x14].copy_from_slice(b"NRO0");
    put32(&mut file, 0x18, to32(size));
    put32(&mut file, 0x20, 0);
    put32(&mut file, 0x24, to32(ro_at));
    put32(&mut file, 0x28, to32(ro_at));
    put32(&mut file, 0x2C, to32(ro.len()));
    put32(&mut file, 0x30, to32(data_at));
    put32(&mut file, 0x34, to32(data.len()));
    put32(&mut file, 0x38, spec.bss);
    file[0x40..0x60].copy_from_slice(&spec.build_id);
    put32(&mut file, 0x70, to32(ro_at));
    put32(&mut file, 0x74, to32(strtab.len()));
    put32(&mut file, 0x78, to32(ro_at + symtab_at));
    put32(&mut file, 0x7C, to32(symtab.len()));
    if spec.icon.is_some() || spec.nacp.is_some() || spec.romfs.is_some() {
        let mut a = vec![0u8; ASSET_HEADER_SIZE as usize];
        a[..4].copy_from_slice(b"ASET");
        for (slot, item) in [&spec.icon, &spec.nacp, &spec.romfs]
            .into_iter()
            .enumerate()
        {
            let Some(bytes) = item else { continue };
            align(&mut a, 0x10);
            let at = a.len();
            a.extend_from_slice(bytes);
            let o = 8 + slot * 0x10;
            a[o..o + 8].copy_from_slice(&(at as u64).to_le_bytes());
            a[o + 8..o + 16].copy_from_slice(&(bytes.len() as u64).to_le_bytes());
        }
        file.extend_from_slice(&a);
    }
    file
}

/// Open an NRO held in memory (a convenience for tests and tools).
///
/// # Errors
///
/// As [`Nro::open`].
pub fn parse(bytes: Vec<u8>) -> Result<Nro> {
    Nro::open(Arc::new(crate::storage::MemStorage(bytes)))
}

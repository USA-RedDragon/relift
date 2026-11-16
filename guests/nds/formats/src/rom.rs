//! A whole image: header, tables and units.

use crate::blz;
use crate::bytes::slice;
use crate::error::{Error, Result};
use crate::fs::{self, FatEntry, File};
use crate::header::Header;
use crate::module::{Cpu, StaticUnit};
use crate::overlay::{self, Overlay};

/// A parsed cartridge image. Parsing reads the header, the allocation
/// table and both overlay tables; decompression happens on request.
#[derive(Clone, Debug)]
pub struct Rom<'a> {
    bytes: &'a [u8],
    /// The header.
    pub header: Header,
    /// The file allocation table.
    pub fat: Vec<FatEntry>,
    /// The ARM9 overlay table.
    pub overlays9: Vec<Overlay>,
    /// The ARM7 overlay table.
    pub overlays7: Vec<Overlay>,
}

impl<'a> Rom<'a> {
    /// Parse `bytes`.
    ///
    /// # Errors
    ///
    /// The image is shorter than the header or a table lies outside it.
    pub fn parse(bytes: &'a [u8]) -> Result<Rom<'a>> {
        let header = Header::parse(bytes)?;
        let fat = fs::parse_fat(bytes, header.fat.offset, header.fat.size)?;
        let overlays9 = overlay::parse_table(bytes, header.ovt9.offset, header.ovt9.size)?;
        let overlays7 = overlay::parse_table(bytes, header.ovt7.offset, header.ovt7.size)?;
        Ok(Rom {
            bytes,
            header,
            fat,
            overlays9,
            overlays7,
        })
    }

    /// The image bytes.
    #[must_use]
    pub fn bytes(&self) -> &'a [u8] {
        self.bytes
    }

    /// The ARM9 static binary.
    ///
    /// # Errors
    ///
    /// The binary lies outside the image or does not decode.
    pub fn arm9(&self) -> Result<StaticUnit<'a>> {
        StaticUnit::load(self.bytes, Cpu::Arm9, &self.header.arm9)
    }

    /// The ARM7 static binary.
    ///
    /// # Errors
    ///
    /// The binary lies outside the image or does not decode.
    pub fn arm7(&self) -> Result<StaticUnit<'a>> {
        StaticUnit::load(self.bytes, Cpu::Arm7, &self.header.arm7)
    }

    /// The overlays of `cpu`.
    #[must_use]
    pub fn overlays(&self, cpu: Cpu) -> &[Overlay] {
        match cpu {
            Cpu::Arm9 => &self.overlays9,
            Cpu::Arm7 => &self.overlays7,
        }
    }

    /// The bytes of file `id` as stored.
    ///
    /// # Errors
    ///
    /// The id is not in the allocation table or the file lies outside the
    /// image.
    pub fn file_bytes(&self, id: u32) -> Result<&'a [u8]> {
        let e = self
            .fat
            .get(id as usize)
            .ok_or(Error::Malformed("file id outside the allocation table"))?;
        if e.end < e.start {
            return Err(Error::Malformed("file ends before it starts"));
        }
        slice(self.bytes, "file", e.start.into(), e.size().into())
    }

    /// The loaded data of `overlay`: its file, decompressed when flagged,
    /// cut to the overlay's size.
    ///
    /// # Errors
    ///
    /// The file lies outside the image or does not decode.
    pub fn overlay_data(&self, overlay: &Overlay) -> Result<Vec<u8>> {
        let file = self.file_bytes(overlay.file_id)?;
        let mut data = if overlay.compressed() {
            let stored = file
                .get(..overlay.compressed_size() as usize)
                .ok_or(Error::Malformed("compressed overlay size past its file"))?;
            blz::decompress(stored).map_err(|source| Error::Blz {
                what: "overlay",
                source,
            })?
        } else {
            file.to_vec()
        };
        data.truncate(overlay.size as usize);
        Ok(data)
    }

    /// The named files, in name table order.
    ///
    /// # Errors
    ///
    /// The name table is outside the image or inconsistent.
    pub fn files(&self) -> Result<Vec<File>> {
        let fnt = slice(
            self.bytes,
            "file name table",
            self.header.fnt.offset.into(),
            self.header.fnt.size.into(),
        )?;
        fs::walk(fnt, &self.fat)
    }
}

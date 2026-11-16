//! The file allocation table and the file name table.

use crate::bytes::{slice, u16_at, u32_at};
use crate::error::{Error, Result};

/// One file allocation table entry: where a file lies in the image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FatEntry {
    /// Offset of the first byte.
    pub start: u32,
    /// Offset one past the last byte.
    pub end: u32,
}

impl FatEntry {
    /// Size of the file.
    #[must_use]
    pub fn size(&self) -> u32 {
        self.end.saturating_sub(self.start)
    }
}

/// A named file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct File {
    /// The path from the root, joined by `/`, without a leading slash.
    pub path: String,
    /// The file id: an index into the allocation table.
    pub id: u16,
    /// Where the file lies.
    pub entry: FatEntry,
}

/// Parse the table of `size` bytes at `offset`.
///
/// # Errors
///
/// The table lies outside the image.
pub fn parse_fat(rom: &[u8], offset: u32, size: u32) -> Result<Vec<FatEntry>> {
    let table = slice(rom, "file allocation table", offset.into(), size.into())?;
    Ok(table
        .as_chunks::<8>()
        .0
        .iter()
        .map(|e| FatEntry {
            start: u32::from_le_bytes([e[0], e[1], e[2], e[3]]),
            end: u32::from_le_bytes([e[4], e[5], e[6], e[7]]),
        })
        .collect())
}

const MAX_DIRS: usize = 0x1000;
const MAX_DEPTH: usize = 64;

struct Walk<'a> {
    fnt: &'a [u8],
    fat: &'a [FatEntry],
    dirs: usize,
    seen: Vec<bool>,
    files: Vec<File>,
}

impl Walk<'_> {
    fn dir(&mut self, index: usize, prefix: &str, depth: usize) -> Result<()> {
        if depth > MAX_DEPTH || std::mem::replace(&mut self.seen[index], true) {
            return Err(Error::Malformed("directory tree loops"));
        }
        let bad = Error::Malformed("file name table entry outside the table");
        let main = self.fnt.get(8 * index..8 * index + 8).ok_or(bad)?;
        let mut at = u32_at(main, 0).unwrap_or(0) as usize;
        let mut next_id = u32::from(u16_at(main, 4).unwrap_or(0));
        loop {
            let tag = *self
                .fnt
                .get(at)
                .ok_or(Error::Malformed("file name table ends inside a directory"))?;
            if tag == 0 {
                return Ok(());
            }
            let len = usize::from(tag & 0x7F);
            let name = self
                .fnt
                .get(at + 1..at + 1 + len)
                .ok_or(Error::Malformed("file name outside the file name table"))?;
            let path = format!("{prefix}{}", String::from_utf8_lossy(name));
            at += 1 + len;
            if tag & 0x80 != 0 {
                let id = u16_at(self.fnt, at)
                    .ok_or(Error::Malformed("directory id outside the file name table"))?;
                at += 2;
                let child = usize::from(id & 0x0FFF);
                if id & 0xF000 != 0xF000 || child >= self.dirs {
                    return Err(Error::Malformed("directory id out of range"));
                }
                self.dir(child, &format!("{path}/"), depth + 1)?;
            } else {
                let entry = *self
                    .fat
                    .get(next_id as usize)
                    .ok_or(Error::Malformed("file id outside the allocation table"))?;
                let id =
                    u16::try_from(next_id).map_err(|_| Error::Malformed("file id too large"))?;
                self.files.push(File { path, id, entry });
                next_id += 1;
            }
        }
    }
}

/// Walk the directory tree of the name table `fnt` and list its files in
/// table order. An empty table lists nothing.
///
/// # Errors
///
/// The table is inconsistent: entries outside it, loops, directory or file
/// ids out of range.
pub fn walk(fnt: &[u8], fat: &[FatEntry]) -> Result<Vec<File>> {
    if fnt.is_empty() {
        return Ok(Vec::new());
    }
    let dirs = usize::from(
        u16_at(fnt, 6).ok_or(Error::Malformed("file name table shorter than its root"))?,
    );
    if dirs == 0 || dirs > MAX_DIRS || dirs * 8 > fnt.len() {
        return Err(Error::Malformed("directory count out of range"));
    }
    let mut w = Walk {
        fnt,
        fat,
        dirs,
        seen: vec![false; dirs],
        files: Vec::new(),
    };
    w.dir(0, "", 0)?;
    Ok(w.files)
}

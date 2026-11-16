//! The overlay tables: 32-byte entries for the overlays of one processor.

use crate::bytes::{slice, u32_at};
use crate::error::Result;

/// Size of one overlay table entry.
pub const ENTRY_LEN: usize = 32;
/// Flag bit: the overlay file is BLZ-compressed.
pub const COMPRESSED: u32 = 1 << 24;
/// Flag bit: the overlay is authenticated.
pub const AUTHENTICATED: u32 = 1 << 25;

/// One overlay table entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Overlay {
    /// The overlay id.
    pub id: u32,
    /// The address the overlay loads to.
    pub ram: u32,
    /// Size of the loaded overlay.
    pub size: u32,
    /// Size of the bss after it.
    pub bss: u32,
    /// Start of the static initializer pointer table.
    pub static_init_start: u32,
    /// End of the static initializer pointer table.
    pub static_init_end: u32,
    /// The file id in the allocation table.
    pub file_id: u32,
    /// The flags word.
    pub flags: u32,
}

impl Overlay {
    /// Size of the compressed data, from the flags word.
    #[must_use]
    pub fn compressed_size(&self) -> u32 {
        self.flags & 0x00FF_FFFF
    }

    /// Whether the file is compressed.
    #[must_use]
    pub fn compressed(&self) -> bool {
        self.flags & COMPRESSED != 0
    }

    /// Whether the overlay is authenticated.
    #[must_use]
    pub fn authenticated(&self) -> bool {
        self.flags & AUTHENTICATED != 0
    }

    /// The pointers of the static initializer table that lie inside
    /// `data`, the loaded overlay.
    #[must_use]
    pub fn static_init_pointers(&self, data: &[u8]) -> Vec<u32> {
        (self.static_init_start..self.static_init_end)
            .step_by(4)
            .filter(|a| *a >= self.ram && ((*a - self.ram) as usize) < data.len())
            .filter_map(|a| u32_at(data, (a - self.ram) as usize))
            .collect()
    }
}

/// Parse the table of `size` bytes at `offset`; a trailing partial entry
/// is ignored.
///
/// # Errors
///
/// The table lies outside the image.
pub fn parse_table(rom: &[u8], offset: u32, size: u32) -> Result<Vec<Overlay>> {
    let table = slice(rom, "overlay table", offset.into(), size.into())?;
    Ok(table
        .as_chunks::<ENTRY_LEN>()
        .0
        .iter()
        .map(|e| {
            let w = |i: usize| u32_at(e, 4 * i).unwrap_or(0);
            Overlay {
                id: w(0),
                ram: w(1),
                size: w(2),
                bss: w(3),
                static_init_start: w(4),
                static_init_end: w(5),
                file_id: w(6),
                flags: w(7),
            }
        })
        .collect())
}

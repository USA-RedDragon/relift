//! Bounds-checked reads.

use crate::error::{Error, Result};

/// `len` bytes of `bytes` at `offset`.
pub(crate) fn slice<'a>(
    bytes: &'a [u8],
    what: &'static str,
    offset: u64,
    len: u64,
) -> Result<&'a [u8]> {
    let range = offset
        .checked_add(len)
        .filter(|end| *end <= bytes.len() as u64)
        .map(|end| offset as usize..end as usize);
    match range {
        Some(r) => Ok(&bytes[r]),
        None => Err(Error::Range {
            what,
            offset,
            len,
            file_len: bytes.len(),
        }),
    }
}

/// The little-endian word at `at`.
pub(crate) fn u32_at(bytes: &[u8], at: usize) -> Option<u32> {
    let b = bytes.get(at..at.checked_add(4)?)?;
    Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// The little-endian halfword at `at`.
pub(crate) fn u16_at(bytes: &[u8], at: usize) -> Option<u16> {
    let b = bytes.get(at..at.checked_add(2)?)?;
    Some(u16::from_le_bytes([b[0], b[1]]))
}

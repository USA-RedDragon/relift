//! Yaz0, the Nintendo LZ77 wrapper of many Switch 1 archives: the magic
//! `Yaz0`, the decompressed size (u32 big endian) at 4, then from 0x10
//! groups of a flag byte (most significant bit first: 1 a literal byte, 0
//! a back reference) and eight items. A back reference is two bytes, the
//! distance minus one in the low 12 bits and the length minus two in the
//! top four; a top nibble of 0 takes a third byte, the length minus 0x12.

use crate::error::{Error, Result};

/// The magic a Yaz0 stream starts with.
pub const MAGIC: [u8; 4] = *b"Yaz0";
/// The largest decompressed size accepted.
const MAX_SIZE: usize = 1 << 30;

/// Whether `b` starts with a Yaz0 header.
pub fn is_yaz0(b: &[u8]) -> bool {
    b.len() >= 16 && b[..4] == MAGIC
}

/// The decompressed bytes of the Yaz0 stream `b`.
///
/// # Errors
///
/// A truncated stream, a reference before the start, or a size past
/// 1 GiB.
pub fn decode(b: &[u8]) -> Result<Vec<u8>> {
    let bad = |m: &str| Error::Format(format!("yaz0: {m}"));
    if !is_yaz0(b) {
        return Err(bad("no header"));
    }
    let size = u32::from_be_bytes([b[4], b[5], b[6], b[7]]) as usize;
    if size > MAX_SIZE {
        return Err(bad("decompressed size past 1 GiB"));
    }
    let mut out = Vec::with_capacity(size);
    let mut i = 16;
    let byte = |i: usize| b.get(i).copied().ok_or_else(|| bad("truncated"));
    while out.len() < size {
        let flags = byte(i)?;
        i += 1;
        for k in 0..8 {
            if out.len() >= size {
                break;
            }
            if flags & (0x80 >> k) != 0 {
                out.push(byte(i)?);
                i += 1;
                continue;
            }
            let (b0, b1) = (byte(i)?, byte(i + 1)?);
            i += 2;
            let dist = ((usize::from(b0 & 0xf) << 8) | usize::from(b1)) + 1;
            let len = match b0 >> 4 {
                0 => {
                    let n = usize::from(byte(i)?) + 0x12;
                    i += 1;
                    n
                }
                n => usize::from(n) + 2,
            };
            let from = out
                .len()
                .checked_sub(dist)
                .ok_or_else(|| bad("reference before the start"))?;
            for j in 0..len.min(size - out.len()) {
                out.push(out[from + j]);
            }
        }
    }
    Ok(out)
}

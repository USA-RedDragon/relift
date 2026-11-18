//! UnityFS asset bundles: the data of every block, joined. The header
//! (signature `UnityFS`, format version, two version strings, the total
//! size, the compressed and decompressed sizes of the block table and the
//! flags) is followed by the block table, compressed as the flags' low six
//! bits say, at the end of the file (flag 0x80) or after the header
//! (padded to 16 from format 7, and again with flag 0x200). The table
//! holds a 16-byte hash, the block count and per block its decompressed
//! and compressed sizes (u32 big endian) and flags; blocks follow each
//! other from the data start.
//!
//! Blocks are stored (0) or LZ4 (2, 3); a bundle with an LZMA (1) block or
//! table is reported and not read.

use crate::error::{Error, Result};

/// The signature a bundle starts with.
pub const SIGNATURE: &[u8] = b"UnityFS\0";
/// The largest bundle data accepted.
const MAX_DATA: u64 = 1 << 32;

/// Whether `b` starts with a UnityFS signature.
pub fn is_bundle(b: &[u8]) -> bool {
    b.starts_with(SIGNATURE)
}

fn bad<T>(m: &str) -> Result<T> {
    Err(Error::Format(format!("unityfs: {m}")))
}

fn be32(b: &[u8], at: usize) -> Result<u32> {
    match b.get(at..at + 4) {
        Some(s) => Ok(u32::from_be_bytes([s[0], s[1], s[2], s[3]])),
        None => bad("truncated"),
    }
}

/// A block of `size` decompressed bytes stored with compression `kind`.
fn block(src: &[u8], kind: u32, size: usize) -> Result<Vec<u8>> {
    match kind {
        0 => Ok(src.to_vec()),
        2 | 3 => match lz4_flex::block::decompress(src, size) {
            Ok(d) if d.len() == size => Ok(d),
            Ok(_) => bad("an LZ4 block decompresses to another size"),
            Err(e) => Err(Error::Format(format!("unityfs: LZ4: {e}"))),
        },
        1 => bad("LZMA compression is not read"),
        k => Err(Error::Format(format!("unityfs: compression {k}"))),
    }
}

/// The joined data of the bundle `b`.
///
/// # Errors
///
/// A truncated or malformed bundle, LZMA compression, or data past 4 GiB.
pub fn data(b: &[u8]) -> Result<Vec<u8>> {
    if !is_bundle(b) {
        return bad("no signature");
    }
    let version = be32(b, 8)?;
    let mut i = 12;
    for _ in 0..2 {
        let end = b[i..].iter().position(|&c| c == 0);
        match end {
            Some(e) => i += e + 1,
            None => return bad("truncated"),
        }
    }
    let csize = be32(b, i + 8)? as usize;
    let usize_ = be32(b, i + 12)? as usize;
    let flags = be32(b, i + 16)?;
    i += 20;
    if version >= 7 {
        i = i.next_multiple_of(16);
    }
    let (table, mut at) = if flags & 0x80 != 0 {
        let from = b
            .len()
            .checked_sub(csize)
            .map_or_else(|| bad("truncated"), Ok)?;
        (&b[from..], i)
    } else {
        let Some(t) = b.get(i..i + csize) else {
            return bad("truncated");
        };
        let mut at = i + csize;
        if flags & 0x200 != 0 {
            at = at.next_multiple_of(16);
        }
        (t, at)
    };
    let table = block(table, flags & 0x3f, usize_)?;
    let n = be32(&table, 16)? as usize;
    let mut out = Vec::new();
    let mut total = 0u64;
    for k in 0..n {
        let e = 20 + 10 * k;
        let (u, c) = (be32(&table, e)? as usize, be32(&table, e + 4)? as usize);
        let Some(f) = table.get(e + 8..e + 10) else {
            return bad("truncated");
        };
        let kind = u32::from(u16::from_be_bytes([f[0], f[1]]) & 0x3f);
        total += u as u64;
        if total > MAX_DATA {
            return bad("data past 4 GiB");
        }
        let Some(src) = b.get(at..at + c) else {
            return bad("truncated");
        };
        out.extend(block(src, kind, u)?);
        at += c;
    }
    Ok(out)
}

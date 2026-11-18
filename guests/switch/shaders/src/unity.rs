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
//! Blocks are stored (0), LZMA (1: the five property bytes, then the
//! stream, with no size field; decoded with `lzma-rs`) or LZ4 (2, 3).

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
        1 => {
            let opts = lzma_rs::decompress::Options {
                unpacked_size: lzma_rs::decompress::UnpackedSize::UseProvided(Some(size as u64)),
                ..Default::default()
            };
            let mut out = Vec::with_capacity(size);
            match lzma_rs::lzma_decompress_with_options(&mut &src[..], &mut out, &opts) {
                Ok(()) if out.len() == size => Ok(out),
                Ok(()) => bad("an LZMA block decompresses to another size"),
                Err(e) => Err(Error::Format(format!("unityfs: LZMA: {e}"))),
            }
        }
        k => Err(Error::Format(format!("unityfs: compression {k}"))),
    }
}

/// The joined data of the bundle `b`.
///
/// # Errors
///
/// A truncated or malformed bundle, or data past 4 GiB.
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

/// The most platforms a Shader object lists.
const MAX_PLATFORMS: u32 = 32;
/// The most segments one platform's blob is split into.
const MAX_SEGMENTS: u32 = 64;
/// The largest decompressed segment accepted.
const MAX_SEGMENT: u32 = 1 << 28;

fn le32(b: &[u8], at: usize) -> Option<u32> {
    let s = b.get(at..at.checked_add(4)?)?;
    Some(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

/// A list of `n` u32 at `at`; the position after it.
fn list(b: &[u8], at: usize, n: u32) -> Option<(Vec<u32>, usize)> {
    let v = (0..n as usize)
        .map(|k| le32(b, at + 4 * k))
        .collect::<Option<Vec<_>>>()?;
    Some((v, at + 4 * n as usize))
}

/// One length array of a Shader object: a count of `n` and per platform
/// either one value (`nested` false) or a counted list of values.
fn lengths(b: &[u8], at: usize, n: u32, nested: bool) -> Option<(Vec<Vec<u32>>, usize)> {
    if le32(b, at)? != n {
        return None;
    }
    let mut at = at + 4;
    let mut out = Vec::new();
    for _ in 0..n {
        if nested {
            let k = le32(b, at)?;
            if k == 0 || k > MAX_SEGMENTS {
                return None;
            }
            let (v, next) = list(b, at + 4, k)?;
            out.push(v);
            at = next;
        } else {
            out.push(vec![le32(b, at)?]);
            at += 4;
        }
    }
    Some((out, at))
}

/// The decompressed segments of the Shader object fields that start at
/// `at`, when they are some: the platform list, then the offsets,
/// compressed lengths and decompressed lengths (one value per platform, or
/// from Unity 2021 a list per platform), then the compressed blob (a
/// counted byte array). Every segment must lie inside the blob and
/// LZ4-decompress to exactly its decompressed length, so other data is not
/// taken for one.
fn blob_at(b: &[u8], at: usize) -> Option<Vec<Vec<u8>>> {
    let n = le32(b, at)?;
    if n == 0 || n > MAX_PLATFORMS {
        return None;
    }
    let (_, at) = list(b, at + 4, n)?;
    for nested in [false, true] {
        let Some((offs, a)) = lengths(b, at, n, nested) else {
            continue;
        };
        let Some((clens, a)) = lengths(b, a, n, nested) else {
            continue;
        };
        let Some((dlens, a)) = lengths(b, a, n, nested) else {
            continue;
        };
        let size = le32(b, a)? as usize;
        let blob = b.get(a + 4..(a + 4).checked_add(size)?)?;
        let mut out = Vec::new();
        for ((o, c), d) in offs.iter().zip(&clens).zip(&dlens) {
            if o.len() != c.len() || o.len() != d.len() {
                return None;
            }
            for ((&o, &c), &d) in o.iter().zip(c).zip(d) {
                if c == 0 || d == 0 || d > MAX_SEGMENT {
                    return None;
                }
                let src = blob.get(o as usize..(o as usize).checked_add(c as usize)?)?;
                let seg = lz4_flex::block::decompress(src, d as usize).ok()?;
                if seg.len() != d as usize {
                    return None;
                }
                out.push(seg);
            }
        }
        return Some(out);
    }
    None
}

/// Every Unity Shader object's compressed blob in `b` (a serialized file,
/// or a bundle's data), as its decompressed segments with the offset of
/// the fields they were found at. The fields are found by their shape (see
/// [`blob_at`]), not by the file's type tree, which Switch builds strip.
pub fn shader_blobs(b: &[u8]) -> Vec<(usize, Vec<u8>)> {
    let mut out = Vec::new();
    let mut at = 0;
    while at + 8 <= b.len() {
        let n = le32(b, at).unwrap_or(0);
        if (1..=MAX_PLATFORMS).contains(&n)
            && let Some(segs) = blob_at(b, at)
        {
            out.extend(segs.into_iter().map(|s| (at, s)));
            at += 4;
            continue;
        }
        at += 1;
    }
    out
}

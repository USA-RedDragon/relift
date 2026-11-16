//! KTGL archives: `.rdb` indexes of `IDRK` entries, the `.rdx`
//! that maps an entry's file index to a `.fdata` file, and the entries
//! themselves, whole or in zlib chunks.

use crate::error::{Result, malformed};
use crate::inflate;

/// Entry type of shader containers built by the G2S converter.
pub const TYPE_S2G: u32 = 0x7BCD_279F;
/// Entry type of the second shader container type.
pub const TYPE_DOK: u32 = 0x20A6_A0BB;
const MAGIC: &[u8; 8] = b"IDRK0000";
const FLAG_ZLIB_U32: u32 = 0x10_0000;
const FLAG_ZLIB_U16: u32 = 0x40_0000;
const FLAG_LZ4: u32 = 0x50_0000;
const MAX_INFLATED: usize = 1 << 30;

const HEADER: usize = 0x30;
const LOC_LEN: usize = 12;

fn u64_at(b: &[u8], at: usize) -> Option<u64> {
    let s = b.get(at..at.checked_add(8)?)?;
    Some(u64::from_le_bytes(s.try_into().ok()?))
}

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    let s = b.get(at..at.checked_add(4)?)?;
    Some(u32::from_le_bytes(s.try_into().ok()?))
}

/// The `.rdx` table: file index to (mount point, file id).
pub fn rdx(b: &[u8]) -> Vec<(u16, u8, u32)> {
    b.as_chunks::<8>()
        .0
        .iter()
        .map(|c| {
            (
                u16::from_le_bytes([c[0], c[1]]),
                c[2],
                u32::from_le_bytes([c[4], c[5], c[6], c[7]]),
            )
        })
        .collect()
}

/// Where a shader entry lives: in an `.fdata` file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Location {
    /// The index into the `.rdx`.
    pub index: u16,
    /// The offset of the entry in the file.
    pub offset: u32,
    /// The size of the entry.
    pub size: u32,
}

/// The shader entries of an `.rdb`, and where the walk stopped early.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Locations {
    /// The shader entries found.
    pub found: Vec<Location>,
    /// Why the walk stopped before the end of the `.rdb`: an entry whose
    /// length is shorter than its header or runs past the file.
    pub bad: Option<&'static str>,
}

/// The locations of the shader entries of an `.rdb`. Every entry must
/// hold its header and end inside the file; the walk stops at the first
/// one that does not.
pub fn shader_locations(rdb: &[u8]) -> Locations {
    let mut out = Locations::default();
    let mut from = 0;
    while let Some(at) = find(rdb, MAGIC, from) {
        let (Some(elen), Some(stored)) = (u64_at(rdb, at + 8), u64_at(rdb, at + 16)) else {
            out.bad = Some("KTGL: an .rdb entry header is cut off");
            break;
        };
        let end = usize::try_from(elen).ok().and_then(|e| at.checked_add(e));
        let Some(end) = end.filter(|&e| e >= at + HEADER && e <= rdb.len()) else {
            out.bad =
                Some("KTGL: an .rdb entry length is shorter than its header or runs past the file");
            break;
        };
        let loc_at = usize::try_from(stored)
            .ok()
            .and_then(|s| end.checked_sub(s))
            .filter(|&l| l >= at + HEADER && l + LOC_LEN <= end);
        if let (Some(ty), Some(loc_at)) = (u32_at(rdb, at + 0x28), loc_at)
            && matches!(ty, TYPE_S2G | TYPE_DOK)
            && let (Some(offset), Some(size), Some(idx)) = (
                u32_at(rdb, loc_at + 2),
                u32_at(rdb, loc_at + 6),
                rdb.get(loc_at + 10..loc_at + 12),
            )
        {
            out.found.push(Location {
                index: u16::from_le_bytes([idx[0], idx[1]]),
                offset,
                size,
            });
        }
        from = end;
    }
    out
}

fn find(b: &[u8], pat: &[u8], from: usize) -> Option<usize> {
    crate::carve::find_all(b.get(from..)?, pat)
        .next()
        .map(|i| i + from)
}

/// The inflated payload of one `IDRK` entry (the bytes an `.fdata` holds
/// at a [`Location`]).
///
/// # Errors
///
/// The entry is not an `IDRK`, is cut off, is stored in LZ4 chunks, or a
/// zlib chunk is bad.
pub fn inflate_entry(e: &[u8]) -> Result<Vec<u8>> {
    if !e.starts_with(MAGIC) {
        return malformed("KTGL: no IDRK at the entry offset");
    }
    let (Some(elen), Some(stored), Some(flags)) = (u64_at(e, 8), u64_at(e, 16), u32_at(e, 0x2C))
    else {
        return malformed("KTGL: the entry header is cut off");
    };
    let data = usize::try_from(elen)
        .ok()
        .and_then(|elen| {
            elen.checked_sub(usize::try_from(stored).ok()?)
                .map(|s| (s, elen))
        })
        .and_then(|(s, elen)| e.get(s..elen));
    let Some(data) = data else {
        return malformed("KTGL: the entry data runs past the entry");
    };
    if flags & FLAG_LZ4 == FLAG_LZ4 {
        return malformed("KTGL: LZ4 chunks");
    }
    let mut out = Vec::new();
    if flags & FLAG_ZLIB_U16 != 0 {
        let mut p = 0;
        while p + 2 <= data.len() {
            if u16::from_le_bytes([data[p], data[p + 1]]) == 0 {
                break;
            }
            let Some(rest) = data.get(p + 10..) else {
                return malformed("KTGL: a chunk header is cut off");
            };
            let (chunk, used) = inflate::zlib(rest, MAX_INFLATED - out.len())?;
            out.extend_from_slice(&chunk);
            p += 10 + used;
        }
        return Ok(out);
    }
    if flags & FLAG_ZLIB_U32 != 0 {
        let mut p = 0;
        while p + 4 <= data.len() {
            let n = u32_at(data, p).unwrap_or(0) as usize;
            if n == 0 {
                break;
            }
            let Some(chunk) = (p + 4).checked_add(n).and_then(|end| data.get(p + 4..end)) else {
                return malformed("KTGL: a chunk runs past the entry");
            };
            out.extend_from_slice(&inflate::zlib(chunk, MAX_INFLATED - out.len())?.0);
            p += 4 + n;
        }
        return Ok(out);
    }
    Ok(data.to_vec())
}

/// Build an `IDRK` entry holding `payload`, for tests: `chunks` of zlib
/// streams (already compressed) under flag `flags`, or the bare payload.
pub fn build_entry(ty: u32, flags: u32, data: &[u8]) -> Vec<u8> {
    let elen = 0x40 + data.len();
    let mut e = vec![0u8; 0x40];
    e[..8].copy_from_slice(MAGIC);
    e[8..16].copy_from_slice(&(elen as u64).to_le_bytes());
    e[16..24].copy_from_slice(&(data.len() as u64).to_le_bytes());
    e[24..32].copy_from_slice(&(data.len() as u64).to_le_bytes());
    e[0x28..0x2C].copy_from_slice(&ty.to_le_bytes());
    e[0x2C..0x30].copy_from_slice(&flags.to_le_bytes());
    e.extend_from_slice(data);
    e
}

/// Build an `.rdb` of entries pointing at `(index, offset, size)` of
/// `.fdata` files, for tests.
pub fn build_rdb(ty: u32, locs: &[Location]) -> Vec<u8> {
    let mut out = Vec::new();
    for l in locs {
        let mut loc = vec![0u8; 2];
        loc.extend_from_slice(&l.offset.to_le_bytes());
        loc.extend_from_slice(&l.size.to_le_bytes());
        loc.extend_from_slice(&l.index.to_le_bytes());
        out.extend_from_slice(&build_entry(ty, 0, &loc));
    }
    out
}

/// Build an `.rdx` of `(mount point, file id)` per index, for tests.
pub fn build_rdx(rows: &[(u8, u32)]) -> Vec<u8> {
    let mut out = Vec::new();
    for (i, (mnt, id)) in rows.iter().enumerate() {
        out.extend_from_slice(&(i as u16).to_le_bytes());
        out.push(*mnt);
        out.push(0);
        out.extend_from_slice(&id.to_le_bytes());
    }
    out
}

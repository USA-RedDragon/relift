//! Binary patch formats: IPS, IPS32, UPS and BPS, applied to a byte image.
//!
//! [`apply`] reads the format from the patch's magic and returns the
//! patched bytes with the ranges the patch wrote ([`Patched::written`]),
//! which callers use to find the functions a patch touches and to detect
//! two patches writing the same bytes. IPS and IPS32 report their records;
//! UPS and BPS rewrite the whole file, so they report the bytes that differ
//! from the input. UPS and BPS carry CRC-32s of their input, output and
//! patch: a damaged patch or a wrong output is an error, while an input
//! whose CRC differs from the declared one is applied anyway and reported
//! in [`Patched::base_mismatch`].

use std::fmt;
use std::ops::Range;

/// A patch format.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Kind {
    /// IPS: 24-bit offsets, "PATCH" ... "EOF".
    Ips,
    /// IPS32: 32-bit offsets, "IPS32" ... "EEOF".
    Ips32,
    /// UPS: XOR hunks with CRC-32 checks, "UPS1".
    Ups,
    /// BPS: copy and read actions with CRC-32 checks, "BPS1".
    Bps,
}

impl Kind {
    /// The format's lowercase name, as the self-description writes it.
    pub fn name(self) -> &'static str {
        match self {
            Kind::Ips => "ips",
            Kind::Ips32 => "ips32",
            Kind::Ups => "ups",
            Kind::Bps => "bps",
        }
    }

    /// The format a patch file holds, from its magic.
    pub fn detect(patch: &[u8]) -> Option<Kind> {
        if patch.starts_with(b"IPS32") {
            Some(Kind::Ips32)
        } else if patch.starts_with(b"PATCH") {
            Some(Kind::Ips)
        } else if patch.starts_with(b"UPS1") {
            Some(Kind::Ups)
        } else if patch.starts_with(b"BPS1") {
            Some(Kind::Bps)
        } else {
            None
        }
    }
}

/// Why a patch could not be applied.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// The magic names no known format.
    UnknownFormat,
    /// The patch ends inside a record.
    Truncated,
    /// The patch's own CRC-32 does not match its bytes.
    PatchChecksum,
    /// The output's CRC-32 does not match the one the patch declares.
    OutputChecksum,
    /// A copy reads outside its source.
    OutOfRange,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Error::UnknownFormat => "not an IPS, IPS32, UPS or BPS patch",
            Error::Truncated => "the patch ends inside a record",
            Error::PatchChecksum => "the patch's checksum does not match: the file is damaged",
            Error::OutputChecksum => "the patched output's checksum does not match the patch",
            Error::OutOfRange => "the patch copies from outside its source",
        })
    }
}

impl std::error::Error for Error {}

/// The result of [`apply`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Patched {
    /// The format.
    pub kind: Kind,
    /// The patched bytes.
    pub data: Vec<u8>,
    /// The ranges the patch wrote, sorted and merged.
    pub written: Vec<Range<u64>>,
    /// The CRC-32 of the input the patch declares (UPS and BPS).
    pub declared_base: Option<u32>,
    /// Whether the input's CRC-32 differs from the declared one.
    pub base_mismatch: bool,
}

/// The CRC-32 (IEEE, reflected) of `data`.
pub fn crc32(data: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in data {
        c ^= u32::from(b);
        for _ in 0..8 {
            c = if c & 1 != 0 {
                (c >> 1) ^ 0xedb8_8320
            } else {
                c >> 1
            };
        }
    }
    !c
}

/// Applies `patch` to `input`, with the format taken from its magic.
///
/// # Errors
///
/// The format is unknown, the patch is truncated or damaged, or it reads
/// outside its source.
pub fn apply(input: &[u8], patch: &[u8]) -> Result<Patched, Error> {
    match Kind::detect(patch).ok_or(Error::UnknownFormat)? {
        Kind::Ips => ips(input, patch, false),
        Kind::Ips32 => ips(input, patch, true),
        Kind::Ups => ups(input, patch),
        Kind::Bps => bps(input, patch),
    }
}

/// Sorts and merges ranges.
pub fn merge(mut r: Vec<Range<u64>>) -> Vec<Range<u64>> {
    r.retain(|r| r.start < r.end);
    r.sort_by_key(|r| r.start);
    let mut out: Vec<Range<u64>> = Vec::new();
    for x in r {
        match out.last_mut() {
            Some(l) if x.start <= l.end => l.end = l.end.max(x.end),
            _ => out.push(x),
        }
    }
    out
}

/// The ranges where `a` and `b` differ; bytes past the end of the shorter
/// one differ.
pub fn diff(a: &[u8], b: &[u8]) -> Vec<Range<u64>> {
    let mut out = Vec::new();
    let mut start: Option<usize> = None;
    let n = a.len().max(b.len());
    for i in 0..n {
        let same = a.get(i).is_some() && a.get(i) == b.get(i);
        match (same, start) {
            (false, None) => start = Some(i),
            (true, Some(s)) => {
                out.push(s as u64..i as u64);
                start = None;
            }
            _ => {}
        }
    }
    if let Some(s) = start {
        out.push(s as u64..n as u64);
    }
    out
}

struct Reader<'a> {
    b: &'a [u8],
    at: usize,
}

impl Reader<'_> {
    fn take(&mut self, n: usize) -> Result<&[u8], Error> {
        let end = self.at.checked_add(n).ok_or(Error::Truncated)?;
        let s = self.b.get(self.at..end).ok_or(Error::Truncated)?;
        self.at = end;
        Ok(s)
    }

    fn be(&mut self, n: usize) -> Result<u64, Error> {
        Ok(self
            .take(n)?
            .iter()
            .fold(0u64, |v, &b| (v << 8) | u64::from(b)))
    }

    fn varint(&mut self) -> Result<u64, Error> {
        let mut v: u64 = 0;
        let mut shift: u64 = 1;
        loop {
            let x = u64::from(self.take(1)?[0]);
            v = v
                .checked_add((x & 0x7f).checked_mul(shift).ok_or(Error::Truncated)?)
                .ok_or(Error::Truncated)?;
            if x & 0x80 != 0 {
                return Ok(v);
            }
            shift = shift.checked_shl(7).ok_or(Error::Truncated)?;
            v = v.checked_add(shift).ok_or(Error::Truncated)?;
        }
    }
}

fn usize_of(v: u64) -> Result<usize, Error> {
    usize::try_from(v).map_err(|_| Error::OutOfRange)
}

fn put(out: &mut Vec<u8>, at: usize, bytes: &[u8]) {
    if out.len() < at + bytes.len() {
        out.resize(at + bytes.len(), 0);
    }
    out[at..at + bytes.len()].copy_from_slice(bytes);
}

fn ips(input: &[u8], patch: &[u8], wide: bool) -> Result<Patched, Error> {
    let (magic, off_bytes, eof): (usize, usize, &[u8]) = if wide {
        (5, 4, b"EEOF")
    } else {
        (5, 3, b"EOF")
    };
    let mut r = Reader {
        b: patch,
        at: magic,
    };
    let mut out = input.to_vec();
    let mut written = Vec::new();
    loop {
        if patch.get(r.at..r.at + eof.len()) == Some(eof) {
            r.at += eof.len();
            break;
        }
        let off = usize_of(r.be(off_bytes)?)?;
        let size = usize_of(r.be(2)?)?;
        if size == 0 {
            let n = usize_of(r.be(2)?)?;
            let v = r.take(1)?[0];
            put(&mut out, off, &vec![v; n]);
            written.push(off as u64..(off + n) as u64);
        } else {
            let data = r.take(size)?.to_vec();
            put(&mut out, off, &data);
            written.push(off as u64..(off + size) as u64);
        }
    }
    if !wide && patch.len() == r.at + 3 {
        let n = usize_of(r.be(3)?)?;
        out.truncate(n);
    }
    Ok(Patched {
        kind: if wide { Kind::Ips32 } else { Kind::Ips },
        data: out,
        written: merge(written),
        declared_base: None,
        base_mismatch: false,
    })
}

/// The three CRC-32s at the end of a UPS or BPS patch, after checking the
/// patch's own.
fn footer(patch: &[u8]) -> Result<(u32, u32), Error> {
    if patch.len() < 16 {
        return Err(Error::Truncated);
    }
    let le =
        |at: usize| u32::from_le_bytes([patch[at], patch[at + 1], patch[at + 2], patch[at + 3]]);
    let n = patch.len();
    if crc32(&patch[..n - 4]) != le(n - 4) {
        return Err(Error::PatchChecksum);
    }
    Ok((le(n - 12), le(n - 8)))
}

fn ups(input: &[u8], patch: &[u8]) -> Result<Patched, Error> {
    let (src_crc, dst_crc) = footer(patch)?;
    let body = &patch[..patch.len() - 12];
    let mut r = Reader { b: body, at: 4 };
    let _in_size = r.varint()?;
    let out_size = usize_of(r.varint()?)?;
    let mut out = input.to_vec();
    out.resize(out_size, 0);
    let mut at = 0usize;
    while r.at < body.len() {
        at = at
            .checked_add(usize_of(r.varint()?)?)
            .ok_or(Error::OutOfRange)?;
        loop {
            let x = r.take(1)?[0];
            if x == 0 {
                at += 1;
                break;
            }
            if at < out.len() {
                out[at] ^= x;
            }
            at += 1;
        }
    }
    if crc32(&out) != dst_crc {
        return Err(Error::OutputChecksum);
    }
    Ok(Patched {
        kind: Kind::Ups,
        written: diff(input, &out),
        data: out,
        declared_base: Some(src_crc),
        base_mismatch: crc32(input) != src_crc,
    })
}

fn bps(input: &[u8], patch: &[u8]) -> Result<Patched, Error> {
    let (src_crc, dst_crc) = footer(patch)?;
    let body = &patch[..patch.len() - 12];
    let mut r = Reader { b: body, at: 4 };
    let _src_size = r.varint()?;
    let dst_size = usize_of(r.varint()?)?;
    let meta = usize_of(r.varint()?)?;
    r.take(meta)?;
    let mut out: Vec<u8> = Vec::with_capacity(dst_size);
    let mut src_rel: i64 = 0;
    let mut dst_rel: i64 = 0;
    let rel = |r: &mut Reader<'_>, base: &mut i64| -> Result<(), Error> {
        let d = r.varint()?;
        let n = i64::try_from(d >> 1).map_err(|_| Error::OutOfRange)?;
        *base += if d & 1 != 0 { -n } else { n };
        Ok(())
    };
    while r.at < body.len() {
        let d = r.varint()?;
        let len = usize_of((d >> 2) + 1)?;
        match d & 3 {
            0 => {
                let at = out.len();
                let s = input.get(at..at + len).ok_or(Error::OutOfRange)?;
                out.extend_from_slice(s);
            }
            1 => {
                let s = r.take(len)?;
                out.extend_from_slice(s);
            }
            2 => {
                rel(&mut r, &mut src_rel)?;
                let at = usize::try_from(src_rel).map_err(|_| Error::OutOfRange)?;
                let s = input.get(at..at + len).ok_or(Error::OutOfRange)?;
                out.extend_from_slice(s);
                src_rel += len as i64;
            }
            _ => {
                rel(&mut r, &mut dst_rel)?;
                for _ in 0..len {
                    let at = usize::try_from(dst_rel).map_err(|_| Error::OutOfRange)?;
                    let b = *out.get(at).ok_or(Error::OutOfRange)?;
                    out.push(b);
                    dst_rel += 1;
                }
            }
        }
    }
    if out.len() != dst_size || crc32(&out) != dst_crc {
        return Err(Error::OutputChecksum);
    }
    Ok(Patched {
        kind: Kind::Bps,
        written: diff(input, &out),
        data: out,
        declared_base: Some(src_crc),
        base_mismatch: crc32(input) != src_crc,
    })
}

#[cfg(test)]
mod tests;

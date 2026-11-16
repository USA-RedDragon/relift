//! Decoders for the Yay0, Yaz0 and MIO0 containers.
//!
//! All three start with a 16-byte header: a four-byte tag and the decoded
//! size as big-endian words. Yaz0 then holds flag bytes interleaved with
//! literals and back-references. Yay0 and MIO0 hold the same three parts
//! apart: a bit mask from offset `0x10`, a table of 16-bit links at the
//! offset in word 2 and the literal bytes at the offset in word 3. A set
//! mask bit takes a literal; a clear one takes a link, whose low 12 bits
//! are the distance minus one and whose high four bits are the length.

use crate::error::{Error, Result};

/// The size of the header of every container.
pub const HEADER: usize = 16;

/// The largest ratio of decoded to encoded bytes any of the formats can
/// reach; sizes above it are rejected before anything is allocated.
const MAX_RATIO: usize = 300;

/// The container formats.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Format {
    /// Yay0.
    Yay0,
    /// Yaz0.
    Yaz0,
    /// MIO0.
    Mio0,
}

impl Format {
    /// The four-byte tag.
    #[must_use]
    pub const fn tag(self) -> &'static [u8; 4] {
        match self {
            Format::Yay0 => b"Yay0",
            Format::Yaz0 => b"Yaz0",
            Format::Mio0 => b"MIO0",
        }
    }

    /// The name used in reports.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Format::Yay0 => "Yay0",
            Format::Yaz0 => "Yaz0",
            Format::Mio0 => "MIO0",
        }
    }

    /// The format whose tag starts `bytes`.
    #[must_use]
    pub fn of(bytes: &[u8]) -> Option<Format> {
        let tag = bytes.first_chunk::<4>()?;
        [Format::Yay0, Format::Yaz0, Format::Mio0]
            .into_iter()
            .find(|f| f.tag() == tag)
    }

    /// Decode a stream of this format.
    ///
    /// # Errors
    ///
    /// The stream is truncated or malformed.
    pub fn decode(self, bytes: &[u8]) -> Result<Vec<u8>> {
        match self {
            Format::Yay0 => split(bytes, Format::Yay0),
            Format::Yaz0 => yaz0(bytes),
            Format::Mio0 => split(bytes, Format::Mio0),
        }
    }
}

/// Decode a Yay0 stream.
///
/// # Errors
///
/// The stream is truncated or malformed.
pub fn decode_yay0(bytes: &[u8]) -> Result<Vec<u8>> {
    Format::Yay0.decode(bytes)
}

/// Decode a Yaz0 stream.
///
/// # Errors
///
/// The stream is truncated or malformed.
pub fn decode_yaz0(bytes: &[u8]) -> Result<Vec<u8>> {
    Format::Yaz0.decode(bytes)
}

/// Decode a MIO0 stream.
///
/// # Errors
///
/// The stream is truncated or malformed.
pub fn decode_mio0(bytes: &[u8]) -> Result<Vec<u8>> {
    Format::Mio0.decode(bytes)
}

fn be32(b: &[u8], at: usize) -> usize {
    u32::from_be_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]]) as usize
}

fn size(bytes: &[u8], f: Format) -> Result<usize> {
    if bytes.len() < HEADER {
        return Err(Error::TooShort(bytes.len()));
    }
    if &bytes[..4] != f.tag() {
        return Err(Error::Invalid("wrong tag"));
    }
    let n = be32(bytes, 4);
    if n > bytes.len().saturating_mul(MAX_RATIO) {
        return Err(Error::Invalid(
            "decoded size beyond what the input can give",
        ));
    }
    Ok(n)
}

fn copy_back(out: &mut Vec<u8>, dist: usize, len: usize, limit: usize) -> Result<()> {
    if dist > out.len() {
        return Err(Error::Invalid("back-reference before the start"));
    }
    if out.len() + len > limit {
        return Err(Error::Invalid("copy past the decoded size"));
    }
    for _ in 0..len {
        out.push(out[out.len() - dist]);
    }
    Ok(())
}

fn yaz0(bytes: &[u8]) -> Result<Vec<u8>> {
    let n = size(bytes, Format::Yaz0)?;
    let mut out = Vec::with_capacity(n);
    let mut at = HEADER;
    let mut take = |what| -> Result<u8> {
        let b = *bytes.get(at).ok_or(Error::Truncated(what))?;
        at += 1;
        Ok(b)
    };
    let (mut flags, mut left) = (0u8, 0);
    while out.len() < n {
        if left == 0 {
            flags = take("flag byte")?;
            left = 8;
        }
        let literal = flags & 0x80 != 0;
        flags <<= 1;
        left -= 1;
        if literal {
            out.push(take("literal")?);
            continue;
        }
        let b1 = usize::from(take("back-reference")?);
        let b2 = usize::from(take("back-reference")?);
        let dist = ((b1 & 0xF) << 8 | b2) + 1;
        let len = if b1 >> 4 == 0 {
            usize::from(take("long length")?) + 0x12
        } else {
            (b1 >> 4) + 2
        };
        copy_back(&mut out, dist, len, n)?;
    }
    Ok(out)
}

fn split(bytes: &[u8], f: Format) -> Result<Vec<u8>> {
    let n = size(bytes, f)?;
    let mut links = be32(bytes, 8);
    let mut chunk = be32(bytes, 12);
    if links < HEADER || chunk < HEADER {
        return Err(Error::Invalid("table offset inside the header"));
    }
    let mut out = Vec::with_capacity(n);
    let mut mask_at = HEADER;
    let mut mask = 0u32;
    let mut left = 0;
    while out.len() < n {
        if left == 0 {
            let w = bytes
                .get(mask_at..mask_at + 4)
                .ok_or(Error::Truncated("bit mask"))?;
            mask = u32::from_be_bytes([w[0], w[1], w[2], w[3]]);
            mask_at += 4;
            left = 32;
        }
        let literal = mask & 0x8000_0000 != 0;
        mask <<= 1;
        left -= 1;
        if literal {
            out.push(*bytes.get(chunk).ok_or(Error::Truncated("literal"))?);
            chunk += 1;
            continue;
        }
        let w = bytes
            .get(links..links + 2)
            .ok_or(Error::Truncated("link table"))?;
        links += 2;
        let link = usize::from(u16::from_be_bytes([w[0], w[1]]));
        let dist = (link & 0xFFF) + 1;
        let len = match (f, link >> 12) {
            (Format::Yay0, 0) => {
                let b = *bytes.get(chunk).ok_or(Error::Truncated("long length"))?;
                chunk += 1;
                usize::from(b) + 0x12
            }
            (Format::Yay0, k) => k + 2,
            (_, k) => k + 3,
        };
        copy_back(&mut out, dist, len, n)?;
    }
    Ok(out)
}

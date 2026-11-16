//! Bottom LZ, the backwards LZ77 the SDK uses for ARM9 binaries and
//! overlays.
//!
//! A compressed blob ends with an 8-byte footer: a word holding the footer
//! length in its top byte and the compressed length (footer included,
//! counted from the end) below, then a signed count of bytes the output
//! grows past the blob. Decoding reads backward from the end: a flag byte
//! (bit 7 first), then per flag a literal byte or two bytes `hi`, `lo`
//! giving length `(v >> 12) + 3` and distance `(v & 0xFFF) + 3`, and
//! writes backward over the end of the output. Bytes below the compressed
//! length are a raw prefix and stay as they are.

use std::collections::HashMap;
use std::fmt;

/// The largest output the decoder allocates.
pub const MAX_OUTPUT: usize = 64 * 1024 * 1024;

/// Why a blob does not decode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    /// Fewer than 8 bytes: no footer.
    Truncated,
    /// The footer's lengths do not fit the blob, or the growth is negative
    /// or past [`MAX_OUTPUT`].
    BadFooter,
    /// The stream reads or writes outside the blob.
    BadStream,
    /// The output overran the unread input, so the stream cannot decode in
    /// place.
    Overrun,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Error::Truncated => "compressed data is shorter than its footer",
            Error::BadFooter => "compressed footer is inconsistent",
            Error::BadStream => "compressed stream is malformed",
            Error::Overrun => "compressed stream overruns in place",
        })
    }
}

impl std::error::Error for Error {}

/// Decode a whole blob: raw prefix, compressed stream and footer.
///
/// # Errors
///
/// The blob is shorter than a footer, the footer is inconsistent or the
/// stream is malformed.
pub fn decompress(blob: &[u8]) -> Result<Vec<u8>, Error> {
    let n = blob.len();
    if n < 8 {
        return Err(Error::Truncated);
    }
    let top = u32::from_le_bytes([blob[n - 8], blob[n - 7], blob[n - 6], blob[n - 5]]);
    let extra = i32::from_le_bytes([blob[n - 4], blob[n - 3], blob[n - 2], blob[n - 1]]);
    let hdr = (top >> 24) as usize;
    let comp = (top & 0x00FF_FFFF) as usize;
    if hdr < 8 || hdr > comp || comp > n || extra < 0 {
        return Err(Error::BadFooter);
    }
    let total = n + extra as usize;
    if total > MAX_OUTPUT {
        return Err(Error::BadFooter);
    }
    let mut out = blob.to_vec();
    out.resize(total, 0);
    let stop = n - comp;
    let mut src = n - hdr;
    let mut dst = total;
    while src > stop {
        src -= 1;
        let mut flags = out[src];
        for _ in 0..8 {
            if src <= stop {
                break;
            }
            if flags & 0x80 != 0 {
                if src < stop + 2 {
                    return Err(Error::BadStream);
                }
                let v = usize::from(out[src - 1]) << 8 | usize::from(out[src - 2]);
                src -= 2;
                let len = (v >> 12) + 3;
                let disp = (v & 0xFFF) + 3;
                for _ in 0..len {
                    dst = dst.checked_sub(1).ok_or(Error::BadStream)?;
                    let from = dst + disp;
                    if from >= total {
                        return Err(Error::BadStream);
                    }
                    out[dst] = out[from];
                }
            } else {
                src -= 1;
                dst = dst.checked_sub(1).ok_or(Error::BadStream)?;
                out[dst] = out[src];
            }
            flags <<= 1;
            if dst < src {
                return Err(Error::Overrun);
            }
        }
        if dst < src {
            return Err(Error::Overrun);
        }
    }
    if dst != stop {
        return Err(Error::BadStream);
    }
    Ok(out)
}

enum Token {
    Literal(u8),
    Copy { len: usize, disp: usize },
}

const MAX_LEN: usize = 18;
const MAX_DISP: usize = 0x1002;
const CANDIDATES: usize = 64;

fn tokens(r: &[u8]) -> Vec<Token> {
    let n = r.len();
    let mut heads: HashMap<[u8; 3], Vec<usize>> = HashMap::new();
    let mut out = Vec::new();
    let mut i = 0;
    while i < n {
        let (mut best_len, mut best_disp) = (0, 0);
        if i + 3 <= n
            && let Some(list) = heads.get(&[r[i], r[i + 1], r[i + 2]])
        {
            for &j in list.iter().rev().take(CANDIDATES) {
                let disp = i - j;
                if disp > MAX_DISP {
                    break;
                }
                if disp < 3 {
                    continue;
                }
                let mut len = 3;
                while len < MAX_LEN && i + len < n && r[j + len] == r[i + len] {
                    len += 1;
                }
                if len > best_len {
                    (best_len, best_disp) = (len, disp);
                    if len == MAX_LEN {
                        break;
                    }
                }
            }
        }
        let step = if best_len >= 3 { best_len } else { 1 };
        for k in i..i + step {
            if k + 3 <= n {
                heads.entry([r[k], r[k + 1], r[k + 2]]).or_default().push(k);
            }
        }
        out.push(if best_len >= 3 {
            Token::Copy {
                len: best_len,
                disp: best_disp,
            }
        } else {
            Token::Literal(r[i])
        });
        i += step;
    }
    out
}

fn pack(data: &[u8], raw: usize) -> Option<Vec<u8>> {
    let reversed: Vec<u8> = data[raw..].iter().rev().copied().collect();
    let mut stream = Vec::new();
    for group in tokens(&reversed).chunks(8) {
        let mut flags = 0u8;
        let mut part = Vec::new();
        for (k, t) in group.iter().enumerate() {
            match t {
                Token::Copy { len, disp } => {
                    flags |= 0x80 >> k;
                    let v = (len - 3) << 12 | (disp - 3);
                    part.push((v >> 8) as u8);
                    part.push(v as u8);
                }
                Token::Literal(b) => part.push(*b),
            }
        }
        stream.push(flags);
        stream.extend(part);
    }
    stream.reverse();
    let pad = (4 - (raw + stream.len()) % 4) % 4;
    let hdr = 8 + pad;
    let comp = stream.len() + hdr;
    let mut blob = data[..raw].to_vec();
    blob.extend(stream);
    blob.extend(std::iter::repeat_n(0xFF, pad));
    let extra = i32::try_from(data.len() as i64 - (blob.len() as i64 + 8)).ok()?;
    if extra < 0 || comp > 0x00FF_FFFF {
        return None;
    }
    blob.extend((((hdr as u32) << 24) | comp as u32).to_le_bytes());
    blob.extend(extra.to_le_bytes());
    Some(blob)
}

/// Encode `data` so that the in-place backward decode never overruns,
/// keeping the first `raw_prefix` bytes uncompressed. The output is valid
/// but not the SDK's: it is a greedy match search. `None` when the data
/// cannot be made smaller.
#[must_use]
pub fn compress(data: &[u8], raw_prefix: usize) -> Option<Vec<u8>> {
    let mut raw = raw_prefix;
    while raw < data.len() {
        if let Some(blob) = pack(data, raw)
            && decompress(&blob).as_deref() == Ok(data)
        {
            return Some(blob);
        }
        raw += 64.max((data.len() - raw) / 32);
    }
    None
}

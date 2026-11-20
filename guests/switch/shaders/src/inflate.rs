//! zlib (RFC 1950) and raw deflate (RFC 1951) decoding, for the zlib chunks
//! of KTGL entries. Output is bounded by a limit the caller gives.

use crate::error::{Result, malformed};

const MAX_BITS: usize = 15;

const LEN_BASE: [u16; 29] = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131,
    163, 195, 227, 258,
];
const LEN_EXTRA: [u8; 29] = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
];
const DIST_BASE: [u16; 30] = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
    2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
];
const DIST_EXTRA: [u8; 30] = [
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13,
    13,
];
const CLEN_ORDER: [usize; 19] = [
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
];

struct Bits<'a> {
    data: &'a [u8],
    pos: usize,
    buf: u64,
    cnt: u32,
}

impl<'a> Bits<'a> {
    fn refill(&mut self) {
        while self.cnt <= 56 {
            let Some(&b) = self.data.get(self.pos) else {
                break;
            };
            self.pos += 1;
            self.buf |= u64::from(b) << self.cnt;
            self.cnt += 8;
        }
    }

    fn need(&mut self, n: u32) -> Result<()> {
        while self.cnt < n {
            let Some(&b) = self.data.get(self.pos) else {
                return malformed("deflate: the stream ends early");
            };
            self.pos += 1;
            self.buf |= u64::from(b) << self.cnt;
            self.cnt += 8;
        }
        Ok(())
    }

    fn bits(&mut self, n: u32) -> Result<u32> {
        if n == 0 {
            return Ok(0);
        }
        self.need(n)?;
        let v = (self.buf & ((1u64 << n) - 1)) as u32;
        self.buf >>= n;
        self.cnt -= n;
        Ok(v)
    }

    fn align(&mut self) {
        let drop = self.cnt % 8;
        self.buf >>= drop;
        self.cnt -= drop;
    }

    fn consumed(&self) -> usize {
        self.pos - (self.cnt / 8) as usize
    }
}

/// How many bits the first-level decoding table of a Huffman code looks up.
const FAST_BITS: u32 = 10;

struct Huffman {
    count: [u16; MAX_BITS + 1],
    symbol: Vec<u16>,
    /// Per `FAST_BITS` bits of input: the symbol shifted left by 4 or'd with
    /// the code length, 0 where the code is longer than `FAST_BITS`.
    fast: Vec<u16>,
}

impl Huffman {
    fn new(lengths: &[u8]) -> Result<Huffman> {
        let mut count = [0u16; MAX_BITS + 1];
        for &l in lengths {
            count[usize::from(l)] += 1;
        }
        let mut left: i32 = 1;
        for &c in &count[1..] {
            left = (left << 1) - i32::from(c);
            if left < 0 {
                return malformed("deflate: an over-subscribed Huffman code");
            }
        }
        let mut offs = [0u16; MAX_BITS + 2];
        for len in 1..=MAX_BITS {
            offs[len + 1] = offs[len] + count[len];
        }
        let mut symbol = vec![0u16; lengths.len()];
        for (sym, &l) in lengths.iter().enumerate() {
            if l != 0 {
                symbol[usize::from(offs[usize::from(l)])] = sym as u16;
                offs[usize::from(l)] += 1;
            }
        }
        let mut fast = vec![0u16; 1 << FAST_BITS];
        let mut next = [0u32; MAX_BITS + 2];
        let mut code = 0u32;
        let mut prev = 0u32;
        for len in 1..=MAX_BITS {
            code = (code + prev) << 1;
            next[len] = code;
            prev = u32::from(count[len]);
        }
        for (sym, &l) in lengths.iter().enumerate() {
            let l = u32::from(l);
            if l == 0 {
                continue;
            }
            let c = next[l as usize];
            next[l as usize] += 1;
            if l <= FAST_BITS {
                let rev = c.reverse_bits() >> (32 - l);
                let entry = ((sym as u16) << 4) | l as u16;
                let mut at = rev as usize;
                while at < fast.len() {
                    fast[at] = entry;
                    at += 1 << l;
                }
            }
        }
        Ok(Huffman {
            count,
            symbol,
            fast,
        })
    }

    fn decode(&self, bits: &mut Bits) -> Result<usize> {
        bits.refill();
        let e = self.fast[(bits.buf & ((1 << FAST_BITS) - 1)) as usize];
        if e != 0 {
            let len = u32::from(e & 15);
            if len > bits.cnt {
                return malformed("deflate: the stream ends early");
            }
            bits.buf >>= len;
            bits.cnt -= len;
            return Ok(usize::from(e >> 4));
        }
        let mut code: i32 = 0;
        let mut first: i32 = 0;
        let mut index: i32 = 0;
        for len in 1..=MAX_BITS {
            code |= bits.bits(1)? as i32;
            let count = i32::from(self.count[len]);
            if code - count < first {
                let at = (index + (code - first)) as usize;
                return match self.symbol.get(at) {
                    Some(&s) => Ok(usize::from(s)),
                    None => malformed("deflate: a bad Huffman symbol"),
                };
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        malformed("deflate: an invalid Huffman code")
    }
}

fn fixed() -> Result<(Huffman, Huffman)> {
    let mut l = [0u8; 288];
    l[..144].fill(8);
    l[144..256].fill(9);
    l[256..280].fill(7);
    l[280..].fill(8);
    Ok((Huffman::new(&l)?, Huffman::new(&[5u8; 30])?))
}

fn dynamic(bits: &mut Bits) -> Result<(Huffman, Huffman)> {
    let nlen = bits.bits(5)? as usize + 257;
    let ndist = bits.bits(5)? as usize + 1;
    let ncode = bits.bits(4)? as usize + 4;
    if nlen > 286 || ndist > 30 {
        return malformed("deflate: too many length or distance codes");
    }
    let mut cl = [0u8; 19];
    for &o in &CLEN_ORDER[..ncode] {
        cl[o] = bits.bits(3)? as u8;
    }
    let clh = Huffman::new(&cl)?;
    let mut lengths = vec![0u8; nlen + ndist];
    let mut i = 0;
    while i < nlen + ndist {
        let sym = clh.decode(bits)?;
        if sym < 16 {
            lengths[i] = sym as u8;
            i += 1;
            continue;
        }
        let (prev, rep) = match sym {
            16 => {
                if i == 0 {
                    return malformed("deflate: a repeat with nothing before it");
                }
                (lengths[i - 1], 3 + bits.bits(2)? as usize)
            }
            17 => (0, 3 + bits.bits(3)? as usize),
            _ => (0, 11 + bits.bits(7)? as usize),
        };
        if i + rep > nlen + ndist {
            return malformed("deflate: a code length repeat runs past the table");
        }
        lengths[i..i + rep].fill(prev);
        i += rep;
    }
    if lengths[256] == 0 {
        return malformed("deflate: no end-of-block code");
    }
    Ok((
        Huffman::new(&lengths[..nlen])?,
        Huffman::new(&lengths[nlen..])?,
    ))
}

fn codes(
    bits: &mut Bits,
    out: &mut Vec<u8>,
    limit: usize,
    lit: &Huffman,
    dist: &Huffman,
) -> Result<()> {
    loop {
        let sym = lit.decode(bits)?;
        if sym < 256 {
            if out.len() >= limit {
                return malformed("deflate: output over the limit");
            }
            out.push(sym as u8);
            continue;
        }
        if sym == 256 {
            return Ok(());
        }
        let s = sym - 257;
        if s >= 29 {
            return malformed("deflate: a bad length symbol");
        }
        let len = usize::from(LEN_BASE[s]) + bits.bits(u32::from(LEN_EXTRA[s]))? as usize;
        let d = dist.decode(bits)?;
        if d >= 30 {
            return malformed("deflate: a bad distance symbol");
        }
        let back = usize::from(DIST_BASE[d]) + bits.bits(u32::from(DIST_EXTRA[d]))? as usize;
        if back > out.len() {
            return malformed("deflate: a distance before the start");
        }
        if out.len() + len > limit {
            return malformed("deflate: output over the limit");
        }
        let from = out.len() - back;
        if back >= len {
            out.extend_from_within(from..from + len);
        } else {
            for k in 0..len {
                out.push(out[from + k]);
            }
        }
    }
}

/// Decode a raw deflate stream; gives the output and how many input bytes
/// the stream took.
///
/// # Errors
///
/// The stream is malformed or truncated, or its output passes `limit`.
pub fn inflate(data: &[u8], limit: usize) -> Result<(Vec<u8>, usize)> {
    let mut bits = Bits {
        data,
        pos: 0,
        buf: 0,
        cnt: 0,
    };
    let mut out = Vec::new();
    loop {
        let last = bits.bits(1)?;
        match bits.bits(2)? {
            0 => {
                bits.align();
                let len = bits.bits(16)?;
                let nlen = bits.bits(16)?;
                if len != !nlen & 0xFFFF {
                    return malformed("deflate: a stored block length check fails");
                }
                if out.len() + len as usize > limit {
                    return malformed("deflate: output over the limit");
                }
                for _ in 0..len {
                    out.push(bits.bits(8)? as u8);
                }
            }
            1 => {
                let (l, d) = fixed()?;
                codes(&mut bits, &mut out, limit, &l, &d)?;
            }
            2 => {
                let (l, d) = dynamic(&mut bits)?;
                codes(&mut bits, &mut out, limit, &l, &d)?;
            }
            _ => return malformed("deflate: a reserved block type"),
        }
        if last == 1 {
            break;
        }
    }
    bits.align();
    Ok((out, bits.consumed()))
}

fn adler32(b: &[u8]) -> u32 {
    let (mut a, mut s) = (1u32, 0u32);
    for chunk in b.chunks(5552) {
        for &x in chunk {
            a += u32::from(x);
            s += a;
        }
        a %= 65521;
        s %= 65521;
    }
    (s << 16) | a
}

/// Decode a zlib stream (header, deflate, Adler-32); gives the output and
/// how many input bytes the stream took, trailer included.
///
/// # Errors
///
/// The header, stream or checksum is bad, or the output passes `limit`.
pub fn zlib(data: &[u8], limit: usize) -> Result<(Vec<u8>, usize)> {
    let (Some(&cmf), Some(&flg)) = (data.first(), data.get(1)) else {
        return malformed("zlib: no header");
    };
    if cmf & 0x0F != 8 || (u32::from(cmf) * 256 + u32::from(flg)) % 31 != 0 || flg & 0x20 != 0 {
        return malformed("zlib: an unsupported or bad header");
    }
    let (out, used) = inflate(&data[2..], limit)?;
    let end = 2 + used;
    let Some(tail) = data.get(end..end + 4) else {
        return malformed("zlib: no checksum");
    };
    if u32::from_be_bytes([tail[0], tail[1], tail[2], tail[3]]) != adler32(&out) {
        return malformed("zlib: the checksum does not match");
    }
    Ok((out, end + 4))
}

fn crc32(b: &[u8]) -> u32 {
    let mut table = [0u32; 256];
    for (i, t) in table.iter_mut().enumerate() {
        let mut c = i as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 {
                0xEDB8_8320 ^ (c >> 1)
            } else {
                c >> 1
            };
        }
        *t = c;
    }
    !b.iter().fold(!0u32, |c, &x| {
        table[((c ^ u32::from(x)) & 0xFF) as usize] ^ (c >> 8)
    })
}

/// Decode a gzip member (RFC 1952: header, deflate, CRC-32 and length);
/// gives the output and how many input bytes the member took.
///
/// # Errors
///
/// The header, stream, checksum or length is bad, or the output passes
/// `limit`.
pub fn gzip(data: &[u8], limit: usize) -> Result<(Vec<u8>, usize)> {
    if data.len() < 18 || data[..3] != [0x1F, 0x8B, 8] || data[3] & 0xE0 != 0 {
        return malformed("gzip: no header");
    }
    let flags = data[3];
    let mut at = 10;
    if flags & 4 != 0 {
        let Some(n) = data.get(at..at + 2) else {
            return malformed("gzip: the header is cut off");
        };
        at += 2 + usize::from(u16::from_le_bytes([n[0], n[1]]));
    }
    for bit in [8, 16] {
        if flags & bit != 0 {
            let Some(z) = data.get(at..).and_then(|r| r.iter().position(|&x| x == 0)) else {
                return malformed("gzip: a text field is not ended");
            };
            at += z + 1;
        }
    }
    if flags & 2 != 0 {
        at += 2;
    }
    let Some(body) = data.get(at..) else {
        return malformed("gzip: the header is cut off");
    };
    let (out, used) = inflate(body, limit)?;
    let end = at + used;
    let Some(tail) = data.get(end..end + 8) else {
        return malformed("gzip: no trailer");
    };
    let crc = u32::from_le_bytes([tail[0], tail[1], tail[2], tail[3]]);
    let size = u32::from_le_bytes([tail[4], tail[5], tail[6], tail[7]]);
    if crc != crc32(&out) || size != out.len() as u32 {
        return malformed("gzip: the checksum or length does not match");
    }
    Ok((out, end + 8))
}

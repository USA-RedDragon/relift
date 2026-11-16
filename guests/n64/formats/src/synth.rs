//! Synthetic images and a small greedy encoder for each container, for
//! tests. Nothing here resembles a game; the boot code area is filler.

use crate::checksum;
use crate::cic::Cic;
use crate::compress::Format;
use crate::header::SIZE;
use crate::order::ByteOrder;

/// What a synthetic image holds.
#[derive(Debug, Clone)]
pub struct Spec {
    /// The image name, at most 20 bytes.
    pub name: String,
    /// The two game id bytes.
    pub game_id: [u8; 2],
    /// The region byte.
    pub region: u8,
    /// The version byte.
    pub version: u8,
    /// The entry point word.
    pub entry: u32,
    /// The length of the image.
    pub len: usize,
    /// The variant whose seed fills CRC1 and CRC2; zero when `None`.
    pub crc_as: Option<Cic>,
}

impl Default for Spec {
    fn default() -> Spec {
        Spec {
            name: "SYNTHETIC IMAGE".to_owned(),
            game_id: *b"XY",
            region: b'E',
            version: 1,
            entry: 0x8000_0400,
            len: 0x10_2000,
            crc_as: None,
        }
    }
}

/// Deterministic filler: a xorshift stream from `seed`.
#[must_use]
pub fn filler(seed: u32, len: usize) -> Vec<u8> {
    let mut x = seed | 1;
    (0..len)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            (x >> 11) as u8
        })
        .collect()
}

/// A big-endian image for `spec`.
#[must_use]
pub fn rom(spec: &Spec) -> Vec<u8> {
    let mut b = filler(0x1234_5678, spec.len.max(SIZE));
    b[..4].copy_from_slice(&ByteOrder::Z64.magic().to_be_bytes());
    b[4..8].copy_from_slice(&0u32.to_be_bytes());
    b[8..12].copy_from_slice(&spec.entry.to_be_bytes());
    b[0x10..0x18].fill(0);
    b[0x18..0x20].fill(0);
    let name = spec.name.as_bytes();
    b[0x20..0x34].fill(b' ');
    b[0x20..0x20 + name.len().min(20)].copy_from_slice(&name[..name.len().min(20)]);
    b[0x34..0x3B].fill(0);
    b[0x3B] = b'N';
    b[0x3C..0x3E].copy_from_slice(&spec.game_id);
    b[0x3E] = spec.region;
    b[0x3F] = spec.version;
    if let Some(cic) = spec.crc_as
        && let Some((c1, c2)) = checksum::compute(&b, cic)
    {
        b[0x10..0x14].copy_from_slice(&c1.to_be_bytes());
        b[0x14..0x18].copy_from_slice(&c2.to_be_bytes());
    }
    b
}

enum Token {
    Literal(u8),
    Copy { dist: usize, len: usize },
}

fn tokens(data: &[u8], min: usize, max: usize) -> Vec<Token> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < data.len() {
        let (mut best, mut best_dist) = (0, 0);
        for dist in 1..=i.min(4096) {
            let mut k = 0;
            while k < max && i + k < data.len() && data[i + k] == data[i + k - dist] {
                k += 1;
            }
            if k > best {
                (best, best_dist) = (k, dist);
            }
        }
        if best >= min {
            out.push(Token::Copy {
                dist: best_dist,
                len: best,
            });
            i += best;
        } else {
            out.push(Token::Literal(data[i]));
            i += 1;
        }
    }
    out
}

fn put32(out: &mut [u8], at: usize, v: usize) {
    out[at..at + 4].copy_from_slice(&u32::try_from(v).expect("fits").to_be_bytes());
}

/// `data` encoded as a stream of `format`, with a greedy matcher that
/// takes overlapping back-references.
#[must_use]
pub fn encode(format: Format, data: &[u8]) -> Vec<u8> {
    let max = if format == Format::Mio0 { 18 } else { 273 };
    let toks = tokens(data, 3, max);
    match format {
        Format::Yaz0 => {
            let mut out = Vec::from(*format.tag());
            out.extend_from_slice(&u32::try_from(data.len()).expect("fits").to_be_bytes());
            out.extend_from_slice(&[0; 8]);
            for group in toks.chunks(8) {
                let mut flags = 0u8;
                let mut body = Vec::new();
                for (k, t) in group.iter().enumerate() {
                    match *t {
                        Token::Literal(b) => {
                            flags |= 0x80 >> k;
                            body.push(b);
                        }
                        Token::Copy { dist, len } => {
                            let d = dist - 1;
                            if len <= 17 {
                                body.push(((len - 2) << 4 | d >> 8) as u8);
                                body.push(d as u8);
                            } else {
                                body.push((d >> 8) as u8);
                                body.push(d as u8);
                                body.push((len - 0x12) as u8);
                            }
                        }
                    }
                }
                out.push(flags);
                out.extend_from_slice(&body);
            }
            out
        }
        Format::Yay0 | Format::Mio0 => {
            let (mut mask, mut links, mut chunk) = (Vec::new(), Vec::new(), Vec::new());
            for group in toks.chunks(32) {
                let mut word = 0u32;
                for (k, t) in group.iter().enumerate() {
                    match *t {
                        Token::Literal(b) => {
                            word |= 0x8000_0000 >> k;
                            chunk.push(b);
                        }
                        Token::Copy { dist, len } => {
                            let d = dist - 1;
                            let hi = if format == Format::Mio0 {
                                len - 3
                            } else if len <= 17 {
                                len - 2
                            } else {
                                chunk.push((len - 0x12) as u8);
                                0
                            };
                            links.extend_from_slice(&((hi << 12 | d) as u16).to_be_bytes());
                        }
                    }
                }
                mask.extend_from_slice(&word.to_be_bytes());
            }
            let link_at = 16 + mask.len();
            let mut out = vec![0; 16];
            out[..4].copy_from_slice(format.tag());
            put32(&mut out, 4, data.len());
            put32(&mut out, 8, link_at);
            put32(&mut out, 12, link_at + links.len());
            out.extend_from_slice(&mask);
            out.extend_from_slice(&links);
            out.extend_from_slice(&chunk);
            out
        }
    }
}

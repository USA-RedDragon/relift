//! ZBIC, the segment codec of NSOs whose header flag bit 7 is set (first
//! seen in system 22.0.0 and used by large Switch 2 executables), and plain
//! Zstandard frames through the same decoder.
//!
//! ZBIC is Zstandard (RFC 8878) with two changes: the frame magic is the
//! bytes `ZBIC` (`0x4349425A` little-endian) and every FSE table
//! description (the normalized counts of the Huffman weight table and of
//! the literal length, offset and match length tables) is written with a
//! binary interpolative code instead of the RFC's variable-width counts.
//! Frames, blocks, literals, Huffman streams and sequence bitstreams are
//! unchanged.
//!
//! An interpolative table description is:
//!
//! * one byte: bit 7 says whether the table uses low-probability (`-1`)
//!   symbols, bits 0..7 give `n`, the number of bytes that follow;
//! * `n` bytes forming a little-endian integer read as a stream of
//!   mixed-radix digits from its most significant end. The reader keeps a
//!   64-bit window that starts empty and takes the next lower byte while
//!   bytes remain and the window is below 2^40; a digit of radix `r` is the
//!   window modulo `r`, after which the window is divided by `r` and
//!   refilled. All `n` bytes must be taken by the end.
//!
//! The digits are, in order: the highest symbol minus one (radix 52); the
//! table log minus 5 (radix 8); and the total minus one (radix
//! 2^table log), where the total is the sum of the stored per-symbol values:
//! the counts themselves, or with low-probability symbols each count plus
//! one, which makes the total the digit plus the highest symbol plus 2.
//! Then come the running sums of the stored values at the symbol boundaries
//! 1..P-1, where P is the smallest power of two above the highest symbol
//! and the running sums at 0 and P are 0 and the total. They are visited
//! in a fixed order, for the range 0..P: take the midpoint of the range,
//! then the upper half, then the lower half, down to ranges of length 2;
//! a midpoint is coded as its offset from the sum at the range's start
//! with radix one more than the difference of the sums at the range's
//! ends, and when those sums are equal, nothing is coded and every sum
//! inside the range takes that value. One more digit follows, with radix
//! one more than the sum at 1, and it is not used. The count of symbol `s`
//! is the difference of the sums at `s + 1` and `s` (less one with
//! low-probability symbols, which makes `-1` from an empty step); the
//! absolute counts must add up to 2^table log.

use crate::error::{Error, Result, malformed};

/// Which frame dialect to decode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Flavor {
    /// ZBIC: `ZBIC` magic, interpolative table descriptions.
    Zbic,
    /// Plain Zstandard.
    Zstd,
}

impl Flavor {
    fn magic(self) -> u32 {
        match self {
            Flavor::Zbic => 0x4349_425A,
            Flavor::Zstd => 0xFD2F_B528,
        }
    }
}

/// The magic at the start of a ZBIC frame.
pub const ZBIC_MAGIC: [u8; 4] = *b"ZBIC";

const BLOCK_MAX: usize = 128 << 10;
const HUF_LOG_MAX: u32 = 12;
const WEIGHT_LOG_MAX: u32 = 6;
const LL_LOG_MAX: u32 = 9;
const OF_LOG_MAX: u32 = 8;
const ML_LOG_MAX: u32 = 9;
const LL_MAX: usize = 35;
const OF_MAX: usize = 31;
const ML_MAX: usize = 52;

const LL_BASE: [u32; 36] = [
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64,
    128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536,
];
const LL_EXTRA: [u8; 36] = [
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11,
    12, 13, 14, 15, 16,
];
const ML_BASE: [u32; 53] = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027,
    2051, 4099, 8195, 16387, 32771, 65539,
];
const ML_EXTRA: [u8; 53] = [
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
];
const LL_DEFAULT: (u32, &[i16]) = (
    6,
    &[
        4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1,
        1, 1, -1, -1, -1, -1,
    ],
);
const ML_DEFAULT: (u32, &[i16]) = (
    6,
    &[
        1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1,
    ],
);
const OF_DEFAULT: (u32, &[i16]) = (
    5,
    &[
        1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1,
    ],
);

fn bad<T>(what: &str) -> Result<T> {
    malformed(format!("zstd: {what}"))
}

/// Decode one frame that fills `src` exactly into `size` bytes.
///
/// # Errors
///
/// A wrong magic, a dictionary, a malformed or truncated frame, a frame
/// content size or decoded length other than `size`, a checksum mismatch,
/// or bytes after the frame.
pub fn decompress(flavor: Flavor, src: &[u8], size: usize) -> Result<Vec<u8>> {
    let mut f = Frame {
        flavor,
        src,
        at: 0,
        out: Vec::with_capacity(size),
        limit: size,
        huf: None,
        tables: [None, None, None],
        rep: [1, 4, 8],
    };
    f.run()?;
    Ok(f.out)
}

struct Frame<'a> {
    flavor: Flavor,
    src: &'a [u8],
    at: usize,
    out: Vec<u8>,
    limit: usize,
    huf: Option<Huffman>,
    tables: [Option<Fse>; 3],
    rep: [usize; 3],
}

impl Frame<'_> {
    fn take(&mut self, n: usize) -> Result<&[u8]> {
        let end = self.at.checked_add(n).filter(|e| *e <= self.src.len());
        let Some(end) = end else {
            return bad("frame is truncated");
        };
        let s = &self.src[self.at..end];
        self.at = end;
        Ok(s)
    }

    fn le(&mut self, n: usize) -> Result<u64> {
        Ok(self
            .take(n)?
            .iter()
            .rev()
            .fold(0u64, |v, &b| v << 8 | u64::from(b)))
    }

    fn run(&mut self) -> Result<()> {
        if self.le(4)? != u64::from(self.flavor.magic()) {
            return bad("bad frame magic");
        }
        let fhd = self.le(1)? as u8;
        if fhd & 8 != 0 {
            return bad("reserved frame header bit set");
        }
        let single = fhd & 0x20 != 0;
        if !single {
            self.take(1)?;
        }
        let dict = self.le([0, 1, 2, 4][usize::from(fhd & 3)])?;
        if dict != 0 {
            return Err(Error::Unsupported("zstd: frames with a dictionary".into()));
        }
        let fcs = match fhd >> 6 {
            0 if single => Some(self.le(1)?),
            0 => None,
            1 => Some(self.le(2)? + 256),
            2 => Some(self.le(4)?),
            _ => Some(self.le(8)?),
        };
        if let Some(n) = fcs
            && n != self.limit as u64
        {
            return bad("frame content size differs from the expected size");
        }
        loop {
            let h = self.le(3)? as usize;
            let (last, kind, n) = (h & 1 != 0, (h >> 1) & 3, h >> 3);
            match kind {
                0 => {
                    let start = self.at;
                    self.take(n)?;
                    self.emit_from(start, n)?;
                }
                1 => {
                    let b = self.take(1)?[0];
                    self.room(n)?;
                    self.out.resize(self.out.len() + n, b);
                }
                2 => {
                    if n > BLOCK_MAX {
                        return bad("block too large");
                    }
                    let start = self.at;
                    self.take(n)?;
                    let src = self.src;
                    self.block(&src[start..start + n])?;
                }
                _ => return bad("reserved block type"),
            }
            if last {
                break;
            }
        }
        if fhd & 4 != 0 {
            let want = self.le(4)?;
            if xxh64(&self.out) & 0xFFFF_FFFF != want {
                return Err(Error::Hash("zstd frame checksum".into()));
            }
        }
        if self.at != self.src.len() {
            return bad("bytes after the frame");
        }
        if self.out.len() != self.limit {
            return bad("frame decodes to a different size");
        }
        Ok(())
    }

    fn room(&self, n: usize) -> Result<()> {
        if n > BLOCK_MAX || self.out.len() + n > self.limit {
            return bad("output larger than the expected size");
        }
        Ok(())
    }

    fn emit_from(&mut self, start: usize, n: usize) -> Result<()> {
        self.room(n)?;
        self.out.extend_from_slice(&self.src[start..start + n]);
        Ok(())
    }

    fn block(&mut self, b: &[u8]) -> Result<()> {
        let (lits, used) = self.literals(b)?;
        let rest = &b[used..];
        let start = self.out.len();
        self.sequences(rest, &lits)?;
        if self.out.len() - start > BLOCK_MAX {
            return bad("block decodes past the block size limit");
        }
        Ok(())
    }

    fn literals(&mut self, b: &[u8]) -> Result<(Vec<u8>, usize)> {
        let Some(&b0) = b.first() else {
            return bad("empty block");
        };
        let (kind, sf) = (b0 & 3, (b0 >> 2) & 3);
        let byte = |i: usize| b.get(i).copied().map(usize::from).ok_or(());
        let short = || Error::Format("zstd: literals header is truncated".into());
        if kind < 2 {
            let (hl, n) = match sf {
                0 | 2 => (1, usize::from(b0 >> 3)),
                1 => (
                    2,
                    usize::from(b0 >> 4) | byte(1).map_err(|()| short())? << 4,
                ),
                _ => (
                    3,
                    usize::from(b0 >> 4)
                        | byte(1).map_err(|()| short())? << 4
                        | byte(2).map_err(|()| short())? << 12,
                ),
            };
            if n > BLOCK_MAX {
                return bad("too many literals");
            }
            return if kind == 0 {
                let Some(l) = b.get(hl..hl + n) else {
                    return bad("raw literals are truncated");
                };
                Ok((l.to_vec(), hl + n))
            } else {
                let Some(&v) = b.get(hl) else {
                    return bad("RLE literals are truncated");
                };
                Ok((vec![v; n], hl + 1))
            };
        }
        let hl = [3, 3, 4, 5][usize::from(sf)];
        let Some(h) = b.get(..hl) else {
            return Err(short());
        };
        let v = h.iter().rev().fold(0u64, |v, &x| v << 8 | u64::from(x));
        let bits = [10, 10, 14, 18][usize::from(sf)];
        let mask = (1u64 << bits) - 1;
        let regen = ((v >> 4) & mask) as usize;
        let comp = ((v >> (4 + bits)) & mask) as usize;
        if regen > BLOCK_MAX {
            return bad("too many literals");
        }
        let Some(body) = b.get(hl..hl + comp) else {
            return bad("compressed literals are truncated");
        };
        let mut at = 0;
        if kind == 2 {
            let (t, n) = Huffman::read(self.flavor, body)?;
            self.huf = Some(t);
            at = n;
        }
        let Some(t) = &self.huf else {
            return bad("treeless literals without a previous table");
        };
        let data = &body[at..];
        let mut lits = vec![0u8; regen];
        if sf == 0 {
            t.stream(data, &mut lits)?;
        } else {
            if data.len() < 6 {
                return bad("literal jump table is truncated");
            }
            let j = |i: usize| usize::from(u16::from_le_bytes([data[i], data[i + 1]]));
            let (a, b2, c) = (j(0), j(2), j(4));
            let streams = &data[6..];
            if a + b2 + c > streams.len() {
                return bad("literal streams overrun the section");
            }
            let seg = regen.div_ceil(4);
            if 3 * seg > regen {
                return bad("too few literals for four streams");
            }
            let cuts = [0, a, a + b2, a + b2 + c, streams.len()];
            for (i, chunk) in lits.chunks_mut(seg).enumerate() {
                t.stream(&streams[cuts[i]..cuts[i + 1]], chunk)?;
            }
        }
        Ok((lits, hl + comp))
    }

    fn sequences(&mut self, b: &[u8], lits: &[u8]) -> Result<()> {
        let Some(&n0) = b.first() else {
            return bad("sequence section is missing");
        };
        let (count, mut at) = match n0 {
            0 => (0, 1),
            1..=127 => (usize::from(n0), 1),
            128..=254 => match b.get(1) {
                Some(&n1) => ((usize::from(n0) - 128) << 8 | usize::from(n1), 2),
                None => return bad("sequence count is truncated"),
            },
            _ => match b.get(1..3) {
                Some(n) => (usize::from(u16::from_le_bytes([n[0], n[1]])) + 0x7F00, 3),
                None => return bad("sequence count is truncated"),
            },
        };
        if count == 0 {
            if at != b.len() {
                return bad("bytes after an empty sequence section");
            }
            self.room(lits.len())?;
            self.out.extend_from_slice(lits);
            return Ok(());
        }
        let Some(&modes) = b.get(at) else {
            return bad("sequence modes are missing");
        };
        if modes & 3 != 0 {
            return bad("reserved sequence mode bits set");
        }
        at += 1;
        let kinds = [
            (modes >> 6, LL_MAX, LL_LOG_MAX, LL_DEFAULT),
            (modes >> 4 & 3, OF_MAX, OF_LOG_MAX, OF_DEFAULT),
            (modes >> 2 & 3, ML_MAX, ML_LOG_MAX, ML_DEFAULT),
        ];
        for (k, (mode, max, log_max, default)) in kinds.into_iter().enumerate() {
            let t = match mode {
                0 => Fse::build(default.0, default.1)?,
                1 => {
                    let Some(&s) = b.get(at) else {
                        return bad("RLE table symbol is missing");
                    };
                    if usize::from(s) > max {
                        return bad("RLE table symbol out of range");
                    }
                    at += 1;
                    Fse::rle(s)
                }
                2 => {
                    let (log, counts, n) = read_counts(self.flavor, &b[at..], max)?;
                    if log > log_max {
                        return bad("sequence table log too large");
                    }
                    at += n;
                    Fse::build(log, &counts)?
                }
                _ => match self.tables[k].take() {
                    Some(t) => t,
                    None => return bad("repeated table without a previous one"),
                },
            };
            self.tables[k] = Some(t);
        }
        let tables = std::mem::take(&mut self.tables);
        let run = self.run_sequences(&tables, &b[at..], count, lits);
        self.tables = tables;
        run
    }

    fn run_sequences(
        &mut self,
        tables: &[Option<Fse>; 3],
        b: &[u8],
        count: usize,
        lits: &[u8],
    ) -> Result<()> {
        let [Some(ll), Some(of), Some(ml)] = tables else {
            return bad("sequence tables are missing");
        };
        let mut bits = Back::new(b)?;
        let mut sl = bits.read(ll.log);
        let mut so = bits.read(of.log);
        let mut sm = bits.read(ml.log);
        let mut lit = 0usize;
        for i in 0..count {
            let (oc, mc, lc) = (of.cell(so).0, ml.cell(sm).0, ll.cell(sl).0);
            let oc = u32::from(oc);
            if oc > 31 {
                return bad("offset code out of range");
            }
            let ofv = (1u64 << oc) + bits.read(oc);
            let ml_len = ML_BASE[usize::from(mc)] as usize
                + bits.read(u32::from(ML_EXTRA[usize::from(mc)])) as usize;
            let ll_len = LL_BASE[usize::from(lc)] as usize
                + bits.read(u32::from(LL_EXTRA[usize::from(lc)])) as usize;
            let off = self.offset(ofv, ll_len)?;
            let Some(run) = lits.get(lit..lit + ll_len) else {
                return bad("sequence takes more literals than the block has");
            };
            self.room_total(ll_len + ml_len)?;
            self.out.extend_from_slice(run);
            lit += ll_len;
            self.copy_match(off, ml_len)?;
            if i + 1 < count {
                sl = ll.next(sl, &mut bits);
                sm = ml.next(sm, &mut bits);
                so = of.next(so, &mut bits);
            }
        }
        if !bits.done() {
            return bad("sequence bitstream not fully used");
        }
        let tail = &lits[lit..];
        self.room_total(tail.len())?;
        self.out.extend_from_slice(tail);
        Ok(())
    }

    fn room_total(&self, n: usize) -> Result<()> {
        if self.out.len() + n > self.limit {
            return bad("output larger than the expected size");
        }
        Ok(())
    }

    fn offset(&mut self, ofv: u64, ll: usize) -> Result<usize> {
        let r = &mut self.rep;
        let off = if ofv > 3 {
            let o = usize::try_from(ofv - 3).unwrap_or(usize::MAX);
            *r = [o, r[0], r[1]];
            o
        } else {
            let idx = usize::try_from(ofv).unwrap_or(0) - 1 + usize::from(ll == 0);
            match idx {
                0 => r[0],
                1 => {
                    let o = r[1];
                    *r = [o, r[0], r[2]];
                    o
                }
                2 => {
                    let o = r[2];
                    *r = [o, r[0], r[1]];
                    o
                }
                _ => {
                    let o = r[0].wrapping_sub(1);
                    *r = [o, r[0], r[1]];
                    o
                }
            }
        };
        if off == 0 {
            return bad("zero match offset");
        }
        Ok(off)
    }

    fn copy_match(&mut self, off: usize, n: usize) -> Result<()> {
        let len = self.out.len();
        if off > len {
            return bad("match reaches before the start of the frame");
        }
        let from = len - off;
        if off >= n {
            self.out.extend_from_within(from..from + n);
        } else {
            for i in 0..n {
                let v = self.out[from + i];
                self.out.push(v);
            }
        }
        Ok(())
    }
}

struct Back<'a> {
    b: &'a [u8],
    left: i64,
}

impl<'a> Back<'a> {
    fn new(b: &'a [u8]) -> Result<Back<'a>> {
        let Some(&last) = b.last() else {
            return bad("empty bitstream");
        };
        if last == 0 {
            return bad("bitstream has no end mark");
        }
        let left = b.len() as i64 * 8 - i64::from(last.leading_zeros()) - 1;
        Ok(Back { b, left })
    }

    fn peek(&self, k: u32) -> u64 {
        if k == 0 {
            return 0;
        }
        let k64 = i64::from(k);
        let start = self.left - k64;
        let (lo, pad) = if start >= 0 { (start, 0) } else { (0, -start) };
        let have = (k64 - pad).max(0);
        if have == 0 {
            return 0;
        }
        let byte = usize::try_from(lo / 8).unwrap_or(0);
        let w = match self.b.get(byte..byte + 8) {
            Some(c) => u64::from_le_bytes([c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]]),
            None => self.b[byte..]
                .iter()
                .rev()
                .fold(0u64, |w, &x| w << 8 | u64::from(x)),
        };
        let v = (w >> (lo % 8)) & ((1u64 << have) - 1);
        v << pad
    }

    fn read(&mut self, k: u32) -> u64 {
        let v = self.peek(k);
        self.left -= i64::from(k);
        v
    }

    fn done(&self) -> bool {
        self.left == 0
    }

    fn overflowed(&self) -> bool {
        self.left < 0
    }
}

#[derive(Clone)]
struct Fse {
    log: u32,
    cells: Vec<(u8, u8, u16)>,
}

impl Fse {
    fn rle(s: u8) -> Fse {
        Fse {
            log: 0,
            cells: vec![(s, 0, 0)],
        }
    }

    fn build(log: u32, counts: &[i16]) -> Result<Fse> {
        let size = 1usize << log;
        let mut sym = vec![0u8; size];
        let mut high = size;
        for (s, &c) in counts.iter().enumerate() {
            if c == -1 {
                if high == 0 {
                    return bad("too many low-probability symbols");
                }
                high -= 1;
                sym[high] = s as u8;
            }
        }
        let step = (size >> 1) + (size >> 3) + 3;
        let mask = size - 1;
        let mut pos = 0usize;
        for (s, &c) in counts.iter().enumerate() {
            for _ in 0..c.max(0) {
                sym[pos] = s as u8;
                pos = (pos + step) & mask;
                while pos >= high {
                    pos = (pos + step) & mask;
                }
            }
        }
        if pos != 0 {
            return bad("FSE counts do not fill the table");
        }
        let mut next: Vec<u32> = counts.iter().map(|&c| c.max(1) as u32).collect();
        let mut cells = Vec::with_capacity(size);
        for &s in &sym {
            let n = next[usize::from(s)];
            next[usize::from(s)] += 1;
            let nb = log - (31 - n.leading_zeros());
            let base = (n << nb) as usize - size;
            cells.push((s, nb as u8, base as u16));
        }
        Ok(Fse { log, cells })
    }

    fn cell(&self, state: u64) -> (u8, u8, u16) {
        self.cells[state as usize & (self.cells.len() - 1)]
    }

    fn next(&self, state: u64, bits: &mut Back) -> u64 {
        let (_, nb, base) = self.cell(state);
        u64::from(base) + bits.read(u32::from(nb))
    }
}

struct Huffman {
    log: u32,
    cells: Vec<(u8, u8)>,
}

impl Huffman {
    fn read(flavor: Flavor, b: &[u8]) -> Result<(Huffman, usize)> {
        let Some(&h) = b.first() else {
            return bad("Huffman table is missing");
        };
        let mut weights = Vec::new();
        let used = if h >= 128 {
            let n = usize::from(h) - 127;
            let Some(packed) = b.get(1..1 + n.div_ceil(2)) else {
                return bad("Huffman weights are truncated");
            };
            for &x in packed {
                weights.extend([x >> 4, x & 15]);
            }
            weights.truncate(n);
            1 + n.div_ceil(2)
        } else {
            let n = usize::from(h);
            let Some(desc) = b.get(1..1 + n) else {
                return bad("Huffman weights are truncated");
            };
            let (log, counts, at) = read_counts(flavor, desc, 255)?;
            if log > WEIGHT_LOG_MAX {
                return bad("Huffman weight table log too large");
            }
            let t = Fse::build(log, &counts)?;
            let mut bits = Back::new(&desc[at..])?;
            let mut s = [bits.read(log), bits.read(log)];
            let mut k = 0;
            loop {
                if weights.len() > 255 {
                    return bad("too many Huffman weights");
                }
                weights.push(t.cell(s[k]).0);
                s[k] = t.next(s[k], &mut bits);
                if bits.overflowed() {
                    weights.push(t.cell(s[1 - k]).0);
                    break;
                }
                k = 1 - k;
            }
            1 + n
        };
        if weights.len() > 255 {
            return bad("too many Huffman weights");
        }
        Ok((Huffman::from_weights(&weights)?, used))
    }

    fn from_weights(w: &[u8]) -> Result<Huffman> {
        let mut total = 0u32;
        for &x in w {
            if x > HUF_LOG_MAX as u8 {
                return bad("Huffman weight too large");
            }
            if x > 0 {
                total += 1 << (x - 1);
            }
        }
        if total == 0 {
            return bad("Huffman weights are all zero");
        }
        let log = 32 - total.leading_zeros();
        if log > HUF_LOG_MAX {
            return bad("Huffman table log too large");
        }
        let rest = (1u32 << log) - total;
        if !rest.is_power_of_two() {
            return bad("Huffman weights do not complete a code");
        }
        let mut all = w.to_vec();
        all.push((rest.trailing_zeros() + 1) as u8);
        let mut order: Vec<usize> = (0..all.len()).filter(|&s| all[s] > 0).collect();
        order.sort_by_key(|&s| (all[s], s));
        let mut cells = Vec::with_capacity(1 << log);
        for s in order {
            let x = all[s];
            let nb = (log + 1 - u32::from(x)) as u8;
            cells.extend(std::iter::repeat_n((s as u8, nb), 1 << (x - 1)));
        }
        Ok(Huffman { log, cells })
    }

    fn stream(&self, b: &[u8], out: &mut [u8]) -> Result<()> {
        let mut bits = Back::new(b)?;
        for o in out.iter_mut() {
            let (s, nb) = self.cells[bits.peek(self.log) as usize];
            *o = s;
            bits.left -= i64::from(nb);
        }
        if !bits.done() {
            return bad("Huffman stream not fully used");
        }
        Ok(())
    }
}

/// Read a table description: (table log, normalized counts, bytes used).
fn read_counts(flavor: Flavor, b: &[u8], max: usize) -> Result<(u32, Vec<i16>, usize)> {
    match flavor {
        Flavor::Zbic => read_counts_interpolative(b, max),
        Flavor::Zstd => read_counts_rfc(b, max),
    }
}

struct Digits<'a> {
    b: &'a [u8],
    left: usize,
    window: u64,
}

impl Digits<'_> {
    fn refill(&mut self) {
        while self.left > 0 && self.window < 1 << 40 {
            self.left -= 1;
            self.window = self.window << 8 | u64::from(self.b[self.left]);
        }
    }

    fn digit(&mut self, radix: u64) -> u64 {
        let d = self.window % radix;
        self.window /= radix;
        self.refill();
        d
    }
}

fn visit(lo: usize, hi: usize, out: &mut Vec<(usize, usize, usize)>) {
    if hi - lo < 2 {
        return;
    }
    let mid = (lo + hi) / 2;
    out.push((mid, lo, hi));
    visit(mid, hi, out);
    visit(lo, mid, out);
}

/// The interpolative table description; see the module documentation.
///
/// # Errors
///
/// A truncated description, a symbol past `max`, counts that do not add up
/// to the table size, or bytes left over.
pub fn read_counts_interpolative(b: &[u8], max: usize) -> Result<(u32, Vec<i16>, usize)> {
    let Some(&h) = b.first() else {
        return bad("table description is missing");
    };
    let low = u64::from(h >> 7);
    let n = usize::from(h & 0x7F);
    if n + 1 >= b.len() {
        return bad("table description runs past its section");
    }
    let mut d = Digits {
        b: &b[1..=n],
        left: n,
        window: 0,
    };
    d.refill();
    let top = d.digit(52) as usize + 1;
    if top > max {
        return bad("table symbol out of range");
    }
    let log = d.digit(8) as u32 + 5;
    let size = 1u64 << log;
    let mut total = d.digit(size) + 1;
    if low == 1 {
        total += top as u64 + 1;
    }
    let p = (top + 1).next_power_of_two();
    let mut sums = vec![0u64; p + 1];
    sums[p] = total;
    let mut order = Vec::with_capacity(p);
    visit(0, p, &mut order);
    order.push((0, 0, 1));
    for (mid, lo, hi) in order {
        let (a, z) = (sums[lo], sums[hi]);
        if a == z {
            sums[lo + 1..hi].fill(a);
        } else if z < a {
            return bad("table sums decrease");
        } else {
            sums[mid] = a + d.digit(z - a + 1);
        }
    }
    if d.left != 0 {
        return bad("table description has bytes left over");
    }
    let mut counts = Vec::with_capacity(top + 1);
    let mut used = 0u64;
    let mut prev = 0u64;
    for s in 0..=top {
        let step = sums[s + 1].checked_sub(prev);
        let Some(step) = step else {
            return bad("table sums decrease");
        };
        prev = sums[s + 1];
        let c = i64::try_from(step).unwrap_or(i64::MAX) - i64::try_from(low).unwrap_or(0);
        let c = i16::try_from(c).or_else(|_| bad("table count too large"))?;
        used += u64::from(c.unsigned_abs());
        counts.push(c);
    }
    if used != size {
        return bad("table counts do not add up to the table size");
    }
    Ok((log, counts, n + 1))
}

fn read_counts_rfc(b: &[u8], max: usize) -> Result<(u32, Vec<i16>, usize)> {
    let mut pos = 0usize;
    let mut read = |n: u32| -> Result<u32> {
        let mut v = 0u32;
        for i in 0..n {
            let Some(&x) = b.get(pos / 8) else {
                return bad("table description is truncated");
            };
            v |= u32::from(x >> (pos % 8) & 1) << i;
            pos += 1;
        }
        Ok(v)
    };
    let log = read(4)? + 5;
    if log > 15 {
        return bad("table log too large");
    }
    let mut remaining = (1i32 << log) + 1;
    let mut threshold = 1i32 << log;
    let mut nbits = log + 1;
    let mut counts: Vec<i16> = Vec::new();
    while remaining > 1 {
        if counts.len() > max {
            return bad("table symbol out of range");
        }
        let most = 2 * threshold - 1 - remaining;
        let low = read(nbits - 1)? as i32;
        let v = if low < most {
            low
        } else {
            let v = low + ((read(1)? as i32) << (nbits - 1));
            if v >= threshold { v - most } else { v }
        };
        let c = v - 1;
        remaining -= c.abs();
        counts.push(c as i16);
        if c == 0 {
            loop {
                let rep = read(2)?;
                counts.extend(std::iter::repeat_n(0, rep as usize));
                if rep != 3 {
                    break;
                }
            }
        }
        while remaining < threshold && threshold > 1 {
            nbits -= 1;
            threshold >>= 1;
        }
    }
    if remaining != 1 || counts.len() > max + 1 {
        return bad("table counts do not add up to the table size");
    }
    Ok((log, counts, pos.div_ceil(8)))
}

const P1: u64 = 0x9E37_79B1_85EB_CA87;
const P2: u64 = 0xC2B2_AE3D_27D4_EB4F;
const P3: u64 = 0x1656_67B1_9E37_79F9;
const P4: u64 = 0x85EB_CA77_C2B2_AE63;
const P5: u64 = 0x27D4_EB2F_1656_67C5;

fn round(acc: u64, v: u64) -> u64 {
    acc.wrapping_add(v.wrapping_mul(P2))
        .rotate_left(31)
        .wrapping_mul(P1)
}

fn merge(acc: u64, v: u64) -> u64 {
    (acc ^ round(0, v)).wrapping_mul(P1).wrapping_add(P4)
}

fn xxh64(b: &[u8]) -> u64 {
    let word = |c: &[u8]| u64::from_le_bytes(c.try_into().unwrap_or([0; 8]));
    let mut rest = b;
    let mut h = if b.len() >= 32 {
        let mut v = [P1.wrapping_add(P2), P2, 0, 0u64.wrapping_sub(P1)];
        while rest.len() >= 32 {
            for (i, lane) in v.iter_mut().enumerate() {
                *lane = round(*lane, word(&rest[i * 8..i * 8 + 8]));
            }
            rest = &rest[32..];
        }
        let mut h = v[0]
            .rotate_left(1)
            .wrapping_add(v[1].rotate_left(7))
            .wrapping_add(v[2].rotate_left(12))
            .wrapping_add(v[3].rotate_left(18));
        for lane in v {
            h = merge(h, lane);
        }
        h
    } else {
        P5
    };
    h = h.wrapping_add(b.len() as u64);
    while rest.len() >= 8 {
        h = (h ^ round(0, word(&rest[..8])))
            .rotate_left(27)
            .wrapping_mul(P1)
            .wrapping_add(P4);
        rest = &rest[8..];
    }
    if rest.len() >= 4 {
        let v = u64::from(u32::from_le_bytes([rest[0], rest[1], rest[2], rest[3]]));
        h = (h ^ v.wrapping_mul(P1))
            .rotate_left(23)
            .wrapping_mul(P2)
            .wrapping_add(P3);
        rest = &rest[4..];
    }
    for &x in rest {
        h = (h ^ u64::from(x).wrapping_mul(P5))
            .rotate_left(11)
            .wrapping_mul(P1);
    }
    h ^= h >> 33;
    h = h.wrapping_mul(P2);
    h ^= h >> 29;
    h = h.wrapping_mul(P3);
    h ^ (h >> 32)
}

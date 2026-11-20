//! The general path of the scan: programs sit in files in forms no list of
//! formats names, so every file is searched for what a decompressor
//! accepts, by content alone.
//!
//! A stream is a zlib stream (a header of CMF 0x78 and one of the four
//! FLG values, deflate data, a matching Adler-32) or a gzip member (a
//! matching CRC-32 and length), found at any offset and checked by
//! decoding it, so a chance match of the header bytes costs a failed
//! decode and nothing else. Streams that follow each other with only a
//! small header between them ([`CHAIN_GAP`]) are chunks of one payload
//! and are decoded together: a program can run across the chunks.
//!
//! The streams of a buffer are found in windows on all workers; the
//! payloads of the chains are decoded on all workers again, since holding
//! every payload of a multi-gigabyte file at once would not fit.

use crate::inflate;

/// How many bytes of a buffer one worker searches for stream starts.
const WINDOW: usize = 8 << 20;
/// The most one stream may inflate to.
const MAX_STREAM: usize = 256 << 20;
/// The most bytes between two streams that belong to one chain: the
/// chunk headers of the containers that split a payload are a few bytes.
pub const CHAIN_GAP: usize = 16;

/// One stream found in a buffer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Span {
    /// Where the stream starts.
    pub at: usize,
    /// One past its last byte.
    pub end: usize,
    /// Whether it is gzip, else zlib.
    pub gzip: bool,
}

/// The inflated bytes of the stream at `at` in `b`, and where it ends.
fn decode(b: &[u8], at: usize) -> Option<(Vec<u8>, usize, bool)> {
    let rest = &b[at..];
    let gz = rest.first() == Some(&0x1F);
    let r = if gz {
        inflate::gzip(rest, MAX_STREAM)
    } else {
        inflate::zlib(rest, MAX_STREAM)
    };
    r.ok().map(|(out, used)| (out, at + used, gz))
}

fn lead(b: &[u8], i: usize) -> bool {
    match b[i] {
        0x78 => matches!(b.get(i + 1), Some(0x01 | 0x5E | 0x9C | 0xDA)),
        0x1F => b.get(i + 1) == Some(&0x8B) && b.get(i + 2) == Some(&8),
        _ => false,
    }
}

/// The streams that start in `lo..hi` of `b`, in order. After a stream
/// the search goes on at its end, so a stream inside another is left to
/// the search of the other's payload.
pub fn spans_in(b: &[u8], lo: usize, hi: usize) -> Vec<Span> {
    let mut out = Vec::new();
    let hi = hi.min(b.len());
    let mut i = lo;
    while i < hi {
        if !lead(b, i) {
            i += 1;
            continue;
        }
        match decode(b, i) {
            Some((_, end, gzip)) => {
                out.push(Span { at: i, end, gzip });
                i = end;
            }
            None => i += 1,
        }
    }
    out
}

/// Every stream of `b`, in order and not inside another.
pub fn spans(b: &[u8]) -> Vec<Span> {
    let windows: Vec<usize> = (0..b.len()).step_by(WINDOW).collect();
    let mut all: Vec<Span> =
        relift_proc::workers::parallel(&windows, |&lo| spans_in(b, lo, lo + WINDOW))
            .into_iter()
            .flatten()
            .collect();
    all.sort_by_key(|s| s.at);
    let mut out: Vec<Span> = Vec::new();
    for s in all {
        if out.last().is_none_or(|p| s.at >= p.end) {
            out.push(s);
        }
    }
    out
}

/// Groups `spans` (in order) into chains: runs where each stream starts
/// within [`CHAIN_GAP`] bytes of the end of the one before.
pub fn chains(spans: &[Span]) -> Vec<&[Span]> {
    let mut out = Vec::new();
    let mut from = 0;
    for i in 1..=spans.len() {
        if i == spans.len() || spans[i].at - spans[i - 1].end > CHAIN_GAP {
            out.push(&spans[from..i]);
            from = i;
        }
    }
    out.retain(|c| !c.is_empty());
    out
}

/// The payload of a chain: its streams inflated and joined.
pub fn payload(b: &[u8], chain: &[Span]) -> Vec<u8> {
    let mut out = Vec::new();
    for s in chain {
        if let Some((d, _, _)) = decode(b, s.at) {
            out.extend_from_slice(&d);
        }
    }
    out
}

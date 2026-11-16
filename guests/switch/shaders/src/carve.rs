//! Finding shader programs in raw bytes: the `BRA self` terminator every
//! program ends with, a walk back over instructions, and for graphics
//! programs the 0x80-byte shader program header (SPH) before the code.

use crate::sha256_hex;

/// The 12-byte pattern of the `BRA self` instruction that ends a program.
pub const BRA_SELF: [u8; 12] = [
    0x47, 0x79, 0x00, 0x00, 0xf0, 0xff, 0xff, 0xff, 0xff, 0xff, 0x83, 0x03,
];

const NOP_LO: u16 = 0x7918;
const LINE: usize = 16;
const SPH_LEN: usize = 0x80;
const MAX_LINES: usize = 65536;
const SPH_SLACK: usize = SPH_LEN + 16 * 8;

/// Which kind of program a carve found.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Kind {
    /// A graphics program: an SPH, then code.
    Graphics,
    /// A compute program: code only.
    Compute,
}

impl Kind {
    /// The manifest name: `sph` or `cs`.
    pub fn name(self) -> &'static str {
        match self {
            Kind::Graphics => "sph",
            Kind::Compute => "cs",
        }
    }

    /// The kind of a manifest name.
    pub fn from_name(s: &str) -> Option<Kind> {
        match s {
            "sph" => Some(Kind::Graphics),
            "cs" => Some(Kind::Compute),
            _ => None,
        }
    }
}

/// One program found in a byte buffer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Found {
    /// Where the program starts (at the SPH for graphics).
    pub start: usize,
    /// Where the code starts.
    pub code: usize,
    /// One past the terminator.
    pub end: usize,
    /// Graphics or compute.
    pub kind: Kind,
}

impl Found {
    /// The pipeline stage from the SPH (1 vertex, 2 tessellation control,
    /// 3 tessellation evaluation, 4 geometry, 5 pixel), 0 for compute.
    pub fn stage(&self, b: &[u8]) -> u8 {
        match self.kind {
            Kind::Graphics => stage_of(b, self.start),
            Kind::Compute => 0,
        }
    }
}

fn word0(b: &[u8], at: usize) -> Option<u32> {
    let s = b.get(at..at.checked_add(4)?)?;
    Some(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

/// The stage field of the SPH at `at`: bits 10..14 of word 0.
pub fn stage_of(b: &[u8], at: usize) -> u8 {
    word0(b, at).map_or(0, |w| ((w >> 10) & 0xF) as u8)
}

/// Whether an SPH of a supported version (1 or 2, type 4 words) with a
/// graphics stage starts at `q`.
pub fn sph_ok(b: &[u8], q: usize) -> bool {
    if q.checked_add(SPH_LEN).is_none_or(|e| e > b.len()) {
        return false;
    }
    let Some(w0) = word0(b, q) else { return false };
    matches!(w0 & 0x1F, 1 | 2) && (w0 >> 5) & 0x1F == 4 && matches!((w0 >> 10) & 0xF, 1..=5)
}

#[derive(PartialEq, Eq)]
enum Line {
    Eof,
    Zero,
    Nop,
    End,
    Ins,
}

fn line_kind(b: &[u8], p: usize) -> Line {
    let Some(q) = b.get(p..p + LINE) else {
        return Line::Eof;
    };
    if q.iter().all(|&x| x == 0) {
        return Line::Zero;
    }
    if u16::from_le_bytes([q[0], q[1]]) == NOP_LO && q[2..12].iter().all(|&x| x == 0) {
        return Line::Nop;
    }
    if q[..12] == BRA_SELF {
        return Line::End;
    }
    Line::Ins
}

/// Every offset where `pat` occurs in `b`, overlapping ones included.
pub fn find_all<'a>(b: &'a [u8], pat: &'a [u8]) -> impl Iterator<Item = usize> + 'a {
    let first = pat[0];
    let mut from = 0usize;
    std::iter::from_fn(move || {
        while from + pat.len() <= b.len() {
            let i = from
                + b[from..=b.len() - pat.len()]
                    .iter()
                    .position(|&x| x == first)?;
            from = i + 1;
            if b[i..].starts_with(pat) {
                return Some(i);
            }
        }
        None
    })
}

/// Every program in `b`: from each terminator, walk back over
/// instruction lines to the first line that is zero, NOP, another
/// terminator or past the start; look for an SPH within 0x80 to 0x100 bytes
/// before that line; without one the program is compute.
pub fn carve(b: &[u8]) -> Vec<Found> {
    let mut out = Vec::new();
    for e in find_all(b, &BRA_SELF) {
        let mut p = e as isize - LINE as isize;
        let mut steps = 0;
        while p >= 0 && line_kind(b, p as usize) == Line::Ins && steps < MAX_LINES {
            p -= LINE as isize;
            steps += 1;
        }
        let first = (p + LINE as isize) as usize;
        let mut found = Found {
            start: first,
            code: first,
            end: e + LINE,
            kind: Kind::Compute,
        };
        for back in (SPH_LEN..=SPH_SLACK).step_by(LINE) {
            let Some(q) = first.checked_sub(back) else {
                break;
            };
            if sph_ok(b, q) {
                found.start = q;
                found.code = q + SPH_LEN;
                found.kind = Kind::Graphics;
                break;
            }
        }
        out.push(found);
    }
    out
}

/// The SHA-256 of a program's bytes as lowercase hex.
pub fn program_sha(b: &[u8], f: &Found) -> String {
    sha256_hex(&b[f.start..f.end])
}

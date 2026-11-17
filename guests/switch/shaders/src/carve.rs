//! Finding shader programs in raw bytes: the `BRA self` terminator every
//! program ends with, a walk back over lines that decode as SM86
//! instructions, and for graphics
//! programs the 0x80-byte shader program header (SPH) before the code.
//!
//! GLSLC writes each program after a header that starts with the magic
//! 0x98761234 and holds, at 0x30, the offset of the code from the header.
//! Part of that header is not zero, so the walk back can take its tail for
//! instructions; where such a header sits before a compute find and names
//! a code start inside it, the find starts there instead.

use crate::sha256_hex;

/// The 12-byte pattern of the `BRA self` instruction that ends a program.
pub const BRA_SELF: [u8; 12] = [
    0x47, 0x79, 0x00, 0x00, 0xf0, 0xff, 0xff, 0xff, 0xff, 0xff, 0x83, 0x03,
];

const NOP_LO: u16 = 0x7918;
const LINE: usize = 16;
const SPH_LEN: usize = 0x80;
/// The longest walk back from a terminator, in 16-byte lines.
pub const MAX_LINES: usize = 65536;
const SPH_SLACK: usize = SPH_LEN + 16 * 8;
/// The magic of a GLSLC program header.
pub const PROGRAM_MAGIC: [u8; 4] = [0x34, 0x12, 0x76, 0x98];
/// Where a GLSLC program header holds the code offset.
const CODE_OFFSET_AT: usize = 0x30;
/// The longest GLSLC program header looked for.
const PROGRAM_HEADER_MAX: usize = 0x200;

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
    /// The walk back stopped at [`MAX_LINES`] with code still before
    /// `code`: the start is not the program's, so the bytes are not one.
    pub cut: bool,
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
    /// Bytes that are no SM86 instruction.
    Data,
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
    let mut w = [0u8; LINE];
    w.copy_from_slice(q);
    match relift_gpu_sm86::decode(relift_gpu_sm86::Word::from_bytes(&w), 0) {
        Ok(_) => Line::Ins,
        Err(_) => Line::Data,
    }
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

/// The start of the SPH within 0x80 to 0x100 bytes before `first`.
fn sph_before(b: &[u8], first: usize) -> Option<usize> {
    (SPH_LEN..=SPH_SLACK)
        .step_by(LINE)
        .filter_map(|back| first.checked_sub(back))
        .find(|&q| sph_ok(b, q))
}

/// The code start a GLSLC program header before `first` names, when it
/// lies after `first`, before `end` and on a line of the find: the header
/// starts on a word boundary within [`PROGRAM_HEADER_MAX`] bytes before it
/// (its code offset need not be a whole number of lines).
fn header_code(b: &[u8], first: usize, end: usize) -> Option<usize> {
    (4..=PROGRAM_HEADER_MAX)
        .step_by(4)
        .filter_map(|back| first.checked_sub(back))
        .find_map(|r| {
            if b.get(r..r + 4)? != PROGRAM_MAGIC {
                return None;
            }
            let off = word0(b, r + CODE_OFFSET_AT)? as usize;
            let code = r.checked_add(off)?;
            (code > first && code < end && (code - first).is_multiple_of(LINE)).then_some(code)
        })
}

/// Where the walk back from the instruction line at `p` over a run of
/// NOP lines continues: a run that follows an instruction line and does
/// not sit right after an SPH is alignment inside a program, so the walk
/// goes on from that instruction line.
fn past_nops(b: &[u8], p: usize) -> Option<usize> {
    if sph_before(b, p + LINE).is_some() {
        return None;
    }
    let mut q = p;
    while line_kind(b, q) == Line::Nop {
        q = q.checked_sub(LINE)?;
    }
    (line_kind(b, q) == Line::Ins).then_some(q)
}

/// Every program in `b`: from each terminator, walk back over
/// instruction lines (and over NOP lines inside a program) to the first
/// line that is zero, no SM86 instruction (data before the code), a NOP
/// run after a terminator or an SPH, another terminator or past the start;
/// a terminator with no instruction before it ends no program; look for an SPH within 0x80 to 0x100
/// bytes before that line; without one the program is compute, starting
/// where a GLSLC program header before it says its code starts. A walk
/// that reaches [`MAX_LINES`] marks the find [`Found::cut`].
pub fn carve(b: &[u8]) -> Vec<Found> {
    let mut out = Vec::new();
    for e in find_all(b, &BRA_SELF) {
        let mut p = e as isize - LINE as isize;
        let mut steps = 0;
        while p >= 0 && steps < MAX_LINES {
            match line_kind(b, p as usize) {
                Line::Ins => p -= LINE as isize,
                Line::Nop => match past_nops(b, p as usize) {
                    Some(q) => p = q as isize,
                    None => break,
                },
                _ => break,
            }
            steps += 1;
        }
        let first = (p + LINE as isize) as usize;
        if first == e {
            continue;
        }
        let mut found = Found {
            start: first,
            code: first,
            end: e + LINE,
            kind: Kind::Compute,
            cut: p >= 0
                && steps >= MAX_LINES
                && matches!(line_kind(b, p as usize), Line::Ins | Line::Nop),
        };
        if let Some(q) = sph_before(b, first) {
            found.start = q;
            found.code = q + SPH_LEN;
            found.kind = Kind::Graphics;
        } else if let Some(code) = header_code(b, first, found.end) {
            found.start = code;
            found.code = code;
        }
        out.push(found);
    }
    out
}

/// The SHA-256 of a program's bytes as lowercase hex.
pub fn program_sha(b: &[u8], f: &Found) -> String {
    sha256_hex(&b[f.start..f.end])
}

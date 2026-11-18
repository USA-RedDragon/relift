//! An NVN Maxwell program as GLSLC writes it: a 0x30-byte code header
//! (the u32 0x12345678 first), the 0x50-byte shader program header (SPH)
//! of a graphics program, then the code in bundles.
//!
//! The code's extent is what control can reach from its first instruction:
//! branch, call and the SSY/PBK/PCNT targets, with a path ending at an
//! unconditional EXIT, BRA, BRK, CONT, SYNC, RET or KIL, as Ryujinx's
//! decoder (MIT) follows it. A target on a scheduling word (GLSLC emits
//! them) runs from the bundle's first instruction.

use crate::bundle::{BUNDLE, slots};
use crate::form::{Op, classify};
use relift_gpu_sm86::program::Stage;

/// The first word of a code header.
pub const CODE_MAGIC: u32 = 0x1234_5678;
/// Bytes in the code header.
pub const CODE_HEADER_LEN: usize = 0x30;
/// Bytes in a Maxwell SPH.
pub const SPH_LEN: usize = 0x50;
/// The most instructions a walk visits.
const MAX_CODE: usize = 1 << 20;

/// A Maxwell shader program header (20 words).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Sph {
    pub words: [u32; 20],
}

impl Sph {
    /// The SPH at the start of `b`, when its first word has type 1
    /// (vertex, tessellation, geometry) or 2 (pixel), version 3 and a
    /// known stage (field positions from Ryujinx's ShaderHeader, MIT).
    pub fn parse(b: &[u8]) -> Option<Sph> {
        let mut words = [0u32; 20];
        for (i, w) in words.iter_mut().enumerate() {
            *w = u32::from_le_bytes(b.get(4 * i..4 * i + 4)?.try_into().ok()?);
        }
        let s = Sph { words };
        let kind = s.words[0] & 0x1f;
        let version = (s.words[0] >> 5) & 0x1f;
        if !(kind == 1 || kind == 2) || version != 3 {
            return None;
        }
        s.stage()?;
        Some(s)
    }

    /// The stage of bits 10 to 13 of word 0.
    pub fn stage(&self) -> Option<Stage> {
        Some(match (self.words[0] >> 10) & 0xf {
            1 => Stage::Vertex,
            2 => Stage::TessControl,
            3 => Stage::TessEval,
            4 => Stage::Geometry,
            5 => Stage::Fragment,
            _ => return None,
        })
    }
}

/// Why bytes are not a program.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ParseError {
    /// The bytes do not start with a code header.
    NoHeader,
    /// No pattern matches the reachable word at this code offset.
    Unknown { at: u64, word: u64 },
    /// Control reaches this code offset, outside the bytes.
    OutOfBounds { at: u64 },
    /// The branch at `at` targets a negative or unaligned offset.
    BadTarget { at: u64, target: i64 },
    /// The walk visited more instructions than any program has.
    TooLong,
}

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ParseError::NoHeader => f.write_str("no NVN code header"),
            ParseError::Unknown { at, word } => {
                write!(f, "unknown instruction {word:#018x} at {at:#x}")
            }
            ParseError::OutOfBounds { at } => write!(f, "control reaches {at:#x}, past the bytes"),
            ParseError::BadTarget { at, target } => {
                write!(f, "the branch at {at:#x} targets {target:#x}")
            }
            ParseError::TooLong => f.write_str("more instructions than a program holds"),
        }
    }
}

impl std::error::Error for ParseError {}

/// One program: its SPH (none for compute) and its code bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Program {
    pub sph: Option<Sph>,
    /// The code, from its first bundle through the last one control reaches.
    pub code: Vec<u8>,
    /// Control leaves through an indirect or absolute jump (BRX, JMP, JMX,
    /// JCAL, LONGJMP, PLONGJMP) whose targets the walk does not know, so
    /// the extent may be short.
    pub indirect: bool,
}

impl Program {
    /// The program at the start of `b` (its code header).
    ///
    /// # Errors
    ///
    /// See [`ParseError`].
    pub fn parse(b: &[u8]) -> Result<Program, ParseError> {
        let magic = b
            .get(..4)
            .map(|m| u32::from_le_bytes([m[0], m[1], m[2], m[3]]));
        if magic != Some(CODE_MAGIC) || b.len() < CODE_HEADER_LEN {
            return Err(ParseError::NoHeader);
        }
        let rest = &b[CODE_HEADER_LEN..];
        let sph = Sph::parse(rest);
        let code = if sph.is_some() {
            rest.get(SPH_LEN..).unwrap_or(&[])
        } else {
            rest
        };
        let (end, indirect) = extent(code)?;
        Ok(Program {
            sph,
            code: code[..end].to_vec(),
            indirect,
        })
    }

    /// The stage: from the SPH, compute without one.
    pub fn stage(&self) -> Stage {
        self.sph
            .as_ref()
            .and_then(Sph::stage)
            .unwrap_or(Stage::Compute)
    }
}

/// The signed 24-bit branch offset of `w` (bits 20 to 43), added to the
/// offset after the instruction.
fn target(at: u64, w: u64) -> i64 {
    let imm = (((w >> 20) & 0xff_ffff) as i64) << 40 >> 40;
    at as i64 + 8 + imm
}

/// Whether `w` always takes its branch: predicate PT not negated, and for
/// BRA and EXIT the condition code test T.
fn always(op: Op, w: u64) -> bool {
    let pt = (w >> 16) & 0xf == 7;
    let cc = !matches!(op, Op::Bra | Op::Exit) || w & 0x1f == 0xf;
    pt && cc
}

/// One past the last code byte control reaches, rounded up to a bundle, and
/// whether an indirect jump leaves the walk.
fn extent(code: &[u8]) -> Result<(usize, bool), ParseError> {
    let words: Vec<u64> = slots(code).map(|s| s.word).collect();
    let word_at = |at: u64| -> Result<u64, ParseError> {
        let k = (at / BUNDLE as u64) as usize * 3 + ((at % BUNDLE as u64) / 8) as usize - 1;
        words.get(k).copied().ok_or(ParseError::OutOfBounds { at })
    };
    let mut seen = std::collections::HashSet::new();
    let mut todo = vec![8u64];
    let mut end = 0u64;
    let mut indirect = false;
    while let Some(mut at) = todo.pop() {
        loop {
            if !seen.insert(at) {
                break;
            }
            if seen.len() > MAX_CODE {
                return Err(ParseError::TooLong);
            }
            let w = word_at(at)?;
            let e = classify(w).ok_or(ParseError::Unknown { at, word: w })?;
            end = end.max(at + 8);
            let next = if (at + 8).is_multiple_of(BUNDLE as u64) {
                at + 16
            } else {
                at + 8
            };
            match e.op {
                Op::Bra | Op::Ssy | Op::Pbk | Op::Pcnt | Op::Cal => {
                    let t = target(at, w);
                    if t < 0 || t & 7 != 0 {
                        return Err(ParseError::BadTarget { at, target: t });
                    }
                    let t = t as u64;
                    todo.push(if crate::bundle::is_sched(t) { t + 8 } else { t });
                    if e.op == Op::Bra && always(e.op, w) {
                        break;
                    }
                }
                Op::Exit | Op::Brk | Op::Cont | Op::Sync | Op::Ret | Op::Kil => {
                    if always(e.op, w) {
                        break;
                    }
                }
                Op::Brx | Op::Jmp | Op::Jmx | Op::Longjmp | Op::Plongjmp => {
                    indirect = true;
                    if always(e.op, w) {
                        break;
                    }
                }
                Op::Jcal => indirect = true,
                _ => {}
            }
            at = next;
        }
    }
    let end = (end as usize).div_ceil(BUNDLE) * BUNDLE;
    Ok((end, indirect))
}

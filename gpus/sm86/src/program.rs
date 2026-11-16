use crate::{DecodeError, Inst, Word, decode};

pub const SPH_SIZE: usize = 0x80;
const BRA_SELF: [u8; 12] = [
    0x47, 0x79, 0, 0, 0xf0, 0xff, 0xff, 0xff, 0xff, 0xff, 0x83, 0x03,
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    Vertex,
    TessControl,
    TessEval,
    Geometry,
    Fragment,
    Compute,
}

#[derive(Clone, Debug)]
pub struct Sph {
    pub words: [u32; 32],
}

impl Sph {
    pub fn parse(b: &[u8]) -> Option<Sph> {
        if b.len() < SPH_SIZE {
            return None;
        }
        let mut words = [0u32; 32];
        for (i, w) in words.iter_mut().enumerate() {
            *w = u32::from_le_bytes(b[4 * i..4 * i + 4].try_into().ok()?);
        }
        let s = Sph { words };
        let kind = s.words[0] & 0x1f;
        let version = (s.words[0] >> 5) & 0x1f;
        if !(kind == 1 || kind == 2) || version != 4 {
            return None;
        }
        s.stage()?;
        Some(s)
    }

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

    /// Bits `lo..lo + len` of the header.
    pub fn field(&self, lo: usize, len: usize) -> u32 {
        let mut v = 0u64;
        for k in 0..len {
            let b = lo + k;
            v |= u64::from((self.words[b / 32] >> (b % 32)) & 1) << k;
        }
        v as u32
    }

    /// Invocations per input primitive: the output vertex count of a
    /// tessellation control program, the invocation count of a geometry
    /// program.
    pub fn threads_per_input_primitive(&self) -> u32 {
        self.field(88, 8)
    }

    /// Geometry output topology: 1 point list, 6 line strip, 7 triangle
    /// strip.
    pub fn output_topology(&self) -> u32 {
        self.field(120, 4)
    }

    pub fn max_output_vertices(&self) -> u32 {
        self.field(128, 12)
    }

    /// Vertex streams a geometry program emits to.
    pub fn stream_out_mask(&self) -> u32 {
        self.field(28, 4)
    }
}

#[derive(Clone, Debug)]
pub struct Program {
    pub sph: Option<Sph>,
    pub words: Vec<Word>,
}

impl Program {
    pub fn parse(b: &[u8]) -> Program {
        let sph = Sph::parse(b);
        let code = if sph.is_some() { &b[SPH_SIZE..] } else { b };
        let mut words = Vec::new();
        for c in code.as_chunks::<16>().0 {
            words.push(Word::from_bytes(c));
            if c[..12] == BRA_SELF {
                break;
            }
        }
        Program { sph, words }
    }

    pub fn stage(&self) -> Stage {
        self.sph
            .as_ref()
            .and_then(Sph::stage)
            .unwrap_or(Stage::Compute)
    }

    pub fn decode(&self) -> Vec<Result<Inst, DecodeError>> {
        self.words
            .iter()
            .enumerate()
            .map(|(i, w)| decode(*w, 16 * i as u64))
            .collect()
    }
}

use std::collections::BTreeMap;

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
    /// A task program: SPH type 1 (vertex) with the stage buffer (ISBE)
    /// header of a task or mesh program (see [`Sph::isbe_program`]).
    Task,
    /// A mesh program: SPH type 3 (tessellation evaluation) after a task
    /// program, or type 1 without one, with the stage buffer header.
    Mesh,
}

impl Stage {
    /// The task and mesh stages, which run as workgroups like compute.
    pub fn is_mesh_pipeline(self) -> bool {
        matches!(self, Stage::Task | Stage::Mesh)
    }
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
        let isbe = self.isbe_program();
        Some(match (self.words[0] >> 10) & 0xf {
            1 if isbe => Stage::Task,
            3 if isbe => Stage::Mesh,
            1 => Stage::Vertex,
            2 => Stage::TessControl,
            3 => Stage::TessEval,
            4 => Stage::Geometry,
            5 => Stage::Fragment,
            _ => return None,
        })
    }

    /// Word 4 of a task or mesh program: no maximum output vertex count
    /// and the whole stage buffer (store request 0 to 255) in place of the
    /// attribute range a vertex or tessellation program stores. NVIDIA
    /// types task programs as vertex and mesh programs as tessellation
    /// evaluation; no vertex or tessellation evaluation program of the
    /// corpus has this word.
    pub fn isbe_program(&self) -> bool {
        self.words[4] == 0xff00_0000
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

/// `ISBEWR.O` to the primitive map or `ISBEWR.O.ATTR.SKEW`: an output only
/// a mesh program writes.
fn writes_mesh_output(w: Word) -> bool {
    w.get(0, 12) == 0x927 && w.get(79, 1) == 1 && (w.get(76, 2) == 0 || w.get(78, 1) == 1)
}

#[derive(Clone, Debug)]
pub struct Program {
    pub sph: Option<Sph>,
    pub words: Vec<Word>,
    /// Pseudo-instructions by word index, in place of the word there: an
    /// instruction of another GPU that no SM86 form matches (see
    /// `translate::pseudo`). Their words are never decoded.
    pub pseudo: BTreeMap<usize, Inst>,
}

impl Program {
    /// The program in `b`: the SPH if there is one, then the code through
    /// the last `BRA self` (a linked CUDA kernel has one per function).
    pub fn parse(b: &[u8]) -> Program {
        let sph = Sph::parse(b);
        let code = if sph.is_some() { &b[SPH_SIZE..] } else { b };
        let lines = code.as_chunks::<16>().0;
        let n = lines
            .iter()
            .rposition(|c| c[..12] == BRA_SELF)
            .map_or(lines.len(), |k| k + 1);
        let words = lines[..n].iter().map(Word::from_bytes).collect();
        Program {
            sph,
            words,
            pseudo: BTreeMap::new(),
        }
    }

    /// The stage. A task program and a mesh program without one share an
    /// SPH; the mesh program is the one that writes mesh outputs.
    pub fn stage(&self) -> Stage {
        match self.sph.as_ref().and_then(Sph::stage) {
            Some(Stage::Task) if self.words.iter().any(|w| writes_mesh_output(*w)) => Stage::Mesh,
            Some(s) => s,
            None => Stage::Compute,
        }
    }

    /// A mesh program that runs after a task program.
    pub fn after_task(&self) -> bool {
        self.sph
            .as_ref()
            .is_some_and(|s| (s.words[0] >> 10) & 0xf == 3)
    }

    pub fn decode(&self) -> Vec<Result<Inst, DecodeError>> {
        self.words
            .iter()
            .enumerate()
            .map(|(i, w)| match self.pseudo.get(&i) {
                Some(p) => Ok(p.clone()),
                None => decode(*w, 16 * i as u64),
            })
            .collect()
    }
}

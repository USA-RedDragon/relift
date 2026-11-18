//! Maxwell (Switch 1) programs in raw bytes: every NVN code header
//! (`relift_gpu_sm53::program::CODE_MAGIC` and 44 zero bytes) that starts a
//! program `relift_gpu_sm53::Program::parse` accepts. A program's bytes
//! are its 0x50-byte SPH (graphics) and its code through the last bundle
//! control reaches.

use relift_gpu_nv::program::Stage;
use relift_gpu_sm53::program::{CODE_HEADER_LEN, CODE_MAGIC, SPH_LEN};
use relift_gpu_sm53::{ParseError, Program};

use crate::carve::{Kind, find_all};

/// The bytes every code header starts with.
fn header() -> [u8; CODE_HEADER_LEN] {
    let mut h = [0u8; CODE_HEADER_LEN];
    h[..4].copy_from_slice(&CODE_MAGIC.to_le_bytes());
    h
}

/// One program found.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Found {
    /// Where its code header starts.
    pub at: usize,
    /// Graphics or compute.
    pub kind: Kind,
    /// The SPH stage (1 VS to 5 PS), 0 for compute.
    pub stage: u8,
    /// The SPH (graphics) and the code.
    pub bytes: Vec<u8>,
    /// Bytes of SPH before the code: 0x50, or 0 for compute.
    pub sph: usize,
}

/// Every program in `b`, and the code headers that start no program with
/// the reason.
pub fn carve(b: &[u8]) -> (Vec<Found>, Vec<(usize, ParseError)>) {
    let h = header();
    let mut found = Vec::new();
    let mut errors = Vec::new();
    for at in find_all(b, &h) {
        match Program::parse(&b[at..]) {
            Ok(p) => {
                let stage = match p.stage() {
                    Stage::Vertex => 1,
                    Stage::TessControl => 2,
                    Stage::TessEval => 3,
                    Stage::Geometry => 4,
                    Stage::Fragment => 5,
                    _ => 0,
                };
                let (kind, sph) = if p.sph.is_some() {
                    (Kind::Graphics, SPH_LEN)
                } else {
                    (Kind::Compute, 0)
                };
                let start = at + CODE_HEADER_LEN;
                let bytes = b[start..start + sph + p.code.len()].to_vec();
                found.push(Found {
                    at,
                    kind,
                    stage,
                    bytes,
                    sph,
                });
            }
            Err(e) => errors.push((at, e)),
        }
    }
    (found, errors)
}

//! SM86 program bytes as the translator takes them.

use relift_gpu_nv::program::{Program, SPH_SIZE, Sph};

use crate::{Word, decode};

const BRA_SELF: [u8; 12] = [
    0x47, 0x79, 0, 0, 0xf0, 0xff, 0xff, 0xff, 0xff, 0xff, 0x83, 0x03,
];

/// The words of the program in `b`: the code after the SPH, if there is
/// one, through the last `BRA self` (a linked CUDA kernel has one per
/// function).
pub fn words(b: &[u8]) -> Vec<Word> {
    let code = if Sph::parse(b).is_some() {
        &b[SPH_SIZE..]
    } else {
        b
    };
    let lines = code.as_chunks::<16>().0;
    let n = lines
        .iter()
        .rposition(|c| c[..12] == BRA_SELF)
        .map_or(lines.len(), |k| k + 1);
    lines[..n].iter().map(Word::from_bytes).collect()
}

/// The program in `b`: the SPH if there is one, then the code (see
/// [`words`]) decoded.
pub fn parse(b: &[u8]) -> Program {
    Program {
        sph: Sph::parse(b),
        code: words(b)
            .into_iter()
            .enumerate()
            .map(|(i, w)| decode(w, 16 * i as u64).map_err(|e| e.to_string()))
            .collect(),
    }
}

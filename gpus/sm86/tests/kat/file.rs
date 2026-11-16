//! The known-answer files in this directory, written by `kat/gen.py` from an
//! RTX 3090 (see `kat/README.md`).
//!
//! Layout, little-endian: the magic `RLKAT\x01\0\0`; input words per row,
//! rows, variants and prefix words (u32 each); the prefix's 16-byte
//! instruction words, shared by every variant; per variant a u16 label
//! length, the label, the count of its own words and of its outputs (u32
//! each), and its words; the input rows; then per variant and output a
//! column: a u16 index of an earlier column (the inputs first, then the
//! outputs in order) and the outputs XORed with it, where the varint `2n+1`
//! is a run of `n` zeros and `2d` the value `d`.

use std::path::{Path, PathBuf};

pub struct Variant {
    pub label: String,
    /// The whole program: prefix, the variant's words, stores, EXIT.
    pub code: Vec<u8>,
    /// The device's outputs, `rows x outs`.
    pub outs: usize,
    pub expect: Vec<u32>,
}

pub struct Family {
    pub nin: usize,
    pub rows: Vec<[u32; 4]>,
    pub variants: Vec<Variant>,
}

struct Reader<'a> {
    b: &'a [u8],
    at: usize,
}

impl Reader<'_> {
    fn take(&mut self, n: usize) -> &[u8] {
        let s = &self.b[self.at..self.at + n];
        self.at += n;
        s
    }
    fn u16(&mut self) -> usize {
        u16::from_le_bytes(self.take(2).try_into().unwrap()) as usize
    }
    fn u32(&mut self) -> u32 {
        u32::from_le_bytes(self.take(4).try_into().unwrap())
    }
    fn varint(&mut self) -> u64 {
        let mut v = 0u64;
        let mut sh = 0;
        loop {
            let b = self.take(1)[0];
            v |= u64::from(b & 0x7f) << sh;
            if b < 0x80 {
                return v;
            }
            sh += 7;
        }
    }
}

pub fn dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/kat")
}

pub fn load(name: &str) -> Family {
    let path = dir().join(format!("{name}.kat"));
    let b = std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let mut r = Reader { b: &b, at: 0 };
    assert_eq!(r.take(8), b"RLKAT\x01\0\0", "{name}: magic");
    let nin = r.u32() as usize;
    let nrows = r.u32() as usize;
    let nvar = r.u32() as usize;
    let npre = r.u32() as usize;
    let prefix = r.take(16 * npre).to_vec();
    let mut variants = Vec::new();
    for _ in 0..nvar {
        let n = r.u16();
        let label = String::from_utf8(r.take(n).to_vec()).unwrap();
        let nw = r.u32() as usize;
        let outs = r.u32() as usize;
        let mut code = prefix.clone();
        code.extend_from_slice(r.take(16 * nw));
        variants.push(Variant {
            label,
            code,
            outs,
            expect: Vec::new(),
        });
    }
    let mut rows = vec![[0u32; 4]; nrows];
    for row in &mut rows {
        for w in row.iter_mut().take(nin) {
            *w = r.u32();
        }
    }
    let mut cols: Vec<Vec<u32>> = (0..nin)
        .map(|k| rows.iter().map(|row| row[k]).collect())
        .collect();
    for v in &mut variants {
        v.expect = vec![0; nrows * v.outs];
        for k in 0..v.outs {
            let base = cols[r.u16()].clone();
            let mut col = Vec::with_capacity(nrows);
            while col.len() < nrows {
                let t = r.varint();
                if t & 1 == 1 {
                    for _ in 0..t >> 1 {
                        col.push(base[col.len()]);
                    }
                } else {
                    col.push(base[col.len()] ^ (t >> 1) as u32);
                }
            }
            for (i, x) in col.iter().enumerate() {
                v.expect[i * v.outs + k] = *x;
            }
            cols.push(col);
        }
    }
    assert_eq!(r.at, b.len(), "{name}: trailing bytes");
    Family {
        nin,
        rows,
        variants,
    }
}

/// Every family in the directory, by name.
pub fn names() -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(dir())
        .unwrap()
        .filter_map(|e| {
            let p = e.ok()?.path();
            (p.extension()? == "kat").then(|| p.file_stem()?.to_str().map(String::from))?
        })
        .collect();
    v.sort();
    v
}

//! The transform feedback layout of a graphics program, from the GLSLC
//! program header NVN takes beside its code.
//!
//! The NVN2 driver reads it there when the program is bound (SDK 22 driver:
//! the header accessors at 0xdd1558, the copy at 0x5e5bc0): the header's
//! byte at 0x54 is the buffer count `n` (0: no transform feedback), its
//! byte at 0xa7 says the entries are hardware attribute indices already
//! ("raw"), and its word at 0xc4 the offset of a section from the header:
//! a version word (1), `n` 16-bit strides in bytes, `n` stream bytes, `n`
//! entry counts, then per buffer the entries' attribute bytes (signed), the
//! first component bytes and the component count bytes. The header sits
//! before the SPH, its word at 0x30 the SPH's offset from it.

use crate::carve::PROGRAM_MAGIC;
use serde_json::{Value, json};

/// The longest GLSLC header looked for before an SPH.
const HEADER_MAX: usize = 0x200;
const CODE_OFFSET_AT: usize = 0x30;
const BUFFERS_AT: usize = 0x54;
const RAW_AT: usize = 0xa7;
const SECTION_AT: usize = 0xc4;
/// The most transform feedback buffers.
const MAX_BUFFERS: usize = 4;

fn u32_at(b: &[u8], at: usize) -> Option<usize> {
    Some(u32::from_le_bytes(b.get(at..at.checked_add(4)?)?.try_into().ok()?) as usize)
}

/// The GLSLC header whose code offset names the SPH at `sph`.
fn header(b: &[u8], sph: usize) -> Option<usize> {
    (4..=HEADER_MAX)
        .step_by(4)
        .filter_map(|back| sph.checked_sub(back))
        .find(|&h| {
            b.get(h..h + 4) == Some(&PROGRAM_MAGIC[..])
                && u32_at(b, h + CODE_OFFSET_AT) == Some(sph - h)
        })
}

/// The layout of the program whose SPH is at `sph`, as `xfb.json` holds it
/// without the program key: `{"raw", "buffers": [{"stride", "stream",
/// "attr", "first", "ncomp"}]}`. None when no header names the SPH, the
/// program captures nothing or the section is not the known version.
pub fn layout(b: &[u8], sph: usize) -> Option<Value> {
    let h = header(b, sph)?;
    let n = *b.get(h + BUFFERS_AT)? as usize;
    if n == 0 || n > MAX_BUFFERS {
        return None;
    }
    let raw = *b.get(h + RAW_AT)? != 0;
    let s = h.checked_add(u32_at(b, h + SECTION_AT)?)?;
    if u32_at(b, s)? != 1 {
        return None;
    }
    let mut p = s + 4;
    let strides: Vec<u16> = (0..n)
        .map(|i| {
            Some(u16::from_le_bytes(
                b.get(p + 2 * i..p + 2 * i + 2)?.try_into().ok()?,
            ))
        })
        .collect::<Option<_>>()?;
    p += 2 * n;
    let streams = b.get(p..p + n)?.to_vec();
    p += n;
    let counts = b.get(p..p + n)?.to_vec();
    p += n;
    let mut buffers = Vec::with_capacity(n);
    for i in 0..n {
        let c = counts[i] as usize;
        let attr: Vec<i8> = b.get(p..p + c)?.iter().map(|&x| x as i8).collect();
        let first = b.get(p + c..p + 2 * c)?.to_vec();
        let ncomp = b.get(p + 2 * c..p + 3 * c)?.to_vec();
        p += 3 * c;
        buffers.push(json!({"stride": strides[i], "stream": streams[i], "attr": attr, "first": first, "ncomp": ncomp}));
    }
    Some(json!({"raw": raw, "buffers": buffers}))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A header in the form the two capturing geometry programs found so
    /// far have: one raw buffer of stride 16 capturing generic 0 x, y, z and
    /// generic 1 x (hardware attributes 0x20, 0x21, 0x22, 0x24).
    fn sample() -> (Vec<u8>, usize) {
        let mut b = vec![0u8; 0x100 + 0x80];
        b[..4].copy_from_slice(&PROGRAM_MAGIC);
        b[CODE_OFFSET_AT..CODE_OFFSET_AT + 4].copy_from_slice(&0xf8u32.to_le_bytes());
        b[BUFFERS_AT] = 1;
        b[RAW_AT] = 1;
        b[SECTION_AT..SECTION_AT + 4].copy_from_slice(&0xe0u32.to_le_bytes());
        let sec = [
            1, 0, 0, 0, 16, 0, 0, 4, 0x20, 0x21, 0x22, 0x24, 0, 0, 0, 0, 1, 1, 1, 1,
        ];
        b[0xe0..0xe0 + sec.len()].copy_from_slice(&sec);
        (b, 0xf8)
    }

    #[test]
    fn the_layout_is_read_from_the_header_before_the_sph() {
        let (b, sph) = sample();
        assert_eq!(
            layout(&b, sph).unwrap(),
            json!({"raw": true, "buffers": [{"stride": 16, "stream": 0, "attr": [32, 33, 34, 36],
                "first": [0, 0, 0, 0], "ncomp": [1, 1, 1, 1]}]})
        );
    }

    #[test]
    fn a_program_that_captures_nothing_has_no_layout() {
        let (mut b, sph) = sample();
        assert!(layout(&b, sph - 16).is_none());
        b[BUFFERS_AT] = 0;
        assert!(layout(&b, sph).is_none());
        b[BUFFERS_AT] = 1;
        b[0xe0] = 2;
        assert!(layout(&b, sph).is_none());
    }
}

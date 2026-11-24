//! Transform feedback: the outputs a program captures, as SPIR-V `Xfb`
//! decorations ([`Options::xfb`](super::super::Options::xfb)).
//!
//! The NVN2 driver programs the capture from the program's GLSLC header
//! (SDK 22 driver, 0x619a84): per buffer `i`,
//! SET_STREAM_OUT_CONTROL_STREAM(i) is the stream,
//! SET_STREAM_OUT_CONTROL_COMPONENT_COUNT(i) the stride in words and
//! SET_STREAM_OUT_CONTROL_STRIDE(i) the stride in bytes, and
//! SET_STREAM_OUT_LAYOUT_SELECT(i, *) one byte per word of a captured
//! vertex: the attribute index (the attribute's byte address over 4) whose
//! value the word takes, 0xff for a word left as it is. [`Layout::from_glslc`]
//! builds those bytes as the driver does.
//!
//! The translation keeps every output as the program writes it, so the
//! next stage's interface does not change, and adds one capture-only
//! output per run of captured words (at most four, consecutive in the
//! buffer, none skipped): a float or vector at a location of its own after
//! the program's, decorated with `XfbBuffer`, `XfbStride` and the run's
//! byte `Offset` (and `Stream` past stream 0). Every store to a captured
//! attribute also stores the value to its capture words, so a vertex the
//! stage emits carries them with the same bits.

use std::collections::BTreeMap;

use super::super::Ctx;
use super::OUTPUT;
use crate::program::Stage;
use crate::spirv::op;

const DECO_LOCATION: u32 = 30;
const DECO_STREAM: u32 = 29;
const DECO_OFFSET: u32 = 35;
const DECO_XFB_BUFFER: u32 = 36;
const DECO_XFB_STRIDE: u32 = 37;
const CAP_TRANSFORM_FEEDBACK: u32 = 53;
const CAP_GEOMETRY_STREAMS: u32 = 54;
const MODE_XFB: u32 = 11;
/// A layout select byte for a word the capture leaves as it is.
pub const SKIP: u8 = 0xff;

/// The driver's attribute index of each GLSLC attribute code, from -2 on
/// (SDK 22 driver, the table at 0xbdb290: the low byte of a 16-bit entry
/// per code; codes -2 and -1 read the bytes before it, and codes past 63
/// the bytes after it, as the driver does). Codes -6 to -3 skip their
/// components. Read with `runtime/tools/nvn2/xfblayout.py table`.
const ATTR_INDEX: [u8; 130] = [
    0xf8, 0x00, 0x1c, 0xa0, 0xa4, 0xa8, 0xac, 0xba, 0x1b, 0xc0, 0xc4, 0xc8, 0xcc, 0xd0, 0xd4, 0xd8,
    0xdc, 0xe0, 0xe4, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xbf, 0x1a, 0x19, 0xe8, 0xff,
    0x18, 0xff, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x54,
    0x58, 0x5c, 0x60, 0x64, 0x68, 0x6c, 0x70, 0x74, 0x78, 0x7c, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94,
    0x98, 0x9c, 0x4c, 0x00, 0x4c, 0x00, 0x0c, 0x00, 0x8c, 0x00, 0xcc, 0x00, 0x01, 0x00, 0x05, 0x00,
    0x04, 0x00, 0x02, 0x00, 0x03, 0x00, 0x14, 0x01, 0x01, 0x00, 0x17, 0x00, 0x19, 0x01, 0x06, 0x00,
    0x06, 0x80, 0x06, 0x80, 0x06, 0x80, 0x06, 0x80, 0x06, 0x80, 0x06, 0x80, 0x06, 0x80, 0x06, 0x80,
    0x06, 0x80, 0x06, 0x80, 0x06, 0x80, 0x06, 0x80, 0x06, 0x00, 0x06, 0x80, 0x06, 0x00, 0x06, 0x00,
    0x06, 0x00,
];

/// One transform feedback buffer as the hardware is programmed for it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Buffer {
    /// Bytes per captured vertex.
    pub stride: u32,
    /// The vertex stream captured (geometry programs; 0 otherwise).
    pub stream: u32,
    /// Per word of a captured vertex, its attribute index or [`SKIP`]; a
    /// word past the list is skipped.
    pub select: Vec<u8>,
}

/// What a program captures: up to four buffers.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Layout {
    pub buffers: Vec<Buffer>,
}

/// One buffer of a GLSLC transform feedback section, as the scan records
/// it (`relift_switch_shaders::xfb`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GlslcBuffer {
    pub stride: u16,
    pub stream: u8,
    pub attr: Vec<i8>,
    pub first: Vec<u8>,
    pub ncomp: Vec<u8>,
}

impl Layout {
    /// The layout the driver programs for a GLSLC section: with `raw` the
    /// attribute bytes are the select bytes, else each entry gives one byte
    /// per component from its first, its code's attribute index plus the
    /// component ([`SKIP`] for codes -6 to -3).
    #[must_use]
    pub fn from_glslc(raw: bool, buffers: &[GlslcBuffer]) -> Layout {
        let buffers = buffers
            .iter()
            .map(|b| {
                let mut select = Vec::new();
                for (j, &a) in b.attr.iter().enumerate() {
                    if raw {
                        select.push(a as u8);
                        continue;
                    }
                    let c0 = b.first.get(j).copied().unwrap_or(0);
                    let end = c0.wrapping_add(b.ncomp.get(j).copied().unwrap_or(0));
                    let mut c = c0;
                    while c < end {
                        select.push(if (i32::from(a) + 6) as u32 >= 4 {
                            ATTR_INDEX[(i32::from(a) + 2) as usize].wrapping_add(c)
                        } else {
                            SKIP
                        });
                        c = c.wrapping_add(1);
                    }
                }
                Buffer {
                    stride: u32::from(b.stride),
                    stream: u32::from(b.stream),
                    select,
                }
            })
            .collect();
        Layout { buffers }
    }

    /// The captured words: (buffer, first word, attribute indices) per run
    /// of at most four consecutive captured words.
    #[must_use]
    pub fn runs(&self) -> Vec<(u32, u32, Vec<u8>)> {
        let mut out = Vec::new();
        for (i, b) in self.buffers.iter().enumerate() {
            let words = (b.stride / 4) as usize;
            let mut cur: Option<(u32, Vec<u8>)> = None;
            for w in 0..words {
                match b.select.get(w).copied().filter(|&s| s != SKIP) {
                    Some(s) => match &mut cur {
                        Some((_, run)) if run.len() < 4 => run.push(s),
                        _ => {
                            if let Some((at, run)) = cur.take() {
                                out.push((i as u32, at, run));
                            }
                            cur = Some((w as u32, vec![s]));
                        }
                    },
                    None => {
                        if let Some((at, run)) = cur.take() {
                            out.push((i as u32, at, run));
                        }
                    }
                }
            }
            if let Some((at, run)) = cur {
                out.push((i as u32, at, run));
            }
        }
        out
    }
}

/// The capture outputs of the program being translated.
#[derive(Default)]
pub(crate) struct State {
    pub layout: Option<Layout>,
    /// Per attribute index, the capture words it feeds: (variable,
    /// component or none for a scalar).
    words: BTreeMap<u8, Vec<(u32, Option<u32>)>>,
    /// The capture variables, with their buffer, word and width.
    vars: Vec<(u32, u32, u32, u32)>,
    declared: bool,
}

/// Whether the stage can be the last before the rasterizer.
fn captures(s: Stage) -> bool {
    matches!(s, Stage::Vertex | Stage::TessEval | Stage::Geometry)
}

impl Ctx {
    fn xfb_declare(&mut self) {
        if self.io.xfb.declared {
            return;
        }
        self.io.xfb.declared = true;
        let Some(layout) = self.io.xfb.layout.clone() else {
            return;
        };
        if !captures(self.stage) {
            return;
        }
        let f = self.f32t();
        for (buf, word, run) in layout.runs() {
            let n = run.len() as u32;
            let t = if n == 1 { f } else { self.m.t_vec(f, n) };
            let p = self.m.t_ptr(OUTPUT, t);
            let var = self.m.global(p, OUTPUT);
            self.globals.push(var);
            for (k, &a) in run.iter().enumerate() {
                let comp = (n > 1).then_some(k as u32);
                self.io.xfb.words.entry(a).or_default().push((var, comp));
            }
            self.io.xfb.vars.push((var, buf, word, n));
        }
    }

    /// Stores `bits` to the capture words of the attribute at byte address
    /// `off`, when the program captures it.
    pub(crate) fn xfb_store(&mut self, off: u32, bits: u32) {
        self.xfb_declare();
        let Some(words) = u8::try_from(off / 4)
            .ok()
            .and_then(|a| self.io.xfb.words.get(&a))
            .cloned()
        else {
            return;
        };
        let v = self.as_float(bits);
        let f = self.f32t();
        let pf = self.m.t_ptr(OUTPUT, f);
        for (var, comp) in words {
            let p = match comp {
                Some(c) => {
                    let k = self.m.c_u32(c);
                    self.m.emit(op::ACCESS_CHAIN, pf, &[var, k])
                }
                None => var,
            };
            self.m.emit_void(op::STORE, &[p, v]);
        }
    }
}

/// The values a vertex program's capture words start with, as its generic
/// outputs do (0, 0, 0, 1): (variable, constant) per capture variable.
pub(crate) fn vertex_defaults(c: &mut Ctx) -> Vec<(u32, u32)> {
    c.xfb_declare();
    let layout = c.io.xfb.layout.clone().unwrap_or_default();
    let runs = layout.runs();
    let vars = c.io.xfb.vars.clone();
    let (zero, one) = (c.m.c_f32(0), c.m.c_f32(0x3f80_0000));
    let f = c.f32t();
    let mut out = Vec::new();
    for ((var, _, _, n), (_, _, run)) in vars.into_iter().zip(runs) {
        let parts: Vec<u32> = run
            .iter()
            .map(|&a| {
                if is_generic(a) && a % 4 == 3 {
                    one
                } else {
                    zero
                }
            })
            .collect();
        let value = if n == 1 {
            parts[0]
        } else {
            let t = c.m.t_vec(f, n);
            c.m.c_composite(t, &parts)
        };
        out.push((var, value));
    }
    out
}

/// Whether attribute index `a` is a generic or fixed-function texture
/// coordinate output (a vector the translation starts at 0, 0, 0, 1).
fn is_generic(a: u8) -> bool {
    let off = 4 * u32::from(a);
    (0x80..0x280).contains(&off) || (0x300..0x3a0).contains(&off)
}

/// Decorates the capture outputs and enables the capture: their locations
/// follow the program's own outputs.
pub(crate) fn finish(c: &mut Ctx, entry: u32) {
    c.xfb_declare();
    if c.io.xfb.vars.is_empty() {
        return;
    }
    let layout = c.io.xfb.layout.clone().unwrap_or_default();
    let first = c.outputs.keys().next_back().map_or(0, |l| l + 1);
    for (loc, (var, buf, word, _)) in (first..).zip(c.io.xfb.vars.clone()) {
        let b = &layout.buffers[buf as usize];
        c.m.deco(var, DECO_LOCATION, &[loc]);
        c.m.deco(var, DECO_XFB_BUFFER, &[buf]);
        c.m.deco(var, DECO_XFB_STRIDE, &[b.stride]);
        c.m.deco(var, DECO_OFFSET, &[4 * word]);
        if b.stream != 0 && c.stage == Stage::Geometry {
            c.m.deco(var, DECO_STREAM, &[b.stream]);
            c.cap(CAP_GEOMETRY_STREAMS);
        }
    }
    c.cap(CAP_TRANSFORM_FEEDBACK);
    crate::spirv::inst(&mut c.m.modes, op::EXECUTION_MODE, &[entry, MODE_XFB]);
    c.iface.xfb = true;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn raw_entries_are_the_select_bytes() {
        let l = Layout::from_glslc(
            true,
            &[GlslcBuffer {
                stride: 24,
                stream: 0,
                attr: vec![0x20, 0x21, 0x22, 0x24, 0x28, 0x29],
                first: vec![0; 6],
                ncomp: vec![1; 6],
            }],
        );
        assert_eq!(l.buffers[0].select, [0x20, 0x21, 0x22, 0x24, 0x28, 0x29]);
        assert_eq!(
            l.runs(),
            [
                (0, 0, vec![0x20, 0x21, 0x22, 0x24]),
                (0, 4, vec![0x28, 0x29])
            ]
        );
    }

    #[test]
    fn coded_entries_go_through_the_driver_table() {
        let l = Layout::from_glslc(
            false,
            &[GlslcBuffer {
                stride: 32,
                stream: 1,
                attr: vec![0, -6, 32],
                first: vec![0, 0, 1],
                ncomp: vec![4, 2, 2],
            }],
        );
        assert_eq!(
            l.buffers[0].select,
            [0x1c, 0x1d, 0x1e, 0x1f, SKIP, SKIP, 0x21, 0x22]
        );
        assert_eq!(
            l.runs(),
            [
                (0, 0, vec![0x1c, 0x1d, 0x1e, 0x1f]),
                (0, 6, vec![0x21, 0x22])
            ]
        );
    }
}

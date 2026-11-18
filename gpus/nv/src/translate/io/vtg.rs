//! Tessellation control, tessellation evaluation and geometry stage I/O.
//!
//! These stages see whole primitives. On SM70+ a per-vertex input is read
//! with `ALD a[off], Rv`, where `Rv` is a vertex handle that
//! `ISBERD [idx]` (map access) gives for vertex `idx` of the warp's
//! primitives, `idx = (SR_INVOCATION_INFO byte 0) * (byte 2) + vertex`:
//! byte 2 is the number of vertices in the input primitive and byte 0 the
//! primitive's slot in the warp (Mesa NAK's `nak_nir_lower_vtg_io.c`, MIT,
//! Collabora). The translation runs one primitive per invocation: byte 0
//! reads 0, ISBERD gives `idx` back, and a handle names vertex `Rv` of the
//! input arrays. Handles past the primitive (the hardware's programs load
//! `0x80000000` for them) read 0.
//!
//! Tessellation control (SPH type 2): `ALD.O a[off], Rv` reads per-vertex
//! output `Rv + SR_INVOCATION_ID - SR_LANEID` (the hardware indexes outputs
//! by lane), `AST a[off]` writes the invocation's own output vertex,
//! `AST.P`/`ALD.O.P` the patch outputs: `a.P[0x0..0x10]` the outer and
//! `a.P[0x10..0x18]` the inner tessellation levels, `a.P[0x20 + 16 n]`
//! patch location `n`. The output vertex count is the SPH's threads per
//! input primitive. A program that declares none is a pass-through: each
//! invocation copies its input vertex (the position and the generic
//! locations of [`Pipeline::tcs_passthrough`]) and the tessellation levels
//! start from specialization constants [`TESS_LEVEL_SPEC`] onwards (outer
//! 0 to 3, inner 0 and 1, 1.0 unless set), as the guest's default levels.
//!
//! Tessellation evaluation (SPH type 3): `ALD.O a[0x2f0]` reads TessCoord
//! (x and y; the program computes the third barycentric itself), `ALD.P`
//! the patch inputs. The domain, spacing and output primitives are not in
//! the SPH (NVIDIA takes them from pipeline state) and come from
//! [`Pipeline`].
//!
//! Geometry (SPH type 4): the input primitive comes from [`Pipeline`];
//! the output topology, the maximum output vertex count and the
//! invocation count from the SPH. `OUT.EMIT` and `OUT.CUT` take and give an
//! output vertex handle the stores name; the translation keeps the
//! handle as it is and emits or ends the primitive of the stream the third
//! operand names. `OUT.FINAL` (the end of the output) has nothing to do.
//! A program may set an ATEXIT handler that only runs `OUT.FINAL`; that
//! setup is accepted and dropped.

use super::{INPUT, OUTPUT, Res, Slot, bad, location};
use crate::program::{Sph, Stage};
use crate::spirv::op;
use crate::translate::Ctx;
use crate::{Inst, Operand};
use std::collections::{BTreeMap, BTreeSet};

/// First specialization constant id of the default tessellation levels a
/// pass-through tessellation control program writes.
pub const TESS_LEVEL_SPEC: u32 = 0x200;

const B_TESS_LEVEL_OUTER: u32 = 11;
const B_TESS_LEVEL_INNER: u32 = 12;
const B_TESS_COORD: u32 = 13;
const B_PATCH_VERTICES: u32 = 14;
const B_INVOCATION_ID: u32 = 8;
const B_POSITION: u32 = 0;
const B_POINT_SIZE: u32 = 1;
const B_CLIP_DISTANCE: u32 = 3;
const B_PRIMITIVE_ID: u32 = 7;
const DECO_PATCH: u32 = 15;
const CAP_GEOMETRY: u32 = 2;
const CAP_TESSELLATION: u32 = 3;
const CAP_CLIP_DISTANCE: u32 = 32;
const CAP_GEOMETRY_STREAMS: u32 = 54;
const OP_EMIT_VERTEX: u16 = 218;
const OP_END_PRIMITIVE: u16 = 219;
const OP_EMIT_STREAM_VERTEX: u16 = 220;
const OP_END_STREAM_PRIMITIVE: u16 = 221;
const SCOPE_WORKGROUP: u32 = 2;
const SCOPE_INVOCATION: u32 = 4;
/// Length of the per-vertex input arrays of the tessellation stages
/// (`maxTessellationPatchSize` of every Vulkan device that has them).
const MAX_PATCH_VERTICES: u32 = 32;

/// Tessellation domain.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TessDomain {
    Isolines,
    #[default]
    Triangles,
    Quads,
}

/// Tessellation spacing.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TessSpacing {
    #[default]
    Equal,
    FractionalOdd,
    FractionalEven,
}

/// What the tessellator puts out, as NVIDIA's pipeline state names it;
/// the winding is the one SPIR-V sees.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TessOutput {
    Points,
    Lines,
    TrianglesCw,
    #[default]
    TrianglesCcw,
}

/// The geometry stage's input primitive.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum GsInput {
    Points,
    Lines,
    LinesAdjacency,
    #[default]
    Triangles,
    TrianglesAdjacency,
}

impl GsInput {
    pub fn vertices(self) -> u32 {
        match self {
            GsInput::Points => 1,
            GsInput::Lines => 2,
            GsInput::LinesAdjacency => 4,
            GsInput::Triangles => 3,
            GsInput::TrianglesAdjacency => 6,
        }
    }

    fn mode(self) -> u32 {
        match self {
            GsInput::Points => 19,
            GsInput::Lines => 20,
            GsInput::LinesAdjacency => 21,
            GsInput::Triangles => 22,
            GsInput::TrianglesAdjacency => 23,
        }
    }
}

/// Pipeline state the tessellation, geometry, task and mesh programs
/// depend on that their SPH does not carry.
#[derive(Clone, Debug)]
pub struct Pipeline {
    pub tess_domain: TessDomain,
    pub tess_spacing: TessSpacing,
    pub tess_output: TessOutput,
    /// Control points per input patch.
    pub patch_vertices: u32,
    pub gs_input: GsInput,
    /// Generic locations (bit `n` for location `n`) a pass-through
    /// tessellation control program copies: the ones the tessellation
    /// evaluation program reads.
    pub tcs_passthrough: u64,
    pub mesh: super::mesh::MeshPipeline,
}

impl Default for Pipeline {
    fn default() -> Pipeline {
        Pipeline {
            tess_domain: TessDomain::default(),
            tess_spacing: TessSpacing::default(),
            tess_output: TessOutput::default(),
            patch_vertices: 3,
            gs_input: GsInput::default(),
            tcs_passthrough: 0,
            mesh: super::mesh::MeshPipeline::default(),
        }
    }
}

/// A per-vertex attribute.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Key {
    Generic(u32),
    Position,
    PointSize,
    Clip,
}

#[derive(Default)]
pub(crate) struct State {
    pub pipe: Pipeline,
    pv_in: BTreeMap<Key, u32>,
    pv_out: BTreeMap<Key, u32>,
    patch: BTreeMap<u32, u32>,
    /// Program counters of the LEPC instructions that only set an ATEXIT
    /// handler.
    pub atexit_lepc: BTreeSet<u64>,
    /// Program counters of the `S2R SR_LANEID` whose value only names the
    /// vertex of TessCoord loads; they read 0.
    pub lane_free: BTreeSet<u64>,
    prim_out: Option<u32>,
}

pub(crate) fn is_vtg(s: Stage) -> bool {
    matches!(s, Stage::TessControl | Stage::TessEval | Stage::Geometry)
}

/// The attribute and component of a per-vertex address.
fn pv_key(off: u32) -> Option<(Key, u32)> {
    Some(match off {
        0x6c => (Key::PointSize, 0),
        0x70..0x80 => (Key::Position, (off - 0x70) / 4),
        0x80..0x280 | 0x300..0x3a0 => (Key::Generic(location(off)), (off % 16) / 4),
        0x2c0..0x2e0 => (Key::Clip, (off - 0x2c0) / 4),
        _ => return None,
    })
}

fn width(i: &Inst) -> u32 {
    if i.has("64") {
        2
    } else if i.has("96") {
        3
    } else if i.has("128") {
        4
    } else {
        1
    }
}

fn attr(o: &Operand) -> Res<u32> {
    match o {
        Operand::Attr {
            index: None,
            offset,
            ..
        } => Ok(*offset),
        o => bad(format!("attribute operand {o}")),
    }
}

impl Ctx {
    fn sph(&self) -> Sph {
        Sph {
            words: self.sph_words,
        }
    }

    /// Output vertices of a tessellation control program.
    fn tcs_out_vertices(&self) -> u32 {
        match self.sph().threads_per_input_primitive() {
            0 => self.io.vtg.pipe.patch_vertices.max(1),
            n => n,
        }
    }

    fn in_len(&self) -> u32 {
        match self.stage {
            Stage::Geometry => self.io.vtg.pipe.gs_input.vertices(),
            _ => MAX_PATCH_VERTICES,
        }
    }

    fn i32_in(&mut self, b: u32) -> u32 {
        let i = self.m.t_i32();
        self.iface.builtins.insert(b);
        let var = self.builtin(b, INPUT, i);
        let v = self.m.emit(op::LOAD, i, &[var]);
        let u = self.u32t();
        self.m.emit(op::BITCAST, u, &[v])
    }

    /// Vertices in the input primitive.
    fn in_count(&mut self) -> u32 {
        match self.stage {
            Stage::Geometry => {
                let n = self.io.vtg.pipe.gs_input.vertices();
                self.m.c_u32(n)
            }
            _ => self.i32_in(B_PATCH_VERTICES),
        }
    }

    fn clip_count(&self, class: u32) -> u32 {
        let map = if class == INPUT {
            (self.sph_words[10] >> 16) & 0xff
        } else {
            self.sph_words[18] & 0xff
        };
        32 - map.leading_zeros()
    }

    fn pv_var(&mut self, k: Key, class: u32) -> Res<u32> {
        let map = if class == INPUT {
            &self.io.vtg.pv_in
        } else {
            &self.io.vtg.pv_out
        };
        if let Some(&v) = map.get(&k) {
            return Ok(v);
        }
        let len = if class == INPUT {
            self.in_len()
        } else {
            self.tcs_out_vertices()
        };
        let f = self.f32t();
        let elem = match k {
            Key::Generic(_) | Key::Position => self.m.t_vec(f, 4),
            Key::PointSize => f,
            Key::Clip => {
                let n = self.clip_count(class);
                if n == 0 {
                    return bad("clip distance outside the SPH");
                }
                self.cap(CAP_CLIP_DISTANCE);
                self.m.t_array(f, n)
            }
        };
        let arr = self.m.t_array(elem, len);
        let p = self.m.t_ptr(class, arr);
        let var = self.m.global(p, class);
        match k {
            Key::Generic(loc) => {
                self.m.deco(var, 30, &[loc]);
                if class == INPUT {
                    self.iface.inputs.entry(loc).or_insert(None);
                } else {
                    self.iface.outputs.insert(loc);
                }
            }
            Key::Position | Key::PointSize | Key::Clip => {
                let b = match k {
                    Key::Position => B_POSITION,
                    Key::PointSize => B_POINT_SIZE,
                    _ => B_CLIP_DISTANCE,
                };
                self.m.deco(var, 11, &[b]);
                self.iface.builtins.insert(b);
            }
        }
        self.globals.push(var);
        if class == INPUT {
            self.io.vtg.pv_in.insert(k, var);
        } else {
            self.io.vtg.pv_out.insert(k, var);
        }
        Ok(var)
    }

    /// A pointer to component `sub` of attribute `k` of vertex `idx`.
    fn pv_ptr(&mut self, k: Key, class: u32, idx: u32, sub: u32) -> Res<u32> {
        let var = self.pv_var(k, class)?;
        let f = self.f32t();
        let p = self.m.t_ptr(class, f);
        Ok(match k {
            Key::PointSize => self.m.emit(op::ACCESS_CHAIN, p, &[var, idx]),
            _ => {
                let s = self.m.c_u32(sub);
                self.m.emit(op::ACCESS_CHAIN, p, &[var, idx, s])
            }
        })
    }

    /// Reads a word of vertex `idx` when `idx < count`, else 0.
    fn pv_read(&mut self, off: u32, class: u32, idx: u32, count: u32) -> Res<u32> {
        let Some((k, sub)) = pv_key(off) else {
            return bad(format!("per-vertex attribute {off:#x}"));
        };
        if k == Key::Clip && sub >= self.clip_count(class) {
            return bad(format!("clip distance {sub} outside the SPH"));
        }
        let b = self.boolt();
        let u = self.u32t();
        let inb = self.m.emit(op::U_LESS_THAN, b, &[idx, count]);
        let zero = self.m.c_u32(0);
        let at = self.m.emit(op::SELECT, u, &[inb, idx, zero]);
        let p = self.pv_ptr(k, class, at, sub)?;
        let f = self.f32t();
        let v = self.m.emit(op::LOAD, f, &[p]);
        let bits = self.as_bits(v);
        Ok(self.m.emit(op::SELECT, u, &[inb, bits, zero]))
    }

    fn patch_var(&mut self, loc: u32, class: u32) -> u32 {
        if let Some(&v) = self.io.vtg.patch.get(&loc) {
            return v;
        }
        let f = self.f32t();
        let v4 = self.m.t_vec(f, 4);
        let p = self.m.t_ptr(class, v4);
        let var = self.m.global(p, class);
        self.m.deco(var, 30, &[loc]);
        self.m.deco(var, DECO_PATCH, &[]);
        self.globals.push(var);
        self.io.vtg.patch.insert(loc, var);
        if class == INPUT {
            self.iface.patch_inputs.insert(loc);
        } else {
            self.iface.patch_outputs.insert(loc);
        }
        var
    }

    fn level_var(&mut self, b: u32, class: u32) -> u32 {
        if let Some(&v) = self.builtins.get(&b) {
            return v;
        }
        let f = self.f32t();
        let n = if b == B_TESS_LEVEL_OUTER { 4 } else { 2 };
        let arr = self.m.t_array(f, n);
        let var = self.builtin(b, class, arr);
        self.m.deco(var, DECO_PATCH, &[]);
        self.iface.builtins.insert(b);
        var
    }

    /// A word of the patch: the tessellation levels or a patch location.
    fn patch_slot(&mut self, off: u32, class: u32) -> Res<Slot> {
        Ok(match off {
            0x0..0x18 => {
                let (b, c) = if off < 0x10 {
                    (B_TESS_LEVEL_OUTER, off / 4)
                } else {
                    (B_TESS_LEVEL_INNER, (off - 0x10) / 4)
                };
                let var = self.level_var(b, class);
                Slot::Float(self.component_ptr(var, class, c), None)
            }
            0x20..0x220 => {
                let var = self.patch_var((off - 0x20) / 16, class);
                Slot::Float(self.component_ptr(var, class, (off % 16) / 4), None)
            }
            _ => return bad(format!("patch attribute {off:#x}")),
        })
    }

    fn tess_coord(&mut self, off: u32) -> Res<u32> {
        if self.stage != Stage::TessEval {
            return bad("TessCoord outside a tessellation evaluation shader");
        }
        let f = self.f32t();
        let v3 = self.m.t_vec(f, 3);
        self.iface.builtins.insert(B_TESS_COORD);
        let var = self.builtin(B_TESS_COORD, INPUT, v3);
        let p = self.component_ptr(var, INPUT, (off - 0x2f0) / 4);
        let v = self.m.emit(op::LOAD, f, &[p]);
        Ok(self.as_bits(v))
    }

    /// The geometry stage's PrimitiveId output, apart from its input of
    /// the same built-in.
    pub(super) fn gs_primitive_out(&mut self) -> u32 {
        if let Some(v) = self.io.vtg.prim_out {
            return v;
        }
        self.cap(CAP_GEOMETRY);
        let i = self.m.t_i32();
        let p = self.m.t_ptr(OUTPUT, i);
        let var = self.m.global(p, OUTPUT);
        self.m.deco(var, 11, &[B_PRIMITIVE_ID]);
        self.iface.builtins.insert(B_PRIMITIVE_ID);
        self.globals.push(var);
        self.io.vtg.prim_out = Some(var);
        var
    }

    /// The special registers of these stages.
    pub(crate) fn vtg_sysval(&mut self, name: &str) -> Option<u32> {
        if !is_vtg(self.stage) {
            return None;
        }
        Some(match name {
            "SR_INVOCATION_ID" if self.stage != Stage::TessEval => self.i32_in(B_INVOCATION_ID),
            "SR_INVOCATION_INFO" => {
                let n = self.in_count();
                let u = self.u32t();
                let ff = self.m.c_u32(0xff);
                let n = self.m.emit(op::BITWISE_AND, u, &[n, ff]);
                let s = self.m.c_u32(16);
                self.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[n, s])
            }
            _ => return None,
        })
    }

    /// The pass-through work of a tessellation control program that
    /// declares no output vertices.
    pub(crate) fn tcs_prologue(&mut self) -> Res<()> {
        if self.stage != Stage::TessControl || self.sph().threads_per_input_primitive() != 0 {
            return Ok(());
        }
        let inv = self.i32_in(B_INVOCATION_ID);
        let mask = self.io.vtg.pipe.tcs_passthrough;
        let mut keys = vec![Key::Position];
        keys.extend((0..64).filter(|n| mask & (1 << n) != 0).map(Key::Generic));
        let f = self.f32t();
        let v4 = self.m.t_vec(f, 4);
        for k in keys {
            let src = self.pv_var(k, INPUT)?;
            let dst = self.pv_var(k, OUTPUT)?;
            let pi = self.m.t_ptr(INPUT, v4);
            let po = self.m.t_ptr(OUTPUT, v4);
            let a = self.m.emit(op::ACCESS_CHAIN, pi, &[src, inv]);
            let v = self.m.emit(op::LOAD, v4, &[a]);
            let b = self.m.emit(op::ACCESS_CHAIN, po, &[dst, inv]);
            self.m.emit_void(op::STORE, &[b, v]);
        }
        for k in 0..6 {
            let off = if k < 4 { 4 * k } else { 0x10 + 4 * (k - 4) };
            let s = self.patch_slot(off, OUTPUT)?;
            let bits = self.m.spec_u32(0x3f80_0000, TESS_LEVEL_SPEC + k);
            self.store_slot(&s, bits);
        }
        Ok(())
    }
}

fn vertex_operand(c: &mut Ctx, o: Option<&Operand>) -> Res<u32> {
    match o {
        None | Some(Operand::Reg(255, _)) => Ok(c.m.c_u32(0)),
        Some(Operand::Reg(n, m)) if !m.neg && !m.abs && !m.not => Ok(c.load_reg(*n)),
        Some(o) => bad(format!("vertex operand {o}")),
    }
}

pub(super) fn ald(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["O", "P", "64", "96", "128"])?;
    if !(2..=3).contains(&i.ops.len()) {
        return bad(format!("ALD form {i}"));
    }
    let d = Ctx::dst_reg(&i.ops[0])?;
    let off = attr(&i.ops[1])?;
    let vtx = vertex_operand(c, i.ops.get(2))?;
    let out = i.has("O");
    let patch = i.has("P");
    let mut vals = Vec::new();
    for k in 0..width(i) {
        let a = off + 4 * k;
        let v = match (c.stage, out, patch) {
            (Stage::TessEval, false, true) => {
                let s = c.patch_slot(a, INPUT)?;
                c.load_slot(&s)
            }
            (Stage::TessControl, true, true) => {
                let s = c.patch_slot(a, OUTPUT)?;
                c.load_slot(&s)
            }
            (Stage::TessEval, _, false) if (0x2f0..0x2f8).contains(&a) => c.tess_coord(a)?,
            (Stage::TessControl, true, false) => {
                let lane = c.lane_id();
                let inv = c.i32_in(B_INVOCATION_ID);
                let u = c.u32t();
                let base = c.m.emit(op::I_SUB, u, &[lane, inv]);
                let idx = c.m.emit(op::I_SUB, u, &[vtx, base]);
                let n = c.tcs_out_vertices();
                let n = c.m.c_u32(n);
                c.pv_read(a, OUTPUT, idx, n)?
            }
            (_, false, false) if a == 0x60 => {
                if c.stage == Stage::Geometry {
                    c.cap(CAP_GEOMETRY);
                }
                let s = c.int_slot(B_PRIMITIVE_ID, INPUT);
                c.load_slot(&s)
            }
            (_, false, false) => {
                let n = c.in_count();
                c.pv_read(a, INPUT, vtx, n)?
            }
            _ => return bad(format!("{i} in a {:?} shader", c.stage)),
        };
        vals.push(v);
    }
    for (k, v) in vals.into_iter().enumerate() {
        c.store_reg(d.wrapping_add(k as u8), v);
    }
    Ok(())
}

pub(super) fn ast(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["P", "64", "96", "128"])?;
    if !(2..=3).contains(&i.ops.len()) {
        return bad(format!("AST form {i}"));
    }
    let off = attr(&i.ops[0])?;
    let s = Ctx::dst_reg(&i.ops[1])?;
    let patch = i.has("P");
    for k in 0..width(i) {
        let a = off + 4 * k;
        let u = c.load_reg(s.wrapping_add(k as u8));
        match (c.stage, patch) {
            (Stage::TessControl, true) => {
                let slot = c.patch_slot(a, OUTPUT)?;
                c.store_slot(&slot, u);
            }
            (Stage::TessControl, false) => {
                let Some((key, sub)) = pv_key(a) else {
                    return bad(format!("per-vertex output {a:#x}"));
                };
                if key == Key::Clip && sub >= c.clip_count(OUTPUT) {
                    return bad(format!("clip distance {sub} outside the SPH"));
                }
                let inv = c.i32_in(B_INVOCATION_ID);
                let p = c.pv_ptr(key, OUTPUT, inv, sub)?;
                let v = c.as_float(u);
                c.m.emit_void(op::STORE, &[p, v]);
            }
            (Stage::TessEval | Stage::Geometry, false) => {
                let slot = c.output_slot(a)?;
                c.store_slot(&slot, u);
            }
            _ => return bad(format!("{i} in a {:?} shader", c.stage)),
        }
    }
    Ok(())
}

/// ISBERD with map access: the vertex handle of primitive vertex `idx`.
pub(super) fn isberd(c: &mut Ctx, i: &Inst) -> Res<()> {
    let w = c.fields;
    let b = w.isbe;
    if b.size != 0 || b.class != 0 || b.skew || b.out {
        let mut name = vec![i.mnemonic];
        name.extend(&i.mods);
        return bad(format!("stage buffer {}", name.join(".")));
    }
    if !is_vtg(c.stage) {
        return bad("ISBERD outside a tessellation or geometry stage");
    }
    let d = Ctx::dst_reg(&i.ops[0])?;
    let r = c.load_reg(w.ra);
    let u = c.u32t();
    let k = c.m.c_u32(b.offset);
    let v = c.m.emit(op::I_ADD, u, &[r, k]);
    c.store_reg(d, v);
    Ok(())
}

/// OUT: geometry vertex emission.
pub(super) fn out(c: &mut Ctx, i: &Inst) -> Res<()> {
    if c.stage != Stage::Geometry {
        return bad("OUT outside a geometry shader");
    }
    let fin = i.has("FINAL");
    if fin {
        return Ok(());
    }
    if i.ops.len() != 3 {
        return bad(format!("OUT form {i}"));
    }
    let stream = match &i.ops[2] {
        Operand::Reg(255, _) => 0,
        Operand::Int(v) if (0..4).contains(v) => *v as u32,
        Operand::Hex(v) if *v < 4 => *v as u32,
        o => return bad(format!("OUT stream {o}")),
    };
    let h = match &i.ops[1] {
        Operand::Reg(n, _) => c.load_reg(*n),
        o => return bad(format!("OUT handle {o}")),
    };
    let emit = i.has("EMIT") || i.has("EMIT_THEN_CUT");
    let cut = i.has("CUT") || i.has("EMIT_THEN_CUT");
    if stream == 0 {
        if emit {
            c.m.emit_void(OP_EMIT_VERTEX, &[]);
        }
        if cut {
            c.m.emit_void(OP_END_PRIMITIVE, &[]);
        }
    } else {
        c.cap(CAP_GEOMETRY_STREAMS);
        let s = c.m.c_u32(stream);
        if emit {
            c.m.emit_void(OP_EMIT_STREAM_VERTEX, &[s]);
        }
        if cut {
            c.m.emit_void(OP_END_STREAM_PRIMITIVE, &[s]);
        }
    }
    c.store_dst(&i.ops[0], h)
}

/// LEPC that only builds an ATEXIT handler address.
pub(super) fn lepc(c: &mut Ctx, i: &Inst) -> Res<()> {
    if !c.io.vtg.atexit_lepc.contains(&c.pc) {
        return bad("LEPC");
    }
    let lo = c.m.c_u32(c.pc as u32);
    let hi = c.m.c_u32(0);
    c.store_pair(&i.ops[0], lo, hi)
}

/// A tessellation control barrier.
pub(crate) fn tcs_barrier(c: &mut Ctx) {
    let exec = c.m.c_u32(SCOPE_WORKGROUP);
    let mem = c.m.c_u32(SCOPE_INVOCATION);
    let sem = c.m.c_u32(0);
    c.m.emit_void(op::CONTROL_BARRIER, &[exec, mem, sem]);
}

fn reg_of(o: Option<&Operand>) -> Option<u8> {
    match o {
        Some(Operand::Reg(n, _)) => Some(*n),
        _ => None,
    }
}

fn imm_of(o: Option<&Operand>) -> Option<u64> {
    match o {
        Some(Operand::Int(v)) => Some(*v as u64),
        Some(Operand::Hex(v)) => Some(*v),
        _ => None,
    }
}

/// Finds the ATEXIT setups (`LEPC Ra; MOV Rb, off; IADD3 Ra, P, Ra, Rb;
/// IADD3.X; BMOV.64 ATEXIT_PC, Ra`) whose handler at `LEPC + off` only
/// runs OUT.FINAL before it clears ATEXIT_PC and exits. Any other ATEXIT
/// write is refused.
pub(crate) fn prescan(c: &mut Ctx, insts: &[Inst]) -> Res<()> {
    for (k, i) in insts.iter().enumerate() {
        if i.mnemonic != "BMOV"
            || !matches!(i.ops.first(), Some(Operand::Sym(s)) if s == "ATEXIT_PC")
        {
            continue;
        }
        if imm_of(i.ops.get(1)) == Some(0) {
            continue;
        }
        let ok = (|| {
            let ra = reg_of(i.ops.get(1))?;
            let at = k.checked_sub(4)?;
            let (l, m, a) = (&insts[at], &insts[at + 1], &insts[at + 2]);
            if l.mnemonic != "LEPC" || reg_of(l.ops.first()) != Some(ra) || l.guard.is_some() {
                return None;
            }
            let rb = reg_of(m.ops.first())?;
            if m.mnemonic != "MOV" || m.guard.is_some() || a.mnemonic != "IADD3" {
                return None;
            }
            let off = imm_of(m.ops.get(1))?;
            let srcs: Vec<Option<u8>> = a.ops.iter().map(|o| reg_of(Some(o))).collect();
            if srcs.first() != Some(&Some(ra)) || !srcs.contains(&Some(rb)) {
                return None;
            }
            let h = (16 * at as u64 + off) / 16;
            let mut j = h as usize;
            loop {
                let x = insts.get(j)?;
                match x.mnemonic {
                    "OUT" if x.has("FINAL") && x.guard.is_none() => {}
                    "BMOV" | "NOP" => {}
                    "EXIT" if x.guard.is_none() => break,
                    _ => return None,
                }
                j += 1;
            }
            Some(16 * at as u64)
        })();
        match ok {
            Some(pc) => {
                c.io.vtg.atexit_lepc.insert(pc);
            }
            None => return bad("ATEXIT handler"),
        }
    }
    Ok(())
}

/// The execution modes and capabilities of these stages.
pub(crate) fn modes(c: &mut Ctx, main: u32) -> Res<()> {
    let m = |c: &mut Ctx, args: &[u32]| {
        let mut a = vec![main];
        a.extend_from_slice(args);
        crate::spirv::inst(&mut c.m.modes, op::EXECUTION_MODE, &a);
    };
    let pipe = c.io.vtg.pipe.clone();
    match c.stage {
        Stage::TessControl => {
            c.cap(CAP_TESSELLATION);
            let n = c.tcs_out_vertices();
            c.iface.output_vertices = n;
            m(c, &[26, n]);
        }
        Stage::TessEval => {
            c.cap(CAP_TESSELLATION);
            let d = match pipe.tess_domain {
                TessDomain::Triangles => 22,
                TessDomain::Quads => 24,
                TessDomain::Isolines => 25,
            };
            m(c, &[d]);
            let s = match pipe.tess_spacing {
                TessSpacing::Equal => 1,
                TessSpacing::FractionalEven => 2,
                TessSpacing::FractionalOdd => 3,
            };
            m(c, &[s]);
            match pipe.tess_output {
                TessOutput::Points => {
                    m(c, &[10]);
                    m(c, &[5]);
                }
                TessOutput::Lines => {}
                TessOutput::TrianglesCw => m(c, &[4]),
                TessOutput::TrianglesCcw => m(c, &[5]),
            }
        }
        Stage::Geometry => {
            c.cap(CAP_GEOMETRY);
            let sph = c.sph();
            m(c, &[0, sph.threads_per_input_primitive().max(1)]);
            m(c, &[pipe.gs_input.mode()]);
            let n = sph.max_output_vertices().max(1);
            c.iface.output_vertices = n;
            m(c, &[26, n]);
            let t = match sph.output_topology() {
                1 => 27,
                6 => 28,
                7 => 29,
                t => return bad(format!("geometry output topology {t}")),
            };
            m(c, &[t]);
        }
        _ => {}
    }
    Ok(())
}

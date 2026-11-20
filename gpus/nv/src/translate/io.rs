//! Stage inputs and outputs: ALD, AST, IPA and the fragment outputs the
//! SPH names.
//!
//! Attribute addresses follow the SM70+ attribute map (Mesa NAK's
//! `nak_attr`, MIT): `a[0x80 + 16 n]` is generic location `n`, `a[0x70]`
//! the position (FragCoord in a fragment shader), `a[0x60]`, `a[0x64]`,
//! `a[0x68]` and `a[0x6c]` the primitive id, layer, viewport index and point
//! size, `a[0x2c0 + 4 n]` clip distance `n`, `a[0x2e0]` the point coordinate,
//! `a[0x2f8]` and `a[0x2fc]` the instance and vertex ids, `a[0x300 + 16 t]`
//! fixed-function texture coordinate `t` (location 32 + `t`), `a[0x3fc]`
//! front facing (all ones when true). `a[0x5c]` is the
//! SM86 variable-rate shading table index; a store to it only lowers the
//! shading rate, so it is dropped and every fragment is shaded.
//!
//! A fragment input's interpolation comes from the SPH input map (two bits
//! per component from bit 192: constant, perspective or screen linear). On
//! SM70+ IPA of a perspective attribute gives the value divided by the
//! clip-space w, and the program multiplies by the reciprocal of `a[0x7c]`
//! (FragCoord.w) itself; the translation reads the perspective-correct value
//! and multiplies FragCoord.w back in. CENTROID decorates the input, or uses
//! InterpolateAtCentroid when the program also reads it at the center.
//!
//! ALD and AST in the vertex and fragment stages name one vertex, so the
//! vertex register they may carry is ignored; the tessellation and
//! geometry stages are in [`vtg`].
//!
//! The instance id `a[0x2f8]` counts from 0 in each draw, without the base
//! instance, and the vertex id `a[0x2fc]` includes the base vertex: NVK
//! (Mesa, MIT) programs SET_GLOBAL_BASE_VERTEX_INDEX and
//! SET_GLOBAL_BASE_INSTANCE_INDEX with the draw's bases, reads Vulkan's
//! VertexIndex as the vertex id and builds InstanceIndex as the instance id
//! plus the base instance (NAK sets neither `vertex_id_zero_based` nor
//! `instance_id_includes_base_index`). So the instance id is InstanceIndex
//! minus BaseInstance (DrawParameters: the device needs
//! shaderDrawParameters) and the vertex id is VertexIndex.
//!
//! Fragment outputs (SPH word 18, four bits per render target) leave in
//! registers from R0: four per target that has any component, then the
//! sample mask and depth when SPH word 19 bit 0 or bit 1 asks for either.
//! The registers hold the bits the target's format takes: floats for float
//! and normalized formats, integers for integer formats. A Vulkan fragment
//! output must have the numeric type of its attachment, so each target's
//! output is a vector of the kind `Options::color_kinds` names for it
//! (float when it names none) and gets the register bits unchanged.

use super::mem::tuple_reg;
use super::{INPUT, Interp, Kind, OUTPUT, Res, bad};
use crate::program::Stage;
use crate::spirv::{glsl, op};
use crate::{Inst, Operand};
use std::collections::BTreeMap;

use super::Ctx;

pub(crate) mod mesh;
pub(crate) mod vtg;

const B_POSITION: u32 = 0;
const B_POINT_SIZE: u32 = 1;
const B_CLIP_DISTANCE: u32 = 3;
const B_PRIMITIVE_ID: u32 = 7;
const B_LAYER: u32 = 9;
const B_VIEWPORT_INDEX: u32 = 10;
const B_FRAG_COORD: u32 = 15;
const B_POINT_COORD: u32 = 16;
const B_FRONT_FACING: u32 = 17;
const B_SAMPLE_MASK: u32 = 20;
const B_FRAG_DEPTH: u32 = 22;
const B_VERTEX_INDEX: u32 = 42;
const B_INSTANCE_INDEX: u32 = 43;
const B_BASE_INSTANCE: u32 = 4425;

/// Location of the first fixed-function texture coordinate, after the 32
/// generic attributes; its SPH input map starts at bit 480.
const TEXCOORD_LOCATION: u32 = 32;

const CAP_GEOMETRY: u32 = 2;
const CAP_CLIP_DISTANCE: u32 = 32;
const CAP_INTERPOLATION_FUNCTION: u32 = 52;
const CAP_MULTI_VIEWPORT: u32 = 57;
const CAP_MESH_SHADING: u32 = 5283;
const DECO_PER_PRIMITIVE: u32 = 5271;
const CAP_VIEWPORT_INDEX_LAYER: u32 = 5254;
const CAP_DEMOTE: u32 = 5379;
const CAP_DRAW_PARAMETERS: u32 = 4427;
const OP_DEMOTE: u16 = 5380;
const PUSH_CONSTANT: u32 = 9;

/// Specialization constant id of the fragment alpha test's compare function
/// (an `NVNalphaFunc`, 1 never to 8 always; 8 when unset, which compiles the
/// test away). The runtime sets it from the bound color state.
pub const ALPHA_FUNC_SPEC: u32 = 0x300;

/// Byte offset in the fixed-state push constant block of the point size a
/// vertex program that does not write one gets.
pub const PUSH_POINT_SIZE: u32 = 0;

/// Byte offset in the fixed-state push constant block of the alpha test's
/// reference value.
pub const PUSH_ALPHA_REF: u32 = 4;

/// What the stage I/O needs to know about the whole program.
#[derive(Default)]
pub(crate) struct State {
    /// The program has a store to the point size attribute.
    writes_point_size: bool,
    /// The fixed-state push constant block variable, once declared.
    push_block: Option<u32>,
    /// Per fragment input location: (read at the center, read at the
    /// centroid).
    sites: BTreeMap<u32, (bool, bool)>,
    centroid: BTreeMap<u32, bool>,
    /// Numeric kind of each render target's format (Options::color_kinds).
    pub(crate) color_kinds: BTreeMap<u32, Kind>,
    pub vtg: vtg::State,
    pub mesh: mesh::State,
}

/// Records how each fragment input is sampled before any code is emitted.
pub(crate) fn prescan(c: &mut Ctx, insts: &[Inst]) {
    c.io.writes_point_size = insts.iter().any(|i| {
        i.mnemonic == "AST"
            && matches!(
                i.ops.first(),
                Some(Operand::Attr {
                    offset: 0x6c,
                    patch: false,
                    ..
                })
            )
    });
    for i in insts.iter().filter(|i| i.mnemonic == "IPA") {
        let Some(Operand::Attr {
            index: None,
            offset,
            ..
        }) = i.ops.get(1)
        else {
            continue;
        };
        if !(0x80..0x280).contains(offset) && !(0x300..0x3a0).contains(offset) {
            continue;
        }
        let e = c.io.sites.entry(location(*offset)).or_default();
        if i.has("CENTROID") {
            e.1 = true;
        } else {
            e.0 = true;
        }
    }
}

/// Input decorations and execution modes, once the body is emitted.
pub(crate) fn finish(c: &mut Ctx, main: u32) {
    let inputs: Vec<(u32, u32)> = c.inputs.iter().map(|(l, v)| (*l, *v)).collect();
    for (loc, var) in inputs {
        let interp = c.iface.inputs.get(&loc).copied().flatten();
        match interp {
            Some(Interp::Flat) => c.m.deco(var, 14, &[]),
            Some(Interp::Linear) => c.m.deco(var, 13, &[]),
            _ => {}
        }
        if c.stage == Stage::Fragment
            && interp != Some(Interp::Flat)
            && c.io.centroid.get(&loc) == Some(&true)
        {
            c.m.deco(var, 16, &[]);
        }
    }
    if c.stage == Stage::Fragment && c.iface.writes_depth {
        crate::spirv::inst(&mut c.m.modes, op::EXECUTION_MODE, &[main, 12]);
    }
}

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "ALD" if vtg::is_vtg(c.stage) => vtg::ald(c, i),
        "AST" if vtg::is_vtg(c.stage) => vtg::ast(c, i),
        "ALD" => ald(c, i),
        "AST" => ast(c, i),
        "IPA" => ipa(c, i),
        "KILL" => kill(c, i),
        "ISBERD" if c.stage.is_mesh_pipeline() => mesh::isberd(c, i),
        "ISBEWR" if c.stage.is_mesh_pipeline() => mesh::isbewr(c, i),
        "ISBERD" => vtg::isberd(c, i),
        "ISBEWR" => bad(format!("stage buffer {}", i.mnemonic)),
        "OUT" => vtg::out(c, i),
        "LEPC" => vtg::lepc(c, i),
        _ => return None,
    })
}

/// Where one 32-bit attribute word lives.
enum Slot {
    /// A float component: pointer, generic location if any.
    Float(u32, Option<u32>),
    /// An integer built-in: pointer and its signed type.
    Int(u32),
    /// The instance id: pointers to InstanceIndex and BaseInstance.
    InstanceId(u32, u32),
    /// A boolean built-in: true reads as all ones.
    Bool(u32),
    /// A store the translation drops.
    Drop,
}

impl Ctx {
    fn input_var(&mut self, loc: u32) -> u32 {
        if let Some(&v) = self.inputs.get(&loc) {
            return v;
        }
        let f = self.f32t();
        let v4 = self.m.t_vec(f, 4);
        let p = self.m.t_ptr(INPUT, v4);
        let var = self.m.global(p, INPUT);
        self.m.deco(var, 30, &[loc]);
        let per_primitive = self.io.vtg.pipe.mesh.fragment_per_primitive;
        if self.stage == Stage::Fragment && loc < 64 && per_primitive & (1 << loc) != 0 {
            self.m.deco(var, DECO_PER_PRIMITIVE, &[]);
            self.cap(CAP_MESH_SHADING);
            self.exts.insert("SPV_EXT_mesh_shader");
        }
        self.inputs.insert(loc, var);
        var
    }

    /// A flat integer fragment input at generic location `loc`.
    fn flat_int_input(&mut self, loc: u32) -> u32 {
        let i = self.m.t_i32();
        let p = self.m.t_ptr(INPUT, i);
        let var = self.m.global(p, INPUT);
        self.m.deco(var, 30, &[loc]);
        self.m.deco(var, 14, &[]);
        self.globals.push(var);
        var
    }

    fn output_var(&mut self, loc: u32) -> u32 {
        if let Some(&v) = self.outputs.get(&loc) {
            return v;
        }
        let f = self.f32t();
        let v4 = self.m.t_vec(f, 4);
        let p = self.m.t_ptr(OUTPUT, v4);
        let var = self.m.global(p, OUTPUT);
        self.m.deco(var, 30, &[loc]);
        self.outputs.insert(loc, var);
        self.iface.outputs.insert(loc);
        var
    }

    fn vec_builtin(&mut self, b: u32, class: u32, n: u32) -> u32 {
        let f = self.f32t();
        let t = self.m.t_vec(f, n);
        self.plain_builtin(b, class, t)
    }

    /// A built-in without the Flat decoration [`Ctx::builtin`] gives
    /// non-float fragment inputs.
    fn plain_builtin(&mut self, b: u32, class: u32, t: u32) -> u32 {
        if let Some(&v) = self.builtins.get(&b) {
            return v;
        }
        let p = self.m.t_ptr(class, t);
        let var = self.m.global(p, class);
        self.m.deco(var, 11, &[b]);
        self.builtins.insert(b, var);
        self.globals.push(var);
        self.iface.builtins.insert(b);
        var
    }

    fn scalar_builtin(&mut self, b: u32, class: u32, int: bool) -> u32 {
        let t = if int { self.m.t_i32() } else { self.f32t() };
        self.iface.builtins.insert(b);
        self.builtin(b, class, t)
    }

    pub(crate) fn component_ptr(&mut self, var: u32, class: u32, c: u32) -> u32 {
        let f = self.f32t();
        let p = self.m.t_ptr(class, f);
        let ci = self.m.c_u32(c);
        self.m.emit(op::ACCESS_CHAIN, p, &[var, ci])
    }

    fn int_slot(&mut self, b: u32, class: u32) -> Slot {
        Slot::Int(self.scalar_builtin(b, class, true))
    }

    fn clip_distance(&mut self, n: u32) -> Res<Slot> {
        let count = 32 - (self.sph_words[18] & 0xff).leading_zeros();
        if n >= count {
            return bad(format!("clip distance {n} outside the SPH"));
        }
        self.cap(CAP_CLIP_DISTANCE);
        self.iface.clip_distances = count;
        self.iface.builtins.insert(B_CLIP_DISTANCE);
        let f = self.f32t();
        let arr = self.m.t_array(f, count);
        let var = self.builtin(B_CLIP_DISTANCE, OUTPUT, arr);
        Ok(Slot::Float(self.component_ptr(var, OUTPUT, n), None))
    }

    fn input_slot(&mut self, off: u32) -> Res<Slot> {
        let frag = self.stage == Stage::Fragment;
        Ok(match off {
            0x60 if frag => match self.io.vtg.pipe.mesh.primitive_id_location {
                Some(loc) => Slot::Int(self.flat_int_input(loc)),
                None => {
                    self.cap(CAP_GEOMETRY);
                    self.int_slot(B_PRIMITIVE_ID, INPUT)
                }
            },
            0x64 if frag => {
                self.cap(CAP_GEOMETRY);
                self.int_slot(B_LAYER, INPUT)
            }
            0x68 if frag => {
                self.cap(CAP_MULTI_VIEWPORT);
                self.int_slot(B_VIEWPORT_INDEX, INPUT)
            }
            0x70..0x80 if frag => {
                let v = self.vec_builtin(B_FRAG_COORD, INPUT, 4);
                Slot::Float(self.component_ptr(v, INPUT, (off - 0x70) / 4), None)
            }
            0x2e0 | 0x2e4 if frag => {
                let v = self.vec_builtin(B_POINT_COORD, INPUT, 2);
                Slot::Float(self.component_ptr(v, INPUT, (off - 0x2e0) / 4), None)
            }
            0x3fc if frag => {
                let b = self.boolt();
                Slot::Bool(self.plain_builtin(B_FRONT_FACING, INPUT, b))
            }
            0x2f8 if self.stage == Stage::Vertex => {
                self.cap(CAP_DRAW_PARAMETERS);
                let index = self.scalar_builtin(B_INSTANCE_INDEX, INPUT, true);
                let base = self.scalar_builtin(B_BASE_INSTANCE, INPUT, true);
                Slot::InstanceId(index, base)
            }
            0x2fc if self.stage == Stage::Vertex => self.int_slot(B_VERTEX_INDEX, INPUT),
            0x80..0x280 | 0x300..0x3a0 => {
                let loc = location(off);
                let v = self.input_var(loc);
                Slot::Float(self.component_ptr(v, INPUT, (off % 16) / 4), Some(loc))
            }
            _ => return bad(format!("attribute {off:#x}")),
        })
    }

    fn output_slot(&mut self, off: u32) -> Res<Slot> {
        if self.stage == Stage::Fragment {
            return bad(format!("attribute store {off:#x} in a fragment shader"));
        }
        Ok(match off {
            0x5c => Slot::Drop,
            0x60 if self.stage == Stage::Geometry => Slot::Int(self.gs_primitive_out()),
            0x64 if self.stage == Stage::Geometry => {
                self.cap(CAP_GEOMETRY);
                self.int_slot(B_LAYER, OUTPUT)
            }
            0x68 if self.stage == Stage::Geometry => {
                self.cap(CAP_MULTI_VIEWPORT);
                self.int_slot(B_VIEWPORT_INDEX, OUTPUT)
            }
            0x64 | 0x68 => {
                self.cap(CAP_VIEWPORT_INDEX_LAYER);
                self.exts.insert("SPV_EXT_shader_viewport_index_layer");
                let b = if off == 0x64 {
                    B_LAYER
                } else {
                    B_VIEWPORT_INDEX
                };
                self.int_slot(b, OUTPUT)
            }
            0x6c => Slot::Float(self.scalar_builtin(B_POINT_SIZE, OUTPUT, false), None),
            0x70..0x80 => {
                let v = self.vec_builtin(B_POSITION, OUTPUT, 4);
                Slot::Float(self.component_ptr(v, OUTPUT, (off - 0x70) / 4), None)
            }
            0x80..0x280 | 0x300..0x3a0 => {
                let loc = location(off);
                let v = self.output_var(loc);
                Slot::Float(self.component_ptr(v, OUTPUT, (off % 16) / 4), Some(loc))
            }
            0x2c0..0x2e0 => self.clip_distance((off - 0x2c0) / 4)?,
            _ => return bad(format!("attribute {off:#x}")),
        })
    }

    /// The value of a slot as the 32 bits a register holds.
    fn load_slot(&mut self, s: &Slot) -> u32 {
        match *s {
            Slot::Float(p, _) => {
                let f = self.f32t();
                let v = self.m.emit(op::LOAD, f, &[p]);
                self.as_bits(v)
            }
            Slot::Int(p) => {
                let i = self.m.t_i32();
                let v = self.m.emit(op::LOAD, i, &[p]);
                let u = self.u32t();
                self.m.emit(op::BITCAST, u, &[v])
            }
            Slot::InstanceId(index, base) => {
                let i = self.m.t_i32();
                let index = self.m.emit(op::LOAD, i, &[index]);
                let base = self.m.emit(op::LOAD, i, &[base]);
                let v = self.m.emit(op::I_SUB, i, &[index, base]);
                let u = self.u32t();
                self.m.emit(op::BITCAST, u, &[v])
            }
            Slot::Bool(p) => {
                let b = self.boolt();
                let v = self.m.emit(op::LOAD, b, &[p]);
                self.bool_to_u32(v, u32::MAX)
            }
            Slot::Drop => self.m.c_u32(0),
        }
    }

    fn store_slot(&mut self, s: &Slot, bits: u32) {
        match *s {
            Slot::Float(p, _) => {
                let v = self.as_float(bits);
                self.m.emit_void(op::STORE, &[p, v]);
            }
            Slot::Int(p) => {
                let i = self.m.t_i32();
                let v = self.m.emit(op::BITCAST, i, &[bits]);
                self.m.emit_void(op::STORE, &[p, v]);
            }
            Slot::InstanceId(..) | Slot::Bool(_) | Slot::Drop => {}
        }
    }

    /// The SPH input map mode of component `comp` of fragment location
    /// `loc`: 0 unused, 1 constant, 2 perspective, 3 screen linear.
    fn imap(&self, loc: u32, comp: u32) -> u32 {
        let (word, n) = match loc.checked_sub(TEXCOORD_LOCATION) {
            Some(t) => (15, t),
            None => (6, loc),
        };
        let w = self.sph_words[(word + n / 4) as usize];
        (w >> (8 * (n % 4) + 2 * comp)) & 3
    }

    fn location_interp(&mut self, loc: u32, constant: bool) -> Res<Interp> {
        if let Some(Some(m)) = self.iface.inputs.get(&loc) {
            let m = *m;
            if constant && m != Interp::Flat {
                return bad("IPA.CONSTANT of an interpolated attribute");
            }
            return Ok(m);
        }
        let mut modes: Vec<u32> = (0..4)
            .map(|k| self.imap(loc, k))
            .filter(|m| *m != 0)
            .collect();
        modes.sort_unstable();
        modes.dedup();
        let m = match modes.as_slice() {
            [] if constant => Interp::Flat,
            [] => Interp::Perspective,
            [1] => Interp::Flat,
            [2] => Interp::Perspective,
            [3] => Interp::Linear,
            _ => return bad("mixed interpolation in one attribute"),
        };
        if constant && m != Interp::Flat {
            return bad("IPA.CONSTANT of an interpolated attribute");
        }
        self.iface.inputs.insert(loc, Some(m));
        Ok(m)
    }

    pub(crate) fn fragment_outputs(&mut self, targets: u32) {
        let mut reg = 0u8;
        for t in 0..8 {
            let mask = (targets >> (4 * t)) & 0xf;
            if mask == 0 {
                continue;
            }
            let kind = self.io.color_kinds.get(&t).copied().unwrap_or(Kind::Float);
            let elem = match kind {
                Kind::Float => self.f32t(),
                Kind::Sint => self.m.t_i32(),
                Kind::Uint => self.u32t(),
            };
            let var = match self.colors.get(&t) {
                Some(&v) => v,
                None => {
                    let v4 = self.m.t_vec(elem, 4);
                    let p = self.m.t_ptr(OUTPUT, v4);
                    let var = self.m.global(p, OUTPUT);
                    self.m.deco(var, 30, &[t]);
                    self.colors.insert(t, var);
                    self.iface.color_targets.insert(t);
                    var
                }
            };
            for c in 0..4 {
                if mask & (1 << c) == 0 {
                    continue;
                }
                let u = self.load_reg(reg + c as u8);
                let v = match kind {
                    Kind::Uint => u,
                    _ => self.m.emit(op::BITCAST, elem, &[u]),
                };
                let pt = self.m.t_ptr(OUTPUT, elem);
                let ci = self.m.c_u32(c);
                let ptr = self.m.emit(op::ACCESS_CHAIN, pt, &[var, ci]);
                self.m.emit_void(op::STORE, &[ptr, v]);
            }
            reg += 4;
        }
        let flags = self.sph_words[19];
        if flags & 3 == 0 {
            return;
        }
        if flags & 1 != 0 {
            self.iface.writes_sample_mask = true;
            self.iface.builtins.insert(B_SAMPLE_MASK);
            let i = self.m.t_i32();
            let arr = self.m.t_array(i, 1);
            let var = self.builtin(B_SAMPLE_MASK, OUTPUT, arr);
            let pi = self.m.t_ptr(OUTPUT, i);
            let zero = self.m.c_u32(0);
            let p = self.m.emit(op::ACCESS_CHAIN, pi, &[var, zero]);
            let u = self.load_reg(reg);
            let v = self.m.emit(op::BITCAST, i, &[u]);
            self.m.emit_void(op::STORE, &[p, v]);
        }
        if flags & 2 != 0 {
            self.iface.writes_depth = true;
            let var = self.scalar_builtin(B_FRAG_DEPTH, OUTPUT, false);
            let u = self.load_reg(reg + 1);
            let v = self.as_float(u);
            self.m.emit_void(op::STORE, &[var, v]);
        }
    }
}

impl Ctx {
    /// The fixed-state push constant block (`{ float point_size; float
    /// alpha_ref; }`, the host's graphics pipelines give it to every stage
    /// that reads it), declared the first time.
    fn push_block(&mut self) -> u32 {
        if let Some(v) = self.io.push_block {
            return v;
        }
        let f = self.f32t();
        let st = self.m.t_struct_unique(&[f, f]);
        self.m.deco(st, 2, &[]);
        self.m.member_deco(st, 0, 35, &[PUSH_POINT_SIZE]);
        self.m.member_deco(st, 1, 35, &[PUSH_ALPHA_REF]);
        let p = self.m.t_ptr(PUSH_CONSTANT, st);
        let var = self.m.global(p, PUSH_CONSTANT);
        self.globals.push(var);
        self.io.push_block = Some(var);
        var
    }

    fn push_float(&mut self, member: u32) -> u32 {
        let var = self.push_block();
        let f = self.f32t();
        let p = self.m.t_ptr(PUSH_CONSTANT, f);
        let i = self.m.c_u32(member);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, i]);
        self.m.emit(op::LOAD, f, &[ptr])
    }

    /// What a vertex program that writes no point size gets as one: the
    /// size the command buffer set (nvnCommandBufferSetPointSize), from the
    /// push constants.
    pub(crate) fn fixed_point_size(&mut self) {
        if self.stage != Stage::Vertex || self.io.writes_point_size {
            return;
        }
        let size = self.push_float(0);
        let var = self.scalar_builtin(B_POINT_SIZE, OUTPUT, false);
        self.m.emit_void(op::STORE, &[var, size]);
    }

    /// The fixed-function alpha test: the invocation is discarded when its
    /// alpha (render target 0) fails the compare function against the
    /// reference value, demoted like a `KILL`. The function is
    /// [`ALPHA_FUNC_SPEC`], always when unset, which the host driver folds
    /// away when it makes the pipeline.
    pub(crate) fn alpha_test(&mut self, targets: u32) {
        if targets & 0xf == 0 || targets & 0x8 == 0 {
            return;
        }
        let u = self.load_reg(3);
        let alpha = self.as_float(u);
        let reference = self.push_float(1);
        let func = self.m.spec_u32(8, ALPHA_FUNC_SPEC);
        let b = self.boolt();
        let cmps = [
            self.m.emit(op::F_ORD_LESS_THAN, b, &[alpha, reference]),
            self.m.emit(op::F_ORD_EQUAL, b, &[alpha, reference]),
            self.m
                .emit(op::F_ORD_LESS_THAN_EQUAL, b, &[alpha, reference]),
            self.m.emit(op::F_ORD_GREATER_THAN, b, &[alpha, reference]),
            self.m.emit(op::F_UNORD_NOT_EQUAL, b, &[alpha, reference]),
            self.m
                .emit(op::F_ORD_GREATER_THAN_EQUAL, b, &[alpha, reference]),
        ];
        let mut pass = self.m.c_bool(true);
        for f in (1..=7u32).rev() {
            let fc = self.m.c_u32(f);
            let is = self.m.emit(op::I_EQUAL, b, &[func, fc]);
            let v = if f == 1 {
                self.m.c_bool(false)
            } else {
                cmps[(f - 2) as usize]
            };
            pass = self.m.emit(op::SELECT, b, &[is, v, pass]);
        }
        let fail = self.m.emit(op::LOGICAL_NOT, b, &[pass]);
        self.cap(CAP_DEMOTE);
        self.exts.insert("SPV_EXT_demote_to_helper_invocation");
        self.iface.kills = true;
        let then = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[fail, then, merge]);
        self.m.emit_void(op::LABEL, &[then]);
        self.m.emit_void(OP_DEMOTE, &[]);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
    }
}

/// The interface location of generic attribute `a[0x80 + 16 n]` (`n`) or
/// fixed-function texture coordinate `a[0x300 + 16 t]` (32 + `t`).
fn location(off: u32) -> u32 {
    if off >= 0x300 {
        TEXCOORD_LOCATION + (off - 0x300) / 16
    } else {
        (off - 0x80) / 16
    }
}

fn attr_operand(o: &Operand) -> Res<u32> {
    match o {
        Operand::Attr {
            index: None,
            offset,
            patch: false,
        } => Ok(*offset),
        o => bad(format!("attribute operand {o}")),
    }
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

fn ald(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["64", "96", "128"])?;
    if !(2..=3).contains(&i.ops.len()) {
        return bad(format!("ALD form {i}"));
    }
    let d = Ctx::dst_reg(&i.ops[0])?;
    let off = attr_operand(&i.ops[1])?;
    for k in 0..width(i) {
        let s = c.input_slot(off + 4 * k)?;
        if let Slot::Float(_, Some(loc)) = s {
            c.iface.inputs.entry(loc).or_insert(None);
        }
        let u = c.load_slot(&s);
        c.store_reg(tuple_reg(d, k)?, u);
    }
    Ok(())
}

fn ast(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["64", "96", "128"])?;
    if !(2..=3).contains(&i.ops.len()) {
        return bad(format!("AST form {i}"));
    }
    let off = attr_operand(&i.ops[0])?;
    let s = Ctx::dst_reg(&i.ops[1])?;
    for k in 0..width(i) {
        let slot = c.output_slot(off + 4 * k)?;
        let u = c.load_reg(tuple_reg(s, k)?);
        c.store_slot(&slot, u);
    }
    Ok(())
}

/// IPA of `a[index + offset]`: every generic component the SPH input map
/// names is interpolated and the one the address picks is selected. The
/// components must share one interpolation mode.
fn ipa_indexed(c: &mut Ctx, i: &Inst, ix: &Operand, offset: u32) -> Res<u32> {
    let index = match ix {
        Operand::Reg(n, m) if !m.neg && !m.abs => c.load_reg(*n),
        Operand::UReg(n, m) if !m.neg && !m.abs => c.load_ureg(*n),
        o => return bad(format!("attribute index {o}")),
    };
    if i.has("CENTROID") {
        return bad("indexed IPA.CENTROID");
    }
    let u = c.u32t();
    let o = c.m.c_u32(offset);
    let addr = c.m.emit(op::I_ADD, u, &[index, o]);
    let f = c.f32t();
    let b = c.boolt();
    let mut acc = c.m.c_u32(0);
    let mut mode = None;
    for loc in 0..32 {
        for comp in 0..4 {
            let m = c.imap(loc, comp);
            if m == 0 {
                continue;
            }
            if mode.is_some_and(|x| x != m) {
                return bad("mixed interpolation in an indexed IPA");
            }
            mode = Some(m);
            c.location_interp(loc, i.has("CONSTANT"))?;
            let var = c.input_var(loc);
            let p = c.component_ptr(var, INPUT, comp);
            let v = c.m.emit(op::LOAD, f, &[p]);
            let bits = c.as_bits(v);
            let k = c.m.c_u32(0x80 + 16 * loc + 4 * comp);
            let hit = c.m.emit(op::I_EQUAL, b, &[addr, k]);
            acc = c.m.emit(op::SELECT, u, &[hit, bits, acc]);
        }
    }
    if mode == Some(2) {
        let fc = c.vec_builtin(B_FRAG_COORD, INPUT, 4);
        let wp = c.component_ptr(fc, INPUT, 3);
        let w = c.m.emit(op::LOAD, f, &[wp]);
        let v = c.as_float(acc);
        let v = c.m.emit(op::F_MUL, f, &[v, w]);
        acc = c.as_bits(v);
    }
    Ok(acc)
}

/// KILL demotes the invocation to a helper and execution goes on, as the
/// hardware does (NAK lowers both demote and terminate to it): the quad
/// keeps its derivatives and the outputs and stores of the killed
/// invocation are dropped.
fn kill(c: &mut Ctx, i: &Inst) -> Res<()> {
    if c.stage != Stage::Fragment {
        return bad("KILL outside a fragment shader");
    }
    let mut cond: Option<u32> = None;
    for o in &i.ops {
        let Operand::Pred(n, not) = o else {
            return bad(format!("KILL operand {o}"));
        };
        if *n == 7 && !*not {
            continue;
        }
        let v = c.pred_operand(o)?;
        cond = Some(match cond {
            None => v,
            Some(a) => {
                let b = c.boolt();
                c.m.emit(op::LOGICAL_AND, b, &[a, v])
            }
        });
    }
    c.cap(CAP_DEMOTE);
    c.exts.insert("SPV_EXT_demote_to_helper_invocation");
    c.iface.kills = true;
    let Some(cv) = cond else {
        c.m.emit_void(OP_DEMOTE, &[]);
        return Ok(());
    };
    let then = c.m.id();
    let merge = c.m.id();
    c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
    c.m.emit_void(op::BRANCH_CONDITIONAL, &[cv, then, merge]);
    c.m.emit_void(op::LABEL, &[then]);
    c.m.emit_void(OP_DEMOTE, &[]);
    c.m.emit_void(op::BRANCH, &[merge]);
    c.m.emit_void(op::LABEL, &[merge]);
    Ok(())
}

fn ipa(c: &mut Ctx, i: &Inst) -> Res<()> {
    if c.stage != Stage::Fragment {
        return bad("IPA outside a fragment shader");
    }
    Ctx::mods_ok(i, &["PASS", "CONSTANT", "CENTROID"])?;
    if i.ops.len() != 2 {
        return bad(format!("IPA form {i}"));
    }
    let d = Ctx::dst_reg(&i.ops[0])?;
    if let Operand::Attr {
        index: Some(ix),
        offset,
        patch: false,
    } = &i.ops[1]
    {
        let v = ipa_indexed(c, i, ix, *offset)?;
        c.store_reg(d, v);
        return Ok(());
    }
    let off = attr_operand(&i.ops[1])?;
    let constant = i.has("CONSTANT");
    let centroid = i.has("CENTROID");
    let s = c.input_slot(off)?;
    let Slot::Float(p, Some(loc)) = s else {
        let u = c.load_slot(&s);
        c.store_reg(d, u);
        return Ok(());
    };
    let mode = c.location_interp(loc, constant)?;
    let f = c.f32t();
    let (center, at_centroid) = c.io.sites.get(&loc).copied().unwrap_or_default();
    let mut v = if centroid && center && mode != Interp::Flat {
        c.cap(CAP_INTERPOLATION_FUNCTION);
        c.ext(f, glsl::INTERPOLATE_AT_CENTROID, &[p])
    } else {
        c.m.emit(op::LOAD, f, &[p])
    };
    c.io.centroid.insert(loc, at_centroid && !center);
    if mode == Interp::Perspective {
        let fc = c.vec_builtin(B_FRAG_COORD, INPUT, 4);
        let wp = c.component_ptr(fc, INPUT, 3);
        let w = c.m.emit(op::LOAD, f, &[wp]);
        v = c.m.emit(op::F_MUL, f, &[v, w]);
    }
    let u = c.as_bits(v);
    c.store_reg(d, u);
    Ok(())
}

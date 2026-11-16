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
//! vertex register they may carry (for the tessellation and geometry
//! stages) is ignored.
//!
//! Fragment outputs (SPH word 18, four bits per render target) leave in
//! registers from R0: four per target that has any component, then the
//! sample mask and depth when SPH word 19 bit 0 or bit 1 asks for either.

use super::{INPUT, Interp, OUTPUT, Res, bad};
use crate::program::Stage;
use crate::spirv::{glsl, op};
use crate::{Inst, Operand};
use std::collections::BTreeMap;

use super::Ctx;

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

/// Location of the first fixed-function texture coordinate, after the 32
/// generic attributes; its SPH input map starts at bit 480.
const TEXCOORD_LOCATION: u32 = 32;

const CAP_GEOMETRY: u32 = 2;
const CAP_CLIP_DISTANCE: u32 = 32;
const CAP_INTERPOLATION_FUNCTION: u32 = 52;
const CAP_MULTI_VIEWPORT: u32 = 57;
const CAP_VIEWPORT_INDEX_LAYER: u32 = 5254;

/// What the stage I/O needs to know about the whole program.
#[derive(Default)]
pub(crate) struct State {
    /// Per fragment input location: (read at the center, read at the
    /// centroid).
    sites: BTreeMap<u32, (bool, bool)>,
    centroid: BTreeMap<u32, bool>,
}

/// Records how each fragment input is sampled before any code is emitted.
pub(crate) fn prescan(c: &mut Ctx, insts: &[Inst]) {
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
        "ALD" => ald(c, i),
        "AST" => ast(c, i),
        "IPA" => ipa(c, i),
        "ISBERD" | "ISBEWR" => bad(format!("stage buffer {}", i.mnemonic)),
        _ => return None,
    })
}

/// Where one 32-bit attribute word lives.
enum Slot {
    /// A float component: pointer, generic location if any.
    Float(u32, Option<u32>),
    /// An integer built-in: pointer and its signed type.
    Int(u32),
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
        self.inputs.insert(loc, var);
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
            0x60 if frag => {
                self.cap(CAP_GEOMETRY);
                self.int_slot(B_PRIMITIVE_ID, INPUT)
            }
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
            0x2f8 if self.stage == Stage::Vertex => self.int_slot(B_INSTANCE_INDEX, INPUT),
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
            Slot::Bool(_) | Slot::Drop => {}
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
        let f = self.f32t();
        let mut reg = 0u8;
        for t in 0..8 {
            let mask = (targets >> (4 * t)) & 0xf;
            if mask == 0 {
                continue;
            }
            let var = match self.colors.get(&t) {
                Some(&v) => v,
                None => {
                    let v4 = self.m.t_vec(f, 4);
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
                let v = self.as_float(u);
                let ptr = self.component_ptr(var, OUTPUT, c);
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
        c.store_reg(d.wrapping_add(k as u8), u);
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
        let u = c.load_reg(s.wrapping_add(k as u8));
        c.store_slot(&slot, u);
    }
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

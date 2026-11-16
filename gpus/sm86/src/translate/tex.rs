//! Textures and surfaces: TEX, TLD, TLD4, TMML, TXD, TXQ, SULD, SUST,
//! SUATOM and SURED.
//!
//! # Handles and descriptors
//!
//! An NVN texture handle is a 32-bit word: the texture id (an index into the
//! texture pool) in bits 0..20 and the sampler id (an index into the sampler
//! pool) in bits 20..32. The bound forms read it from a constant bank
//! (`TEX ..., 0x3, 0x4` reads `c[3]` byte `0x10`), the `.B` forms from a
//! register. Both pools are runtime descriptor arrays in descriptor set
//! [`TEXTURE_SET`](super::TEXTURE_SET), indexed by those ids:
//!
//! * binding [`SAMPLER_BINDING`]: the sampler pool (`OpTypeSampler`);
//! * [`texture_binding`]: one sampled-image array per numeric [`Kind`] and
//!   [`Dim`], all indexed by texture id;
//! * [`image_binding`]: one storage-image array per [`ImageKind`] and
//!   [`ImageDim`], indexed by texture id.
//!
//! The runtime mirrors every texture of the guest's pool into each array
//! whose view type it can take (a 2D texture as a 2D and a 2D array view,
//! a six-layer 2D array also as a cube) with a view format of the array's
//! numeric kind from the texture format's compatibility class, so that any
//! array an instruction names holds a valid descriptor. The instruction's
//! dimension field picks the array; TXQ has none and uses the dimension of
//! the other instructions on the same bound handle (2D if none).
//! Indices from a register are decorated `NonUniform`.
//!
//! Binary (`.D`) surface accesses read and write unsigned texels of the
//! access size (8 to 128 bits; `.BA` byte x coordinates are divided by it),
//! so the runtime gives those arrays views of a size-compatible unsigned
//! format; atomics use 32-bit `R32ui` views.
//!
//! The numeric kind of a bound handle's texture is chosen by the runtime:
//! each bound handle that a TEX, TLD, TLD4, TXD, SULD.P or SUST.P reads gets
//! a specialization constant ([`TEXTURE_KIND_SPEC`] onwards, listed in
//! `Interface::texture_kinds`; 0 float, 1 signed, 2 unsigned) that selects
//! the array, so integer textures read their integer bits. Depth compares,
//! TMML, TXQ and handles from a register always use the float arrays.
//!
//! # Sources
//!
//! As Mesa NAK packs them for SM70 (and checked on an RTX 3090): Ra holds
//! [array index or min LOD] and the coordinates, Rb holds [handle], [LOD or
//! bias], [offsets], [depth reference], [sample]; TXD has [handle],
//! coordinates and [array index | offsets << 16] in Ra and the gradients in
//! Rb. With `.SCR` the list (at most four words) is split 1: Ra; 2: Ra, Rb;
//! 3: Ra, Ra+1, Rb; 4: Ra, Ra+1, Rb, Rb+1. The array index is an integer in
//! bits 0..16, a min LOD a u4.8 in bits 16..32. Results: the components the
//! mask selects, in order, to Rd, Rd+1, Rd2, Rd2+1, or with `.F16` as half
//! pairs to Rd and Rd2.
//!
//! # Limits
//!
//! Vulkan allows a non-constant texel offset only for gathers. A TEX or
//! TXD offset is a constant when a block-local MOV sets its register;
//! otherwise the coordinates move by the offset over the size of the LOD
//! the instruction names (level 0 for implicit LODs), which is exact for
//! one level only. Implicit LODs and biases outside fragment shaders sample
//! level 0. Buffer textures, TLD.CL, TXD.LC, TXQ sample positions, uniform
//! register handles, SUQUERY, SUATOM.CAS and wrapping INC and DEC are not
//! translated; sparse fault predicates read as resident.

use super::{Ctx, Res, TEXTURE_SET, bad};
use crate::program::Stage;
use crate::spirv::{glsl, op};
use crate::{Inst, Operand, Word};
use std::collections::BTreeMap;

const UNIFORM_CONSTANT: u32 = 0;
const IMAGE_CLASS: u32 = 11;
const NON_UNIFORM: u32 = 5300;

/// Binding of the sampler pool in set [`TEXTURE_SET`](super::TEXTURE_SET).
pub const SAMPLER_BINDING: u32 = 0;

/// First specialization constant id of the per-handle numeric kinds.
pub const TEXTURE_KIND_SPEC: u32 = 0x100;

/// How a texture's texels read: the sampled type of its array.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Float = 0,
    Sint = 1,
    Uint = 2,
}

/// The view type of a sampled-image array.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dim {
    D1 = 0,
    D2 = 1,
    D3 = 2,
    Cube = 3,
    Array1 = 4,
    Array2 = 5,
    ArrayCube = 6,
    Ms2 = 7,
    ArrayMs2 = 8,
}

/// Binding of the sampled-image array of `kind` and `dim`.
pub fn texture_binding(kind: Kind, dim: Dim) -> u32 {
    1 + 10 * kind as u32 + dim as u32
}

/// How a storage image's texels read: formatted (P) accesses by numeric
/// kind, binary (D) accesses as unsigned texels of the access size, and
/// 32-bit atomics on an `R32ui` image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImageKind {
    Float = 0,
    Sint = 1,
    Uint = 2,
    Raw8 = 3,
    Raw16 = 4,
    Raw32 = 5,
    Raw64 = 6,
    Raw128 = 7,
    Atomic32 = 8,
}

/// The view type of a storage-image array, in the instructions' order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImageDim {
    D1 = 0,
    Buffer = 1,
    Array1 = 2,
    D2 = 3,
    Array2 = 4,
    D3 = 5,
}

/// Binding of the storage-image array of `kind` and `dim`.
pub fn image_binding(kind: ImageKind, dim: ImageDim) -> u32 {
    32 + 8 * kind as u32 + dim as u32
}

impl Dim {
    fn spv(self) -> u32 {
        match self {
            Dim::D1 | Dim::Array1 => 0,
            Dim::D2 | Dim::Array2 | Dim::Ms2 | Dim::ArrayMs2 => 1,
            Dim::D3 => 2,
            Dim::Cube | Dim::ArrayCube => 3,
        }
    }
    fn arrayed(self) -> bool {
        matches!(
            self,
            Dim::Array1 | Dim::Array2 | Dim::ArrayCube | Dim::ArrayMs2
        )
    }
    fn ms(self) -> bool {
        matches!(self, Dim::Ms2 | Dim::ArrayMs2)
    }
    fn cube(self) -> bool {
        matches!(self, Dim::Cube | Dim::ArrayCube)
    }
    /// Coordinates besides the array index.
    fn coords(self) -> usize {
        match self {
            Dim::D1 | Dim::Array1 => 1,
            Dim::D2 | Dim::Array2 | Dim::Ms2 | Dim::ArrayMs2 => 2,
            Dim::D3 | Dim::Cube | Dim::ArrayCube => 3,
        }
    }
    /// Components of a texel offset.
    fn offsets(self) -> usize {
        if self.cube() { 0 } else { self.coords() }
    }
    /// Components of the size query result.
    fn size_len(self) -> usize {
        match self {
            Dim::D1 => 1,
            Dim::D2 | Dim::Cube | Dim::Ms2 | Dim::Array1 => 2,
            Dim::D3 | Dim::Array2 | Dim::ArrayCube | Dim::ArrayMs2 => 3,
        }
    }
}

const TEX_DIMS: [Option<Dim>; 8] = [
    Some(Dim::D1),
    Some(Dim::D2),
    Some(Dim::D3),
    Some(Dim::Cube),
    Some(Dim::Array1),
    Some(Dim::Array2),
    None,
    Some(Dim::ArrayCube),
];

/// Facts about the whole program that single instructions need: the
/// dimension each bound handle is sampled with (for TXQ) and the
/// immediates that block-local moves leave in registers (texel offsets).
#[derive(Default)]
pub(crate) struct Prep {
    dims: BTreeMap<(u32, u32), Dim>,
    starts: Vec<bool>,
    imm: Vec<Option<(u8, u32)>>,
    writes: Vec<Vec<(u8, u8)>>,
    vars: BTreeMap<u32, u32>,
}

fn is_texture(m: &str) -> bool {
    matches!(m, "TEX" | "TLD" | "TLD4" | "TMML" | "TXD" | "TXQ")
}

fn imm_of(o: &Operand) -> Option<u32> {
    match o {
        Operand::Hex(v) => Some(*v as u32),
        Operand::Int(v) => Some(*v as u32),
        _ => None,
    }
}

impl Prep {
    pub(crate) fn new(insts: &[Inst], words: &[Word]) -> Prep {
        let n = insts.len();
        let mut p = Prep {
            starts: vec![false; n + 1],
            imm: vec![None; n],
            writes: vec![Vec::new(); n],
            ..Prep::default()
        };
        p.starts[0] = true;
        for (k, (i, w)) in insts.iter().zip(words).enumerate() {
            for o in &i.ops {
                if let Operand::Target(t) = o
                    && let Ok(t) = usize::try_from(*t / 16)
                    && t < n
                {
                    p.starts[t] = true;
                }
            }
            if matches!(
                i.mnemonic,
                "BRA"
                    | "BRX"
                    | "BRXU"
                    | "JMP"
                    | "JMX"
                    | "CALL"
                    | "RET"
                    | "EXIT"
                    | "KILL"
                    | "BSYNC"
                    | "BSSY"
                    | "BREAK"
                    | "WARPSYNC"
                    | "BPT"
                    | "RTT"
            ) {
                p.starts[k + 1] = true;
            }
            let firsts: Vec<u8> = if is_texture(i.mnemonic) || i.mnemonic.starts_with("SU") {
                i.ops
                    .iter()
                    .take(3)
                    .filter_map(|o| match o {
                        Operand::Reg(r, _) => Some(*r),
                        _ => None,
                    })
                    .collect()
            } else {
                i.ops
                    .iter()
                    .find(|o| !matches!(o, Operand::Pred(..) | Operand::UPred(..)))
                    .and_then(|o| match o {
                        Operand::Reg(r, _) => Some(*r),
                        _ => None,
                    })
                    .into_iter()
                    .collect()
            };
            p.writes[k] = firsts
                .into_iter()
                .filter(|r| *r != 255)
                .map(|r| (r, 4))
                .collect();
            if i.guard.is_none() {
                let mov = match (i.mnemonic, i.ops.as_slice()) {
                    ("MOV", [Operand::Reg(d, _), v]) => imm_of(v).map(|v| (*d, v)),
                    ("MOV", [Operand::Reg(d, _), v, m]) if imm_of(m) == Some(0xf) => {
                        imm_of(v).map(|v| (*d, v))
                    }
                    (
                        "IMAD",
                        [
                            Operand::Reg(d, _),
                            Operand::Reg(255, _),
                            Operand::Reg(255, _),
                            v,
                        ],
                    ) if i.has("MOV") => imm_of(v).map(|v| (*d, v)),
                    _ => None,
                };
                p.imm[k] = mov.filter(|(d, _)| *d != 255);
            }
            let opc = w.get(0, 9);
            if w.get(9, 3) == 5 && matches!(opc, 0x160 | 0x163 | 0x166 | 0x169 | 0x16c) {
                let key = (w.get(54, 5), 4 * w.get(40, 14));
                let dim = match opc {
                    0x166 if w.bit(78) => match w.get(61, 3) {
                        1 => Some(Dim::Ms2),
                        5 => Some(Dim::ArrayMs2),
                        _ => None,
                    },
                    _ => TEX_DIMS[w.get(61, 3) as usize],
                };
                if let Some(d) = dim {
                    p.dims.entry(key).or_insert(d);
                }
            }
        }
        p
    }

    /// The value register `r` holds before instruction `k`, when an
    /// unpredicated move of an immediate in the same block sets it.
    fn known(&self, k: usize, r: u8) -> Option<u32> {
        let mut j = k;
        loop {
            if j == 0 || self.starts.get(j).copied().unwrap_or(true) {
                return None;
            }
            j -= 1;
            if let Some((d, v)) = self.imm.get(j).copied().flatten()
                && d == r
            {
                return Some(v);
            }
            if self.writes[j]
                .iter()
                .any(|&(lo, n)| r >= lo && u32::from(r) < u32::from(lo) + u32::from(n))
            {
                return None;
            }
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Slot {
    Packed,
    Coord(u8),
    Handle,
    Lod,
    Off,
    Ptp(u8),
    Dc,
    Ms,
    Grad(u8),
}

struct Srcs {
    vals: Vec<(Slot, u32, Option<u8>)>,
}

impl Srcs {
    fn get(&self, s: Slot) -> Res<u32> {
        match self.vals.iter().find(|v| v.0 == s) {
            Some(v) => Ok(v.1),
            None => bad(format!("texture source {s:?}")),
        }
    }
    fn reg(&self, s: Slot) -> Option<u8> {
        self.vals.iter().find(|v| v.0 == s).and_then(|v| v.2)
    }
}

/// Loads the source words named by `g0` (from Ra) and `g1` (from Rb), or
/// with `scr` the scalar split of both lists.
fn sources(c: &mut Ctx, g0: &[Slot], g1: &[Slot], scr: bool) -> Res<Srcs> {
    let ra = c.word.get(24, 8) as u8;
    let rb = c.word.get(32, 8) as u8;
    let mut place: Vec<(Slot, u8, u8)> = Vec::new();
    if scr {
        let all: Vec<Slot> = g0.iter().chain(g1).copied().collect();
        let spots: &[(u8, u8)] = match all.len() {
            1 => &[(ra, 0)],
            2 => &[(ra, 0), (rb, 0)],
            3 => &[(ra, 0), (ra, 1), (rb, 0)],
            4 => &[(ra, 0), (ra, 1), (rb, 0), (rb, 1)],
            n => return bad(format!("scalar texture with {n} sources")),
        };
        for (s, &(b, o)) in all.iter().zip(spots) {
            place.push((*s, b, o));
        }
    } else {
        for (k, s) in g0.iter().enumerate() {
            place.push((*s, ra, k as u8));
        }
        for (k, s) in g1.iter().enumerate() {
            place.push((*s, rb, k as u8));
        }
    }
    let mut vals = Vec::new();
    for (s, base, off) in place {
        if base == 255 {
            let z = c.m.c_u32(0);
            vals.push((s, z, None));
            continue;
        }
        let r = u32::from(base) + u32::from(off);
        if r >= 255 {
            return bad("texture source past R254");
        }
        let v = c.load_reg(r as u8);
        vals.push((s, v, Some(r as u8)));
    }
    Ok(Srcs { vals })
}

/// The handle word of a bound (`Some((bank, byte offset))`) or bindless
/// texture, and whether its index may diverge.
fn handle(c: &mut Ctx, key: Option<(u32, u32)>, s: &Srcs) -> Res<(u32, bool)> {
    if key.is_none() && c.word.get(9, 3) == 7 {
        return Ok((ur_handle(c)?, false));
    }
    match key {
        Some((bank, off)) => {
            c.iface.textures.insert((bank, off));
            Ok((c.load_cbuf(bank, i64::from(off))?, false))
        }
        None => Ok((s.get(Slot::Handle)?, true)),
    }
}

/// Whether the handle of a texture instruction is a register source.
fn reg_handle(c: &Ctx, key: Option<(u32, u32)>) -> bool {
    key.is_none() && c.word.get(9, 3) != 7
}

/// The handle of a texture instruction that names a uniform register
/// pair: the texture index in the first register and the sampler index in
/// the second, repacked into the combined handle word.
fn ur_handle(c: &mut Ctx) -> Res<u32> {
    let w = c.word;
    let n = w.get(40, 6);
    if w.get(46, 8) != 0 || w.get(54, 5) != 0 || n >= 62 {
        return bad("texture handle in a uniform register with an offset");
    }
    let t = c.load_ureg(n as u8);
    let sm = c.load_ureg(n as u8 + 1);
    let t = c.and_k(t, 0xfffff);
    let sm = c.sh(op::SHIFT_LEFT_LOGICAL, sm, 20);
    Ok(c.ib(op::BITWISE_OR, t, sm))
}

/// The bound handle of a texture instruction, `None` for `.B`.
fn texref(c: &Ctx) -> Res<Option<(u32, u32)>> {
    match c.word.get(9, 3) {
        5 => Ok(Some((c.word.get(54, 5), 4 * c.word.get(40, 14)))),
        1 | 7 => Ok(None),
        _ => bad("texture handle form"),
    }
}

impl Ctx {
    fn tx_i32(&mut self) -> u32 {
        self.m.t_i32()
    }

    fn tx_op(&mut self, opc: u16, a: u32, b: u32) -> u32 {
        let u = self.u32t();
        self.m.emit(opc, u, &[a, b])
    }

    fn tx_opk(&mut self, opc: u16, a: u32, k: u32) -> u32 {
        let kc = self.m.c_u32(k);
        self.tx_op(opc, a, kc)
    }

    fn descriptor_var(&mut self, binding: u32, ty: u32) -> u32 {
        if let Some(&v) = self.tex.vars.get(&binding) {
            return v;
        }
        self.cap(5302);
        let rt = self.m.t_runtime_array(ty);
        let p = self.m.t_ptr(UNIFORM_CONSTANT, rt);
        let var = self.m.global(p, UNIFORM_CONSTANT);
        self.m.deco(var, 34, &[TEXTURE_SET]);
        self.m.deco(var, 33, &[binding]);
        self.globals.push(var);
        self.iface.texture_bindings.insert(binding);
        self.tex.vars.insert(binding, var);
        var
    }

    /// A pointer to element `index` of the descriptor array at `binding`.
    fn descriptor_ptr(&mut self, binding: u32, ty: u32, index: u32, nonuniform: bool) -> u32 {
        let var = self.descriptor_var(binding, ty);
        let p = self.m.t_ptr(UNIFORM_CONSTANT, ty);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, index]);
        if nonuniform {
            self.cap(5301);
            self.m.deco(ptr, NON_UNIFORM, &[]);
        }
        ptr
    }

    fn descriptor(&mut self, binding: u32, ty: u32, index: u32, nonuniform: bool) -> u32 {
        let ptr = self.descriptor_ptr(binding, ty, index, nonuniform);
        let v = self.m.emit(op::LOAD, ty, &[ptr]);
        if nonuniform {
            self.m.deco(v, NON_UNIFORM, &[]);
        }
        v
    }

    fn sampled_type(&mut self, k: Kind) -> u32 {
        match k {
            Kind::Float => self.f32t(),
            Kind::Sint => self.tx_i32(),
            Kind::Uint => self.u32t(),
        }
    }

    fn texture_type(&mut self, k: Kind, d: Dim) -> u32 {
        let st = self.sampled_type(k);
        match d {
            Dim::D1 | Dim::Array1 => self.cap(43),
            Dim::ArrayCube => self.cap(45),
            _ => {}
        }
        self.m.t_image([
            st,
            d.spv(),
            0,
            u32::from(d.arrayed()),
            u32::from(d.ms()),
            1,
            0,
        ])
    }

    /// The texture at `handle` of kind `k` and dimension `d`.
    fn texture(&mut self, k: Kind, d: Dim, h: u32, nonuniform: bool) -> u32 {
        let ty = self.texture_type(k, d);
        let idx = self.tx_opk(op::BITWISE_AND, h, 0xf_ffff);
        if nonuniform {
            self.cap(5307);
        }
        self.descriptor(texture_binding(k, d), ty, idx, nonuniform)
    }

    /// The texture at `handle` combined with the sampler it names.
    fn sampled(&mut self, k: Kind, d: Dim, h: u32, nonuniform: bool) -> (u32, u32) {
        let img = self.texture(k, d, h, nonuniform);
        let ity = self.texture_type(k, d);
        let st = self.m.t_sampler();
        let sidx = self.tx_opk(op::SHIFT_RIGHT_LOGICAL, h, 20);
        let smp = self.descriptor(SAMPLER_BINDING, st, sidx, nonuniform);
        let sit = self.m.t_sampled_image(ity);
        let si = self.m.emit(op::SAMPLED_IMAGE, sit, &[img, smp]);
        if nonuniform {
            self.m.deco(si, NON_UNIFORM, &[]);
        }
        (si, img)
    }

    fn tx_uvec4(&mut self) -> u32 {
        let u = self.u32t();
        self.m.t_vec(u, 4)
    }

    /// A texel of any kind as four words.
    fn texel_bits(&mut self, v: u32, k: Kind) -> u32 {
        let t = self.tx_uvec4();
        if k == Kind::Uint {
            v
        } else {
            self.m.emit(op::BITCAST, t, &[v])
        }
    }

    /// Runs `f` for the numeric kind the runtime picks for bound handle
    /// `key` (or for floats only), merging the four result words.
    fn by_kind(
        &mut self,
        key: Option<(u32, u32)>,
        ints: bool,
        f: &mut dyn FnMut(&mut Ctx, Kind) -> Res<u32>,
    ) -> Res<u32> {
        let Some(key) = key.filter(|_| ints) else {
            return f(self, Kind::Float);
        };
        let next = TEXTURE_KIND_SPEC + self.iface.texture_kinds.len() as u32;
        let id = *self.iface.texture_kinds.entry(key).or_insert(next);
        let kc = self.m.spec_u32(0, id);
        let b = self.boolt();
        let zero = self.m.c_u32(0);
        let one = self.m.c_u32(1);
        let is_f = self.m.spec_op(b, op::I_EQUAL, &[kc, zero]);
        let is_s = self.m.spec_op(b, op::I_EQUAL, &[kc, one]);
        let (lf, ln, ls, lu, m2, m) = (
            self.m.id(),
            self.m.id(),
            self.m.id(),
            self.m.id(),
            self.m.id(),
            self.m.id(),
        );
        let t = self.tx_uvec4();
        self.m.emit_void(op::SELECTION_MERGE, &[m, 0]);
        self.m.emit_void(op::BRANCH_CONDITIONAL, &[is_f, lf, ln]);
        self.m.emit_void(op::LABEL, &[lf]);
        let vf = f(self, Kind::Float)?;
        self.m.emit_void(op::BRANCH, &[m]);
        self.m.emit_void(op::LABEL, &[ln]);
        self.m.emit_void(op::SELECTION_MERGE, &[m2, 0]);
        self.m.emit_void(op::BRANCH_CONDITIONAL, &[is_s, ls, lu]);
        self.m.emit_void(op::LABEL, &[ls]);
        let vs = f(self, Kind::Sint)?;
        self.m.emit_void(op::BRANCH, &[m2]);
        self.m.emit_void(op::LABEL, &[lu]);
        let vu = f(self, Kind::Uint)?;
        self.m.emit_void(op::BRANCH, &[m2]);
        self.m.emit_void(op::LABEL, &[m2]);
        let vn = self.m.emit(op::PHI, t, &[vs, ls, vu, lu]);
        self.m.emit_void(op::BRANCH, &[m]);
        self.m.emit_void(op::LABEL, &[m]);
        Ok(self.m.emit(op::PHI, t, &[vf, lf, vn, m2]))
    }

    fn tx_fvec(&mut self, parts: &[u32]) -> u32 {
        if parts.len() == 1 {
            return parts[0];
        }
        let f = self.f32t();
        let t = self.m.t_vec(f, parts.len() as u32);
        self.m.emit(op::COMPOSITE_CONSTRUCT, t, parts)
    }

    fn tx_ivec(&mut self, parts: &[u32]) -> u32 {
        if parts.len() == 1 {
            return parts[0];
        }
        let i = self.tx_i32();
        let t = self.m.t_vec(i, parts.len() as u32);
        self.m.emit(op::COMPOSITE_CONSTRUCT, t, parts)
    }

    fn tx_as_i32(&mut self, u: u32) -> u32 {
        let i = self.tx_i32();
        self.m.emit(op::BITCAST, i, &[u])
    }

    fn tx_u2f(&mut self, u: u32) -> u32 {
        let f = self.f32t();
        self.m.emit(op::CONVERT_U_TO_F, f, &[u])
    }

    /// The array layer of a packed index word, as a float coordinate.
    fn layer_f(&mut self, packed: u32) -> u32 {
        let l = self.tx_opk(op::BITWISE_AND, packed, 0xffff);
        self.tx_u2f(l)
    }

    /// The min LOD (u4.8 in bits 16..32) of a packed index word.
    fn min_lod(&mut self, packed: u32) -> u32 {
        let hi = self.tx_opk(op::SHIFT_RIGHT_LOGICAL, packed, 16);
        let f = self.tx_u2f(hi);
        let k = self.m.c_f32((1.0f32 / 256.0).to_bits());
        let t = self.f32t();
        self.m.emit(op::F_MUL, t, &[f, k])
    }

    /// Constant texel offsets: `bits`-bit signed fields of `v`.
    fn const_offset(&mut self, v: u32, n: usize, bits: u32) -> u32 {
        let parts: Vec<u32> = (0..n as u32)
            .map(|k| {
                let f = (v >> (bits * k)) & ((1 << bits) - 1);
                let s = ((f << (32 - bits)) as i32) >> (32 - bits);
                self.m.c_i32(s)
            })
            .collect();
        if n == 1 {
            return parts[0];
        }
        let i = self.tx_i32();
        let t = self.m.t_vec(i, n as u32);
        self.m.c_composite(t, &parts)
    }

    /// Run-time texel offsets: `bits`-bit signed fields of `v` from bit
    /// `at`.
    fn dyn_offsets(&mut self, v: u32, n: usize, bits: u32, at: u32) -> Vec<u32> {
        let i = self.tx_i32();
        let vi = self.tx_as_i32(v);
        (0..n as u32)
            .map(|k| {
                let o = self.m.c_u32(at + bits * k);
                let w = self.m.c_u32(bits);
                self.m.emit(op::BIT_FIELD_S_EXTRACT, i, &[vi, o, w])
            })
            .collect()
    }

    /// The texel offset image operand of word `v` (4-bit fields from bit
    /// `at`) for register `reg` at this instruction, when it is a known
    /// constant.
    fn known_offset(&mut self, reg: Option<u8>, d: Dim, bits: u32, at: u32) -> Option<u32> {
        let k = (self.pc / 16) as usize;
        let v = match reg {
            None => 0,
            Some(r) => self.tex.known(k, r)?,
        };
        Some(self.const_offset(v >> at, d.offsets(), bits))
    }

    /// Moves float coordinates by run-time texel offsets at LOD `level`.
    fn shift_coords(&mut self, coords: &mut [u32], offs: &[u32], img: u32, d: Dim, level: u32) {
        self.cap(50);
        let i = self.tx_i32();
        let n = d.size_len();
        let st = if n == 1 { i } else { self.m.t_vec(i, n as u32) };
        let size = self.m.emit(op::IMAGE_QUERY_SIZE_LOD, st, &[img, level]);
        let f = self.f32t();
        for (k, o) in offs.iter().enumerate() {
            let s = if n == 1 {
                size
            } else {
                self.m.emit(op::COMPOSITE_EXTRACT, i, &[size, k as u32])
            };
            let sf = self.m.emit(op::CONVERT_S_TO_F, f, &[s]);
            let of = self.m.emit(op::CONVERT_S_TO_F, f, &[*o]);
            let q = self.m.emit(op::F_DIV, f, &[of, sf]);
            coords[k] = self.m.emit(op::F_ADD, f, &[coords[k], q]);
        }
    }

    /// f32 bits to f16 bits, rounding to nearest even or toward zero.
    fn f16_bits(&mut self, x: u32, rz: bool) -> u32 {
        let u = self.u32t();
        let b = self.boolt();
        let sign = self.tx_opk(op::SHIFT_RIGHT_LOGICAL, x, 16);
        let sign = self.tx_opk(op::BITWISE_AND, sign, 0x8000);
        let e = self.tx_opk(op::SHIFT_RIGHT_LOGICAL, x, 23);
        let e = self.tx_opk(op::BITWISE_AND, e, 0xff);
        let man = self.tx_opk(op::BITWISE_AND, x, 0x7f_ffff);
        let c113 = self.m.c_u32(113);
        let norm = self.m.emit(op::U_GREATER_THAN_EQUAL, b, &[e, c113]);
        let en = self.tx_opk(op::I_SUB, e, 112);
        let en = self.tx_opk(op::SHIFT_LEFT_LOGICAL, en, 10);
        let mh = self.tx_opk(op::SHIFT_RIGHT_LOGICAL, man, 13);
        let qn = self.tx_op(op::BITWISE_OR, en, mh);
        let rn = self.tx_opk(op::BITWISE_AND, man, 0x1fff);
        let zero = self.m.c_u32(0);
        let ez = self.m.emit(op::I_EQUAL, b, &[e, zero]);
        let imp = self.m.c_u32(0x80_0000);
        let imp = self.select(u, ez, zero, imp);
        let m = self.tx_op(op::BITWISE_OR, man, imp);
        let c126 = self.m.c_u32(126);
        let sh = self.tx_op(op::I_SUB, c126, e);
        let c31 = self.m.c_u32(31);
        let sh = self.ext(u, glsl::U_MIN, &[sh, c31]);
        let qs = self.tx_op(op::SHIFT_RIGHT_LOGICAL, m, sh);
        let one = self.m.c_u32(1);
        let bit = self.tx_op(op::SHIFT_LEFT_LOGICAL, one, sh);
        let mask = self.tx_op(op::I_SUB, bit, one);
        let rs = self.tx_op(op::BITWISE_AND, m, mask);
        let hs = self.tx_opk(op::SHIFT_RIGHT_LOGICAL, bit, 1);
        let q = self.select(u, norm, qn, qs);
        let q = if rz {
            q
        } else {
            let rem = self.select(u, norm, rn, rs);
            let h = self.m.c_u32(0x1000);
            let half = self.select(u, norm, h, hs);
            let gt = self.m.emit(op::U_GREATER_THAN, b, &[rem, half]);
            let eq = self.m.emit(op::I_EQUAL, b, &[rem, half]);
            let odd = self.tx_opk(op::BITWISE_AND, q, 1);
            let odd = self.m.emit(op::I_NOT_EQUAL, b, &[odd, zero]);
            let tie = self.m.emit(op::LOGICAL_AND, b, &[eq, odd]);
            let up = self.m.emit(op::LOGICAL_OR, b, &[gt, tie]);
            let inc = self.select(u, up, one, zero);
            self.tx_op(op::I_ADD, q, inc)
        };
        let c143 = self.m.c_u32(143);
        let big = self.m.emit(op::U_GREATER_THAN_EQUAL, b, &[e, c143]);
        let ovf = self.m.c_u32(if rz { 0x7bff } else { 0x7c00 });
        let q = self.select(u, big, ovf, q);
        let c255 = self.m.c_u32(255);
        let special = self.m.emit(op::I_EQUAL, b, &[e, c255]);
        let mz = self.m.emit(op::I_EQUAL, b, &[man, zero]);
        let inf = self.m.c_u32(0x7c00);
        let nan = self.m.c_u32(0x7fff);
        let sp = self.select(u, mz, inf, nan);
        let q = self.select(u, special, sp, q);
        self.tx_op(op::BITWISE_OR, sign, q)
    }

    /// Writes the components of `v` (four words) that the mask selects to
    /// Rd, Rd+1, Rd2, Rd2+1, or as half pairs to Rd and Rd2.
    fn tex_result(&mut self, v: u32, mask: u32, f16: u32) {
        let rd = self.word.get(16, 8) as u8;
        let rd2 = self.word.get(64, 8) as u8;
        let u = self.u32t();
        let comps: Vec<u32> = (0..4)
            .filter(|k| mask >> k & 1 != 0)
            .map(|k| self.m.emit(op::COMPOSITE_EXTRACT, u, &[v, k]))
            .collect();
        let next = |r: u8| if r == 255 { 255 } else { r.saturating_add(1) };
        let mut outs = Vec::new();
        if f16 != 0 {
            let halves: Vec<u32> = comps.iter().map(|&x| self.f16_bits(x, f16 == 2)).collect();
            for (n, pair) in halves.chunks(2).enumerate() {
                let w = if pair.len() == 2 {
                    let hi = self.tx_opk(op::SHIFT_LEFT_LOGICAL, pair[1], 16);
                    self.tx_op(op::BITWISE_OR, pair[0], hi)
                } else {
                    pair[0]
                };
                outs.push((if n == 0 { rd } else { rd2 }, w));
            }
        } else {
            let dests = [rd, next(rd), rd2, next(rd2)];
            for (d, x) in dests.iter().zip(comps) {
                outs.push((*d, x));
            }
        }
        for (d, x) in outs {
            self.store_reg(d, x);
        }
    }

    /// Clears the sparse fault predicate at bits 81..84, if any.
    fn no_fault(&mut self) {
        let p = self.word.get(81, 3) as u8;
        if p != 7 {
            let f = self.m.c_bool(false);
            self.store_pred(p, f);
        }
    }

    fn f4_from_scalar(&mut self, r: u32) -> u32 {
        let z = self.m.c_f32(0);
        let one = self.m.c_f32(1.0f32.to_bits());
        let f = self.f32t();
        let t = self.m.t_vec(f, 4);
        self.m.emit(op::COMPOSITE_CONSTRUCT, t, &[r, z, z, one])
    }

    fn tx_uvec4_of(&mut self, parts: [u32; 4]) -> u32 {
        let t = self.tx_uvec4();
        self.m.emit(op::COMPOSITE_CONSTRUCT, t, &parts)
    }
}

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "TEX" => tex(c),
        "TLD" => tld(c),
        "TLD4" => tld4(c),
        "TMML" => tmml(c),
        "TXD" => txd(c),
        "TXQ" => txq(c),
        "SULD" | "SUST" | "SUATOM" | "SURED" => surface(c, i),
        "SUQUERY" => bad("opcode SUQUERY"),
        _ => return None,
    })
}

fn dim_at(w: Word, table: &[Option<Dim>; 8]) -> Res<Dim> {
    match table[w.get(61, 3) as usize] {
        Some(d) => Ok(d),
        None => bad("texture dimension"),
    }
}

fn coord_slots(d: Dim) -> Vec<Slot> {
    (0..d.coords() as u8).map(Slot::Coord).collect()
}

/// Float coordinates of a sample. The hardware takes cube coordinates as
/// already divided by the major axis (the compiler normalizes them first)
/// and does not divide again, so the major component becomes exactly ±1
/// for Vulkan's divide to keep the other two as given.
fn float_coords(c: &mut Ctx, s: &Srcs, d: Dim) -> Res<Vec<u32>> {
    let mut v = Vec::new();
    for k in 0..d.coords() as u8 {
        let x = s.get(Slot::Coord(k))?;
        v.push(c.as_float(x));
    }
    if d.cube() {
        let f = c.f32t();
        let b = c.boolt();
        let a: Vec<u32> = v.iter().map(|&x| c.ext(f, glsl::F_ABS, &[x])).collect();
        let z_ge_x = c.m.emit(op::F_ORD_GREATER_THAN_EQUAL, b, &[a[2], a[0]]);
        let z_ge_y = c.m.emit(op::F_ORD_GREATER_THAN_EQUAL, b, &[a[2], a[1]]);
        let is_z = c.m.emit(op::LOGICAL_AND, b, &[z_ge_x, z_ge_y]);
        let y_ge_x = c.m.emit(op::F_ORD_GREATER_THAN_EQUAL, b, &[a[1], a[0]]);
        let not_z = c.m.emit(op::LOGICAL_NOT, b, &[is_z]);
        let is_y = c.m.emit(op::LOGICAL_AND, b, &[not_z, y_ge_x]);
        let not_y = c.m.emit(op::LOGICAL_NOT, b, &[is_y]);
        let is_x = c.m.emit(op::LOGICAL_AND, b, &[not_z, not_y]);
        for (k, pick) in [(0, is_x), (1, is_y), (2, is_z)] {
            let unit = c.ext(f, glsl::F_SIGN, &[v[k]]);
            v[k] = c.select(f, pick, unit, v[k]);
        }
    }
    Ok(v)
}

fn with_layer(c: &mut Ctx, s: &Srcs, d: Dim, mut coords: Vec<u32>) -> Res<u32> {
    if d.arrayed() {
        let p = s.get(Slot::Packed)?;
        let l = c.layer_f(p);
        coords.push(l);
    }
    Ok(c.tx_fvec(&coords))
}

fn tex(c: &mut Ctx) -> Res<()> {
    let w = c.word;
    let key = texref(c)?;
    let d = dim_at(w, &TEX_DIMS)?;
    let scr = w.bit(60);
    let lod = w.get(87, 3);
    let aoffi = w.bit(76);
    let dc = w.bit(78);
    if lod >= 6 {
        return bad("TEX.LC.FDV");
    }
    if aoffi && d.cube() {
        return bad("TEX.AOFFI on a cube");
    }
    let lc = matches!(lod, 4 | 5);
    let mut g0 = Vec::new();
    if d.arrayed() || lc {
        g0.push(Slot::Packed);
    }
    g0.extend(coord_slots(d));
    let mut g1 = Vec::new();
    if reg_handle(c, key) {
        g1.push(Slot::Handle);
    }
    if matches!(lod, 2 | 3 | 5) {
        g1.push(Slot::Lod);
    }
    if aoffi {
        g1.push(Slot::Off);
    }
    if dc {
        g1.push(Slot::Dc);
    }
    let s = sources(c, &g0, &g1, scr)?;
    let (h, nu) = handle(c, key, &s)?;
    let frag = c.stage == Stage::Fragment;
    let implicit = frag && matches!(lod, 0 | 2 | 4 | 5);
    let v = c.by_kind(key, !dc, &mut |c, k| {
        let (si, img) = c.sampled(k, d, h, nu);
        let mut coords = float_coords(c, &s, d)?;
        let mut mask = 0u32;
        let mut ops = Vec::new();
        let lod_v = if implicit {
            None
        } else if lod == 3 {
            let l = s.get(Slot::Lod)?;
            Some(c.as_float(l))
        } else {
            Some(c.m.c_f32(0))
        };
        let mut konst = None;
        if aoffi {
            konst = c.known_offset(s.reg(Slot::Off), d, 4, 0);
            if konst.is_none() {
                let o = s.get(Slot::Off)?;
                let offs = c.dyn_offsets(o, d.offsets(), 4, 0);
                let level = match (lod, lod_v) {
                    (3, Some(l)) => {
                        let i = c.tx_i32();
                        let f = c.f32t();
                        let z = c.m.c_f32(0);
                        let l = c.ext(f, glsl::N_MAX, &[l, z]);
                        c.m.emit(op::CONVERT_F_TO_S, i, &[l])
                    }
                    _ => c.m.c_i32(0),
                };
                c.shift_coords(&mut coords, &offs, img, d, level);
            }
        }
        let coord = with_layer(c, &s, d, coords)?;
        if implicit && matches!(lod, 2 | 5) {
            mask |= 1;
            let b = s.get(Slot::Lod)?;
            ops.push(c.as_float(b));
        }
        if let Some(l) = lod_v {
            mask |= 2;
            ops.push(l);
        }
        if let Some(k) = konst {
            mask |= 8;
            ops.push(k);
        }
        if implicit && lc {
            c.cap(42);
            mask |= 0x80;
            let p = s.get(Slot::Packed)?;
            ops.push(c.min_lod(p));
        }
        let mut args = vec![si, coord];
        if dc {
            let r = s.get(Slot::Dc)?;
            args.push(c.as_float(r));
        }
        if mask != 0 {
            args.push(mask);
            args.extend(ops);
        }
        let opc = match (dc, implicit) {
            (false, true) => op::IMAGE_SAMPLE_IMPLICIT_LOD,
            (false, false) => op::IMAGE_SAMPLE_EXPLICIT_LOD,
            (true, true) => op::IMAGE_SAMPLE_DREF_IMPLICIT_LOD,
            (true, false) => op::IMAGE_SAMPLE_DREF_EXPLICIT_LOD,
        };
        if dc {
            let f = c.f32t();
            let r = c.m.emit(opc, f, &args);
            let v4 = c.f4_from_scalar(r);
            Ok(c.texel_bits(v4, Kind::Float))
        } else {
            let st = c.sampled_type(k);
            let t = c.m.t_vec(st, 4);
            let r = c.m.emit(opc, t, &args);
            Ok(c.texel_bits(r, k))
        }
    })?;
    c.no_fault();
    let mask = w.get(72, 4);
    let f16 = w.get(79, 2);
    if f16 == 3 {
        return bad("TEX.F16 mode");
    }
    c.tex_result(v, mask, f16);
    Ok(())
}

fn tld(c: &mut Ctx) -> Res<()> {
    let w = c.word;
    let key = texref(c)?;
    let ms = w.bit(78);
    if w.bit(77) {
        return bad("TLD.CL");
    }
    let lod = w.get(87, 3);
    let aoffi = w.bit(76);
    let d = match (w.get(61, 3), ms) {
        (0, false) => Dim::D1,
        (1, false) => Dim::D2,
        (2, false) => Dim::D3,
        (4, false) => Dim::Array1,
        (5, false) => Dim::Array2,
        (1, true) => Dim::Ms2,
        (5, true) => Dim::ArrayMs2,
        _ => return bad("TLD dimension"),
    };
    let mut g0 = Vec::new();
    if d.arrayed() {
        g0.push(Slot::Packed);
    }
    g0.extend(coord_slots(d));
    let mut g1 = Vec::new();
    if reg_handle(c, key) {
        g1.push(Slot::Handle);
    }
    if lod == 3 {
        g1.push(Slot::Lod);
    }
    if aoffi {
        g1.push(Slot::Off);
    }
    if ms {
        g1.push(Slot::Ms);
    }
    let s = sources(c, &g0, &g1, w.bit(60))?;
    let (h, nu) = handle(c, key, &s)?;
    let v = c.by_kind(key, true, &mut |c, k| {
        let img = c.texture(k, d, h, nu);
        let mut coords = Vec::new();
        for n in 0..d.coords() as u8 {
            let x = s.get(Slot::Coord(n))?;
            coords.push(c.tx_as_i32(x));
        }
        let mut konst = None;
        if aoffi {
            konst = c.known_offset(s.reg(Slot::Off), d, 4, 0);
            if konst.is_none() {
                let o = s.get(Slot::Off)?;
                let offs = c.dyn_offsets(o, d.offsets(), 4, 0);
                let i = c.tx_i32();
                for (n, o) in offs.iter().enumerate() {
                    coords[n] = c.m.emit(op::I_ADD, i, &[coords[n], *o]);
                }
            }
        }
        if d.arrayed() {
            let p = s.get(Slot::Packed)?;
            let l = c.tx_opk(op::BITWISE_AND, p, 0xffff);
            coords.push(c.tx_as_i32(l));
        }
        let coord = c.tx_ivec(&coords);
        let mut args = vec![img, coord];
        let mut mask = 0;
        let mut ops = Vec::new();
        if !ms {
            mask |= 2;
            ops.push(if lod == 3 {
                let l = s.get(Slot::Lod)?;
                c.tx_as_i32(l)
            } else {
                c.m.c_i32(0)
            });
        }
        if let Some(k) = konst {
            mask |= 8;
            ops.push(k);
        }
        if ms {
            mask |= 0x40;
            let m = s.get(Slot::Ms)?;
            ops.push(c.tx_as_i32(m));
        }
        args.push(mask);
        args.extend(ops);
        let st = c.sampled_type(k);
        let t = c.m.t_vec(st, 4);
        let r = c.m.emit(op::IMAGE_FETCH, t, &args);
        Ok(c.texel_bits(r, k))
    })?;
    c.no_fault();
    let f16 = w.get(79, 2);
    if f16 == 3 {
        return bad("TLD.F16 mode");
    }
    c.tex_result(v, w.get(72, 4), f16);
    Ok(())
}

fn tld4(c: &mut Ctx) -> Res<()> {
    let w = c.word;
    let key = texref(c)?;
    let d = match w.get(61, 3) {
        1 => Dim::D2,
        3 => Dim::Cube,
        5 => Dim::Array2,
        7 => Dim::ArrayCube,
        _ => return bad("TLD4 dimension"),
    };
    let comp = w.get(87, 2);
    let offmode = w.get(76, 2);
    let dc = w.bit(78);
    if offmode == 3 || (offmode != 0 && d.cube()) {
        return bad("TLD4 offsets");
    }
    let mut g0 = Vec::new();
    if d.arrayed() {
        g0.push(Slot::Packed);
    }
    g0.extend(coord_slots(d));
    let mut g1 = Vec::new();
    if reg_handle(c, key) {
        g1.push(Slot::Handle);
    }
    match offmode {
        1 => g1.push(Slot::Off),
        2 => g1.extend([Slot::Ptp(0), Slot::Ptp(1)]),
        _ => {}
    }
    if dc {
        g1.push(Slot::Dc);
    }
    let s = sources(c, &g0, &g1, w.bit(60))?;
    let (h, nu) = handle(c, key, &s)?;
    let k_at = (c.pc / 16) as usize;
    let v = c.by_kind(key, !dc, &mut |c, k| {
        c.cap(25);
        let (si, _) = c.sampled(k, d, h, nu);
        let coords = float_coords(c, &s, d)?;
        let coord = with_layer(c, &s, d, coords)?;
        let st = c.sampled_type(k);
        let t = c.m.t_vec(st, 4);
        let mut base = vec![si, coord];
        if dc {
            let r = s.get(Slot::Dc)?;
            base.push(c.as_float(r));
        } else {
            base.push(c.m.c_u32(comp));
        }
        let opc = if dc {
            op::IMAGE_DREF_GATHER
        } else {
            op::IMAGE_GATHER
        };
        let r = match offmode {
            1 => {
                let mut a = base.clone();
                match c.known_offset(s.reg(Slot::Off), d, 8, 0) {
                    Some(k) => a.extend([8, k]),
                    None => {
                        let o = s.get(Slot::Off)?;
                        let offs = c.dyn_offsets(o, 2, 8, 0);
                        let ov = c.tx_ivec(&offs);
                        a.extend([0x10, ov]);
                    }
                }
                c.m.emit(opc, t, &a)
            }
            2 => {
                let p0 = s.reg(Slot::Ptp(0)).map(|r| c.tex.known(k_at, r));
                let p1 = s.reg(Slot::Ptp(1)).map(|r| c.tex.known(k_at, r));
                let known = match (p0, p1) {
                    (Some(Some(a)), Some(Some(b))) => Some((a, b)),
                    (None, None) => Some((0, 0)),
                    (None, Some(Some(b))) => Some((0, b)),
                    (Some(Some(a)), None) => Some((a, 0)),
                    _ => None,
                };
                if let Some((a, b)) = known {
                    let i = c.tx_i32();
                    let iv2 = c.m.t_vec(i, 2);
                    let parts: Vec<u32> = (0..4)
                        .map(|n| {
                            let word = if n < 2 { a } else { b };
                            c.const_offset(word >> (16 * (n % 2)), 2, 8)
                        })
                        .collect();
                    let arr = c.m.t_array(iv2, 4);
                    let k = c.m.c_composite(arr, &parts);
                    let mut a = base.clone();
                    a.extend([0x20, k]);
                    c.m.emit(opc, t, &a)
                } else {
                    let p0 = s.get(Slot::Ptp(0))?;
                    let p1 = s.get(Slot::Ptp(1))?;
                    let mut parts = Vec::new();
                    for n in 0..4u32 {
                        let word = if n < 2 { p0 } else { p1 };
                        let offs = c.dyn_offsets(word, 2, 8, 16 * (n % 2));
                        let ov = c.tx_ivec(&offs);
                        let mut a = base.clone();
                        a.extend([0x10, ov]);
                        let g = c.m.emit(opc, t, &a);
                        parts.push(c.m.emit(op::COMPOSITE_EXTRACT, st, &[g, 3]));
                    }
                    c.m.emit(op::COMPOSITE_CONSTRUCT, t, &parts)
                }
            }
            _ => c.m.emit(opc, t, &base),
        };
        Ok(c.texel_bits(r, k))
    })?;
    c.no_fault();
    let f16 = w.get(79, 2);
    if f16 == 3 {
        return bad("TLD4.F16 mode");
    }
    c.tex_result(v, w.get(72, 4), f16);
    Ok(())
}

fn tmml(c: &mut Ctx) -> Res<()> {
    let w = c.word;
    let key = texref(c)?;
    let d = dim_at(w, &TEX_DIMS)?;
    let mut g0 = Vec::new();
    if d.arrayed() {
        g0.push(Slot::Packed);
    }
    g0.extend(coord_slots(d));
    let g1 = if reg_handle(c, key) {
        vec![Slot::Handle]
    } else {
        Vec::new()
    };
    let s = sources(c, &g0, &g1, false)?;
    let (h, nu) = handle(c, key, &s)?;
    let zero = c.m.c_u32(0);
    let v = if c.stage == Stage::Fragment {
        c.cap(50);
        let (si, _) = c.sampled(Kind::Float, d, h, nu);
        let coords = float_coords(c, &s, d)?;
        let coord = c.tx_fvec(&coords);
        let f = c.f32t();
        let v2 = c.m.t_vec(f, 2);
        let q = c.m.emit(op::IMAGE_QUERY_LOD, v2, &[si, coord]);
        let x = c.m.emit(op::COMPOSITE_EXTRACT, f, &[q, 0]);
        let y = c.m.emit(op::COMPOSITE_EXTRACT, f, &[q, 1]);
        let k256 = c.m.c_f32(256.0f32.to_bits());
        let fz = c.m.c_f32(0);
        let x = c.ext(f, glsl::N_MAX, &[x, fz]);
        let xs = c.m.emit(op::F_MUL, f, &[x, k256]);
        let ys = c.m.emit(op::F_MUL, f, &[y, k256]);
        let i = c.tx_i32();
        let u = c.u32t();
        let yi = c.m.emit(op::CONVERT_F_TO_S, i, &[ys]);
        let yu = c.m.emit(op::BITCAST, u, &[yi]);
        let xu = c.m.emit(op::CONVERT_F_TO_U, u, &[xs]);
        let rel = c.tx_opk(op::BITWISE_AND, yu, 0xffff);
        let abs = c.tx_opk(op::BITWISE_AND, xu, 0xffff);
        c.tx_uvec4_of([rel, abs, zero, zero])
    } else {
        let rel = c.m.c_u32(0xe000);
        c.tx_uvec4_of([rel, zero, zero, zero])
    };
    c.tex_result(v, w.get(72, 4), 0);
    Ok(())
}

fn txd(c: &mut Ctx) -> Res<()> {
    let w = c.word;
    let key = texref(c)?;
    let d = match w.get(61, 3) {
        0 => Dim::D1,
        1 => Dim::D2,
        4 => Dim::Array1,
        5 => Dim::Array2,
        _ => return bad("TXD dimension"),
    };
    if w.bit(87) {
        return bad("TXD.LC");
    }
    let aoffi = w.bit(76);
    let mut g0 = Vec::new();
    if reg_handle(c, key) {
        g0.push(Slot::Handle);
    }
    g0.extend(coord_slots(d));
    if d.arrayed() || aoffi {
        g0.push(Slot::Packed);
    }
    let g1: Vec<Slot> = (0..2 * d.coords() as u8).map(Slot::Grad).collect();
    let s = sources(c, &g0, &g1, false)?;
    let (h, nu) = handle(c, key, &s)?;
    let v = c.by_kind(key, true, &mut |c, k| {
        let (si, img) = c.sampled(k, d, h, nu);
        let mut coords = float_coords(c, &s, d)?;
        let mut konst = None;
        if aoffi {
            konst = c.known_offset(s.reg(Slot::Packed), d, 4, 16);
            if konst.is_none() {
                let p = s.get(Slot::Packed)?;
                let offs = c.dyn_offsets(p, d.offsets(), 4, 16);
                let z = c.m.c_i32(0);
                c.shift_coords(&mut coords, &offs, img, d, z);
            }
        }
        let coord = with_layer(c, &s, d, coords)?;
        let mut dx = Vec::new();
        let mut dy = Vec::new();
        for n in 0..d.coords() as u8 {
            let a = s.get(Slot::Grad(2 * n))?;
            let b = s.get(Slot::Grad(2 * n + 1))?;
            dx.push(c.as_float(a));
            dy.push(c.as_float(b));
        }
        let dx = c.tx_fvec(&dx);
        let dy = c.tx_fvec(&dy);
        let mut args = vec![si, coord];
        match konst {
            Some(k) => args.extend([4 | 8, dx, dy, k]),
            None => args.extend([4, dx, dy]),
        }
        let st = c.sampled_type(k);
        let t = c.m.t_vec(st, 4);
        let r = c.m.emit(op::IMAGE_SAMPLE_EXPLICIT_LOD, t, &args);
        Ok(c.texel_bits(r, k))
    })?;
    c.no_fault();
    let f16 = w.get(79, 2);
    if f16 == 3 {
        return bad("TXD.F16 mode");
    }
    c.tex_result(v, w.get(72, 4), f16);
    Ok(())
}

fn txq(c: &mut Ctx) -> Res<()> {
    let w = c.word;
    let key = texref(c)?;
    let query = w.get(62, 2);
    if query >= 2 {
        return bad("TXQ query");
    }
    let mut g0 = Vec::new();
    if reg_handle(c, key) {
        g0.push(Slot::Handle);
    }
    if query == 0 {
        g0.push(Slot::Lod);
    }
    let s = sources(c, &g0, &[], false)?;
    let (h, nu) = handle(c, key, &s)?;
    let d = key
        .and_then(|k| c.tex.dims.get(&k).copied())
        .unwrap_or(Dim::D2);
    c.cap(50);
    let img = c.texture(Kind::Float, d, h, nu);
    let u = c.u32t();
    let i = c.tx_i32();
    let b = c.boolt();
    let zero = c.m.c_u32(0);
    let one = c.m.c_u32(1);
    let v = if query == 0 {
        let lod = s.get(Slot::Lod)?;
        let n = d.size_len();
        let st = if n == 1 { i } else { c.m.t_vec(i, n as u32) };
        let (size, levels) = if d.ms() {
            (c.m.emit(op::IMAGE_QUERY_SIZE, st, &[img]), one)
        } else {
            let li = c.m.emit(op::IMAGE_QUERY_LEVELS, i, &[img]);
            let lu = c.m.emit(op::BITCAST, u, &[li]);
            let top = c.tx_op(op::I_SUB, lu, one);
            let cl = c.ext(u, glsl::U_MIN, &[lod, top]);
            let cli = c.m.emit(op::BITCAST, i, &[cl]);
            (c.m.emit(op::IMAGE_QUERY_SIZE_LOD, st, &[img, cli]), lu)
        };
        let ok = c.m.emit(op::U_LESS_THAN, b, &[lod, levels]);
        let mut parts = Vec::new();
        for k in 0..n {
            let x = if n == 1 {
                size
            } else {
                c.m.emit(op::COMPOSITE_EXTRACT, i, &[size, k as u32])
            };
            let x = c.m.emit(op::BITCAST, u, &[x]);
            parts.push(c.select(u, ok, x, zero));
        }
        if d == Dim::D1 {
            parts.push(c.select(u, ok, one, zero));
        }
        while parts.len() < 3 {
            parts.push(zero);
        }
        c.tx_uvec4_of([parts[0], parts[1], parts[2], levels])
    } else {
        let samples = if d.ms() {
            let si = c.m.emit(op::IMAGE_QUERY_SAMPLES, i, &[img]);
            c.m.emit(op::BITCAST, u, &[si])
        } else {
            one
        };
        c.tx_uvec4_of([zero, zero, samples, zero])
    };
    c.tex_result(v, w.get(72, 4), 0);
    Ok(())
}

const SURF_DIMS: [ImageDim; 6] = [
    ImageDim::D1,
    ImageDim::Buffer,
    ImageDim::Array1,
    ImageDim::D2,
    ImageDim::Array2,
    ImageDim::D3,
];

impl ImageDim {
    fn coords(self) -> usize {
        match self {
            ImageDim::D1 | ImageDim::Buffer => 1,
            ImageDim::Array1 | ImageDim::D2 => 2,
            ImageDim::Array2 | ImageDim::D3 => 3,
        }
    }
}

fn image_type(c: &mut Ctx, k: ImageKind, d: ImageDim) -> u32 {
    let st = match k {
        ImageKind::Float => c.f32t(),
        ImageKind::Sint => c.m.t_i32(),
        _ => c.u32t(),
    };
    let (dim, arrayed) = match d {
        ImageDim::D1 => (0, 0),
        ImageDim::Array1 => (0, 1),
        ImageDim::Buffer => (5, 0),
        ImageDim::D2 => (1, 0),
        ImageDim::Array2 => (1, 1),
        ImageDim::D3 => (2, 0),
    };
    match d {
        ImageDim::D1 | ImageDim::Array1 => c.cap(44),
        ImageDim::Buffer => c.cap(47),
        _ => {}
    }
    let format = if k == ImageKind::Atomic32 { 33 } else { 0 };
    c.m.t_image([st, dim, 0, arrayed, 0, 2, format])
}

fn surface(c: &mut Ctx, i: &Inst) -> Res<()> {
    let w = c.word;
    let opc = w.get(0, 12);
    let (bound, kind_bits) = match opc {
        0x998 | 0x99a | 0x99c | 0x99e | 0x394 | 0x3a0 => (false, opc),
        0xb97 | 0xb99 | 0xb9b | 0xb9d | 0x593 | 0x59f => (true, opc),
        _ => return bad(format!("{} handle form", i.mnemonic)),
    };
    let formatted = matches!(kind_bits, 0x998 | 0x99c | 0xb97 | 0xb9b);
    let load = matches!(kind_bits, 0x998 | 0x99a | 0xb97 | 0xb99);
    let store = matches!(kind_bits, 0x99c | 0x99e | 0xb9b | 0xb9d);
    let red = matches!(kind_bits, 0x3a0 | 0x59f);
    let d = match SURF_DIMS.get(w.get(61, 3) as usize) {
        Some(d) => *d,
        None => return bad("surface dimension"),
    };
    let key = if bound {
        Some((w.get(54, 5), 4 * w.get(40, 14)))
    } else {
        None
    };
    let ra = w.get(24, 8) as u8;
    let rb = w.get(32, 8) as u8;
    let rd = w.get(16, 8) as u8;
    let h = match key {
        Some((bank, off)) => {
            c.iface.textures.insert((bank, off));
            c.load_cbuf(bank, i64::from(off))?
        }
        None => {
            let r = w.get(64, 8) as u8;
            if r == 255 {
                return bad("surface handle RZ");
            }
            c.load_reg(r)
        }
    };
    let nu = key.is_none();
    let reg_at = |c: &mut Ctx, base: u8, k: usize| -> Res<u32> {
        if base == 255 {
            return Ok(c.m.c_u32(0));
        }
        let r = usize::from(base) + k;
        if r >= 255 {
            return bad("surface operand past R254");
        }
        Ok(c.load_reg(r as u8))
    };
    let mut coords = Vec::new();
    for k in 0..d.coords() {
        let x = reg_at(c, ra, k)?;
        coords.push(x);
    }
    let size = if formatted || !w.bit(72) {
        1
    } else if !(load || store) {
        4
    } else {
        match w.get(73, 3) {
            0 | 1 => 1,
            2 | 3 => 2,
            5 => 8,
            6 => 16,
            _ => 4,
        }
    };
    if size > 1 {
        coords[0] = c.tx_opk(op::U_DIV, coords[0], size);
    }
    let ci: Vec<u32> = coords.iter().map(|&x| c.tx_as_i32(x)).collect();
    let coord = c.tx_ivec(&ci);
    if nu {
        c.cap(5301);
        c.cap(5309);
    }
    let idx = c.tx_opk(op::BITWISE_AND, h, 0xf_ffff);
    c.no_fault();
    if formatted {
        let n = match w.get(72, 4) {
            1 => 1,
            3 => 2,
            15 => 4,
            _ => return bad("surface channels"),
        };
        let mut data = Vec::new();
        if store {
            for k in 0..n {
                let x = reg_at(c, rb, k)?;
                data.push(x);
            }
        }
        let v = c.by_kind(key, true, &mut |c, k| {
            let ik = match k {
                Kind::Float => ImageKind::Float,
                Kind::Sint => ImageKind::Sint,
                Kind::Uint => ImageKind::Uint,
            };
            let ty = image_type(c, ik, d);
            let img = c.descriptor(image_binding(ik, d), ty, idx, nu);
            let st = c.sampled_type(k);
            let t = c.m.t_vec(st, 4);
            if load {
                c.cap(55);
                let r = c.m.emit(op::IMAGE_READ, t, &[img, coord]);
                Ok(c.texel_bits(r, k))
            } else {
                c.cap(56);
                let zero = c.m.c_u32(0);
                let mut parts = data.clone();
                while parts.len() < 4 {
                    parts.push(zero);
                }
                let uv = c.tx_uvec4_of([parts[0], parts[1], parts[2], parts[3]]);
                let tv = if k == Kind::Uint {
                    uv
                } else {
                    c.m.emit(op::BITCAST, t, &[uv])
                };
                c.m.emit_void(op::IMAGE_WRITE, &[img, coord, tv]);
                Ok(uv)
            }
        })?;
        if load {
            let u = c.u32t();
            for k in 0..n {
                let x = c.m.emit(op::COMPOSITE_EXTRACT, u, &[v, k as u32]);
                if rd != 255 {
                    c.store_reg(rd + k as u8, x);
                }
            }
        }
        return Ok(());
    }
    if load || store {
        let t = w.get(73, 3);
        let (ik, n) = match t {
            0 | 1 => (ImageKind::Raw8, 1),
            2 | 3 => (ImageKind::Raw16, 1),
            4 => (ImageKind::Raw32, 1),
            5 => (ImageKind::Raw64, 2),
            6 => (ImageKind::Raw128, 4),
            _ => return bad("surface type"),
        };
        let ty = image_type(c, ik, d);
        let img = c.descriptor(image_binding(ik, d), ty, idx, nu);
        let t4 = c.tx_uvec4();
        let u = c.u32t();
        if load {
            c.cap(55);
            let r = c.m.emit(op::IMAGE_READ, t4, &[img, coord]);
            for k in 0..n {
                let mut x = c.m.emit(op::COMPOSITE_EXTRACT, u, &[r, k as u32]);
                if t == 1 || t == 3 {
                    let i = c.tx_i32();
                    let xi = c.tx_as_i32(x);
                    let o = c.m.c_u32(0);
                    let wd = c.m.c_u32(if t == 1 { 8 } else { 16 });
                    let s = c.m.emit(op::BIT_FIELD_S_EXTRACT, i, &[xi, o, wd]);
                    x = c.m.emit(op::BITCAST, u, &[s]);
                }
                if rd != 255 {
                    c.store_reg(rd + k as u8, x);
                }
            }
        } else {
            c.cap(56);
            let zero = c.m.c_u32(0);
            let mut parts = Vec::new();
            for k in 0..n {
                parts.push(reg_at(c, rb, k)?);
            }
            while parts.len() < 4 {
                parts.push(zero);
            }
            let uv = c.tx_uvec4_of([parts[0], parts[1], parts[2], parts[3]]);
            c.m.emit_void(op::IMAGE_WRITE, &[img, coord, uv]);
        }
        return Ok(());
    }
    let aop = w.get(87, if red { 3 } else { 4 });
    let ty = w.get(73, 3);
    if !matches!(ty, 0 | 1) {
        return bad("SUATOM type");
    }
    let signed = ty == 1;
    let opc = match aop {
        0 => op::ATOMIC_I_ADD,
        1 if signed => op::ATOMIC_S_MIN,
        1 => op::ATOMIC_U_MIN,
        2 if signed => op::ATOMIC_S_MAX,
        2 => op::ATOMIC_U_MAX,
        5 => op::ATOMIC_AND,
        6 => op::ATOMIC_OR,
        7 => op::ATOMIC_XOR,
        8 => op::ATOMIC_EXCHANGE,
        _ => return bad("SUATOM operation"),
    };
    let ity = image_type(c, ImageKind::Atomic32, d);
    let ptr = c.descriptor_ptr(image_binding(ImageKind::Atomic32, d), ity, idx, nu);
    let u = c.u32t();
    let pt = c.m.t_ptr(IMAGE_CLASS, u);
    let zero = c.m.c_u32(0);
    let tp = c.m.emit(op::IMAGE_TEXEL_POINTER, pt, &[ptr, coord, zero]);
    if nu {
        c.m.deco(tp, NON_UNIFORM, &[]);
    }
    let data = reg_at(c, rb, 0)?;
    let scope = c.m.c_u32(1);
    let r = c.m.emit(opc, u, &[tp, scope, zero, data]);
    if !red && rd != 255 {
        c.store_reg(rd, r);
    }
    Ok(())
}

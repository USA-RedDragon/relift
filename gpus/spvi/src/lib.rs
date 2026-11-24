//! A small interpreter for the SPIR-V the translator emits for compute
//! programs: one invocation, logical variables, uniform buffers and
//! physical storage buffer pointers, 32-bit floats and 32/64-bit integers.
//!
//! Where Vulkan leaves a result open, the interpreter picks one a real
//! device could give, chosen to expose a translation that relies on it:
//! NaN results of float arithmetic have an odd payload and sign
//! ([`ODD_NAN`]), out-of-range float-to-integer conversions and shifts by
//! the bit width or more return garbage, and NMin/NMax of equal operands
//! (+0 and -0) return the second. Denormal handling follows [`Denorm`].
//! Anything outside the subset panics, so a test never passes by skipping
//! an operation.
//!
//! Each function is compiled once ([`code`]) to flat code over one
//! register file per invocation, which [`Run`] runs. [`group::run_workgroup`]
//! runs a whole workgroup with barriers, subgroup shuffles and shared
//! memory, its invocations taking turns on one thread, and
//! [`group::run_dispatch`] the workgroups of a dispatch on several threads.
//! [`reference`] is the tree-walking interpreter the compiled one is
//! checked against.
//!
//! Images are arrays of [`Image`]s at (set, binding, element)
//! ([`Env::images`]) whose texels lie in the memory: fetches, reads, writes
//! and size and level queries of the formats [`Image::texel_bytes`] lists.
//! Texels outside the image read as zero and writes to them are dropped,
//! as robust image access gives. Filtered sampling panics. The reference
//! interpreter runs no image instructions.

mod arith;
mod code;
mod exec;
pub mod group;
pub mod reference;

use std::collections::HashMap;
use std::rc::Rc;
use std::sync::OnceLock;

/// What arithmetic NaN results read as (any NaN is allowed).
pub const ODD_NAN: u32 = 0xffd5_a5a5;
pub(crate) const GARBAGE: u64 = 0xa5a5_a5a5_a5a5_a5a5;

/// How the device treats 32-bit denormals.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Denorm {
    /// IEEE 754: kept.
    Preserve,
    /// Denormal inputs read as signed zero; results are rounded to 24 bits
    /// with an unbounded exponent and flushed below the normal range (SM86
    /// `.FTZ`).
    FlushBefore,
    /// Denormal inputs read as signed zero; results are rounded to the
    /// denormal grid, then flushed if denormal.
    FlushAfter,
}

#[derive(Clone, Debug)]
enum Ty {
    Void,
    Bool,
    Int(u32),
    Float(u32),
    Vec(u32, u32),
    Array(u32, u64),
    RtArray(u32),
    Struct(Vec<u32>),
    /// The pointee type.
    Ptr(u32),
    /// A storage or sampled image, or a sampled image (image and sampler):
    /// two registers, as a pointer ([`code`]'s image tag and the element).
    Image,
    Other,
}

/// An image whose texels lie in the memory: each level in turn from
/// `addr`, in a level each layer (or depth slice) in turn, rows of texels
/// without padding.
#[derive(Clone, Debug)]
pub struct Image {
    pub addr: u64,
    pub width: u32,
    pub height: u32,
    /// Depth slices of a 3D image, else 1.
    pub depth: u32,
    pub layers: u32,
    pub levels: u32,
    /// The VkFormat number: 9 R8 unorm, 16 R8G8 unorm, 37 R8G8B8A8 unorm,
    /// 44 B8G8R8A8 unorm, 64 A2B10G10R10 unorm, 76 R16 float, 83 R16G16
    /// float, 97 R16G16B16A16 float, 98 R32 uint, 99 R32 sint, 100 R32
    /// float, 103 R32G32 float, 107 R32G32B32A32 uint, 109 R32G32B32A32
    /// float.
    pub format: u32,
}

impl Image {
    /// Bytes per texel, or `None` for a format the interpreter lacks.
    pub fn texel_bytes(format: u32) -> Option<u64> {
        Some(match format {
            9 => 1,
            16 | 76 => 2,
            37 | 44 | 64 | 83 | 98 | 99 | 100 => 4,
            97 | 103 => 8,
            107 | 109 => 16,
            _ => return None,
        })
    }

    fn dims(&self, level: u32) -> (u32, u32, u32) {
        let f = |v: u32| (v >> level).max(1);
        let d = if self.depth > 1 {
            f(self.depth)
        } else {
            self.layers.max(1)
        };
        (f(self.width), f(self.height), d)
    }

    /// The address of texel (x, y, z) of `level`, z a layer or depth slice;
    /// `None` outside the image.
    fn texel(&self, x: i64, y: i64, z: i64, level: u32) -> Option<u64> {
        let bpp = Self::texel_bytes(self.format)?;
        if level >= self.levels {
            return None;
        }
        let (w, h, d) = self.dims(level);
        if x < 0 || y < 0 || z < 0 || x >= i64::from(w) || y >= i64::from(h) || z >= i64::from(d) {
            return None;
        }
        let mut at = self.addr;
        for l in 0..level {
            let (lw, lh, ld) = self.dims(l);
            at += u64::from(lw) * u64::from(lh) * u64::from(ld) * bpp;
        }
        Some(at + ((z as u64 * u64::from(h) + y as u64) * u64::from(w) + x as u64) * bpp)
    }
}

/// The four channels of a texel's bits: float formats give f32 bits,
/// unorm formats their value as f32 bits, integer formats the integers.
fn texel_decode(format: u32, raw: u128) -> [u64; 4] {
    let f = |x: f32| u64::from(x.to_bits());
    let un = |v: u128, bits: u32| f((v & ((1 << bits) - 1)) as f32 / ((1u32 << bits) - 1) as f32);
    let one = f(1.0);
    let h = |v: u128| f(f16_to_f64(v as u16) as f32);
    match format {
        9 => [un(raw, 8), 0, 0, one],
        16 => [un(raw, 8), un(raw >> 8, 8), 0, one],
        37 => [
            un(raw, 8),
            un(raw >> 8, 8),
            un(raw >> 16, 8),
            un(raw >> 24, 8),
        ],
        44 => [
            un(raw >> 16, 8),
            un(raw >> 8, 8),
            un(raw, 8),
            un(raw >> 24, 8),
        ],
        64 => [
            un(raw, 10),
            un(raw >> 10, 10),
            un(raw >> 20, 10),
            un(raw >> 30, 2),
        ],
        76 => [h(raw), 0, 0, one],
        83 => [h(raw), h(raw >> 16), 0, one],
        97 => [h(raw), h(raw >> 16), h(raw >> 32), h(raw >> 48)],
        98..=100 => [raw as u32 as u64, 0, 0, if format == 100 { one } else { 1 }],
        103 => [raw as u32 as u64, (raw >> 32) as u32 as u64, 0, one],
        107 | 109 => [0, 1, 2, 3].map(|k| (raw >> (32 * k)) as u32 as u64),
        _ => panic!("texel format {format}"),
    }
}

/// A texel's bits from four channels as [`texel_decode`] gives them;
/// unorm channels round to nearest after clamping.
fn texel_encode(format: u32, c: [u64; 4]) -> u128 {
    let un = |v: u64, bits: u32| -> u128 {
        let x = f32::from_bits(v as u32);
        let max = ((1u32 << bits) - 1) as f32;
        let x = if x.is_nan() { 0.0 } else { x.clamp(0.0, 1.0) };
        (x * max + 0.5).floor() as u128
    };
    let h = |v: u64| u128::from(round_f16(f64::from(f32::from_bits(v as u32)), 0.0));
    match format {
        9 => un(c[0], 8),
        16 => un(c[0], 8) | un(c[1], 8) << 8,
        37 => un(c[0], 8) | un(c[1], 8) << 8 | un(c[2], 8) << 16 | un(c[3], 8) << 24,
        44 => un(c[2], 8) | un(c[1], 8) << 8 | un(c[0], 8) << 16 | un(c[3], 8) << 24,
        64 => un(c[0], 10) | un(c[1], 10) << 10 | un(c[2], 10) << 20 | un(c[3], 2) << 30,
        76 => h(c[0]),
        83 => h(c[0]) | h(c[1]) << 16,
        97 => h(c[0]) | h(c[1]) << 16 | h(c[2]) << 32 | h(c[3]) << 48,
        98..=100 => u128::from(c[0] as u32),
        103 => u128::from(c[0] as u32) | u128::from(c[1] as u32) << 32,
        107 | 109 => (0..4).fold(0, |acc, k| acc | u128::from(c[k] as u32) << (32 * k)),
        _ => panic!("texel format {format}"),
    }
}

#[derive(Clone, Debug)]
pub enum V {
    Undef,
    S(u64),
    C(Rc<Vec<V>>),
    P(Ptr),
}

#[derive(Clone, Debug)]
pub enum Ptr {
    /// A logical variable (slot) and a path into it.
    Var(usize, Rc<Vec<u64>>),
    /// Explicitly laid out memory: address and pointee type.
    Mem(u64, u32),
    /// An array of buffer descriptors at (set, binding), its element type:
    /// the first index of an access chain picks the buffer
    /// ([`Env::arrays`]).
    Desc(u32, u32, u32),
}

impl V {
    fn s(&self) -> u64 {
        match self {
            V::S(x) => *x,
            V::Undef => GARBAGE,
            v => panic!("scalar expected, got {v:?}"),
        }
    }
    fn c(&self) -> &[V] {
        match self {
            V::C(v) => v,
            v => panic!("composite expected, got {v:?}"),
        }
    }
}

struct Inst {
    op: u16,
    ty: u32,
    id: u32,
    args: Vec<u32>,
}

struct Func {
    params: Vec<u32>,
    /// label id -> index of the label instruction
    labels: HashMap<u32, usize>,
    code: Vec<Inst>,
}

pub struct Module {
    bound: usize,
    types: HashMap<u32, Ty>,
    consts: Vec<(u32, V)>,
    globals: Vec<(u32, u32, u32, Option<u32>)>, // id, type, class, init
    deco: HashMap<(u32, u32), Vec<u32>>,        // (id, decoration) -> args
    member_off: HashMap<(u32, u32), u64>,
    /// The result type of every id that has one.
    id_ty: Vec<u32>,
    funcs: HashMap<u32, Func>,
    entry: u32,
    pub modes: Vec<u32>,
    /// The compiled code, made on first use.
    program: OnceLock<code::Program>,
}

// Opcodes.
const OP_EXT_INST_IMPORT: u16 = 11;
const OP_EXT_INST: u16 = 12;
const OP_ENTRY_POINT: u16 = 15;
const OP_EXECUTION_MODE: u16 = 16;
const OP_TYPE_VOID: u16 = 19;
const OP_TYPE_BOOL: u16 = 20;
const OP_TYPE_INT: u16 = 21;
const OP_TYPE_FLOAT: u16 = 22;
const OP_TYPE_VECTOR: u16 = 23;
const OP_TYPE_ARRAY: u16 = 28;
const OP_TYPE_RUNTIME_ARRAY: u16 = 29;
const OP_TYPE_STRUCT: u16 = 30;
const OP_TYPE_POINTER: u16 = 32;
const OP_CONSTANT_TRUE: u16 = 41;
const OP_CONSTANT_FALSE: u16 = 42;
const OP_CONSTANT: u16 = 43;
const OP_CONSTANT_COMPOSITE: u16 = 44;
const OP_CONSTANT_NULL: u16 = 46;
const OP_SPEC_CONSTANT_TRUE: u16 = 48;
const OP_SPEC_CONSTANT_FALSE: u16 = 49;
const OP_SPEC_CONSTANT: u16 = 50;
const OP_SPEC_CONSTANT_COMPOSITE: u16 = 51;
const OP_FUNCTION: u16 = 54;
const OP_FUNCTION_PARAMETER: u16 = 55;
const OP_FUNCTION_END: u16 = 56;
const OP_VARIABLE: u16 = 59;
const OP_DECORATE: u16 = 71;
const OP_MEMBER_DECORATE: u16 = 72;

/// Ops with a result type and id (everything else that matters has none).
fn has_result(op: u16) -> bool {
    !matches!(
        op,
        0 | 3
            | 5
            | 6
            | 7
            | 10
            | 11
            | 14
            | 15
            | 16
            | 17
            | 56
            | 62
            | 63
            | 71
            | 72
            | 99
            | 224
            | 225
            | 228
            | 246
            | 247
            | 249
            | 250
            | 251
            | 252
            | 253
            | 254
            | 255
            | 256
            | 4416
    ) && !(19..=39).contains(&op)
}

impl Module {
    pub fn parse(w: &[u32]) -> Module {
        assert_eq!(w[0], 0x0723_0203, "magic");
        let mut m = Module {
            bound: w[3] as usize,
            types: HashMap::new(),
            consts: Vec::new(),
            globals: Vec::new(),
            deco: HashMap::new(),
            member_off: HashMap::new(),
            id_ty: vec![0; w[3] as usize],
            funcs: HashMap::new(),
            entry: 0,
            modes: Vec::new(),
            program: OnceLock::new(),
        };
        let mut i = 5;
        let mut cur: Option<(u32, Func)> = None;
        // Imports of non-semantic instruction sets (debug info), whose
        // instructions change nothing the program computes.
        let mut non_semantic = Vec::new();
        while i < w.len() {
            let n = (w[i] >> 16) as usize;
            let op = (w[i] & 0xffff) as u16;
            let a = &w[i + 1..i + n];
            i += n;
            if (41..=51).contains(&op) || op == OP_VARIABLE || op == OP_FUNCTION_PARAMETER {
                m.id_ty[a[1] as usize] = a[0];
            }
            match op {
                OP_TYPE_VOID => {
                    m.types.insert(a[0], Ty::Void);
                }
                OP_TYPE_BOOL => {
                    m.types.insert(a[0], Ty::Bool);
                }
                OP_TYPE_INT => {
                    m.types.insert(a[0], Ty::Int(a[1]));
                }
                OP_TYPE_FLOAT => {
                    m.types.insert(a[0], Ty::Float(a[1]));
                }
                OP_TYPE_VECTOR => {
                    m.types.insert(a[0], Ty::Vec(a[1], a[2]));
                }
                OP_TYPE_ARRAY => {
                    let len = m.const_u64(a[2]);
                    m.types.insert(a[0], Ty::Array(a[1], len));
                }
                OP_TYPE_RUNTIME_ARRAY => {
                    m.types.insert(a[0], Ty::RtArray(a[1]));
                }
                OP_TYPE_STRUCT => {
                    m.types.insert(a[0], Ty::Struct(a[1..].to_vec()));
                }
                OP_TYPE_POINTER => {
                    m.types.insert(a[0], Ty::Ptr(a[2]));
                }
                25 | 27 => {
                    m.types.insert(a[0], Ty::Image);
                }
                op if (19..=39).contains(&op) => {
                    m.types.insert(a[0], Ty::Other);
                }
                OP_CONSTANT_TRUE | OP_SPEC_CONSTANT_TRUE => m.consts.push((a[1], V::S(1))),
                OP_CONSTANT_FALSE | OP_SPEC_CONSTANT_FALSE => m.consts.push((a[1], V::S(0))),
                OP_CONSTANT | OP_SPEC_CONSTANT => {
                    let v = if a.len() > 3 {
                        u64::from(a[2]) | (u64::from(a[3]) << 32)
                    } else {
                        u64::from(a[2])
                    };
                    m.consts.push((a[1], V::S(v)));
                }
                OP_CONSTANT_COMPOSITE | OP_SPEC_CONSTANT_COMPOSITE => {
                    let parts = a[2..].iter().map(|&p| m.const_val(p)).collect();
                    m.consts.push((a[1], V::C(Rc::new(parts))));
                }
                OP_CONSTANT_NULL => {
                    let v = m.zero(a[0]);
                    m.consts.push((a[1], v));
                }
                OP_DECORATE => {
                    m.deco.insert((a[0], a[1]), a[2..].to_vec());
                }
                OP_MEMBER_DECORATE => {
                    if a[2] == 35 {
                        m.member_off.insert((a[0], a[1]), u64::from(a[3]));
                    }
                }
                OP_ENTRY_POINT => m.entry = a[1],
                OP_EXT_INST_IMPORT => {
                    let name: Vec<u8> = a[1..].iter().flat_map(|x| x.to_le_bytes()).collect();
                    if name.starts_with(b"NonSemantic.") {
                        non_semantic.push(a[0]);
                    }
                }
                OP_EXT_INST if non_semantic.contains(&a[2]) => {}
                OP_EXECUTION_MODE => m.modes.push(a[1]),
                OP_VARIABLE if cur.is_none() => {
                    m.globals.push((a[1], a[0], a[2], a.get(3).copied()));
                }
                OP_FUNCTION => {
                    cur = Some((
                        a[1],
                        Func {
                            params: Vec::new(),
                            labels: HashMap::new(),
                            code: Vec::new(),
                        },
                    ));
                }
                OP_FUNCTION_PARAMETER => cur.as_mut().unwrap().1.params.push(a[1]),
                OP_FUNCTION_END => {
                    let (id, f) = cur.take().unwrap();
                    m.funcs.insert(id, f);
                }
                _ => {
                    if let Some((_, f)) = cur.as_mut() {
                        if op == 248 {
                            f.labels.insert(a[0], f.code.len());
                        }
                        let (ty, id, args) = if has_result(op) && op != 248 {
                            m.id_ty[a[1] as usize] = a[0];
                            (a[0], a[1], a[2..].to_vec())
                        } else {
                            (0, 0, a.to_vec())
                        };
                        f.code.push(Inst { op, ty, id, args });
                    }
                }
            }
        }
        m
    }

    /// The (set, binding) of every uniform and storage block and
    /// descriptor array the module declares.
    pub fn buffers(&self) -> Vec<(u32, u32)> {
        self.globals
            .iter()
            .filter(|g| matches!(g.2, 2 | 12))
            .filter_map(|g| self.binding(g.0))
            .collect()
    }

    /// Whether the module declares descriptor array `binding`.
    pub fn is_array(&self, binding: (u32, u32)) -> bool {
        self.globals.iter().any(|&(id, ty, class, _)| {
            matches!(class, 2 | 12)
                && self.binding(id) == Some(binding)
                && matches!(self.types.get(&ty), Some(Ty::Ptr(p)) if matches!(self.types.get(p), Some(Ty::Array(..) | Ty::RtArray(_))))
        })
    }

    /// The compiled code.
    pub(crate) fn program(&self) -> &code::Program {
        self.program.get_or_init(|| code::compile(self))
    }

    fn const_val(&self, id: u32) -> V {
        self.consts
            .iter()
            .rev()
            .find(|c| c.0 == id)
            .unwrap_or_else(|| panic!("constant %{id}"))
            .1
            .clone()
    }

    fn const_u64(&self, id: u32) -> u64 {
        self.const_val(id).s()
    }

    fn ty(&self, id: u32) -> &Ty {
        self.types.get(&id).unwrap_or_else(|| panic!("type %{id}"))
    }

    fn zero(&self, ty: u32) -> V {
        match self.ty(ty).clone() {
            Ty::Vec(e, n) => V::C(Rc::new((0..n).map(|_| self.zero(e)).collect())),
            Ty::Array(e, n) => V::C(Rc::new((0..n).map(|_| self.zero(e)).collect())),
            Ty::Struct(ms) => V::C(Rc::new(ms.iter().map(|&t| self.zero(t)).collect())),
            _ => V::S(0),
        }
    }

    fn width(&self, ty: u32) -> u32 {
        match *self.ty(ty) {
            Ty::Int(w) | Ty::Float(w) => w,
            Ty::Bool => 1,
            Ty::Vec(e, _) => self.width(e),
            ref t => panic!("width of {t:?}"),
        }
    }

    fn scalar_of(&self, ty: u32) -> u32 {
        match *self.ty(ty) {
            Ty::Vec(e, _) => e,
            _ => ty,
        }
    }

    fn size(&self, ty: u32) -> u64 {
        match self.ty(ty).clone() {
            Ty::Int(w) | Ty::Float(w) => u64::from(w / 8),
            Ty::Vec(e, n) => self.size(e) * u64::from(n),
            Ty::Array(e, n) => self.stride(ty, e) * n,
            Ty::Struct(ms) => ms
                .iter()
                .enumerate()
                .map(|(k, &t)| self.member_off[&(ty, k as u32)] + self.size(t))
                .max()
                .unwrap_or(0),
            t => panic!("size of {t:?}"),
        }
    }

    fn stride(&self, arr: u32, elem: u32) -> u64 {
        self.deco
            .get(&(arr, 6))
            .map_or_else(|| self.size(elem), |a| u64::from(a[0]))
    }

    /// The (set, binding) of a variable.
    fn binding(&self, id: u32) -> Option<(u32, u32)> {
        Some((self.deco.get(&(id, 34))?[0], self.deco.get(&(id, 33))?[0]))
    }

    fn builtin(&self, id: u32) -> Option<u32> {
        self.deco.get(&(id, 11)).map(|a| a[0])
    }
}

/// Sets the default of the specialization constant with SpecId `spec` (a
/// 32-bit OpSpecConstant) to `value`, as a pipeline's specialization info
/// does; false when the module has none.
pub fn specialize(words: &mut [u32], spec: u32, value: u32) -> bool {
    let mut target = None;
    let mut i = 5;
    while i < words.len() {
        let n = (words[i] >> 16) as usize;
        if words[i] & 0xffff == u32::from(OP_DECORATE)
            && n == 4
            && words[i + 2] == 1
            && words[i + 3] == spec
        {
            target = Some(words[i + 1]);
        }
        if words[i] & 0xffff == u32::from(OP_SPEC_CONSTANT)
            && n == 4
            && Some(words[i + 2]) == target
        {
            words[i + 3] = value;
            return true;
        }
        i += n.max(1);
    }
    false
}

/// Flat memory for explicitly laid out data: (base, bytes) regions.
#[derive(Clone, Default)]
pub struct Memory {
    pub regions: Vec<(u64, Vec<u8>)>,
}

impl Memory {
    fn find(&mut self, addr: u64, n: u64) -> Option<&mut [u8]> {
        for (base, b) in &mut self.regions {
            if addr >= *base && addr + n <= *base + b.len() as u64 {
                let o = (addr - *base) as usize;
                return Some(&mut b[o..o + n as usize]);
            }
        }
        None
    }
    fn at(&mut self, addr: u64, n: u64) -> &mut [u8] {
        self.find(addr, n)
            .unwrap_or_else(|| panic!("access to unmapped memory {addr:#x}+{n}"))
    }
    pub fn read(&mut self, addr: u64, n: u64) -> u64 {
        let b = self.at(addr, n);
        let mut v = [0u8; 8];
        v[..n as usize].copy_from_slice(b);
        u64::from_le_bytes(v)
    }
    pub fn write(&mut self, addr: u64, n: u64, v: u64) {
        self.at(addr, n)
            .copy_from_slice(&v.to_le_bytes()[..n as usize]);
    }
}

/// One invocation's inputs.
#[derive(Clone)]
pub struct Env {
    pub denorm: Denorm,
    /// Single-precision arithmetic writes `0x7fffffff` for every NaN result
    /// (a host with `HostProfile::canonical_nan`), not [`ODD_NAN`].
    pub canonical_nan: bool,
    /// A single-precision multiplication gives every zero result the sign
    /// plus (Apple GPUs).
    pub mul_zero_positive: bool,
    /// Contents of uniform blocks by (set, binding), placed in memory.
    pub uniforms: HashMap<(u32, u32), u64>,
    /// Buffers of descriptor arrays by (set, binding, element), placed in
    /// memory.
    pub arrays: HashMap<(u32, u32, u64), u64>,
    /// Images by (set, binding, element) ([`Image`]).
    pub images: HashMap<(u32, u32, u64), Image>,
    /// A workgroup run logs every store outside the workgroup memory
    /// ([`group::Store`]).
    pub log_stores: bool,
    pub local_id: [u32; 3],
    pub group_id: [u32; 3],
    /// The invocation's index in its workgroup (x fastest).
    pub local_index: u32,
    /// Invocations per subgroup.
    pub subgroup_size: u32,
    /// Fold as drivers do: a 32-bit OpFMul by a constant 1 or -1 gives the
    /// other operand (negated for -1) and an OpFAdd of a constant -0 the
    /// other operand, without arithmetic, so no denormal flush (constants
    /// reach a called function's parameters as the driver inlines it).
    pub fold_identity: bool,
}

/// One invocation, run on its own: barriers do nothing and a shuffle
/// fails.
pub struct Run<'m> {
    m: &'m Module,
    env: &'m Env,
    pub mem: Memory,
    pub steps: u64,
    /// The most instructions the invocation may run.
    pub max_steps: u64,
}

impl<'m> Run<'m> {
    pub fn new(m: &'m Module, env: &'m Env, mem: Memory) -> Run<'m> {
        Run {
            m,
            env,
            mem,
            steps: 0,
            max_steps: 50_000_000,
        }
    }

    /// Runs the entry point.
    pub fn main(&mut self) {
        let p = self.m.program();
        let mut inv = exec::Inv::new(p, self.env, None);
        inv.max_steps = self.max_steps;
        let mut mem = exec::Plain {
            mem: &mut self.mem,
            hint: 0,
        };
        inv.run(p, &arith::Fx::of(self.env), self.env, &mut mem, false);
        self.steps = inv.steps;
    }
}

/// Ones in the low `w` bits.
pub(crate) fn mask(w: u32) -> u64 {
    if w >= 64 { u64::MAX } else { (1u64 << w) - 1 }
}

/// `v` sign-extended from `w` bits.
pub(crate) fn sext(v: u64, w: u32) -> i64 {
    if w >= 64 {
        v as i64
    } else {
        ((v << (64 - w)) as i64) >> (64 - w)
    }
}

/// Knuth's TwoSum: s + e == a + b exactly (no overflow in f64 here).
pub fn two_sum(a: f64, b: f64) -> (f64, f64) {
    let s = a + b;
    if !s.is_finite() {
        return (s, 0.0);
    }
    let bb = s - a;
    let aa = s - bb;
    (s, (a - aa) + (b - bb))
}

/// `s + e` correctly rounded to f32 (to nearest even), with `denorm`.
pub fn round_f32(s: f64, e: f64, denorm: Denorm) -> u32 {
    if s.is_nan() {
        return ODD_NAN;
    }
    let rn = |s: f64, e: f64| -> f32 {
        // Round to odd at 53 bits, then to nearest at 24: no double rounding.
        if e == 0.0 || !s.is_finite() {
            return s as f32;
        }
        let bits = s.to_bits();
        let s2 = if bits & 1 == 1 {
            s
        } else if (e > 0.0) == (s > 0.0) {
            f64::from_bits(bits + 1)
        } else {
            f64::from_bits(bits - 1)
        };
        s2 as f32
    };
    let tiny = s.abs() < f64::from(f32::MIN_POSITIVE);
    match denorm {
        Denorm::Preserve => rn(s, e).to_bits(),
        Denorm::FlushAfter => {
            let r = rn(s, e).to_bits();
            if r & 0x7f80_0000 == 0 {
                r & 0x8000_0000
            } else {
                r
            }
        }
        Denorm::FlushBefore => {
            if !tiny {
                return rn(s, e).to_bits();
            }
            let k = 2f64.powi(64);
            let r = rn(s * k, e * k);
            if r.abs() < f32::MIN_POSITIVE * 2f32.powi(64) {
                // s carries the sign of the exact value (|e| < ulp(s)).
                if s.is_sign_negative() { 0x8000_0000 } else { 0 }
            } else {
                (r / 2f32.powi(64)).to_bits()
            }
        }
    }
}

/// The value of binary16 bits `h`.
pub fn f16_to_f64(h: u16) -> f64 {
    let sign = if h & 0x8000 != 0 { -1.0 } else { 1.0 };
    let e = i32::from((h >> 10) & 31);
    let m = f64::from(h & 0x3ff);
    match e {
        0 => sign * m * 2f64.powi(-24),
        31 if m == 0.0 => sign * f64::INFINITY,
        31 => f64::NAN,
        _ => sign * (1024.0 + m) * 2f64.powi(e - 25),
    }
}

/// `s + e` (exact, `|e|` below half an ulp of `s`) rounded to binary16,
/// nearest even, denormals kept; a NaN is [`ODD_NAN`]'s half `0xfd5a`.
pub fn round_f16(s: f64, e: f64) -> u16 {
    if s.is_nan() {
        return 0xfd5a;
    }
    let sign = if s.is_sign_negative() { 0x8000 } else { 0 };
    let a = s.abs();
    if a.is_infinite() {
        return sign | 0x7c00;
    }
    let ea = if (e < 0.0) == (s < 0.0) {
        e.abs()
    } else {
        -e.abs()
    };
    let exp = if a == 0.0 {
        -24
    } else {
        a.log2().floor() as i32
    };
    let q = 2f64.powi(exp.max(-14) - 10);
    let units = a / q;
    let fl = units.floor();
    let rest = (units - fl) + ea / q;
    let mut n = fl as u64;
    if rest > 0.5 || (rest == 0.5 && n & 1 == 1) {
        n += 1;
    }
    let v = n as f64 * q;
    if v >= 65520.0 {
        return sign | 0x7c00;
    }
    if v == 0.0 {
        return sign;
    }
    let ve = v.log2().floor() as i32;
    if ve < -14 {
        return sign | (v / 2f64.powi(-24)) as u16;
    }
    let mant = (v / 2f64.powi(ve - 10)) as u16 - 1024;
    sign | (((ve + 15) as u16) << 10) | mant
}

pub(crate) fn int_to_f32(neg: bool, mag: u64) -> u32 {
    let v = if mag >> 53 != 0 {
        // Exact as s + e: high part and the rest.
        let hi = (mag >> 11 << 11) as f64;
        let lo = (mag & 0x7ff) as f64;
        round_f32(hi, lo, Denorm::Preserve)
    } else {
        (mag as f64 as f32).to_bits()
    };
    if neg { v | 0x8000_0000 } else { v }
}

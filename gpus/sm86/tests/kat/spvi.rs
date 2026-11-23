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

use std::collections::HashMap;
use std::rc::Rc;

/// What arithmetic NaN results read as (any NaN is allowed).
pub const ODD_NAN: u32 = 0xffd5_a5a5;
const GARBAGE: u64 = 0xa5a5_a5a5_a5a5_a5a5;

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
    Other,
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
}

// Opcodes.
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
            | 224
            | 225
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
        };
        let mut i = 5;
        let mut cur: Option<(u32, Func)> = None;
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

/// Flat memory for explicitly laid out data: (base, bytes) regions.
#[derive(Default)]
pub struct Memory {
    pub regions: Vec<(u64, Vec<u8>)>,
}

impl Memory {
    fn at(&mut self, addr: u64, n: u64) -> &mut [u8] {
        for (base, b) in &mut self.regions {
            if addr >= *base && addr + n <= *base + b.len() as u64 {
                let o = (addr - *base) as usize;
                return &mut b[o..o + n as usize];
            }
        }
        panic!("access to unmapped memory {addr:#x}+{n}");
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
    pub local_id: [u32; 3],
    pub group_id: [u32; 3],
}

pub struct Run<'m> {
    m: &'m Module,
    env: &'m Env,
    pub mem: Memory,
    vals: Vec<V>,
    slots: Vec<V>,
    pub steps: u64,
}

fn mask(w: u32) -> u64 {
    if w >= 64 { u64::MAX } else { (1u64 << w) - 1 }
}

fn sext(v: u64, w: u32) -> i64 {
    if w >= 64 {
        v as i64
    } else {
        ((v << (64 - w)) as i64) >> (64 - w)
    }
}

impl<'m> Run<'m> {
    pub fn new(m: &'m Module, env: &'m Env, mem: Memory) -> Run<'m> {
        Run {
            m,
            env,
            mem,
            vals: vec![V::Undef; m.bound],
            slots: Vec::new(),
            steps: 0,
        }
    }

    pub fn main(&mut self) {
        let m = self.m;
        for (id, v) in &m.consts {
            self.vals[*id as usize] = v.clone();
        }
        for &(id, ty, class, init) in &m.globals {
            let Ty::Ptr(pointee) = *m.ty(ty) else {
                panic!("variable type")
            };
            let v = match class {
                // Uniform, StorageBuffer: explicit layout at a fixed address.
                2 | 12 if matches!(m.ty(pointee), Ty::Array(..) | Ty::RtArray(_)) => {
                    let b = m.binding(id).expect("buffer binding");
                    let (Ty::Array(e, _) | Ty::RtArray(e)) = m.ty(pointee).clone() else {
                        unreachable!()
                    };
                    V::P(Ptr::Desc(b.0, b.1, e))
                }
                2 | 12 => {
                    let b = m.binding(id).expect("buffer binding");
                    let addr = *self
                        .env
                        .uniforms
                        .get(&b)
                        .unwrap_or_else(|| panic!("no buffer at {b:?}"));
                    V::P(Ptr::Mem(addr, pointee))
                }
                // Input: built-ins.
                1 => {
                    let b = m.builtin(id).expect("input without a built-in");
                    let v = match b {
                        27 => self.vec3(self.env.local_id),
                        26 => self.vec3(self.env.group_id),
                        41 => V::S(u64::from(self.env.local_id[0] % 32)),
                        28 => self.vec3([
                            self.env.group_id[0] + self.env.local_id[0],
                            self.env.local_id[1],
                            self.env.local_id[2],
                        ]),
                        _ => panic!("built-in {b}"),
                    };
                    self.slots.push(v);
                    V::P(Ptr::Var(self.slots.len() - 1, Rc::new(Vec::new())))
                }
                _ => {
                    let v = init.map_or_else(|| m.zero(pointee), |i| self.vals[i as usize].clone());
                    self.slots.push(v);
                    V::P(Ptr::Var(self.slots.len() - 1, Rc::new(Vec::new())))
                }
            };
            self.vals[id as usize] = v;
        }
        self.call(m.entry, &[]);
    }

    fn vec3(&self, v: [u32; 3]) -> V {
        V::C(Rc::new(v.iter().map(|&x| V::S(u64::from(x))).collect()))
    }

    fn val(&self, id: u32) -> V {
        self.vals[id as usize].clone()
    }

    fn sv(&self, id: u32) -> u64 {
        self.vals[id as usize].s()
    }

    fn call(&mut self, fid: u32, args: &[V]) -> V {
        let m = self.m;
        let f = &m.funcs[&fid];
        for (p, a) in f.params.iter().zip(args) {
            self.vals[*p as usize] = a.clone();
        }
        let mut pc = 0;
        let mut prev_label = 0;
        let mut cur_label = 0;
        loop {
            let ins = &f.code[pc];
            pc += 1;
            self.steps += 1;
            assert!(self.steps < 50_000_000, "runaway");
            let a = &ins.args;
            match ins.op {
                248 => {
                    prev_label = cur_label;
                    cur_label = a[0];
                }
                247 | 246 => {} // SelectionMerge, LoopMerge
                249 => {
                    pc = f.labels[&a[0]];
                }
                250 => {
                    let t = if self.sv(a[0]) != 0 { a[1] } else { a[2] };
                    pc = f.labels[&t];
                }
                251 => {
                    let sel = self.sv(a[0]);
                    let mut t = a[1];
                    for c in a[2..].chunks(2) {
                        if u64::from(c[0]) == sel {
                            t = c[1];
                        }
                    }
                    pc = f.labels[&t];
                }
                253 => return V::Undef,
                254 => return self.val(a[0]),
                252 | 4416 => {
                    // Kill, TerminateInvocation: stop the invocation.
                    return V::Undef;
                }
                255 => panic!("OpUnreachable reached"),
                245 => {
                    // Phi
                    let v = a
                        .chunks(2)
                        .find(|c| c[1] == prev_label)
                        .map(|c| self.val(c[0]))
                        .unwrap_or_else(|| panic!("phi without edge from %{prev_label}"));
                    self.vals[ins.id as usize] = v;
                }
                OP_VARIABLE => {
                    let Ty::Ptr(pointee) = *m.ty(ins.ty) else {
                        panic!("variable type")
                    };
                    let v = a.get(1).map_or_else(|| m.zero(pointee), |&i| self.val(i));
                    self.slots.push(v);
                    self.vals[ins.id as usize] =
                        V::P(Ptr::Var(self.slots.len() - 1, Rc::new(Vec::new())));
                }
                61 => {
                    let p = self.val(a[0]);
                    let v = self.load(&p, ins.ty);
                    self.vals[ins.id as usize] = v;
                }
                62 => {
                    let p = self.val(a[0]);
                    let v = self.val(a[1]);
                    self.store(&p, v);
                }
                65 | 66 => {
                    let base = self.val(a[0]);
                    let idx: Vec<u64> = a[1..].iter().map(|&i| self.sv(i)).collect();
                    let p = self.chain(&base, &idx);
                    self.vals[ins.id as usize] = p;
                }
                57 => {
                    let args: Vec<V> = a[1..].iter().map(|&i| self.val(i)).collect();
                    let r = self.call(a[0], &args);
                    if ins.id != 0 {
                        self.vals[ins.id as usize] = r;
                    }
                }
                224 | 225 => {} // barriers: one invocation
                _ => {
                    let v = self.op(ins);
                    self.vals[ins.id as usize] = v;
                }
            }
        }
    }

    fn chain(&self, base: &V, idx: &[u64]) -> V {
        let m = self.m;
        match base {
            V::P(Ptr::Var(slot, path)) => {
                let mut p = (**path).clone();
                p.extend_from_slice(idx);
                V::P(Ptr::Var(*slot, Rc::new(p)))
            }
            V::P(Ptr::Mem(addr, ty)) => {
                let mut addr = *addr;
                let mut ty = *ty;
                for &i in idx {
                    match m.ty(ty).clone() {
                        Ty::Struct(ms) => {
                            addr += m.member_off[&(ty, i as u32)];
                            ty = ms[i as usize];
                        }
                        Ty::Array(e, _) | Ty::RtArray(e) => {
                            addr += m.stride(ty, e) * i;
                            ty = e;
                        }
                        Ty::Vec(e, _) => {
                            addr += m.size(e) * i;
                            ty = e;
                        }
                        t => panic!("access chain into {t:?}"),
                    }
                }
                V::P(Ptr::Mem(addr, ty))
            }
            V::P(Ptr::Desc(set, binding, e)) => {
                let addr = *self
                    .env
                    .arrays
                    .get(&(*set, *binding, idx[0]))
                    .unwrap_or_else(|| panic!("no buffer at ({set}, {binding})[{}]", idx[0]));
                self.chain(&V::P(Ptr::Mem(addr, *e)), &idx[1..])
            }
            v => panic!("access chain base {v:?}"),
        }
    }

    fn load(&mut self, p: &V, ty: u32) -> V {
        match p {
            V::P(Ptr::Var(slot, path)) => {
                let mut v = &self.slots[*slot];
                for &i in path.iter() {
                    v = &v.c()[i as usize];
                }
                v.clone()
            }
            V::P(Ptr::Mem(addr, _)) => self.load_mem(*addr, ty),
            v => panic!("load from {v:?}"),
        }
    }

    fn load_mem(&mut self, addr: u64, ty: u32) -> V {
        let m = self.m;
        match m.ty(ty).clone() {
            Ty::Int(w) | Ty::Float(w) => V::S(self.mem.read(addr, u64::from(w / 8))),
            Ty::Vec(e, n) => {
                let sz = m.size(e);
                V::C(Rc::new(
                    (0..u64::from(n))
                        .map(|k| self.load_mem(addr + k * sz, e))
                        .collect(),
                ))
            }
            Ty::Array(e, n) => {
                let st = m.stride(ty, e);
                V::C(Rc::new(
                    (0..n).map(|k| self.load_mem(addr + k * st, e)).collect(),
                ))
            }
            Ty::Struct(ms) => V::C(Rc::new(
                ms.iter()
                    .enumerate()
                    .map(|(k, &t)| self.load_mem(addr + m.member_off[&(ty, k as u32)], t))
                    .collect(),
            )),
            t => panic!("load of {t:?}"),
        }
    }

    fn store(&mut self, p: &V, v: V) {
        match p {
            V::P(Ptr::Var(slot, path)) => {
                fn put(dst: &mut V, path: &[u64], v: V) {
                    if path.is_empty() {
                        *dst = v;
                        return;
                    }
                    let V::C(c) = dst else {
                        panic!("store path into scalar")
                    };
                    put(&mut Rc::make_mut(c)[path[0] as usize], &path[1..], v);
                }
                put(&mut self.slots[*slot], path, v);
            }
            V::P(Ptr::Mem(addr, ty)) => self.store_mem(*addr, *ty, &v),
            v => panic!("store to {v:?}"),
        }
    }

    fn store_mem(&mut self, addr: u64, ty: u32, v: &V) {
        let m = self.m;
        match m.ty(ty).clone() {
            Ty::Int(w) | Ty::Float(w) => self.mem.write(addr, u64::from(w / 8), v.s()),
            Ty::Vec(e, _) => {
                let sz = m.size(e);
                for (k, x) in v.c().iter().enumerate() {
                    self.store_mem(addr + k as u64 * sz, e, x);
                }
            }
            t => panic!("store of {t:?}"),
        }
    }

    // --- arithmetic ---

    fn op(&mut self, ins: &Inst) -> V {
        let m = self.m;
        let a = &ins.args;
        let op = ins.op;
        match op {
            // CompositeExtract
            81 => {
                let mut v = self.val(a[0]);
                for &i in &a[1..] {
                    v = v.c()[i as usize].clone();
                }
                v
            }
            // CompositeInsert
            82 => {
                let mut c = self.val(a[1]);
                fn put(dst: &mut V, path: &[u32], v: V) {
                    if path.is_empty() {
                        *dst = v;
                        return;
                    }
                    let V::C(c) = dst else {
                        panic!("insert into scalar")
                    };
                    put(&mut Rc::make_mut(c)[path[0] as usize], &path[1..], v);
                }
                put(&mut c, &a[2..], self.val(a[0]));
                c
            }
            // CompositeConstruct
            80 => {
                let mut parts = Vec::new();
                let is_vec = matches!(m.ty(ins.ty), Ty::Vec(..));
                for &i in a {
                    match self.val(i) {
                        V::C(c) if is_vec => parts.extend(c.iter().cloned()),
                        v => parts.push(v),
                    }
                }
                V::C(Rc::new(parts))
            }
            // VectorShuffle
            79 => {
                let x = self.val(a[0]);
                let y = self.val(a[1]);
                let (xs, ys) = (x.c().to_vec(), y.c().to_vec());
                V::C(Rc::new(
                    a[2..]
                        .iter()
                        .map(|&k| {
                            let k = k as usize;
                            if k < xs.len() {
                                xs[k].clone()
                            } else {
                                ys[k - xs.len()].clone()
                            }
                        })
                        .collect(),
                ))
            }
            83 => self.val(a[0]), // CopyObject
            1 => V::Undef,
            // Bitcast
            124 => {
                let v = self.val(a[0]);
                let rt = m.ty(ins.ty).clone();
                match (&v, rt) {
                    (V::C(c), Ty::Ptr(pointee)) => {
                        V::P(Ptr::Mem(c[0].s() | (c[1].s() << 32), pointee))
                    }
                    (V::S(x), Ty::Vec(e, n)) => {
                        let w = m.width(e);
                        V::C(Rc::new(
                            (0..n).map(|k| V::S((x >> (w * k)) & mask(w))).collect(),
                        ))
                    }
                    (V::C(c), _) => {
                        let src_w = self.src_width(a[0]);
                        V::S(
                            c.iter()
                                .enumerate()
                                .fold(0, |acc, (k, e)| acc | (e.s() << (src_w * k as u32))),
                        )
                    }
                    (V::S(x), _) => V::S(*x),
                    (v, t) => panic!("bitcast {v:?} to {t:?}"),
                }
            }
            // ConvertUToPtr
            120 => {
                let Ty::Ptr(pointee) = *m.ty(ins.ty) else {
                    panic!()
                };
                V::P(Ptr::Mem(self.sv(a[0]), pointee))
            }
            // Select
            169 => {
                let c = self.val(a[0]);
                let x = self.val(a[1]);
                let y = self.val(a[2]);
                match c {
                    V::C(cs) => V::C(Rc::new(
                        cs.iter()
                            .zip(x.c())
                            .zip(y.c())
                            .map(|((c, x), y)| if c.s() != 0 { x.clone() } else { y.clone() })
                            .collect(),
                    )),
                    c => {
                        if c.s() != 0 {
                            x
                        } else {
                            y
                        }
                    }
                }
            }
            // IAddCarry, ISubBorrow, UMulExtended, SMulExtended: struct results
            149..=152 => {
                let w = m.width(m.scalar_of(self.member0(ins.ty)));
                let x = self.sv(a[0]);
                let y = self.sv(a[1]);
                let mk = mask(w);
                let (lo, hi) = match op {
                    149 => {
                        let s = (x as u128) + (y as u128);
                        ((s as u64) & mk, u64::from((s >> w) & 1 != 0))
                    }
                    150 => ((x.wrapping_sub(y)) & mk, u64::from(y > x)),
                    151 => {
                        let p = (x as u128) * (y as u128);
                        ((p as u64) & mk, ((p >> w) as u64) & mk)
                    }
                    _ => {
                        let p = i128::from(sext(x, w)) * i128::from(sext(y, w));
                        ((p as u64) & mk, ((p >> w) as u64) & mk)
                    }
                };
                V::C(Rc::new(vec![V::S(lo), V::S(hi)]))
            }
            OP_EXT_INST => self.ext(ins),
            _ => {
                // Elementwise ops.
                let ty = ins.ty;
                let st = m.scalar_of(ty);
                let ops: Vec<V> = a.iter().map(|&i| self.val(i)).collect();
                if let Some(V::C(c)) = ops.first() {
                    let n = c.len();
                    let parts = (0..n)
                        .map(|k| {
                            let xs: Vec<u64> = ops.iter().map(|o| o.c()[k].s()).collect();
                            V::S(self.scalar(op, st, &xs, a))
                        })
                        .collect();
                    return V::C(Rc::new(parts));
                }
                let xs: Vec<u64> = ops.iter().map(V::s).collect();
                V::S(self.scalar(op, st, &xs, a))
            }
        }
    }

    fn member0(&self, ty: u32) -> u32 {
        match self.m.ty(ty) {
            Ty::Struct(ms) => ms[0],
            t => panic!("struct expected, {t:?}"),
        }
    }

    fn scalar(&mut self, op: u16, st: u32, x: &[u64], ids: &[u32]) -> u64 {
        let m = self.m;
        let w = m.width(st);
        let mk = mask(w);
        let f = |v: u64| f32::from_bits(v as u32);
        let b = |c: bool| u64::from(c);
        match op {
            // integer
            128 => x[0].wrapping_add(x[1]) & mk,
            130 => x[0].wrapping_sub(x[1]) & mk,
            132 => x[0].wrapping_mul(x[1]) & mk,
            134 => x[0].checked_div(x[1]).unwrap_or(GARBAGE) & mk,
            137 => x[0].checked_rem(x[1]).unwrap_or(GARBAGE) & mk,
            126 => x[0].wrapping_neg() & mk,
            194..=196 => {
                if x[1] >= u64::from(w) {
                    return GARBAGE & mk;
                }
                match op {
                    194 => (x[0] >> x[1]) & mk,
                    195 => ((sext(x[0], w) >> x[1]) as u64) & mk,
                    _ => (x[0] << x[1]) & mk,
                }
            }
            197 => x[0] | x[1],
            198 => x[0] ^ x[1],
            199 => x[0] & x[1],
            200 => !x[0] & mk,
            201 => {
                // BitFieldInsert base insert offset count
                let (off, cnt) = (x[2], x[3]);
                if off + cnt > u64::from(w) {
                    return GARBAGE & mk;
                }
                let fm = mask(cnt as u32) << off;
                (x[0] & !fm) | ((x[1] << off) & fm)
            }
            202 | 203 => {
                let (off, cnt) = (x[1], x[2]);
                if off + cnt > u64::from(w) {
                    return GARBAGE & mk;
                }
                if cnt == 0 {
                    return 0;
                }
                let v = (x[0] >> off) & mask(cnt as u32);
                if op == 202 {
                    (sext(v, cnt as u32) as u64) & mk
                } else {
                    v
                }
            }
            204 => (x[0].reverse_bits() >> (64 - w)) & mk,
            205 => u64::from(x[0].count_ones()),
            170 => b(x[0] == x[1]),
            171 => b(x[0] != x[1]),
            172 => b(x[0] > x[1]),
            174 => b(x[0] >= x[1]),
            176 => b(x[0] < x[1]),
            178 => b(x[0] <= x[1]),
            173 | 175 | 177 | 179 => {
                let wi = self.src_width(ids[0]);
                let (p, q) = (sext(x[0], wi), sext(x[1], wi));
                b(match op {
                    173 => p > q,
                    175 => p >= q,
                    177 => p < q,
                    _ => p <= q,
                })
            }
            // logical
            164 => b(x[0] == x[1]),
            165 => b(x[0] != x[1]),
            166 => b(x[0] != 0 || x[1] != 0),
            167 => b(x[0] != 0 && x[1] != 0),
            168 => b(x[0] == 0),
            // UConvert, SConvert
            113 => x[0] & mk,
            114 => {
                let wi = self.src_width(ids[0]);
                (sext(x[0], wi) as u64) & mk
            }
            // float -> int
            109 | 110 => {
                let v = self.flush_in(x[0]);
                let fv = f64::from(f(v)).trunc();
                let (lo, hi) = if op == 110 {
                    (-(2f64.powi(w as i32 - 1)), 2f64.powi(w as i32 - 1))
                } else {
                    (0.0, 2f64.powi(w as i32))
                };
                if fv.is_nan() || fv < lo || fv >= hi {
                    return GARBAGE & mk;
                }
                if op == 110 {
                    (fv as i64 as u64) & mk
                } else {
                    fv as u64 & mk
                }
            }
            // int -> float (round to nearest even)
            111 | 112 => {
                let wi = self.src_width(ids[0]);
                let (neg, mag) = if op == 111 && sext(x[0], wi) < 0 {
                    (true, sext(x[0], wi).unsigned_abs())
                } else {
                    (false, x[0] & mask(wi))
                };
                u64::from(int_to_f32(neg, mag))
            }
            // half-precision arithmetic: binary16, nearest even, denormals kept
            129 | 131 | 133 | 4427 if w == 16 => {
                let h = |v: u64| f16_to_f64(v as u16);
                let (s, e) = match op {
                    129 => two_sum(h(x[0]), h(x[1])),
                    131 => two_sum(h(x[0]), -h(x[1])),
                    133 => (h(x[0]) * h(x[1]), 0.0),
                    _ => two_sum(h(x[0]) * h(x[1]), h(x[2])),
                };
                u64::from(round_f16(s, e))
            }
            // FConvert
            115 if w == 16 => u64::from(round_f16(f64::from(f(x[0])), 0.0)),
            115 if w == 32 => u64::from((f16_to_f64(x[0] as u16) as f32).to_bits()),
            // float arithmetic
            127 => x[0] ^ 0x8000_0000, // FNegate
            129 | 131 | 133 => {
                let (p, q) = (f(self.flush_in(x[0])), f(self.flush_in(x[1])));
                let (s, e) = match op {
                    129 => two_sum(f64::from(p), f64::from(q)),
                    131 => two_sum(f64::from(p), -f64::from(q)),
                    _ => (f64::from(p) * f64::from(q), 0.0),
                };
                let r = u64::from(self.round_arith(s, e));
                if op == 133 && self.env.mul_zero_positive && r & 0x7fff_ffff == 0 {
                    0
                } else {
                    r
                }
            }
            4427 => {
                let (p, q, r) = (
                    f(self.flush_in(x[0])),
                    f(self.flush_in(x[1])),
                    f(self.flush_in(x[2])),
                );
                let (s, e) = two_sum(f64::from(p) * f64::from(q), f64::from(r));
                u64::from(self.round_arith(s, e))
            }
            // comparisons
            180..=191 => {
                let (p, q) = (f(self.flush_in(x[0])), f(self.flush_in(x[1])));
                let un = p.is_nan() || q.is_nan();
                let r = match op {
                    180 | 181 => p == q,
                    182 | 183 => p != q,
                    184 | 185 => p < q,
                    186 | 187 => p > q,
                    188 | 189 => p <= q,
                    _ => p >= q,
                };
                let ordered = op.is_multiple_of(2);
                if ordered { b(!un && r) } else { b(un || r) }
            }
            156 => b(f(x[0]).is_nan()),
            157 => b(f(x[0]).is_infinite()),
            _ => panic!("unsupported op {op}"),
        }
    }

    /// The scalar bit width of an operand.
    fn src_width(&self, id: u32) -> u32 {
        let m = self.m;
        m.width(m.scalar_of(m.id_ty[id as usize]))
    }

    fn flush_in(&self, v: u64) -> u64 {
        if self.env.denorm != Denorm::Preserve && v & 0x7f80_0000 == 0 {
            v & 0x8000_0000
        } else {
            v
        }
    }

    /// [`Run::round`] of an arithmetic result, with the host's NaN.
    fn round_arith(&self, s: f64, e: f64) -> u32 {
        if s.is_nan() && self.env.canonical_nan {
            return 0x7fff_ffff;
        }
        self.round(s, e)
    }

    /// The exact value `s + e` (an unevaluated f64 sum) rounded to single
    /// precision under the denormal mode.
    fn round(&self, s: f64, e: f64) -> u32 {
        round_f32(s, e, self.env.denorm)
    }

    fn ext(&mut self, ins: &Inst) -> V {
        let a = &ins.args;
        let inst = a[1];
        let ops: Vec<V> = a[2..].iter().map(|&i| self.val(i)).collect();
        if let Some(V::C(c)) = ops.first() {
            let n = c.len();
            let parts = (0..n)
                .map(|k| {
                    let xs: Vec<u64> = ops.iter().map(|o| o.c()[k].s()).collect();
                    V::S(self.ext_scalar(inst, ins.ty, &xs))
                })
                .collect();
            return V::C(Rc::new(parts));
        }
        let xs: Vec<u64> = ops.iter().map(V::s).collect();
        V::S(self.ext_scalar(inst, ins.ty, &xs))
    }

    fn ext_scalar(&mut self, inst: u32, ty: u32, x: &[u64]) -> u64 {
        let m = self.m;
        let st = m.scalar_of(ty);
        let w = m.width(st);
        let f = |v: u64| f32::from_bits(v as u32);
        let fi = |s: &Self, v: u64| f(s.flush_in(v));
        let out = |s: &Self, v: f32| -> u64 { u64::from(s.flush_out_exact(v)) };
        match inst {
            4 => x[0] & 0x7fff_ffff, // FAbs
            8 => out(self, fi(self, x[0]).floor()),
            9 => out(self, fi(self, x[0]).ceil()),
            3 => out(self, fi(self, x[0]).trunc()),
            2 => out(self, fi(self, x[0]).round_ties_even()),
            50 => {
                // Fma, fused
                let (p, q, r) = (fi(self, x[0]), fi(self, x[1]), fi(self, x[2]));
                let (s, e) = two_sum(f64::from(p) * f64::from(q), f64::from(r));
                u64::from(self.round_arith(s, e))
            }
            53 => {
                // Ldexp
                let v = fi(self, x[0]);
                let n = sext(x[1], 32).clamp(-400, 400) as i32;
                u64::from(self.round(f64::from(v) * 2f64.powi(n), 0.0))
            }
            79 | 80 => {
                // NMin, NMax
                let (p, q) = (fi(self, x[0]), fi(self, x[1]));
                if p.is_nan() {
                    return u64::from(q.to_bits());
                }
                if q.is_nan() {
                    return u64::from(p.to_bits());
                }
                let r = if p == q {
                    q
                } else if (inst == 79) == (p < q) {
                    p
                } else {
                    q
                };
                u64::from(r.to_bits())
            }
            81 => {
                // NClamp x lo hi = NMin(NMax(x, lo), hi)
                let (v, lo, hi) = (fi(self, x[0]), fi(self, x[1]), fi(self, x[2]));
                let mx = if v.is_nan() {
                    lo
                } else if lo.is_nan() || v > lo {
                    v
                } else {
                    lo
                };
                let r = if mx.is_nan() {
                    hi
                } else if hi.is_nan() || mx < hi {
                    mx
                } else {
                    hi
                };
                u64::from(r.to_bits())
            }
            38 => x[0].min(x[1]),
            41 => x[0].max(x[1]),
            39 => sext(x[0], w).min(sext(x[1], w)) as u64 & mask(w),
            42 => sext(x[0], w).max(sext(x[1], w)) as u64 & mask(w),
            44 => x[0].max(x[1]).min(x[2]),
            45 => sext(x[0], w).max(sext(x[1], w)).min(sext(x[2], w)) as u64 & mask(w),
            5 => (sext(x[0], w).unsigned_abs()) & mask(w),
            73 => {
                if x[0] == 0 {
                    mask(w)
                } else {
                    u64::from(x[0].trailing_zeros())
                }
            }
            74 => {
                // FindSMsb
                let v = sext(x[0], w);
                let u = if v < 0 { !v as u64 } else { v as u64 };
                if u == 0 {
                    mask(w)
                } else {
                    u64::from(63 - u.leading_zeros())
                }
            }
            75 => {
                if x[0] == 0 {
                    mask(w)
                } else {
                    u64::from(63 - x[0].leading_zeros())
                }
            }
            13 | 14 | 29 | 30 | 31 | 32 => {
                // Sin, Cos, Exp2, Log2, Sqrt, InverseSqrt: a host's
                // operation, here the correctly rounded one.
                let v = f64::from(fi(self, x[0]));
                let r = match inst {
                    13 => v.sin(),
                    14 => v.cos(),
                    29 => v.exp2(),
                    30 => v.log2(),
                    31 => v.sqrt(),
                    _ => 1.0 / v.sqrt(),
                };
                u64::from(self.round(r, 0.0))
            }
            _ => panic!("unsupported GLSL.std.450 instruction {inst}"),
        }
    }

    /// An exactly computed result (Floor and the like) written out: a
    /// denormal flushes on a flushing device.
    fn flush_out_exact(&self, v: f32) -> u32 {
        let b = v.to_bits();
        if self.env.denorm != Denorm::Preserve && b & 0x7f80_0000 == 0 {
            b & 0x8000_0000
        } else {
            b
        }
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

fn int_to_f32(neg: bool, mag: u64) -> u32 {
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

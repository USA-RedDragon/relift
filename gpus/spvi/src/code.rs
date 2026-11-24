//! Compiles a [`Module`] once into flat code for [`crate::exec`]: every
//! id gets a fixed place in one register file (a composite takes one
//! register per scalar, a pointer two), operands become register indices,
//! labels become code positions and each straight run of instructions
//! counts its steps at once.
//!
//! A pointer is two registers: a tag and a value. Tag [`MEM`] is memory
//! at an address, [`VAR`] a place in the invocation's variable store,
//! [`DESC`] (with set and binding above the low byte) an array of buffer
//! descriptors. Anything else is a value no pointer operation accepts.
//!
//! What the reference interpreter refuses at run time compiles to an
//! [`Op::Panic`] at the same place, so a program fails only where it would
//! have failed there.

use std::collections::HashMap;

use crate::{GARBAGE, Module, Ptr, Ty, V};

/// Pointer tag: memory at the address.
pub(crate) const MEM: u64 = 0;
/// Pointer tag: a place in the variable store.
pub(crate) const VAR: u64 = 1;
/// Pointer tag (low byte): a descriptor array; set in bits 8..32, binding
/// in bits 32..64.
pub(crate) const DESC: u64 = 2;
/// Pointer tag of a scalar where a pointer was expected.
pub(crate) const NOT_PTR: u64 = 0xff;
/// A variable store place no access reaches (an index past the end).
pub(crate) const POISON: u64 = u64::MAX;

/// Where a function's instructions start and where its parameters live.
pub(crate) struct Func {
    pub id: u32,
    pub start: u32,
    pub params: Vec<(u32, u32)>,
}

/// One step of an access chain, for each kind of base.
#[derive(Clone)]
pub(crate) struct Link {
    /// The index register, or a constant.
    pub idx: Index,
    pub var: Result<VarStep, u32>,
    pub mem: Result<MemStep, u32>,
}

#[derive(Clone, Copy)]
pub(crate) enum Index {
    Reg(u32),
    Const(u64),
}

#[derive(Clone, Copy)]
pub(crate) enum VarStep {
    /// A struct member: a fixed offset.
    Add(u64),
    /// An array or vector element of `size` registers, of `len` elements
    /// (unbounded for a runtime array).
    Elem { size: u64, len: u64 },
}

#[derive(Clone, Copy)]
pub(crate) enum MemStep {
    Add(u64),
    Mul(u64),
}

/// A load or store of laid out memory: (byte offset, bytes) per register.
pub(crate) type Layout = Result<Vec<(u64, u64)>, u32>;

pub(crate) struct Chain {
    pub links: Vec<Link>,
}

/// One elementwise component: operand registers and what the operation
/// reads of its types.
pub(crate) struct Gen {
    pub op: u16,
    pub w: u32,
    pub src_w: u32,
    pub n: u8,
    pub args: [u32; 4],
    /// Registers whose known flag the fold reads (the operands' first).
    pub known: [u32; 2],
}

pub(crate) struct ExtGen {
    pub inst: u32,
    pub w: u32,
    pub n: u8,
    pub args: [u32; 4],
}

/// A compiled instruction. Register operands are indices into the
/// register file; `n` is a count of registers.
#[derive(Clone, Copy, Debug)]
pub(crate) enum Op {
    /// A block's label: counts `n` steps (the label through the first call,
    /// synchronization or terminator).
    Label {
        label: u32,
        n: u32,
    },
    /// Counts `n` steps after a call or synchronization in a block.
    Count {
        n: u32,
    },
    Jump {
        to: u32,
    },
    Branch {
        c: u32,
        t: u32,
        f: u32,
    },
    /// Switch: cases at `side[at..at + 2 * k]` as (literal, code position),
    /// a missing label `u32::MAX`.
    Switch {
        sel: u32,
        default: u32,
        at: u32,
        k: u32,
    },
    Return,
    ReturnValue {
        s: u32,
        n: u32,
    },
    /// Phi: (source register, label) pairs at `side[at..at + 2 * k]`.
    Phi {
        d: u32,
        n: u32,
        at: u32,
        k: u32,
    },
    /// OpVariable: `n` registers of the store, from `init` or the zero
    /// template `zero` (an index into the zero templates).
    Var {
        d: u32,
        init: u32,
        zero: u32,
    },
    /// Loads `n` registers.
    Load {
        d: u32,
        p: u32,
        plan: u32,
        n: u32,
    },
    Store {
        p: u32,
        s: u32,
        plan: u32,
        n: u32,
    },
    /// An atomic read, modify and write: kind 234 (IAdd), 230
    /// (CompareExchange), 229 (Exchange). Operands at `side[at..]`.
    Atomic {
        d: u32,
        p: u32,
        at: u32,
    },
    Chain {
        d: u32,
        base: u32,
        chain: u32,
    },
    /// Calls `func` with the argument registers at `side[at..at + k]`; the
    /// result (`side[at + k]` registers) goes to `d`.
    Call {
        func: u32,
        at: u32,
        k: u32,
        d: u32,
    },
    Barrier,
    Shuffle {
        d: u32,
        v: u32,
        lane: u32,
    },
    Copy {
        d: u32,
        s: u32,
        n: u32,
    },
    /// One register, carrying the known flag (Bitcast, CopyObject).
    Copy1K {
        d: u32,
        s: u32,
    },
    CopyK {
        d: u32,
        s: u32,
        n: u32,
    },
    /// Pointer from a two-component vector (low, high).
    PtrOfVec {
        d: u32,
        s: u32,
    },
    PtrOfU {
        d: u32,
        s: u32,
    },
    NotPtr {
        d: u32,
        s: u32,
    },
    /// Bitcast scalar to `n` components of `w` bits.
    Split {
        d: u32,
        s: u32,
        n: u32,
        w: u32,
    },
    /// Bitcast `n` components of `w` bits to a scalar.
    Join {
        d: u32,
        s: u32,
        n: u32,
        w: u32,
    },
    Select {
        d: u32,
        c: u32,
        at: u32,
    },
    /// Per-component select: c, x, y at `side[at..at + 3]`, `n`
    /// components.
    SelectV {
        d: u32,
        at: u32,
        n: u32,
    },
    /// IAddCarry, ISubBorrow, UMulExtended, SMulExtended.
    Wide {
        op: u16,
        w: u8,
        d: u32,
        a: u32,
        b: u32,
    },
    IAdd {
        w: u8,
        d: u32,
        a: u32,
        b: u32,
    },
    ISub {
        w: u8,
        d: u32,
        a: u32,
        b: u32,
    },
    IMul {
        w: u8,
        d: u32,
        a: u32,
        b: u32,
    },
    And {
        d: u32,
        a: u32,
        b: u32,
    },
    Or {
        d: u32,
        a: u32,
        b: u32,
    },
    Xor {
        d: u32,
        a: u32,
        b: u32,
    },
    Not {
        w: u8,
        d: u32,
        a: u32,
    },
    Shr {
        w: u8,
        d: u32,
        a: u32,
        b: u32,
    },
    Sar {
        w: u8,
        d: u32,
        a: u32,
        b: u32,
    },
    Shl {
        w: u8,
        d: u32,
        a: u32,
        b: u32,
    },
    Eq {
        d: u32,
        a: u32,
        b: u32,
    },
    Ne {
        d: u32,
        a: u32,
        b: u32,
    },
    /// Unsigned compares: `k` 0 >, 1 >=, 2 <, 3 <=.
    UCmp {
        k: u8,
        d: u32,
        a: u32,
        b: u32,
    },
    SCmp {
        k: u8,
        w: u8,
        d: u32,
        a: u32,
        b: u32,
    },
    LOr {
        d: u32,
        a: u32,
        b: u32,
    },
    LAnd {
        d: u32,
        a: u32,
        b: u32,
    },
    LNot {
        d: u32,
        a: u32,
    },
    /// An elementwise component through [`crate::arith::Fx::scalar`].
    Gen {
        d: u32,
        g: u32,
    },
    Ext {
        d: u32,
        g: u32,
    },
    /// Stops with message `msg`.
    Panic {
        msg: u32,
    },
}

/// How the entry point finds a global variable: its pointer register and
/// what it points at.
pub(crate) enum Global {
    /// An array of buffer descriptors.
    Desc { r: u32, set: u32, binding: u32 },
    /// A uniform or storage block at the address [`crate::Env::uniforms`]
    /// gives.
    Block { r: u32, set: u32, binding: u32 },
    /// An input built-in, `n` registers.
    BuiltIn { r: u32, b: u32, n: u32 },
    /// A workgroup variable (memory in a workgroup run).
    Workgroup { r: u32, id: u32, zero: Vec<u64> },
    /// A logical variable: from register `init`, or zero.
    Logical {
        r: u32,
        init: Option<u32>,
        zero: Vec<u64>,
    },
    /// What the reference refuses when it starts.
    Panic(String),
}

/// A compiled module.
pub(crate) struct Program {
    pub globals: Vec<Global>,
    pub code: Vec<Op>,
    pub funcs: Vec<Func>,
    pub entry: u32,
    pub side: Vec<u32>,
    pub chains: Vec<Chain>,
    pub layouts: Vec<Layout>,
    pub gens: Vec<Gen>,
    pub exts: Vec<ExtGen>,
    pub msgs: Vec<String>,
    /// Zero templates of variables.
    pub zeros: Vec<Vec<u64>>,
    /// The register file before the entry point runs: constants, every
    /// other register [`GARBAGE`].
    pub init: Vec<u64>,
    /// Registers whose value comes from a constant.
    pub known: Vec<bool>,
    /// Register of each id.
    pub reg: Vec<u32>,
}

impl Module {
    /// Registers a value of type `ty` takes.
    pub(crate) fn flat(&self, ty: u32) -> u32 {
        match self.types.get(&ty) {
            Some(Ty::Bool | Ty::Int(_) | Ty::Float(_)) => 1,
            Some(Ty::Vec(e, n)) => self.flat(*e) * n,
            Some(Ty::Array(e, n)) => self.flat(*e) * *n as u32,
            Some(Ty::Struct(ms)) => ms.iter().map(|&t| self.flat(t)).sum(),
            Some(Ty::Ptr(_)) => 2,
            _ => 0,
        }
    }

    fn is_ptr(&self, ty: u32) -> bool {
        matches!(self.types.get(&ty), Some(Ty::Ptr(_)))
    }

    /// A value as registers of type `ty`.
    pub(crate) fn put(&self, v: &V, ty: u32, out: &mut Vec<u64>) {
        let n = self.flat(ty) as usize;
        let start = out.len();
        match (v, self.types.get(&ty)) {
            (V::S(x), Some(Ty::Ptr(_))) => out.extend([NOT_PTR, *x]),
            (V::S(x), Some(Ty::Bool | Ty::Int(_) | Ty::Float(_))) => out.push(*x),
            (V::P(p), Some(Ty::Ptr(_))) => out.extend(enc_ptr(p)),
            (V::C(parts), Some(Ty::Vec(e, _) | Ty::Array(e, _))) => {
                for p in parts.iter() {
                    self.put(p, *e, out);
                }
            }
            (V::C(parts), Some(Ty::Struct(ms))) => {
                for (p, &t) in parts.iter().zip(ms) {
                    self.put(p, t, out);
                }
            }
            _ => {}
        }
        out.resize(start + n, GARBAGE);
    }
}

pub(crate) fn enc_ptr(p: &Ptr) -> [u64; 2] {
    match p {
        Ptr::Mem(a, _) => [MEM, *a],
        Ptr::Var(..) => panic!("a variable pointer is no constant"),
        Ptr::Desc(s, b, _) => [DESC | u64::from(*s) << 8 | u64::from(*b) << 32, 0],
    }
}

const NONE: u32 = u32::MAX;

struct Cx<'a> {
    m: &'a Module,
    p: Program,
    msg_ids: HashMap<String, u32>,
    labels: HashMap<u32, u32>,
    fixups: Vec<(usize, u8)>,
    /// Function ids, sorted: a call names its function by position here.
    fids: Vec<u32>,
}

impl Cx<'_> {
    fn msg(&mut self, s: String) -> u32 {
        if let Some(&i) = self.msg_ids.get(&s) {
            return i;
        }
        let i = self.p.msgs.len() as u32;
        self.p.msgs.push(s.clone());
        self.msg_ids.insert(s, i);
        i
    }

    fn panic(&mut self, s: String) {
        let msg = self.msg(s);
        self.p.code.push(Op::Panic { msg });
    }

    fn r(&self, id: u32) -> u32 {
        self.p.reg[id as usize]
    }

    fn ty_of(&self, id: u32) -> u32 {
        self.m.id_ty[id as usize]
    }

    fn flat_id(&self, id: u32) -> u32 {
        self.m.flat(self.ty_of(id))
    }

    fn side(&mut self, xs: &[u32]) -> u32 {
        let at = self.p.side.len() as u32;
        self.p.side.extend_from_slice(xs);
        at
    }

    /// A scalar operand: not a composite, not a pointer.
    fn is_scalar(&self, id: u32) -> bool {
        matches!(
            self.m.types.get(&self.ty_of(id)),
            Some(Ty::Bool | Ty::Int(_) | Ty::Float(_))
        )
    }

    /// The component count of a vector or array of scalars, if `id` is
    /// one.
    fn comps(&self, id: u32) -> Option<u32> {
        match self.m.types.get(&self.ty_of(id)) {
            Some(Ty::Vec(e, n)) if self.m.flat(*e) == 1 && !self.m.is_ptr(*e) => Some(*n),
            Some(Ty::Array(e, n)) if self.m.flat(*e) == 1 && !self.m.is_ptr(*e) => Some(*n as u32),
            _ => None,
        }
    }

    fn width(&self, ty: u32) -> Option<u32> {
        match self.m.types.get(&ty)? {
            Ty::Int(w) | Ty::Float(w) => Some(*w),
            Ty::Bool => Some(1),
            Ty::Vec(e, _) => self.width(*e),
            _ => None,
        }
    }

    fn scalar_of(&self, ty: u32) -> u32 {
        match self.m.types.get(&ty) {
            Some(Ty::Vec(e, _)) => *e,
            _ => ty,
        }
    }

    fn const_of(&self, id: u32) -> Option<u64> {
        if !self.p.known[self.r(id) as usize] || self.flat_id(id) != 1 {
            return None;
        }
        Some(self.p.init[self.r(id) as usize])
    }

    /// Byte layout of a load of type `ty` from memory.
    fn load_layout(&self, ty: u32, at: u64, out: &mut Vec<(u64, u64)>) -> Result<(), String> {
        let m = self.m;
        match m.types.get(&ty) {
            Some(Ty::Int(w) | Ty::Float(w)) => out.push((at, u64::from(w / 8))),
            Some(Ty::Vec(e, n)) => {
                let sz = m.size(*e);
                for k in 0..u64::from(*n) {
                    self.load_layout(*e, at + k * sz, out)?;
                }
            }
            Some(Ty::Array(e, n)) => {
                let st = m.stride(ty, *e);
                for k in 0..*n {
                    self.load_layout(*e, at + k * st, out)?;
                }
            }
            Some(Ty::Struct(ms)) => {
                for (k, &t) in ms.iter().enumerate() {
                    let off = *m
                        .member_off
                        .get(&(ty, k as u32))
                        .ok_or_else(|| format!("no offset of member {k} of %{ty}"))?;
                    self.load_layout(t, at + off, out)?;
                }
            }
            Some(t) => return Err(format!("load of {t:?}")),
            None => return Err(format!("type %{ty}")),
        }
        Ok(())
    }

    fn store_layout(&self, ty: u32, at: u64, out: &mut Vec<(u64, u64)>) -> Result<(), String> {
        let m = self.m;
        match m.types.get(&ty) {
            Some(Ty::Int(w) | Ty::Float(w)) => out.push((at, u64::from(w / 8))),
            Some(Ty::Vec(e, n)) => {
                let sz = m.size(*e);
                for k in 0..u64::from(*n) {
                    self.store_layout(*e, at + k * sz, out)?;
                }
            }
            Some(t) => return Err(format!("store of {t:?}")),
            None => return Err(format!("type %{ty}")),
        }
        Ok(())
    }

    fn layout(&mut self, r: Result<Vec<(u64, u64)>, String>) -> u32 {
        let l = match r {
            Ok(v) => Ok(v),
            Err(s) => Err(self.msg(s)),
        };
        self.p.layouts.push(l);
        (self.p.layouts.len() - 1) as u32
    }

    /// Elementwise operands: `Some(None)` scalars, `Some(Some(n))` `n`
    /// components each, `None` what the reference refuses.
    fn elementwise(&self, ids: &[u32]) -> Option<Option<u32>> {
        match ids.first() {
            None => Some(None),
            Some(&f) if self.comps(f).is_some() => {
                let n = self.comps(f)?;
                ids.iter()
                    .all(|&i| self.comps(i).is_some_and(|k| k >= n))
                    .then_some(Some(n))
            }
            Some(_) => ids.iter().all(|&i| self.is_scalar(i)).then_some(None),
        }
    }

    fn global(&self, id: u32, ty: u32, class: u32, init: Option<u32>) -> Global {
        let m = self.m;
        let Some(Ty::Ptr(pointee)) = m.types.get(&ty).cloned() else {
            return Global::Panic("variable type".into());
        };
        let r = self.r(id);
        let zero = || {
            let mut z = Vec::new();
            m.put(&m.zero(pointee), pointee, &mut z);
            z
        };
        match class {
            2 | 12 => {
                let Some((set, binding)) = m.binding(id) else {
                    return Global::Panic("buffer binding".into());
                };
                if matches!(m.types.get(&pointee), Some(Ty::Array(..) | Ty::RtArray(_))) {
                    Global::Desc { r, set, binding }
                } else {
                    Global::Block { r, set, binding }
                }
            }
            1 => match m.builtin(id) {
                Some(b) => Global::BuiltIn {
                    r,
                    b,
                    n: m.flat(pointee),
                },
                None => Global::Panic("input without a built-in".into()),
            },
            4 => Global::Workgroup {
                r,
                id,
                zero: match init {
                    Some(i) => {
                        let n = m.flat(pointee) as usize;
                        let s = self.r(i) as usize;
                        self.p.init[s..s + n].to_vec()
                    }
                    None => zero(),
                },
            },
            _ => Global::Logical {
                r,
                init: init.map(|i| self.r(i)),
                zero: zero(),
            },
        }
    }

    fn compile_func(&mut self, fid: u32) -> Func {
        let m = self.m;
        let f = &m.funcs[&fid];
        let start = self.p.code.len() as u32;
        let params = f
            .params
            .iter()
            .map(|&p| (self.r(p), self.flat_id(p)))
            .collect();
        self.labels.clear();
        self.fixups.clear();
        let mut seg: Option<usize> = None;
        let mut seg_n = 0u32;
        for ins in &f.code {
            if seg.is_none() {
                if ins.op == 248 {
                    self.labels.insert(ins.args[0], self.p.code.len() as u32);
                    self.p.code.push(Op::Label {
                        label: ins.args[0],
                        n: 0,
                    });
                } else {
                    self.p.code.push(Op::Count { n: 0 });
                }
                seg = Some(self.p.code.len() - 1);
                seg_n = 0;
            } else if ins.op == 248 {
                panic!("label %{} inside a block", ins.args[0]);
            }
            seg_n += 1;
            self.inst(ins);
            if matches!(
                ins.op,
                57 | 224 | 345 | 249 | 250 | 251 | 253 | 254 | 252 | 4416 | 255
            ) {
                let at = seg.take().unwrap_or_default();
                match &mut self.p.code[at] {
                    Op::Label { n, .. } | Op::Count { n } => *n = seg_n,
                    _ => unreachable!(),
                }
            }
        }
        if let Some(at) = seg {
            match &mut self.p.code[at] {
                Op::Label { n, .. } | Op::Count { n } => *n = seg_n,
                _ => unreachable!(),
            }
            let msg = self.msg(format!("function %{fid} runs off its end"));
            self.p.code.push(Op::Panic { msg });
        }
        let fixups = std::mem::take(&mut self.fixups);
        for &(at, slot) in &fixups {
            let resolve = |cx: &mut Cx, l: u32| -> u32 {
                match cx.labels.get(&l) {
                    Some(&pc) => pc,
                    None => {
                        let msg = cx.msg(format!("no label %{l} in function %{fid}"));
                        cx.p.code.push(Op::Panic { msg });
                        (cx.p.code.len() - 1) as u32
                    }
                }
            };
            match self.p.code[at] {
                Op::Jump { to } => {
                    let to = resolve(self, to);
                    self.p.code[at] = Op::Jump { to };
                }
                Op::Branch { c, t, f } => {
                    let (t, f) = (resolve(self, t), resolve(self, f));
                    self.p.code[at] = Op::Branch { c, t, f };
                }
                Op::Switch {
                    sel,
                    default,
                    at: sa,
                    k,
                } => {
                    if slot == 0 {
                        let default = resolve(self, default);
                        self.p.code[at] = Op::Switch {
                            sel,
                            default,
                            at: sa,
                            k,
                        };
                        for j in 0..k as usize {
                            let l = self.p.side[sa as usize + 2 * j + 1];
                            if l != NONE {
                                let pc = resolve(self, l);
                                self.p.side[sa as usize + 2 * j + 1] = pc;
                            }
                        }
                    }
                }
                _ => unreachable!(),
            }
        }
        self.fixups = fixups;
        Func {
            id: fid,
            start,
            params,
        }
    }

    fn inst(&mut self, ins: &crate::Inst) {
        let m = self.m;
        let a = &ins.args;
        let d = if ins.id != 0 { self.r(ins.id) } else { 0 };
        match ins.op {
            248 | 247 | 246 | 225 => {}
            249 => {
                self.fixups.push((self.p.code.len(), 0));
                self.p.code.push(Op::Jump { to: a[0] });
            }
            250 => {
                if !self.is_scalar(a[0]) {
                    self.panic("scalar expected".into());
                    return;
                }
                self.fixups.push((self.p.code.len(), 0));
                self.p.code.push(Op::Branch {
                    c: self.r(a[0]),
                    t: a[1],
                    f: a[2],
                });
            }
            251 => {
                if !self.is_scalar(a[0]) {
                    self.panic("scalar expected".into());
                    return;
                }
                let mut cases = Vec::new();
                for c in a[2..].chunks(2) {
                    cases.push(c[0]);
                    cases.push(c.get(1).copied().unwrap_or(NONE));
                }
                let at = self.side(&cases);
                self.fixups.push((self.p.code.len(), 0));
                self.p.code.push(Op::Switch {
                    sel: self.r(a[0]),
                    default: a[1],
                    at,
                    k: (cases.len() / 2) as u32,
                });
            }
            253 | 252 | 4416 => self.p.code.push(Op::Return),
            254 => {
                let (s, n) = (self.r(a[0]), self.flat_id(a[0]));
                self.p.code.push(Op::ReturnValue { s, n });
            }
            255 => self.panic("OpUnreachable reached".into()),
            245 => {
                let mut xs = Vec::new();
                for c in a.chunks(2) {
                    xs.push(self.r(c[0]));
                    xs.push(c[1]);
                }
                let at = self.side(&xs);
                self.p.code.push(Op::Phi {
                    d,
                    n: self.flat_id(ins.id),
                    at,
                    k: (xs.len() / 2) as u32,
                });
            }
            59 => {
                let Some(Ty::Ptr(pointee)) = m.types.get(&ins.ty) else {
                    self.panic("variable type".into());
                    return;
                };
                let pointee = *pointee;
                let init = a.get(1).map_or(NONE, |&i| self.r(i));
                let mut z = Vec::new();
                m.put(&m.zero(pointee), pointee, &mut z);
                self.p.zeros.push(z);
                self.p.code.push(Op::Var {
                    d,
                    init,
                    zero: (self.p.zeros.len() - 1) as u32,
                });
            }
            61 | 227 => {
                let p = self.r(a[0]);
                let mut l = Vec::new();
                let r = self.load_layout(ins.ty, 0, &mut l).map(|()| l);
                let plan = self.layout(r);
                let n = self.flat_id(ins.id);
                self.p.code.push(Op::Load { d, p, plan, n });
            }
            62 | 228 => {
                let v = if ins.op == 62 { a[1] } else { a[3] };
                let p = self.r(a[0]);
                let pointee = match m.types.get(&self.ty_of(a[0])) {
                    Some(Ty::Ptr(t)) => *t,
                    _ => 0,
                };
                let mut l = Vec::new();
                let r = self.store_layout(pointee, 0, &mut l).map(|()| l);
                let plan = self.layout(r);
                let s = self.r(v);
                let n = self.flat_id(v);
                self.p.code.push(Op::Store { p, s, plan, n });
            }
            234 | 230 | 229 => {
                let p = self.r(a[0]);
                let mut l = Vec::new();
                let r = self.load_layout(ins.ty, 0, &mut l).map(|()| l);
                let load = self.layout(r);
                let pointee = match m.types.get(&self.ty_of(a[0])) {
                    Some(Ty::Ptr(t)) => *t,
                    _ => 0,
                };
                let mut l = Vec::new();
                let r = self.store_layout(pointee, 0, &mut l).map(|()| l);
                let store = self.layout(r);
                let w = self.width(ins.ty).unwrap_or(64);
                let ops: Vec<u32> = match ins.op {
                    234 => vec![u32::from(ins.op), load, store, w, self.r(a[3])],
                    230 => vec![
                        u32::from(ins.op),
                        load,
                        store,
                        w,
                        self.r(a[5]),
                        self.r(a[6]),
                    ],
                    _ => vec![u32::from(ins.op), load, store, w, self.r(a[3])],
                };
                if self.flat_id(ins.id) != 1 {
                    self.panic("atomic of a composite".into());
                    return;
                }
                let at = self.side(&ops);
                self.p.code.push(Op::Atomic { d, p, at });
            }
            65 | 66 => self.chain(ins),
            57 => {
                let Ok(fi) = self.fids.binary_search(&a[0]) else {
                    self.panic(format!("no function %{}", a[0]));
                    return;
                };
                let mut args: Vec<u32> = a[1..].iter().map(|&i| self.r(i)).collect();
                let k = args.len() as u32;
                args.push(self.flat_id(ins.id));
                let at = self.side(&args);
                self.p.code.push(Op::Call {
                    func: fi as u32,
                    at,
                    k,
                    d,
                });
            }
            224 => self.p.code.push(Op::Barrier),
            345 => {
                if !self.is_scalar(a[1]) || !self.is_scalar(a[2]) {
                    self.panic("scalar expected".into());
                    return;
                }
                self.p.code.push(Op::Shuffle {
                    d,
                    v: self.r(a[1]),
                    lane: self.r(a[2]),
                });
            }
            81 => match self.extract_at(self.ty_of(a[0]), &a[1..]) {
                Some(off) => {
                    let n = self.flat_id(ins.id);
                    self.p.code.push(Op::Copy {
                        d,
                        s: self.r(a[0]) + off,
                        n,
                    });
                }
                None => self.panic("composite expected".into()),
            },
            82 => match self.extract_at(self.ty_of(a[1]), &a[2..]) {
                Some(off) => {
                    let n = self.flat_id(a[1]);
                    self.p.code.push(Op::Copy {
                        d,
                        s: self.r(a[1]),
                        n,
                    });
                    let k = self.flat_id(a[0]);
                    self.p.code.push(Op::Copy {
                        d: d + off,
                        s: self.r(a[0]),
                        n: k,
                    });
                }
                None => self.panic("insert into scalar".into()),
            },
            80 => {
                let mut off = 0;
                for &i in a {
                    let n = self.flat_id(i);
                    self.p.code.push(Op::Copy {
                        d: d + off,
                        s: self.r(i),
                        n,
                    });
                    off += n;
                }
            }
            79 => {
                let (Some(nx), Some(ny)) = (self.comps(a[0]), self.comps(a[1])) else {
                    self.panic("composite expected".into());
                    return;
                };
                if a[2..].iter().any(|&k| k >= nx + ny) {
                    self.panic("vector shuffle component out of range".into());
                    return;
                }
                for (j, &k) in a[2..].iter().enumerate() {
                    let s = if k < nx {
                        self.r(a[0]) + k
                    } else {
                        self.r(a[1]) + k - nx
                    };
                    self.p.code.push(Op::Copy {
                        d: d + j as u32,
                        s,
                        n: 1,
                    });
                }
            }
            83 => {
                let n = self.flat_id(a[0]);
                self.p.code.push(Op::CopyK {
                    d,
                    s: self.r(a[0]),
                    n,
                });
            }
            1 => {}
            124 => self.bitcast(ins),
            120 => {
                if !self.is_scalar(a[0]) {
                    self.panic("scalar expected".into());
                    return;
                }
                self.p.code.push(Op::PtrOfU { d, s: self.r(a[0]) });
            }
            169 => {
                let n = self.flat_id(ins.id);
                if self.is_scalar(a[0]) {
                    let at = self.side(&[self.r(a[1]), self.r(a[2]), n]);
                    self.p.code.push(Op::Select {
                        d,
                        c: self.r(a[0]),
                        at,
                    });
                } else if let Some(k) = self.comps(a[0]) {
                    if self.comps(a[1]).is_none_or(|x| x < k)
                        || self.comps(a[2]).is_none_or(|x| x < k)
                    {
                        self.panic("composite expected".into());
                        return;
                    }
                    let at = self.side(&[self.r(a[0]), self.r(a[1]), self.r(a[2])]);
                    self.p.code.push(Op::SelectV { d, at, n: k });
                } else {
                    self.panic("select condition".into());
                }
            }
            149..=152 => {
                let w = match m.types.get(&ins.ty) {
                    Some(Ty::Struct(ms)) => self.width(self.scalar_of(ms[0])),
                    _ => None,
                };
                let Some(w) = w else {
                    self.panic("struct expected".into());
                    return;
                };
                if !self.is_scalar(a[0]) || !self.is_scalar(a[1]) {
                    self.panic("scalar expected".into());
                    return;
                }
                self.p.code.push(Op::Wide {
                    op: ins.op,
                    w: w as u8,
                    d,
                    a: self.r(a[0]),
                    b: self.r(a[1]),
                });
            }
            crate::OP_EXT_INST => self.ext(ins),
            _ => self.elementwise_op(ins),
        }
    }

    fn extract_at(&self, mut ty: u32, path: &[u32]) -> Option<u32> {
        let m = self.m;
        let mut off = 0;
        for &k in path {
            match m.types.get(&ty)? {
                Ty::Vec(e, n) if k < *n => {
                    off += k * m.flat(*e);
                    ty = *e;
                }
                Ty::Array(e, n) if u64::from(k) < *n => {
                    off += k * m.flat(*e);
                    ty = *e;
                }
                Ty::Struct(ms) if (k as usize) < ms.len() => {
                    off += ms[..k as usize].iter().map(|&t| m.flat(t)).sum::<u32>();
                    ty = ms[k as usize];
                }
                _ => return None,
            }
        }
        Some(off)
    }

    fn chain(&mut self, ins: &crate::Inst) {
        let m = self.m;
        let a = &ins.args;
        let d = self.r(ins.id);
        let Some(Ty::Ptr(mut ty)) = m.types.get(&self.ty_of(a[0])).cloned() else {
            self.panic("access chain base".into());
            return;
        };
        let mut links = Vec::new();
        for &i in &a[1..] {
            if !self.is_scalar(i) {
                self.panic("scalar expected".into());
                return;
            }
            let c = self.const_of(i);
            let idx = c.map_or(Index::Reg(self.r(i)), Index::Const);
            let (var, mem, next) = match m.types.get(&ty).cloned() {
                Some(Ty::Struct(ms)) => {
                    let Some(k) = c else {
                        self.panic("struct index not a constant".into());
                        return;
                    };
                    let Some(&t) = ms.get(k as usize) else {
                        self.panic(format!("struct %{ty} has no member {k}"));
                        return;
                    };
                    let var = Ok(VarStep::Add(u64::from(
                        ms[..k as usize].iter().map(|&t| m.flat(t)).sum::<u32>(),
                    )));
                    let mem = match m.member_off.get(&(ty, k as u32)) {
                        Some(&o) => Ok(MemStep::Add(o)),
                        None => Err(self.msg(format!("no offset of member {k} of %{ty}"))),
                    };
                    (var, mem, t)
                }
                Some(Ty::Array(e, n)) => (
                    Ok(VarStep::Elem {
                        size: u64::from(m.flat(e)),
                        len: n,
                    }),
                    Ok(MemStep::Mul(m.stride(ty, e))),
                    e,
                ),
                Some(Ty::RtArray(e)) => (
                    Ok(VarStep::Elem {
                        size: u64::from(m.flat(e)),
                        len: u64::MAX,
                    }),
                    Ok(MemStep::Mul(m.stride(ty, e))),
                    e,
                ),
                Some(Ty::Vec(e, n)) => (
                    Ok(VarStep::Elem {
                        size: u64::from(m.flat(e)),
                        len: u64::from(n),
                    }),
                    Ok(MemStep::Mul(m.size(e))),
                    e,
                ),
                t => {
                    let msg = match t {
                        Some(t) => self.msg(format!("access chain into {t:?}")),
                        None => self.msg(format!("type %{ty}")),
                    };
                    (Err(msg), Err(msg), ty)
                }
            };
            links.push(Link { idx, var, mem });
            ty = next;
        }
        self.p.chains.push(Chain { links });
        self.p.code.push(Op::Chain {
            d,
            base: self.r(a[0]),
            chain: (self.p.chains.len() - 1) as u32,
        });
    }

    fn bitcast(&mut self, ins: &crate::Inst) {
        let m = self.m;
        let a = &ins.args;
        let d = self.r(ins.id);
        let s = self.r(a[0]);
        let src_ty = self.ty_of(a[0]);
        let dst = m.types.get(&ins.ty).cloned();
        if m.is_ptr(src_ty) {
            self.panic("bitcast of a pointer".into());
            return;
        }
        if self.is_scalar(a[0]) {
            match dst {
                Some(Ty::Vec(e, n)) => match self.width(e) {
                    Some(w) => self.p.code.push(Op::Split { d, s, n, w }),
                    None => self.panic(format!("width of %{e}")),
                },
                Some(Ty::Ptr(_)) => self.p.code.push(Op::NotPtr { d, s }),
                _ => self.p.code.push(Op::Copy1K { d, s }),
            }
            return;
        }
        let Some(n) = self.comps(a[0]) else {
            self.panic("bitcast of a non-vector composite".into());
            return;
        };
        match dst {
            Some(Ty::Ptr(_)) if n >= 2 => self.p.code.push(Op::PtrOfVec { d, s }),
            Some(Ty::Vec(..)) => {
                self.panic("bitcast between vectors (the reference gives a scalar)".into());
            }
            Some(_) => match self.width(self.scalar_of(src_ty)) {
                Some(w) => self.p.code.push(Op::Join { d, s, n, w }),
                None => self.panic("width of a bitcast source".into()),
            },
            None => self.panic(format!("type %{}", ins.ty)),
        }
    }

    fn ext(&mut self, ins: &crate::Inst) {
        let a = &ins.args;
        let ids = &a[2..];
        let Some(shape) = self.elementwise(ids) else {
            self.panic("scalar expected".into());
            return;
        };
        let Some(w) = self.width(self.scalar_of(ins.ty)) else {
            self.panic(format!("width of %{}", ins.ty));
            return;
        };
        if ids.len() > 4 {
            self.panic("too many operands".into());
            return;
        }
        let d = self.r(ins.id);
        for k in 0..shape.unwrap_or(1) {
            let mut args = [0; 4];
            for (j, &i) in ids.iter().enumerate() {
                args[j] = self.r(i) + k;
            }
            self.p.exts.push(ExtGen {
                inst: a[1],
                w,
                n: ids.len() as u8,
                args,
            });
            self.p.code.push(Op::Ext {
                d: d + k,
                g: (self.p.exts.len() - 1) as u32,
            });
        }
    }

    fn elementwise_op(&mut self, ins: &crate::Inst) {
        let a = &ins.args;
        let op = ins.op;
        let Some(shape) = self.elementwise(a) else {
            self.panic("scalar expected".into());
            return;
        };
        let Some(w) = self.width(self.scalar_of(ins.ty)) else {
            self.panic(format!("type %{}", ins.ty));
            return;
        };
        let needs_src = matches!(op, 173 | 175 | 177 | 179 | 114 | 111 | 112);
        let src_w = match a
            .first()
            .and_then(|&i| self.width(self.scalar_of(self.ty_of(i))))
        {
            Some(w) => w,
            None if needs_src => {
                self.panic("width of an operand".into());
                return;
            }
            None => 0,
        };
        if a.len() > 4 {
            self.panic(format!("unsupported op {op}"));
            return;
        }
        let d = self.r(ins.id);
        let r = |cx: &Self, j: usize, k: u32| cx.r(a[j]) + k;
        for k in 0..shape.unwrap_or(1) {
            let dk = d + k;
            let fast = match (op, a.len()) {
                (128, 2) => Some(Op::IAdd {
                    w: w as u8,
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (130, 2) => Some(Op::ISub {
                    w: w as u8,
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (132, 2) => Some(Op::IMul {
                    w: w as u8,
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (199, 2) => Some(Op::And {
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (197, 2) => Some(Op::Or {
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (198, 2) => Some(Op::Xor {
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (200, 1) => Some(Op::Not {
                    w: w as u8,
                    d: dk,
                    a: r(self, 0, k),
                }),
                (194, 2) => Some(Op::Shr {
                    w: w as u8,
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (195, 2) => Some(Op::Sar {
                    w: w as u8,
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (196, 2) => Some(Op::Shl {
                    w: w as u8,
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (170 | 164, 2) => Some(Op::Eq {
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (171 | 165, 2) => Some(Op::Ne {
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (172 | 174 | 176 | 178, 2) => Some(Op::UCmp {
                    k: ((op - 172) / 2) as u8,
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (173 | 175 | 177 | 179, 2) => Some(Op::SCmp {
                    k: ((op - 173) / 2) as u8,
                    w: src_w as u8,
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (166, 2) => Some(Op::LOr {
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (167, 2) => Some(Op::LAnd {
                    d: dk,
                    a: r(self, 0, k),
                    b: r(self, 1, k),
                }),
                (168, 1) => Some(Op::LNot {
                    d: dk,
                    a: r(self, 0, k),
                }),
                _ => None,
            };
            if let Some(o) = fast
                && (1..=64).contains(&w)
                && (src_w >= 1 || !needs_src)
            {
                self.p.code.push(o);
                continue;
            }
            let mut args = [0; 4];
            for (j, x) in args.iter_mut().enumerate().take(a.len()) {
                *x = r(self, j, k);
            }
            let known = [
                a.first().map_or(0, |&i| self.r(i)),
                a.get(1).map_or(0, |&i| self.r(i)),
            ];
            self.p.gens.push(Gen {
                op,
                w,
                src_w,
                n: a.len() as u8,
                args,
                known,
            });
            self.p.code.push(Op::Gen {
                d: dk,
                g: (self.p.gens.len() - 1) as u32,
            });
        }
    }
}

/// Compiles `m`. Workgroup variables are memory at `vars` (a workgroup
/// run) or, with `None`, logical variables.
pub(crate) fn compile(m: &Module) -> Program {
    let mut reg = vec![u32::MAX; m.bound];
    let mut regs = 0u32;
    let mut flat_of = HashMap::new();
    for (r, &ty) in reg.iter_mut().zip(&m.id_ty) {
        if ty == 0 {
            continue;
        }
        let n = *flat_of.entry(ty).or_insert_with(|| m.flat(ty));
        *r = regs;
        regs += n.max(1);
    }
    let unused = regs;
    regs += 1;
    for r in &mut reg {
        if *r == u32::MAX {
            *r = unused;
        }
    }
    let mut init = vec![GARBAGE; regs as usize];
    let mut known = vec![false; regs as usize];
    for (id, v) in &m.consts {
        let ty = m.id_ty[*id as usize];
        let mut out = Vec::new();
        m.put(v, ty, &mut out);
        let r = reg[*id as usize] as usize;
        init[r..r + out.len()].copy_from_slice(&out);
        known[r] = true;
    }
    let mut cx = Cx {
        m,
        p: Program {
            globals: Vec::new(),
            code: Vec::new(),
            funcs: Vec::new(),
            entry: 0,
            side: Vec::new(),
            chains: Vec::new(),
            layouts: Vec::new(),
            gens: Vec::new(),
            exts: Vec::new(),
            msgs: Vec::new(),
            zeros: Vec::new(),
            init,
            known,
            reg,
        },
        msg_ids: HashMap::new(),
        labels: HashMap::new(),
        fixups: Vec::new(),
        fids: {
            let mut v: Vec<u32> = m.funcs.keys().copied().collect();
            v.sort_unstable();
            v
        },
    };
    for &(id, ty, class, init) in &m.globals {
        let g = cx.global(id, ty, class, init);
        cx.p.globals.push(g);
    }
    for &fid in &cx.fids.clone() {
        let f = cx.compile_func(fid);
        cx.p.funcs.push(f);
    }
    cx.p.entry = cx
        .fids
        .iter()
        .position(|&f| f == m.entry)
        .map_or(u32::MAX, |i| i as u32);
    cx.p
}

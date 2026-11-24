//! The tree-walking interpreter the compiled one ([`crate::Run`]) was
//! built to match: it reads each instruction's operands by id and
//! boxes every value. The equivalence tests run both and compare every
//! result.

pub mod group;

use std::rc::Rc;
use std::sync::Arc;

use crate::{
    Denorm, Env, GARBAGE, Inst, Memory, Module, OP_EXT_INST, OP_VARIABLE, Ptr, Ty, V, f16_to_f64,
    int_to_f32, mask, round_f16, round_f32, sext, two_sum,
};

/// One invocation run by walking its instructions.
pub struct Run<'m> {
    m: &'m Module,
    env: &'m Env,
    pub mem: Memory,
    vals: Vec<V>,
    slots: Vec<V>,
    pub steps: u64,
    /// The most instructions the invocation may run.
    pub max_steps: u64,
    /// The workgroup this invocation runs in, and its index there: memory
    /// is the group's, and barriers and shuffles wait for the others.
    pub group: Option<(Arc<group::Group>, usize)>,
    /// The ids whose value comes from a constant (see [`Env::fold_identity`]).
    known: Vec<bool>,
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
            max_steps: 50_000_000,
            group: None,
            known: vec![false; m.bound],
        }
    }

    pub fn main(&mut self) {
        let m = self.m;
        for (id, v) in &m.consts {
            self.vals[*id as usize] = v.clone();
            self.known[*id as usize] = true;
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
                        41 => V::S(u64::from(
                            self.env.local_index % self.env.subgroup_size.max(1),
                        )),
                        40 => V::S(u64::from(
                            self.env.local_index / self.env.subgroup_size.max(1),
                        )),
                        29 => V::S(u64::from(self.env.local_index)),
                        36 => V::S(u64::from(self.env.subgroup_size.max(1))),
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
                4 if self.group.is_some() => {
                    let g = &self.group.as_ref().unwrap().0;
                    V::P(Ptr::Mem(g.vars[&id], pointee))
                }
                _ => {
                    let v = init.map_or_else(|| m.zero(pointee), |i| self.vals[i as usize].clone());
                    self.slots.push(v);
                    V::P(Ptr::Var(self.slots.len() - 1, Rc::new(Vec::new())))
                }
            };
            self.vals[id as usize] = v;
        }
        self.call(m.entry, &[], &[]);
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

    fn call(&mut self, fid: u32, args: &[V], known: &[bool]) -> V {
        let m = self.m;
        let f = &m.funcs[&fid];
        for (k, (p, a)) in f.params.iter().zip(args).enumerate() {
            self.vals[*p as usize] = a.clone();
            self.known[*p as usize] = known.get(k).copied().unwrap_or(false);
        }
        let mut pc = 0;
        let mut prev_label = 0;
        let mut cur_label = 0;
        loop {
            let ins = &f.code[pc];
            pc += 1;
            self.steps += 1;
            assert!(
                self.steps < self.max_steps,
                "runaway: {} steps, in block %{cur_label} of function %{fid}",
                self.steps
            );
            let a = &ins.args;
            match ins.op {
                248 => {
                    prev_label = cur_label;
                    cur_label = a[0];
                    if let Some((g, i)) = &self.group {
                        g.enter(*i, fid, cur_label);
                        g.count(*i, self.steps);
                    }
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
                    let known: Vec<bool> = a[1..].iter().map(|&i| self.known[i as usize]).collect();
                    let r = self.call(a[0], &args, &known);
                    if ins.id != 0 {
                        self.vals[ins.id as usize] = r;
                    }
                }
                224 => {
                    if let Some((g, i)) = &self.group {
                        g.barrier(*i, cur_label);
                    }
                }
                225 => {} // MemoryBarrier: every access is in order
                // GroupNonUniformShuffle
                345 => {
                    let Some((g, i)) = &self.group else {
                        panic!("shuffle outside a workgroup run")
                    };
                    let v = g.shuffle(*i, cur_label, self.sv(a[1]), self.sv(a[2]) as u32);
                    self.vals[ins.id as usize] = V::S(v);
                }
                // AtomicLoad, AtomicStore: each access is atomic.
                227 => {
                    let p = self.val(a[0]);
                    let v = self.load(&p, ins.ty);
                    self.vals[ins.id as usize] = v;
                }
                228 => {
                    let p = self.val(a[0]);
                    let v = self.val(a[3]);
                    self.store(&p, v);
                }
                // AtomicIAdd, AtomicCompareExchange, AtomicExchange: a read
                // and a write under the group's atomic lock.
                234 | 230 | 229 => {
                    let g = self.group.as_ref().map(|(g, _)| Arc::clone(g));
                    let _held = g.as_ref().map(|g| group::lock(&g.atomic));
                    let p = self.val(a[0]);
                    let old = self.load(&p, ins.ty);
                    let new = match ins.op {
                        234 => V::S(old.s().wrapping_add(self.sv(a[3])) & mask(m.width(ins.ty))),
                        230 if old.s() == self.sv(a[6]) => self.val(a[5]),
                        230 => old.clone(),
                        _ => self.val(a[3]),
                    };
                    self.store(&p, new);
                    self.vals[ins.id as usize] = old;
                }
                _ => {
                    let v = self.op(ins);
                    self.vals[ins.id as usize] = v;
                    if ins.op == 124 || ins.op == 83 {
                        self.known[ins.id as usize] = self.known[a[0] as usize];
                    }
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

    /// Reads memory: in a workgroup, the invocation's own read-only copy
    /// (uniform blocks) first, then the group's.
    fn read(&mut self, addr: u64, n: u64) -> u64 {
        if let Some(b) = self.mem.find(addr, n) {
            let mut v = [0u8; 8];
            v[..n as usize].copy_from_slice(b);
            return u64::from_le_bytes(v);
        }
        match &self.group {
            Some((g, _)) => g.mem.read(addr, n),
            None => self.mem.read(addr, n),
        }
    }

    fn write(&mut self, addr: u64, n: u64, v: u64) {
        match &self.group {
            Some((g, _)) => g.mem.write(addr, n, v),
            None => self.mem.write(addr, n, v),
        }
    }

    fn load_mem(&mut self, addr: u64, ty: u32) -> V {
        let m = self.m;
        match m.ty(ty).clone() {
            Ty::Int(w) | Ty::Float(w) => V::S(self.read(addr, u64::from(w / 8))),
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
            Ty::Int(w) | Ty::Float(w) => self.write(addr, u64::from(w / 8), v.s()),
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
            133 | 129 if self.env.fold_identity && self.folds(op, ids, x).is_some() => {
                self.folds(op, ids, x).unwrap_or(0)
            }
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

    /// What a driver folds a 32-bit OpFMul (133) or OpFAdd (129) of the ids
    /// `a` with the values `x` to (see [`Env::fold_identity`]), if anything.
    fn folds(&self, op: u16, a: &[u32], x: &[u64]) -> Option<u64> {
        for (k, o) in [(0, 1), (1, 0)] {
            if !self.known[a[k] as usize] {
                continue;
            }
            match (op, x[k]) {
                (133, 0x3f80_0000) => return Some(x[o]),
                (133, 0xbf80_0000) => return Some(x[o] ^ 0x8000_0000),
                (129, 0x8000_0000) => return Some(x[o]),
                _ => {}
            }
        }
        None
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

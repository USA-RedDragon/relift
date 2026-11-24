//! Runs compiled code ([`crate::code`]) for one invocation. The state of
//! an invocation is plain data with its own call stack, so a workgroup
//! run can stop it at a barrier or shuffle and go on with another one on
//! the same thread.

use std::collections::BTreeMap;

use crate::arith::Fx;
use crate::code::{
    DESC, Global, Index, MEM, MemStep, NOT_PTR, Op, Program, VAR, VAR_OOB, VAR_SCALAR, VarStep,
};
use crate::{Env, GARBAGE, Memory, mask, sext};

/// Memory as an invocation sees it.
pub(crate) trait Mem {
    /// The `n` bytes at `addr`, little-endian.
    fn read(&mut self, addr: u64, n: u64) -> u64;
    /// Writes the low `n` bytes of `v` at `addr`.
    fn write(&mut self, addr: u64, n: u64, v: u64);
    /// Runs an atomic read, modify and write with no other one between.
    fn atomic<R>(&mut self, f: impl FnOnce(&mut Self) -> R) -> R {
        f(self)
    }
}

/// The region of `regions` holding `n` bytes at `addr`, trying `hint`
/// first: (region, offset).
#[inline]
pub(crate) fn find<T>(
    regions: &[(u64, T)],
    len: impl Fn(&T) -> u64,
    addr: u64,
    n: u64,
    hint: &mut usize,
) -> Option<(usize, usize)> {
    if let Some((base, b)) = regions.get(*hint)
        && addr >= *base
        && addr + n <= *base + len(b)
    {
        return Some((*hint, (addr - *base) as usize));
    }
    for (k, (base, b)) in regions.iter().enumerate() {
        if addr >= *base && addr + n <= *base + len(b) {
            *hint = k;
            return Some((k, (addr - *base) as usize));
        }
    }
    None
}

/// [`Memory`] with a region hint.
pub(crate) struct Plain<'a> {
    pub mem: &'a mut Memory,
    pub hint: usize,
}

impl Plain<'_> {
    pub fn get(&mut self, addr: u64, n: u64) -> Option<u64> {
        let (r, o) = find(
            &self.mem.regions,
            |b| b.len() as u64,
            addr,
            n,
            &mut self.hint,
        )?;
        let b = &self.mem.regions[r].1[o..o + n as usize];
        Some(le(b))
    }

    pub fn put(&mut self, addr: u64, n: u64, v: u64) -> bool {
        let Some((r, o)) = find(
            &self.mem.regions,
            |b| b.len() as u64,
            addr,
            n,
            &mut self.hint,
        ) else {
            return false;
        };
        self.mem.regions[r].1[o..o + n as usize].copy_from_slice(&v.to_le_bytes()[..n as usize]);
        true
    }
}

#[inline]
pub(crate) fn le(b: &[u8]) -> u64 {
    match *b {
        [a, b2, c, d] => u64::from(u32::from_le_bytes([a, b2, c, d])),
        _ => {
            let mut v = [0u8; 8];
            v[..b.len()].copy_from_slice(b);
            u64::from_le_bytes(v)
        }
    }
}

impl Mem for Plain<'_> {
    fn read(&mut self, addr: u64, n: u64) -> u64 {
        self.get(addr, n)
            .unwrap_or_else(|| panic!("access to unmapped memory {addr:#x}+{n}"))
    }

    fn write(&mut self, addr: u64, n: u64, v: u64) {
        if !self.put(addr, n, v) {
            panic!("access to unmapped memory {addr:#x}+{n}");
        }
    }
}

/// Why [`Inv::run`] returned.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Event {
    Done,
    /// Waits at a barrier in this block.
    Barrier(u32),
    /// Waits at a shuffle in `block`: gives `value`, reads lane `lane`.
    Shuffle {
        block: u32,
        value: u64,
        lane: u32,
    },
    /// In lockstep, stops before an instruction whose order across lanes
    /// matters (see [`Inv::lockstep`]).
    At,
}

struct Frame {
    pc: u32,
    func: u32,
    prev: u32,
    cur: u32,
    d: u32,
    n: u32,
    vm: usize,
}

/// One invocation's state.
pub(crate) struct Inv {
    regs: Vec<u64>,
    known: Vec<bool>,
    /// Logical variables, flattened.
    vm: Vec<u64>,
    frames: Vec<Frame>,
    pc: u32,
    func: u32,
    prev: u32,
    cur: u32,
    /// Where a shuffle's result goes.
    pending: u32,
    pub steps: u64,
    pub max_steps: u64,
    /// The (function, block) last entered.
    pub at: (u32, u32),
    /// Stop before every access to memory other invocations see, every
    /// jump or branch that may go back, call, return and switch, with
    /// [`Event::At`], so a warp's lanes can run them in lockstep.
    pub lockstep: bool,
    /// Runs the instruction it stopped before instead of stopping again.
    pub pass: bool,
    /// The code position of the instruction the invocation last stopped
    /// at (before it, or at a barrier or shuffle).
    stop_pc: u32,
}

impl Inv {
    /// An invocation of program `p` about to run its entry point.
    /// Workgroup variables are memory at `vars`, or logical variables
    /// without.
    pub fn new(p: &Program, env: &Env, vars: Option<&BTreeMap<u32, u64>>) -> Inv {
        let mut inv = Inv {
            regs: p.init.clone(),
            known: p.known.clone(),
            vm: Vec::new(),
            frames: Vec::new(),
            pc: 0,
            func: p.entry,
            prev: 0,
            cur: 0,
            pending: 0,
            steps: 0,
            max_steps: 50_000_000,
            at: (0, 0),
            lockstep: false,
            pass: false,
            stop_pc: 0,
        };
        for g in &p.globals {
            let (r, v) = match g {
                Global::Desc { r, set, binding } => (
                    *r,
                    [DESC | u64::from(*set) << 8 | u64::from(*binding) << 32, 0],
                ),
                Global::Block { r, set, binding } => {
                    let b = (*set, *binding);
                    let addr = *env
                        .uniforms
                        .get(&b)
                        .unwrap_or_else(|| panic!("no buffer at {b:?}"));
                    (*r, [MEM, addr])
                }
                Global::BuiltIn { r, b, n } => {
                    let vec3 = |v: [u32; 3]| v.map(u64::from).to_vec();
                    let one = |x: u32| vec![u64::from(x)];
                    let mut v = match b {
                        27 => vec3(env.local_id),
                        26 => vec3(env.group_id),
                        41 => one(env.local_index % env.subgroup_size.max(1)),
                        40 => one(env.local_index / env.subgroup_size.max(1)),
                        29 => one(env.local_index),
                        36 => one(env.subgroup_size.max(1)),
                        28 => vec3([
                            env.group_id[0] + env.local_id[0],
                            env.local_id[1],
                            env.local_id[2],
                        ]),
                        _ => panic!("built-in {b}"),
                    };
                    if v.len() != *n as usize {
                        v = vec![GARBAGE; *n as usize];
                    }
                    let at = inv.vm.len();
                    inv.vm.extend(v);
                    (*r, [VAR, at as u64])
                }
                Global::Workgroup { r, id, .. } if vars.is_some() => {
                    (*r, [MEM, vars.unwrap_or_else(|| unreachable!())[id]])
                }
                Global::Workgroup { r, zero, .. }
                | Global::Logical {
                    r,
                    zero,
                    init: None,
                } => {
                    let at = inv.vm.len();
                    inv.vm.extend_from_slice(zero);
                    (*r, [VAR, at as u64])
                }
                Global::Logical {
                    r,
                    zero,
                    init: Some(i),
                } => {
                    let at = inv.vm.len();
                    let s = *i as usize;
                    inv.vm.extend_from_slice(&p.init[s..s + zero.len()]);
                    (*r, [VAR, at as u64])
                }
                Global::Panic(msg) => panic!("{msg}"),
            };
            inv.regs[r as usize..r as usize + 2].copy_from_slice(&v);
        }
        match p.funcs.get(p.entry as usize) {
            Some(f) => inv.pc = f.start,
            None => panic!("no entry point"),
        }
        inv
    }

    /// Orders invocations by where they stopped: the return positions of
    /// their calls, outermost first, then the stop's code position. A
    /// warp runs its lowest lanes first, so divergent paths run in turn
    /// (the earlier in the code first, a loop until it exits) and lanes
    /// reconverge where their paths meet.
    pub fn pos_cmp(&self, o: &Inv) -> std::cmp::Ordering {
        self.frames
            .iter()
            .map(|f| f.pc)
            .chain([self.stop_pc])
            .cmp(o.frames.iter().map(|f| f.pc).chain([o.stop_pc]))
    }

    /// Whether the instruction at `pc` is one a lockstep run stops before.
    #[inline]
    fn stops(&self, op: Op, pc: usize) -> bool {
        match op {
            Op::Load { p, .. } | Op::Store { p, .. } => self.regs[p as usize] != VAR,
            Op::Atomic { .. }
            | Op::Call { .. }
            | Op::Return
            | Op::ReturnValue { .. }
            | Op::Switch { .. } => true,
            Op::Jump { to } => to as usize <= pc,
            Op::Branch { t, f, .. } => t as usize <= pc || f as usize <= pc,
            _ => false,
        }
    }

    /// Gives a shuffle the invocation waits at its result.
    pub fn resume(&mut self, v: u64) {
        self.regs[self.pending as usize] = v;
    }

    #[cold]
    #[inline(never)]
    fn runaway(&self, p: &Program, n: u32, at_label: Option<u32>) -> ! {
        let j = self
            .max_steps
            .saturating_sub(self.steps + 1)
            .min(u64::from(n) - 1);
        let block = match at_label {
            Some(l) if j > 0 => l,
            _ => self.cur,
        };
        panic!(
            "runaway: {} steps, in block %{block} of function %{}",
            self.steps + j + 1,
            p.funcs[self.func as usize].id
        )
    }

    /// Panics as the reference does on an operand of the wrong shape,
    /// printing it as the reference's value would print.
    #[cold]
    #[inline(never)]
    fn refuse(&self, composite: bool, kind: u8, r: usize, ty: u32) -> ! {
        let s = |v: u64| {
            if v == GARBAGE {
                "Undef".to_string()
            } else {
                format!("S({v})")
            }
        };
        let got = match kind {
            0 => s(self.regs[r]),
            1 => match self.regs[r] {
                MEM => format!("P(Mem({}, {ty}))", self.regs[r + 1]),
                NOT_PTR => s(self.regs[r + 1]),
                t => format!("P(tag {t:#x})"),
            },
            _ => {
                let parts: Vec<String> = (0..ty as usize).map(|k| s(self.regs[r + k])).collect();
                format!("C([{}])", parts.join(", "))
            }
        };
        let what = if composite { "composite" } else { "scalar" };
        panic!("{what} expected, got {got}")
    }

    #[cold]
    #[inline(never)]
    fn bad_ptr(what: &str, tag: u64, v: u64) -> ! {
        if tag & 0xff == VAR_OOB {
            panic!(
                "index out of bounds: the len is {} but the index is {v}",
                tag >> 8
            )
        }
        if tag == VAR_SCALAR {
            if what == "store to" {
                panic!("store path into scalar")
            }
            panic!("composite expected, got S(0)")
        }
        if tag == NOT_PTR {
            panic!("{what} S({v})")
        }
        panic!("{what} pointer tag {tag:#x}")
    }

    fn load<M: Mem>(&mut self, p: &Program, mem: &mut M, d: usize, ptr: usize, plan: u32, n: u32) {
        let (tag, v) = (self.regs[ptr], self.regs[ptr + 1]);
        match tag {
            VAR => {
                let o = v as usize;
                let n = n as usize;
                self.regs[d..d + n].copy_from_slice(&self.vm[o..o + n]);
            }
            MEM => match &p.layouts[plan as usize] {
                Ok(l) => {
                    for (k, &(o, b)) in l.iter().enumerate() {
                        self.regs[d + k] = mem.read(v + o, b);
                    }
                }
                Err(msg) => panic!("{}", p.msgs[*msg as usize]),
            },
            _ => Inv::bad_ptr("load from", tag, v),
        }
    }

    fn store<M: Mem>(&mut self, p: &Program, mem: &mut M, ptr: usize, s: usize, plan: u32, n: u32) {
        let (tag, v) = (self.regs[ptr], self.regs[ptr + 1]);
        match tag {
            VAR => {
                let o = v as usize;
                let n = n as usize;
                self.vm[o..o + n].copy_from_slice(&self.regs[s..s + n]);
            }
            MEM => match &p.layouts[plan as usize] {
                Ok(l) => {
                    for (k, &(o, b)) in l.iter().enumerate() {
                        mem.write(v + o, b, self.regs[s + k]);
                    }
                }
                Err(msg) => panic!("{}", p.msgs[*msg as usize]),
            },
            _ => Inv::bad_ptr("store to", tag, v),
        }
    }

    fn chain(&mut self, p: &Program, env: &Env, d: usize, base: usize, c: u32) {
        let links = &p.chains[c as usize].links;
        let (tag, v) = (self.regs[base], self.regs[base + 1]);
        let idx = |i: &Index| match *i {
            Index::Reg(r) => self.regs[r as usize],
            Index::Const(k) => k,
        };
        let mem_walk = |mut addr: u64, links: &[crate::code::Link]| -> u64 {
            for l in links {
                match l.mem {
                    Ok(MemStep::Add(o)) => addr += o,
                    Ok(MemStep::Mul(st)) => addr += st * idx(&l.idx),
                    Err(msg) => panic!("{}", p.msgs[msg as usize]),
                }
            }
            addr
        };
        let out = match tag {
            VAR => {
                let mut out = [VAR, v];
                for l in links {
                    let o = out[1];
                    out = match l.var {
                        Ok(VarStep::Add(k)) => [VAR, o + k],
                        Ok(VarStep::Elem { size, len }) => {
                            let i = idx(&l.idx);
                            if i >= len {
                                [VAR_OOB | len << 8, i]
                            } else {
                                [VAR, o + i * size]
                            }
                        }
                        Ok(VarStep::Scalar) | Err(_) => [VAR_SCALAR, 0],
                    };
                    if out[0] != VAR {
                        break;
                    }
                }
                out
            }
            VAR_SCALAR => [VAR_SCALAR, 0],
            t if t & 0xff == VAR_OOB => [t, v],
            MEM => [MEM, mem_walk(v, links)],
            t if t & 0xff == DESC => {
                let (set, binding) = (((t >> 8) & 0xff_ffff) as u32, (t >> 32) as u32);
                let i0 = idx(&links[0].idx);
                let addr = *env
                    .arrays
                    .get(&(set, binding, i0))
                    .unwrap_or_else(|| panic!("no buffer at ({set}, {binding})[{i0}]"));
                [MEM, mem_walk(addr, &links[1..])]
            }
            _ => Inv::bad_ptr("access chain base", tag, v),
        };
        self.regs[d..d + 2].copy_from_slice(&out);
    }

    /// Runs until the invocation returns from its entry point or waits at
    /// a barrier or shuffle (`group`; without it a barrier does nothing
    /// and a shuffle stops the invocation).
    pub fn run<M: Mem>(
        &mut self,
        p: &Program,
        fx: &Fx,
        env: &Env,
        mem: &mut M,
        group: bool,
    ) -> Event {
        let code = &p.code[..];
        let mut pc = self.pc as usize;
        loop {
            let op = code[pc];
            if self.lockstep && self.stops(op, pc) {
                if self.pass {
                    self.pass = false;
                } else {
                    self.pc = pc as u32;
                    self.stop_pc = pc as u32;
                    return Event::At;
                }
            }
            pc += 1;
            match op {
                Op::Label { label, n } => {
                    let s = self.steps + u64::from(n);
                    if s >= self.max_steps {
                        self.runaway(p, n, Some(label));
                    }
                    self.steps = s;
                    self.prev = self.cur;
                    self.cur = label;
                    self.at = (p.funcs[self.func as usize].id, label);
                }
                Op::Count { n } => {
                    let s = self.steps + u64::from(n);
                    if s >= self.max_steps {
                        self.runaway(p, n, None);
                    }
                    self.steps = s;
                }
                Op::Jump { to } => pc = to as usize,
                Op::Branch { c, t, f } => {
                    pc = if self.regs[c as usize] != 0 { t } else { f } as usize;
                }
                Op::Switch {
                    sel,
                    default,
                    at,
                    k,
                } => {
                    let sel = self.regs[sel as usize];
                    let mut t = default;
                    for c in p.side[at as usize..(at + 2 * k) as usize].chunks(2) {
                        if u64::from(c[0]) == sel {
                            assert!(c[1] != u32::MAX, "switch case without a label");
                            t = c[1];
                        }
                    }
                    pc = t as usize;
                }
                Op::Return => {
                    let Some(f) = self.frames.pop() else {
                        return Event::Done;
                    };
                    self.regs[f.d as usize..(f.d + f.n) as usize].fill(GARBAGE);
                    self.vm.truncate(f.vm);
                    pc = f.pc as usize;
                    (self.func, self.prev, self.cur) = (f.func, f.prev, f.cur);
                }
                Op::ReturnValue { s, n } => {
                    let Some(f) = self.frames.pop() else {
                        return Event::Done;
                    };
                    let k = n.min(f.n);
                    self.regs
                        .copy_within(s as usize..(s + k) as usize, f.d as usize);
                    self.regs[(f.d + k) as usize..(f.d + f.n) as usize].fill(GARBAGE);
                    self.vm.truncate(f.vm);
                    pc = f.pc as usize;
                    (self.func, self.prev, self.cur) = (f.func, f.prev, f.cur);
                }
                Op::Phi { d, n, at, k } => {
                    let pairs = &p.side[at as usize..(at + 2 * k) as usize];
                    let Some(c) = pairs.chunks(2).find(|c| c[1] == self.prev) else {
                        panic!("phi without edge from %{}", self.prev)
                    };
                    let s = c[0] as usize;
                    self.regs.copy_within(s..s + n as usize, d as usize);
                }
                Op::Var { d, init, zero } => {
                    let z = &p.zeros[zero as usize];
                    let at = self.vm.len();
                    if init == u32::MAX {
                        self.vm.extend_from_slice(z);
                    } else {
                        let s = init as usize;
                        self.vm.extend_from_slice(&self.regs[s..s + z.len()]);
                    }
                    self.regs[d as usize] = VAR;
                    self.regs[d as usize + 1] = at as u64;
                }
                Op::Load { d, p: ptr, plan, n } => {
                    if n == 1 && self.regs[ptr as usize] == VAR {
                        self.regs[d as usize] = self.vm[self.regs[ptr as usize + 1] as usize];
                    } else {
                        self.load(p, mem, d as usize, ptr as usize, plan, n);
                    }
                }
                Op::Store { p: ptr, s, plan, n } => {
                    if n == 1 && self.regs[ptr as usize] == VAR {
                        let o = self.regs[ptr as usize + 1] as usize;
                        self.vm[o] = self.regs[s as usize];
                    } else {
                        self.store(p, mem, ptr as usize, s as usize, plan, n);
                    }
                }
                Op::Atomic { d, p: ptr, at } => {
                    let sd = &p.side[at as usize..];
                    let (kind, load, store, w) = (sd[0], sd[1], sd[2], sd[3]);
                    let (x, y, missing) =
                        (self.regs[sd[4] as usize], self.regs[sd[5] as usize], sd[6]);
                    let tmp = self.regs.len() - 1;
                    let old = mem.atomic(|mem| {
                        self.load(p, mem, tmp, ptr as usize, load, 1);
                        let old = self.regs[tmp];
                        if missing != u32::MAX {
                            panic!("{}", p.msgs[missing as usize]);
                        }
                        let new = match kind {
                            234 => old.wrapping_add(x) & mask(w),
                            230 if old == y => x,
                            230 => old,
                            _ => x,
                        };
                        self.regs[tmp] = new;
                        self.store(p, mem, ptr as usize, tmp, store, 1);
                        old
                    });
                    self.regs[tmp] = GARBAGE;
                    self.regs[d as usize] = old;
                }
                Op::Chain { d, base, chain } => {
                    self.chain(p, env, d as usize, base as usize, chain)
                }
                Op::Call { func, at, k, d } => {
                    let f = &p.funcs[func as usize];
                    let args = &p.side[at as usize..(at + k) as usize];
                    for (&(pr, n), &a) in f.params.iter().zip(args) {
                        let (pr, a) = (pr as usize, a as usize);
                        self.regs.copy_within(a..a + n as usize, pr);
                        self.known[pr] = self.known[a];
                    }
                    self.frames.push(Frame {
                        pc: pc as u32,
                        func: self.func,
                        prev: self.prev,
                        cur: self.cur,
                        d,
                        n: p.side[(at + k) as usize],
                        vm: self.vm.len(),
                    });
                    self.func = func;
                    self.prev = 0;
                    self.cur = 0;
                    pc = f.start as usize;
                }
                Op::Barrier => {
                    if group {
                        self.pc = pc as u32;
                        self.stop_pc = pc as u32 - 1;
                        return Event::Barrier(self.cur);
                    }
                }
                Op::Shuffle { d, v, lane } => {
                    assert!(group, "shuffle outside a workgroup run");
                    self.pc = pc as u32;
                    self.stop_pc = pc as u32 - 1;
                    self.pending = d;
                    return Event::Shuffle {
                        block: self.cur,
                        value: self.regs[v as usize],
                        lane: self.regs[lane as usize] as u32,
                    };
                }
                Op::Copy { d, s, n } => {
                    if n == 1 {
                        self.regs[d as usize] = self.regs[s as usize];
                    } else {
                        self.regs
                            .copy_within(s as usize..(s + n) as usize, d as usize);
                    }
                }
                Op::Copy1K { d, s } => {
                    self.regs[d as usize] = self.regs[s as usize];
                    self.known[d as usize] = self.known[s as usize];
                }
                Op::CopyK { d, s, n } => {
                    self.regs
                        .copy_within(s as usize..(s + n) as usize, d as usize);
                    self.known[d as usize] = self.known[s as usize];
                }
                Op::PtrOfVec { d, s } => {
                    let v = self.regs[s as usize] | (self.regs[s as usize + 1] << 32);
                    self.regs[d as usize] = MEM;
                    self.regs[d as usize + 1] = v;
                    self.known[d as usize] = self.known[s as usize];
                }
                Op::PtrOfU { d, s } => {
                    self.regs[d as usize] = MEM;
                    self.regs[d as usize + 1] = self.regs[s as usize];
                }
                Op::NotPtr { d, s } => {
                    self.regs[d as usize] = NOT_PTR;
                    self.regs[d as usize + 1] = self.regs[s as usize];
                    self.known[d as usize] = self.known[s as usize];
                }
                Op::Split { d, s, n, w } => {
                    let x = self.regs[s as usize];
                    for k in 0..n {
                        self.regs[(d + k) as usize] = (x >> (w * k)) & mask(w);
                    }
                    self.known[d as usize] = self.known[s as usize];
                }
                Op::Join { d, s, n, w } => {
                    let mut acc = 0;
                    for k in 0..n {
                        acc |= self.regs[(s + k) as usize] << (w * k);
                    }
                    self.regs[d as usize] = acc;
                    self.known[d as usize] = self.known[s as usize];
                }
                Op::Select { d, c, at } => {
                    let sd = &p.side[at as usize..at as usize + 3];
                    let s = if self.regs[c as usize] != 0 {
                        sd[0]
                    } else {
                        sd[1]
                    } as usize;
                    let n = sd[2] as usize;
                    if n == 1 {
                        self.regs[d as usize] = self.regs[s];
                    } else {
                        self.regs.copy_within(s..s + n, d as usize);
                    }
                }
                Op::SelectV { d, at, n } => {
                    let sd = &p.side[at as usize..at as usize + 3];
                    for k in 0..n {
                        let c = self.regs[(sd[0] + k) as usize];
                        let s = if c != 0 { sd[1] } else { sd[2] } + k;
                        self.regs[(d + k) as usize] = self.regs[s as usize];
                    }
                }
                Op::Wide { op, w, d, a, b } => {
                    let w = u32::from(w);
                    let x = self.regs[a as usize];
                    let y = self.regs[b as usize];
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
                    self.regs[d as usize] = lo;
                    self.regs[d as usize + 1] = hi;
                }
                Op::IAdd { w, d, a, b } => {
                    self.regs[d as usize] = self.regs[a as usize]
                        .wrapping_add(self.regs[b as usize])
                        & mask(u32::from(w));
                }
                Op::ISub { w, d, a, b } => {
                    self.regs[d as usize] = self.regs[a as usize]
                        .wrapping_sub(self.regs[b as usize])
                        & mask(u32::from(w));
                }
                Op::IMul { w, d, a, b } => {
                    self.regs[d as usize] = self.regs[a as usize]
                        .wrapping_mul(self.regs[b as usize])
                        & mask(u32::from(w));
                }
                Op::And { d, a, b } => {
                    self.regs[d as usize] = self.regs[a as usize] & self.regs[b as usize];
                }
                Op::Or { d, a, b } => {
                    self.regs[d as usize] = self.regs[a as usize] | self.regs[b as usize];
                }
                Op::Xor { d, a, b } => {
                    self.regs[d as usize] = self.regs[a as usize] ^ self.regs[b as usize];
                }
                Op::Not { w, d, a } => {
                    self.regs[d as usize] = !self.regs[a as usize] & mask(u32::from(w));
                }
                Op::Shr { w, d, a, b } | Op::Sar { w, d, a, b } | Op::Shl { w, d, a, b } => {
                    let w = u32::from(w);
                    let mk = mask(w);
                    let (x, y) = (self.regs[a as usize], self.regs[b as usize]);
                    self.regs[d as usize] = if y >= u64::from(w) {
                        GARBAGE & mk
                    } else {
                        match op {
                            Op::Shr { .. } => (x >> y) & mk,
                            Op::Sar { .. } => ((sext(x, w) >> y) as u64) & mk,
                            _ => (x << y) & mk,
                        }
                    };
                }
                Op::Eq { d, a, b } => {
                    self.regs[d as usize] =
                        u64::from(self.regs[a as usize] == self.regs[b as usize]);
                }
                Op::Ne { d, a, b } => {
                    self.regs[d as usize] =
                        u64::from(self.regs[a as usize] != self.regs[b as usize]);
                }
                Op::UCmp { k, d, a, b } => {
                    let (x, y) = (self.regs[a as usize], self.regs[b as usize]);
                    self.regs[d as usize] = u64::from(match k {
                        0 => x > y,
                        1 => x >= y,
                        2 => x < y,
                        _ => x <= y,
                    });
                }
                Op::SCmp { k, w, d, a, b } => {
                    let w = u32::from(w);
                    let (x, y) = (
                        sext(self.regs[a as usize], w),
                        sext(self.regs[b as usize], w),
                    );
                    self.regs[d as usize] = u64::from(match k {
                        0 => x > y,
                        1 => x >= y,
                        2 => x < y,
                        _ => x <= y,
                    });
                }
                Op::LOr { d, a, b } => {
                    self.regs[d as usize] =
                        u64::from(self.regs[a as usize] != 0 || self.regs[b as usize] != 0);
                }
                Op::LAnd { d, a, b } => {
                    self.regs[d as usize] =
                        u64::from(self.regs[a as usize] != 0 && self.regs[b as usize] != 0);
                }
                Op::LNot { d, a } => {
                    self.regs[d as usize] = u64::from(self.regs[a as usize] == 0);
                }
                Op::Gen { d, g } => {
                    let g = &p.gens[g as usize];
                    let mut x = [0u64; 4];
                    for (k, &r) in g.args[..g.n as usize].iter().enumerate() {
                        x[k] = self.regs[r as usize];
                    }
                    let known = [
                        self.known[g.known[0] as usize],
                        self.known[g.known[1] as usize],
                    ];
                    self.regs[d as usize] =
                        fx.scalar(g.op, g.w, g.src_w, &x[..g.n as usize], known);
                }
                Op::Ext { d, g } => {
                    let g = &p.exts[g as usize];
                    let mut x = [0u64; 4];
                    for (k, &r) in g.args[..g.n as usize].iter().enumerate() {
                        x[k] = self.regs[r as usize];
                    }
                    self.regs[d as usize] = fx.ext_scalar(g.inst, g.w, &x[..g.n as usize]);
                }
                Op::Panic { msg } => panic!("{}", p.msgs[msg as usize]),
                Op::Refuse {
                    composite,
                    kind,
                    r,
                    ty,
                } => self.refuse(composite, kind, r as usize, ty),
            }
        }
    }
}

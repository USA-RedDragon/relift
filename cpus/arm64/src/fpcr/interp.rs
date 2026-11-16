//! The FPCR proof's abstract interpretation of one root function, with its
//! callees inlined.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::sync::Arc;

use relift_ir::{BinOp, Block, CmpOp, ExtKind, Flow, Next, Op, UnOp, Value, Width};

use super::value::{Base, Mem, TOP, V};
use super::{Code, Host};
use crate::regs;

/// The FPCR bits the guest sees (`RL_FPCR_GUEST`).
pub(super) const GUEST: u128 = 0x07c8_0000;
/// The runtime's exact-flags bit of the FPCR slot (`RL_FPCR_EXACT`).
const EXACT: u128 = 1 << 31;
/// The general and flag registers the state tracks: x0 to x30, sp, NZCV.
const TRACKED: usize = 36;

#[derive(Clone, Debug, PartialEq, Eq)]
struct St {
    regs: [V; TRACKED],
    mem: Mem,
}

impl St {
    fn join(&self, o: &St) -> St {
        let mut regs = self.regs;
        for (r, b) in regs.iter_mut().zip(o.regs.iter()) {
            *r = r.join(*b);
        }
        St {
            regs,
            mem: self.mem.join(&o.mem),
        }
    }
}

/// One frame of the inlined call tree: the context it was called from (or
/// none for the root), the call's address and return address, the
/// function whose code runs (a tail call changes it), and the depth below
/// the nearest frame of a function that must be followed (one that
/// reaches a write), so the callees of those get the full inline depth.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Frame {
    parent: Option<u32>,
    call: u64,
    ret: u64,
    func: u64,
    depth: u32,
}

/// What one root's interpretation found.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Check {
    /// Every `MSR FPCR` the interpretation reached, and whether each visit
    /// wrote the default mode.
    pub sites: BTreeMap<u64, bool>,
    /// Why it stopped without a result.
    pub gave_up: Option<String>,
    /// The inlined callee where it stopped, when that callee may be
    /// treated as a call the analysis does not follow instead.
    pub blame: Option<u64>,
    /// The blocks interpreted.
    pub steps: usize,
}

impl Check {
    /// Whether every reached write is the default and nothing stopped it.
    pub fn proven(&self) -> bool {
        self.gave_up.is_none() && self.sites.values().all(|&ok| ok)
    }
}

pub(super) struct Interp<'a> {
    code: &'a Code<'a>,
    must: &'a dyn Fn(u64) -> bool,
    skip: &'a BTreeSet<u64>,
    depth: u32,
    budget: usize,
    frames: Vec<Frame>,
    frame_of: HashMap<(Option<u32>, u64, u64), u32>,
    codes: Vec<Vec<u64>>,
    code_of: HashMap<Vec<u64>, u32>,
    states: HashMap<(u32, u64), St>,
    todo: BTreeSet<(u32, u64)>,
    out: Check,
}

fn ptr_bytes(w: Width) -> Option<u32> {
    w.bytes()
}

impl<'a> Interp<'a> {
    pub(super) fn new(
        code: &'a Code<'a>,
        must: &'a dyn Fn(u64) -> bool,
        skip: &'a BTreeSet<u64>,
        depth: u32,
        budget: usize,
    ) -> Interp<'a> {
        Interp {
            code,
            must,
            skip,
            depth,
            budget,
            frames: Vec::new(),
            frame_of: HashMap::new(),
            codes: Vec::new(),
            code_of: HashMap::new(),
            states: HashMap::new(),
            todo: BTreeSet::new(),
            out: Check::default(),
        }
    }

    fn frame(&mut self, f: Frame) -> u32 {
        let key = (f.parent, f.call, f.func);
        if let Some(&i) = self.frame_of.get(&key) {
            return i;
        }
        let i = self.frames.len() as u32;
        self.frames.push(f);
        self.frame_of.insert(key, i);
        i
    }

    fn code_value(&mut self, ts: Vec<u64>) -> V {
        if let Some(&i) = self.code_of.get(&ts) {
            return V::Code(i);
        }
        let i = self.codes.len() as u32;
        self.codes.push(ts.clone());
        self.code_of.insert(ts, i);
        V::Code(i)
    }

    fn flow(&mut self, at: (u32, u64), st: St) {
        let merged = match self.states.get(&at) {
            Some(old) => {
                let j = old.join(&st);
                if &j == old {
                    return;
                }
                j
            }
            None => st,
        };
        self.states.insert(at, merged);
        self.todo.insert(at);
    }

    fn give_up(&mut self, why: String) {
        if self.out.gave_up.is_none() {
            self.out.gave_up = Some(why);
        }
    }

    fn give_up_in(&mut self, fr: u32, why: String) {
        if self.out.gave_up.is_none() {
            let f = self.frames[fr as usize];
            if f.depth > 0 && !(self.must)(f.func) {
                self.out.blame = Some(f.func);
            }
        }
        self.give_up(why);
    }

    pub(super) fn run(mut self, root: u64) -> Check {
        let f = self.frame(Frame {
            parent: None,
            call: 0,
            ret: 0,
            func: root,
            depth: 0,
        });
        let mut regs = [TOP; TRACKED];
        regs[regs::SP.index()] = V::Ptr(Base::Root, 0);
        self.flow(
            (f, root),
            St {
                regs,
                mem: Mem::default(),
            },
        );
        while let Some(at) = self.todo.pop_first() {
            if self.out.gave_up.is_some() {
                break;
            }
            self.out.steps += 1;
            if self.out.steps > self.budget {
                self.give_up(format!("more than {} steps", self.budget));
                break;
            }
            let st = self.states[&at].clone();
            self.step(at, st);
        }
        self.out
    }

    fn step(&mut self, (fr, pc): (u32, u64), st: St) {
        let Some(b) = (self.code.block)(pc) else {
            return;
        };
        let mut st = st;
        let vals = self.block(&b, fr, &mut st);
        let val = |v: Value| vals[v.index()];
        let targets = |me: &Self, n: Next| -> Option<Vec<u64>> {
            match n {
                Next::Const(t) => Some(vec![t]),
                Next::Select {
                    cond,
                    taken,
                    not_taken,
                } => Some(match val(cond).known(Width::W1) {
                    Some(1) => vec![taken],
                    Some(_) => vec![not_taken],
                    None => vec![taken, not_taken],
                }),
                Next::Dynamic(v) => match val(v) {
                    V::Code(i) => Some(me.codes[i as usize].clone()),
                    x => x.known(Width::W64).map(|t| vec![t as u64]),
                },
            }
        };
        match b.exit.flow {
            Flow::Jump => {
                let ts = match targets(self, b.exit.next) {
                    Some(ts) => ts,
                    None => match (self.code.table)(pc) {
                        Some(ts) => ts,
                        None => {
                            self.give_up_in(fr, format!("computed jump at {pc:#x}"));
                            return;
                        }
                    },
                };
                for t in ts {
                    if self.skip.contains(&t) && t != self.frames[fr as usize].func {
                        self.opaque_tail(fr, &st);
                        continue;
                    }
                    let to = self.jump(fr, t);
                    self.flow((to, t), st.clone());
                }
            }
            Flow::Trap(_) => {
                let after = opaque(&st);
                for t in targets(self, b.exit.next).unwrap_or_default() {
                    self.flow((fr, t), after.clone());
                }
            }
            Flow::Return => {
                let f = self.frames[fr as usize];
                let Some(parent) = f.parent else {
                    return;
                };
                if st.regs[30].known(Width::W64) != Some(u128::from(f.ret)) {
                    self.give_up_in(fr, format!("return at {pc:#x} to an unknown address"));
                    return;
                }
                self.flow((parent, f.ret), st);
            }
            Flow::Call { ret } => {
                let ts = targets(self, b.exit.next);
                self.call(fr, pc, ret, ts, &st);
            }
        }
    }

    /// A tail call from frame `fr` to a function the analysis does not
    /// follow: it returns to the frame's caller.
    fn opaque_tail(&mut self, fr: u32, st: &St) {
        let after = opaque(st);
        let f = self.frames[fr as usize];
        if let Some(parent) = f.parent {
            self.throw(parent, f.call, &after);
            self.flow((parent, f.ret), after);
        }
    }

    /// The frame that runs code at `t` after a jump from frame `fr`: a jump
    /// to a function entry is a tail call, which keeps the frame's return.
    fn jump(&mut self, fr: u32, t: u64) -> u32 {
        let f = self.frames[fr as usize];
        if f.func == t || !(self.code.is_entry)(t) {
            return fr;
        }
        self.frame(Frame { func: t, ..f })
    }

    fn call(&mut self, fr: u32, pc: u64, ret: u64, ts: Option<Vec<u64>>, st: &St) {
        let f = self.frames[fr as usize];
        let mut opaque_call = ts.is_none();
        for t in ts.unwrap_or_default() {
            match (self.code.host)(t) {
                Host::Unknown => {
                    self.give_up(format!("call at {pc:#x} to code it cannot follow"));
                    return;
                }
                Host::Override => {
                    opaque_call = true;
                    continue;
                }
                Host::Guest => {}
            }
            let mut chain = Some(fr);
            let mut recursive = false;
            while let Some(c) = chain {
                let cf = self.frames[c as usize];
                if cf.func == t {
                    recursive = true;
                }
                chain = cf.parent;
            }
            let must = (self.must)(t);
            if recursive || (!must && (f.depth >= self.depth || self.skip.contains(&t))) {
                if must {
                    self.give_up(format!("call at {pc:#x} to {t:#x} cannot be followed"));
                    return;
                }
                opaque_call = true;
                continue;
            }
            let callee = self.frame(Frame {
                parent: Some(fr),
                call: pc,
                ret,
                func: t,
                depth: if must { 0 } else { f.depth + 1 },
            });
            self.flow((callee, t), st.clone());
        }
        if opaque_call {
            let after = opaque(st);
            self.throw(fr, pc, &after);
            self.flow((fr, ret), after);
        }
    }

    /// A call at `pc` in frame `fr` that the analysis does not follow may
    /// throw: the landing pad serving the call, and those serving each call
    /// of the frames above it, may run, with the memory as the call left it
    /// and each frame's callee-saved registers as they were at its call.
    fn throw(&mut self, fr: u32, pc: u64, after: &St) {
        let mut seed = after.clone();
        let mut at = Some((fr, pc));
        while let Some((c, site)) = at {
            let f = self.frames[c as usize];
            for p in (self.code.pads)(site) {
                self.flow((c, p), seed.clone());
            }
            let Some(parent) = f.parent else {
                break;
            };
            let Some(call_state) = self.states.get(&(parent, f.call)) else {
                break;
            };
            let mut regs = opaque(call_state).regs;
            regs[regs::SP.index()] = call_state.regs[regs::SP.index()];
            seed = St {
                regs,
                mem: seed.mem.clone(),
            };
            at = Some((parent, f.call));
        }
    }

    fn block(&mut self, b: &Arc<Block>, fr: u32, st: &mut St) -> Vec<V> {
        let mut vals: Vec<V> = Vec::with_capacity(b.insts.len());
        let mut sets: Vec<(usize, V, Width)> = Vec::new();
        let w_of = |v: Value| b.insts[v.index()].width.unwrap_or(Width::W1);
        for inst in &b.insts {
            let w = inst.width.unwrap_or(Width::W1);
            let g = |v: Value| vals[v.index()];
            let r = match &inst.op {
                Op::Const(k) => V::konst(k.get(), w),
                Op::Get(r) => get(st, r.index(), w),
                Op::Set(r, v) => {
                    sets.push((r.index(), g(*v), w_of(*v)));
                    TOP
                }
                Op::Un(op, x) => un(*op, g(*x), w),
                Op::Bin(op, x, y) => bin(*op, g(*x), g(*y), w),
                Op::Cmp(op, x, y) => cmp(*op, g(*x), g(*y), w_of(*x)),
                Op::Select(c, x, y) => match g(*c).known(Width::W1) {
                    Some(1) => g(*x),
                    Some(_) => g(*y),
                    None => g(*x).join(g(*y)),
                },
                Op::Ext(k, x) => ext(*k, g(*x), w_of(*x), w),
                Op::Extract(x, lo) => extract(g(*x), *lo, w_of(*x), w),
                Op::Concat(h, l) => concat(g(*h), g(*l), w_of(*l), w),
                Op::Load { addr, .. } | Op::LoadExclusive { addr, .. } => {
                    self.load(st, g(*addr), w)
                }
                Op::Store { addr, value, .. } => {
                    store(st, g(*addr), g(*value), w_of(*value));
                    TOP
                }
                Op::StoreExclusive { addr, value, .. }
                | Op::AtomicRmw { addr, value, .. }
                | Op::CompareSwap { addr, value, .. } => {
                    let vw = w_of(*value);
                    escape_value(st, g(*value));
                    if let V::Ptr(pb, po) = g(*addr) {
                        st.mem.forget(pb, po, i64::from(vw.bytes().unwrap_or(16)));
                    }
                    TOP
                }
                _ => TOP,
            };
            if !matches!(r, V::Ptr(..)) && w.bits() >= 64 && consumes_pointers(&inst.op) {
                for x in inst.op.operands() {
                    escape_value(st, vals[x.index()]);
                }
            }
            vals.push(r);
        }
        for (r, v, w) in sets {
            self.set(st, r, v, w, b.pc, fr);
        }
        vals
    }

    fn set(&mut self, st: &mut St, r: usize, v: V, w: Width, pc: u64, fr: u32) {
        if r == regs::FPCR.index() {
            let (zero, _) = v.bits(w);
            let ok = zero & GUEST == GUEST;

            let e = self.out.sites.entry(pc).or_insert(true);
            *e &= ok;
            return;
        }
        if r >= TRACKED {
            return;
        }
        let v = if r == regs::SP.index() {
            match v {
                V::Ptr(..) => v,
                _ => {
                    let b = Base::Fresh(pc, fr);
                    st.mem.reset(b);
                    for x in &mut st.regs {
                        if matches!(x, V::Ptr(pb, _) if *pb == b) {
                            *x = TOP;
                        }
                    }
                    V::Ptr(b, 0)
                }
            }
        } else {
            if let V::Ptr(pb, po) = v {
                st.mem.take(pb, po);
            }
            v
        };
        st.regs[r] = v;
    }

    fn load(&mut self, st: &St, a: V, w: Width) -> V {
        let Some(n) = ptr_bytes(w) else {
            return TOP;
        };
        match a {
            V::Ptr(b, off) => st.mem.load(b, off, n),
            _ => match a.known(Width::W64) {
                Some(addr) if n == 8 => match (self.code.slot)(addr as u64) {
                    Some(ts) => self.code_value(ts),
                    None => TOP,
                },
                _ => TOP,
            },
        }
    }
}

fn get(st: &St, r: usize, w: Width) -> V {
    if r == regs::FPCR.index() {
        // The proof's hypothesis: the guest bits are the default (zero).
        return V::Bits {
            zero: w.mask() & !EXACT,
            one: 0,
        };
    }
    if r < TRACKED { st.regs[r] } else { TOP }
}

/// Whether an operation that gives no pointer from a pointer operand
/// loses track of what the pointer may reach (when its result is wide
/// enough to hold an address): every operation but the accesses, which use
/// it only as their address, compares, and writes of registers (whose
/// pointers [`Interp::set`] handles).
fn consumes_pointers(op: &Op) -> bool {
    !matches!(
        op,
        Op::Load { .. } | Op::LoadExclusive { .. } | Op::Cmp(..) | Op::Set(..) | Op::Store { .. }
    )
}

fn escape_value(st: &mut St, v: V) {
    if let V::Ptr(b, off) = v {
        st.mem.escape(b, off);
    }
}

/// A store: a pointer stored to tracked memory stays there (it escapes
/// when that memory does, [`Mem::escape_stored`]); one stored anywhere else
/// escapes.
fn store(st: &mut St, a: V, v: V, vw: Width) {
    let n = vw.bytes();
    match (a, n) {
        (V::Ptr(b, off), Some(n)) => {
            if n != 8 {
                escape_value(st, v);
            }
            st.mem.store(b, off, n, v);
        }
        _ => escape_value(st, v),
    }
}

/// The state after a call the analysis does not follow (or a system call):
/// the pointers in registers but the frame pointer escape (a callee reads
/// its callers' frame records only to unwind), and so do the pointers
/// stored in memory that escaped; the caller-saved registers and the flags
/// are unknown, and the callee-saved ones and sp are kept (AAPCS64).
fn opaque(st: &St) -> St {
    let mut out = st.clone();
    for r in 0..31 {
        if let V::Ptr(b, off) = st.regs[r]
            && r != 29
        {
            out.mem.escape(b, off);
        }
        if r <= 18 || r == 30 {
            out.regs[r] = TOP;
        }
    }
    for r in regs::N.index()..TRACKED {
        out.regs[r] = TOP;
    }
    out.mem.escape_stored();
    out
}

fn un(op: UnOp, x: V, w: Width) -> V {
    let m = w.mask();
    match op {
        UnOp::Not => {
            let (z, o) = x.bits(w);
            V::Bits { zero: o, one: z }
        }
        UnOp::Neg => match x.known(w) {
            Some(c) => V::konst(c.wrapping_neg() & m, w),
            None => TOP,
        },
        _ => TOP,
    }
}

/// The number of low bits known in both.
fn low_known(a: (u128, u128), b: (u128, u128), w: Width) -> u32 {
    let k = (a.0 | a.1) & (b.0 | b.1) & w.mask();
    k.trailing_ones().min(w.bits())
}

fn low(c: u128, n: u32, w: Width) -> V {
    if n == 0 {
        return TOP;
    }
    let m = if n >= 128 {
        u128::MAX
    } else {
        (1u128 << n) - 1
    } & w.mask();
    V::Bits {
        zero: !c & m,
        one: c & m,
    }
}

fn bin(op: BinOp, x: V, y: V, w: Width) -> V {
    let m = w.mask();
    match (op, x, y) {
        (BinOp::Add, V::Ptr(b, off), c) | (BinOp::Add, c, V::Ptr(b, off)) if w.bits() >= 64 => {
            match c.known(w) {
                Some(k) => V::Ptr(b, off.wrapping_add(k as u64 as i64)),
                None => TOP,
            }
        }
        (BinOp::Sub, V::Ptr(b, off), c) if w.bits() >= 64 => match c {
            V::Ptr(b2, off2) if b2 == b => {
                V::konst(off.wrapping_sub(off2) as i128 as u128 & w.mask(), w)
            }
            _ => match c.known(w) {
                Some(k) => V::Ptr(b, off.wrapping_sub(k as u64 as i64)),
                None => TOP,
            },
        },
        (_, V::Bits { .. }, V::Bits { .. }) => {
            let (za, oa) = x.bits(w);
            let (zb, ob) = y.bits(w);
            match op {
                BinOp::And => V::Bits {
                    zero: (za | zb) & m,
                    one: oa & ob,
                },
                BinOp::Or => V::Bits {
                    zero: za & zb,
                    one: (oa | ob) & m,
                },
                BinOp::Xor => {
                    let k = (za | oa) & (zb | ob);
                    let v = oa ^ ob;
                    V::Bits {
                        zero: k & !v & m,
                        one: k & v,
                    }
                }
                BinOp::Add => low(oa.wrapping_add(ob), low_known((za, oa), (zb, ob), w), w),
                BinOp::Sub => low(oa.wrapping_sub(ob), low_known((za, oa), (zb, ob), w), w),
                BinOp::Shl | BinOp::LShr => match y.known(w) {
                    Some(n) if n >= u128::from(w.bits()) => V::konst(0, w),
                    Some(n) => {
                        let n = n as u32;
                        let fill = if n == 0 { 0 } else { (1u128 << n) - 1 };
                        if op == BinOp::Shl {
                            V::Bits {
                                zero: ((za << n) | fill) & m,
                                one: (oa << n) & m,
                            }
                        } else {
                            V::Bits {
                                zero: (za >> n) | (m & !(m >> n)),
                                one: oa >> n,
                            }
                        }
                    }
                    None => TOP,
                },
                _ => match (x.known(w), y.known(w)) {
                    (Some(a), Some(b)) => fold(op, a, b, w),
                    _ => TOP,
                },
            }
        }
        _ => TOP,
    }
}

fn fold(op: BinOp, a: u128, b: u128, w: Width) -> V {
    let m = w.mask();
    let bits = w.bits();
    let sx = |v: u128| -> i128 {
        if bits >= 128 {
            v as i128
        } else {
            ((v << (128 - bits)) as i128) >> (128 - bits)
        }
    };
    let r = match op {
        BinOp::Mul => a.wrapping_mul(b),
        BinOp::UDiv => a.checked_div(b).unwrap_or(0),
        BinOp::URem => a.checked_rem(b).unwrap_or(a),
        BinOp::AShr => {
            let n = b.min(u128::from(bits - 1)) as u32;
            (sx(a) >> n) as u128
        }
        BinOp::RotR => {
            let n = (b % u128::from(bits)) as u32;
            if n == 0 {
                a
            } else {
                (a >> n) | (a << (bits - n))
            }
        }
        _ => return TOP,
    };
    V::konst(r & m, w)
}

fn cmp(op: CmpOp, x: V, y: V, w: Width) -> V {
    let t = |b: bool| V::konst(u128::from(b), Width::W1);
    if let (V::Ptr(b1, o1), V::Ptr(b2, o2)) = (x, y)
        && b1 == b2
    {
        return match op {
            CmpOp::Eq => t(o1 == o2),
            CmpOp::Ne => t(o1 != o2),
            _ => TOP,
        };
    }
    match (x.known(w), y.known(w)) {
        (Some(a), Some(b)) => {
            let bits = w.bits();
            let sx = |v: u128| -> i128 {
                if bits >= 128 {
                    v as i128
                } else {
                    ((v << (128 - bits)) as i128) >> (128 - bits)
                }
            };
            t(match op {
                CmpOp::Eq => a == b,
                CmpOp::Ne => a != b,
                CmpOp::Ult => a < b,
                CmpOp::Ule => a <= b,
                CmpOp::Slt => sx(a) < sx(b),
                CmpOp::Sle => sx(a) <= sx(b),
            })
        }
        _ => {
            let (za, oa) = x.bits(w);
            let (zb, ob) = y.bits(w);
            let differ = (oa & zb) | (za & ob) != 0;
            match op {
                CmpOp::Eq if differ => t(false),
                CmpOp::Ne if differ => t(true),
                _ => TOP,
            }
        }
    }
}

fn ext(k: ExtKind, x: V, from: Width, to: Width) -> V {
    if from == to || (matches!(x, V::Ptr(..)) && from.bits() == 64) {
        return x;
    }
    let (z, o) = x.bits(from);
    let hi = to.mask() & !from.mask();
    let sign = 1u128 << (from.bits() - 1);
    match k {
        ExtKind::Zero => V::Bits {
            zero: z | hi,
            one: o,
        },
        ExtKind::Sign if o & sign != 0 => V::Bits {
            zero: z,
            one: o | hi,
        },
        ExtKind::Sign if z & sign != 0 => V::Bits {
            zero: z | hi,
            one: o,
        },
        ExtKind::Sign => V::Bits { zero: z, one: o },
    }
}

fn extract(x: V, lo: u8, from: Width, w: Width) -> V {
    if lo == 0 && (from == w || (matches!(x, V::Ptr(..)) && w.bits() == 64)) {
        return x;
    }
    let (z, o) = x.bits(from);
    let m = w.mask();
    V::Bits {
        zero: (z >> lo) & m,
        one: (o >> lo) & m,
    }
}

fn concat(h: V, l: V, lw: Width, w: Width) -> V {
    let hw = Width::new(w.bits() - lw.bits());
    let (zh, oh) = h.bits(hw);
    let (zl, ol) = l.bits(lw);
    V::Bits {
        zero: (zh << lw.bits()) | zl,
        one: (oh << lw.bits()) | ol,
    }
}

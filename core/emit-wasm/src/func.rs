//! One guest function as one WebAssembly function.
//!
//! A function with more than one entry point (its entry, the targets of
//! branches inside it and the return points of its calls) is a `loop` around
//! nested blocks and a `br_table` over those labels, one case per label;
//! code between labels falls through, as the C emitter's `goto` labels do.

use std::collections::{BTreeMap, BTreeSet};

use relift_ir::{Block, Flow, Next, Op, Reg, RegFile, Value, Width};
use wasm_encoder::{BlockType, Function as WasmFn, Instruction as I, MemArg, ValType};

use crate::{EmitError, Exits, Function, Insn, ModuleCx, RC_CHAIN, RC_OK, T_LIFTED, ctx, rt};

/// A value's locals: one `i64` up to 64 bits, a low and high `i64` above.
#[derive(Clone, Copy, Debug)]
pub(crate) enum V {
    S(u32),
    P(u32, u32),
}

impl V {
    pub(crate) fn lo(self) -> u32 {
        match self {
            V::S(l) | V::P(l, _) => l,
        }
    }
}

pub(crate) fn nat(w: Width) -> bool {
    w.bits() <= 64
}

pub(crate) fn mask64(bits: u32) -> Option<i64> {
    if bits >= 64 {
        None
    } else {
        Some(((1u64 << bits) - 1) as i64)
    }
}

pub(crate) const fn ma(offset: u64) -> MemArg {
    MemArg {
        offset,
        align: 0,
        target: wasm_encoder::MemArgTarget::Memory(0),
        ordering: None,
    }
}

pub(crate) const L_LABEL: u32 = 2;
pub(crate) const L_RC: u32 = 3;
pub(crate) const L_T: u32 = 4;
pub(crate) const N_T: u32 = 12;
pub(crate) const L_BUDGET: u32 = L_T + N_T;
const L_FIRST_FREE: u32 = L_BUDGET + 1;

/// The emitter state of one function.
pub(crate) struct Em<'a> {
    pub(crate) out: Vec<I<'static>>,
    pub(crate) cx: &'a ModuleCx<'a>,
    pub(crate) reg_locals: BTreeMap<Reg, V>,
    val_base: u32,
    pub(crate) regs: RegFile,
    written: BTreeSet<Reg>,
    used: BTreeSet<Reg>,
    /// Extra block depth from `if`s inside the current case.
    pub(crate) depth: u32,
    case: u32,
    ncases: u32,
    labels: BTreeMap<u64, u32>,
    next_in_order: Option<u64>,
    entry: u64,
    clock: bool,
    pub(crate) dev_r: Vec<(u64, u64)>,
    pub(crate) dev_w: Vec<(u64, u64)>,
    pub(crate) addr_mask: u64,
    pub(crate) clash: &'a BTreeSet<&'static str>,
    tables: &'a BTreeMap<u64, Vec<u64>>,
}

/// What one function emitted.
pub(crate) struct Emitted {
    pub(crate) func: WasmFn,
    pub(crate) undefined: usize,
    pub(crate) errors: Vec<EmitError>,
}

/// The byte intervals `(first, last)` of addresses below `2^bits` plus the
/// bytes an access may run past the top, which an access must not touch to
/// stay memory, for one direction.
pub(crate) fn device_intervals(map: &relift_ir::IoMap, bits: u32, write: bool) -> Vec<(u64, u64)> {
    use relift_ir::AddrClass;
    let top: u128 = if bits >= 64 {
        1u128 << 64
    } else {
        (1u128 << bits) + 16
    };
    let mut cuts: Vec<u128> = vec![0, top];
    for r in &map.ranges {
        cuts.push(u128::from(r.first).min(top));
        cuts.push((u128::from(r.last) + 1).min(top));
    }
    cuts.sort_unstable();
    cuts.dedup();
    let mut out: Vec<(u64, u64)> = Vec::new();
    for w in cuts.windows(2) {
        let (a, b) = (w[0], w[1]);
        if a >= b {
            continue;
        }
        if map.class_of(a as u64, write) == AddrClass::Device {
            match out.last_mut() {
                Some(l) if u128::from(l.1) + 1 == a => l.1 = (b - 1) as u64,
                _ => out.push((a as u64, (b - 1) as u64)),
            }
        }
    }
    out
}

impl Em<'_> {
    pub(crate) fn i(&mut self, x: I<'static>) {
        self.out.push(x);
    }
    pub(crate) fn c64(&mut self, v: i64) {
        self.i(I::I64Const(v));
    }
    pub(crate) fn c32(&mut self, v: i32) {
        self.i(I::I32Const(v));
    }
    pub(crate) fn get(&mut self, l: u32) {
        self.i(I::LocalGet(l));
    }
    pub(crate) fn set(&mut self, l: u32) {
        self.i(I::LocalSet(l));
    }
    pub(crate) fn tee(&mut self, l: u32) {
        self.i(I::LocalTee(l));
    }
    pub(crate) fn t(n: u32) -> u32 {
        assert!(n < N_T, "temp {n} out of range");
        L_T + n
    }

    fn val(&self, v: Value) -> V {
        let base = self.val_base + 2 * v.0;
        V::P(base, base + 1)
    }

    /// The locals of value `v` with width `w`.
    pub(crate) fn vloc(&self, v: Value, w: Width) -> V {
        match self.val(v) {
            V::P(l, _) if nat(w) => V::S(l),
            p => p,
        }
    }

    pub(crate) fn push_lo(&mut self, x: V) {
        self.get(x.lo());
    }

    pub(crate) fn push_hi(&mut self, x: V) {
        match x {
            V::S(_) => self.c64(0),
            V::P(_, h) => self.get(h),
        }
    }

    pub(crate) fn and_mask(&mut self, bits: u32) {
        if let Some(m) = mask64(bits) {
            self.c64(m);
            self.i(I::I64And);
        }
    }

    /// Sign-extends the `i64` on the stack from `bits` to 64.
    pub(crate) fn sx64(&mut self, bits: u32) {
        if bits < 64 {
            let s = 64 - i64::from(bits);
            self.c64(s);
            self.i(I::I64Shl);
            self.c64(s);
            self.i(I::I64ShrS);
        }
    }

    fn br_loop(&mut self) {
        let d = self.ncases - 1 - self.case + self.depth;
        self.i(I::Br(d));
    }

    pub(crate) fn store_budget(&mut self) {
        if self.clock {
            self.get(0);
            self.get(L_BUDGET);
            self.i(I::I64Store(ma(u64::from(ctx::BUDGET))));
        }
    }

    pub(crate) fn load_budget(&mut self) {
        if self.clock {
            self.get(0);
            self.i(I::I64Load(ma(u64::from(ctx::BUDGET))));
            self.set(L_BUDGET);
        }
    }

    pub(crate) fn store_regs(&mut self) {
        self.store_budget();
        for r in self.written.clone() {
            let v = self.reg_locals[&r];
            let off = u64::from(ctx::slot(r));
            self.get(0);
            self.push_lo(v);
            self.i(I::I64Store(ma(off)));
            if let V::P(_, h) = v {
                self.get(0);
                self.get(h);
                self.i(I::I64Store(ma(off + 8)));
            }
        }
    }

    pub(crate) fn load_regs(&mut self) {
        self.load_budget();
        for r in self.used.clone() {
            let v = self.reg_locals[&r];
            let off = u64::from(ctx::slot(r));
            self.get(0);
            self.i(I::I64Load(ma(off)));
            self.set(v.lo());
            if let V::P(_, h) = v {
                self.get(0);
                self.i(I::I64Load(ma(off + 8)));
                self.set(h);
            }
        }
    }

    fn store_pc_const(&mut self, t: u64) {
        self.get(0);
        self.c64(t as i64);
        self.i(I::I64Store(ma(u64::from(ctx::PC))));
    }

    fn store_pc_local(&mut self, l: u32) {
        self.get(0);
        self.get(l);
        self.i(I::I64Store(ma(u64::from(ctx::PC))));
    }

    /// Returns `rc` (in [`L_RC`]) when it is not [`RC_OK`].
    fn check_rc(&mut self) {
        self.get(L_RC);
        self.i(I::If(BlockType::Empty));
        self.get(L_RC);
        self.i(I::Return);
        self.i(I::End);
    }

    /// Pushes `c, mem` and the table slot of the guest address in `l`.
    fn lookup(&mut self, l: u32) {
        self.get(0);
        self.get(1);
        self.get(0);
        self.get(l);
        self.i(I::Call(rt::LOOKUP));
    }

    /// Leaves for the guest address in local `l`, which the registers are
    /// already stored for.
    fn leave_indirect(&mut self, l: u32) {
        self.store_pc_local(l);
        if self.cx.opts.tail_calls {
            self.lookup(l);
            self.i(I::ReturnCallIndirect {
                type_index: T_LIFTED,
                table_index: 0,
            });
        } else {
            self.c32(RC_CHAIN);
            self.i(I::Return);
        }
    }

    /// A transfer to the constant `t` that leaves no frame.
    fn jump(&mut self, t: u64) {
        if self.cx.opts.exits == Exits::Step {
            self.store_regs();
            self.store_pc_const(t & self.addr_mask);
            self.c32(RC_OK);
            self.i(I::Return);
            return;
        }
        if let Some(&k) = self.labels.get(&t) {
            if self.depth == 0 && self.next_in_order == Some(t) {
                return;
            }
            self.c32(k as i32);
            self.set(L_LABEL);
            self.br_loop();
            return;
        }
        if self.depth == 0 && self.next_in_order == Some(t) {
            return;
        }
        self.store_regs();
        match self.cx.targets.get(&t) {
            Some(&f) if self.cx.opts.tail_calls => {
                self.get(0);
                self.get(1);
                self.i(I::ReturnCall(f));
            }
            _ => {
                self.c64(t as i64);
                self.set(Self::t(0));
                self.leave_indirect(Self::t(0));
            }
        }
    }

    /// Calls the function at the guest address in local `l` (or the known
    /// `t`), then checks the return code and reloads the registers.
    fn call(&mut self, known: Option<u32>, l: Option<u32>) {
        match (known, l) {
            (Some(f), _) => {
                self.get(0);
                self.get(1);
                self.i(I::Call(f));
            }
            (None, Some(l)) => {
                self.store_pc_local(l);
                self.lookup(l);
                self.i(I::CallIndirect {
                    type_index: T_LIFTED,
                    table_index: 0,
                });
            }
            (None, None) => self.c32(RC_OK),
        }
        if !self.cx.opts.tail_calls {
            self.set(L_RC);
            self.get(0);
            self.get(1);
            self.get(L_RC);
            self.i(I::Call(self.cx.helper(crate::Helper::Run)));
        }
        self.set(L_RC);
        self.check_rc();
        self.load_regs();
    }

    /// Stores the registers and reports the instruction `insn` at `pc` as
    /// undefined, returning the runtime's code.
    fn undefined(&mut self, pc: u64, insn: u64) {
        self.store_regs();
        self.get(0);
        self.get(1);
        self.c64(pc as i64);
        self.c64(insn as i64);
        self.i(I::Call(rt::UNDEFINED));
        self.i(I::Return);
    }

    fn block(&mut self, b: &Block, raw: u64) -> Result<(), String> {
        if b.regs != self.regs {
            return Err("a block over another register file".into());
        }
        let mut sets: Vec<(Reg, Value)> = Vec::new();
        let wid = |v: Value| b.insts[v.index()].width.unwrap_or(Width::W1);
        for (i, inst) in b.insts.iter().enumerate() {
            let w = inst.width.unwrap_or(Width::W1);
            let dst = self.vloc(Value(i as u32), w);
            match &inst.op {
                Op::Set(r, v) => {
                    sets.push((*r, *v));
                    continue;
                }
                Op::Guard(c, code) => {
                    let c = self.vloc(*c, Width::W1);
                    self.push_lo(c);
                    self.i(I::I32WrapI64);
                    self.i(I::If(BlockType::Empty));
                    self.depth += 1;
                    self.store_regs();
                    self.get(0);
                    self.get(1);
                    self.c32(*code as i32);
                    self.c64(b.pc as i64);
                    self.c64(raw as i64);
                    self.i(I::Call(rt::SLOW));
                    self.set(L_RC);
                    self.check_rc();
                    self.load_regs();
                    self.jump(b.pc + u64::from(b.len));
                    self.depth -= 1;
                    self.i(I::End);
                    continue;
                }
                _ => {}
            }
            self.op(b, &inst.op, inst.width, dst, &wid)?;
        }
        for (r, v) in sets {
            let rw = self.regs.width(r);
            let src = self.vloc(v, rw);
            let dst = self.reg_locals[&r];
            self.push_lo(src);
            self.set(dst.lo());
            if let V::P(_, h) = dst {
                self.push_hi(src);
                self.set(h);
            }
        }
        self.exit(b)
    }

    /// Pushes the exit's target as an `i64`.
    fn push_next(&mut self, b: &Block) {
        let wid = |v: Value| b.insts[v.index()].width.unwrap_or(Width::W1);
        match b.exit.next {
            Next::Const(t) => self.c64((t & self.addr_mask) as i64),
            Next::Select {
                cond,
                taken,
                not_taken,
            } => {
                self.c64((taken & self.addr_mask) as i64);
                self.c64((not_taken & self.addr_mask) as i64);
                let c = self.vloc(cond, Width::W1);
                self.push_lo(c);
                self.i(I::I32WrapI64);
                self.i(I::Select);
            }
            Next::Dynamic(v) => {
                let x = self.vloc(v, wid(v));
                self.push_lo(x);
            }
        }
    }

    fn exit(&mut self, b: &Block) -> Result<(), String> {
        let wid = |v: Value| b.insts[v.index()].width.unwrap_or(Width::W1);
        if let Next::Dynamic(v) = b.exit.next
            && !nat(wid(v))
        {
            return Err("a target wider than 64 bits".into());
        }
        let sync = matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync));
        if self.cx.opts.exits == Exits::Step {
            self.store_regs();
            self.get(0);
            self.push_next(b);
            self.i(I::I64Store(ma(u64::from(ctx::PC))));
            if sync {
                self.get(L_BUDGET);
                self.c64(0);
                self.i(I::I64LeS);
                self.i(I::If(BlockType::Empty));
                self.get(0);
                self.get(1);
                self.get(0);
                self.i(I::I64Load(ma(u64::from(ctx::PC))));
                self.i(I::Call(rt::SYNC));
                self.i(I::Return);
                self.i(I::End);
            }
            self.c32(RC_OK);
            self.i(I::Return);
            return Ok(());
        }
        match (b.exit.flow, b.exit.next) {
            (Flow::Call { .. }, Next::Select { .. }) => return Err("a conditional call".into()),
            (Flow::Trap(_), Next::Select { .. } | Next::Dynamic(_)) => {
                return Err("a trap with a computed resume".into());
            }
            _ => {}
        }
        if sync {
            self.push_next(b);
            self.set(Self::t(1));
            self.get(L_BUDGET);
            self.c64(0);
            self.i(I::I64LeS);
            self.i(I::If(BlockType::Empty));
            self.depth += 1;
            self.store_regs();
            self.get(0);
            self.get(1);
            self.get(Self::t(1));
            self.i(I::Call(rt::SYNC));
            self.set(L_RC);
            self.check_rc();
            self.load_regs();
            self.depth -= 1;
            self.i(I::End);
        }
        match b.exit.flow {
            Flow::Return => {
                self.store_regs();
                self.c32(RC_OK);
                self.i(I::Return);
            }
            Flow::Call { ret } => {
                self.store_regs();
                match b.exit.next {
                    Next::Const(t) => match self.cx.targets.get(&t) {
                        Some(&f) => self.call(Some(f), None),
                        None => {
                            self.c64(t as i64);
                            self.set(Self::t(0));
                            self.call(None, Some(Self::t(0)));
                        }
                    },
                    Next::Dynamic(v) => {
                        let x = self.vloc(v, wid(v));
                        self.call(None, Some(x.lo()));
                    }
                    Next::Select { .. } => {}
                }
                self.jump(ret);
            }
            Flow::Trap(code) => {
                self.store_regs();
                self.get(0);
                self.get(1);
                self.c32(code as i32);
                self.c64(b.pc as i64);
                self.i(I::Call(rt::TRAP));
                self.set(L_RC);
                self.check_rc();
                self.load_regs();
                if let Next::Const(t) = b.exit.next {
                    self.jump(t);
                }
            }
            Flow::Jump => match b.exit.next {
                Next::Const(t) => self.jump(t),
                Next::Select {
                    cond,
                    taken,
                    not_taken,
                } => {
                    let c = self.vloc(cond, Width::W1);
                    self.push_lo(c);
                    self.i(I::I32WrapI64);
                    self.i(I::If(BlockType::Empty));
                    self.depth += 1;
                    self.jump(taken);
                    self.depth -= 1;
                    self.i(I::End);
                    self.jump(not_taken);
                }
                Next::Dynamic(v) => {
                    let x = self.vloc(v, wid(v));
                    let known: BTreeSet<u64> = self
                        .tables
                        .get(&b.pc)
                        .map(|t| {
                            t.iter()
                                .copied()
                                .filter(|t| self.labels.contains_key(t))
                                .collect()
                        })
                        .unwrap_or_default();
                    for t in known {
                        self.push_lo(x);
                        self.c64(t as i64);
                        self.i(I::I64Eq);
                        self.i(I::If(BlockType::Empty));
                        self.depth += 1;
                        self.jump(t);
                        self.depth -= 1;
                        self.i(I::End);
                    }
                    self.store_regs();
                    self.leave_indirect(x.lo());
                }
            },
        }
        Ok(())
    }
}

/// Emits `f`; `clash` names intrinsics imported with two signatures.
pub(crate) fn emit_function<'a>(
    cx: &'a ModuleCx<'a>,
    f: &'a Function<'a>,
    clash: &'a BTreeSet<&'static str>,
) -> Emitted {
    let blocks: Vec<&Block> = f
        .insts
        .values()
        .filter_map(|i| match i {
            Insn::Lifted { block, .. } => Some(*block),
            Insn::Failed(_) => None,
        })
        .collect();
    let clock = blocks.iter().any(|b| {
        b.insts.iter().any(|i| {
            matches!(
                i.op,
                Op::Tick(_) | Op::TickIf(..) | Op::IoLoad { .. } | Op::IoStore { .. } | Op::Sync
            )
        })
    });
    let aw = blocks.first().map_or(64, |b| b.addr_width.bits()).min(64);
    let addr_mask = if aw >= 64 { u64::MAX } else { (1u64 << aw) - 1 };
    let map = &cx.opts.io_map;
    let (dev_r, dev_w) = (
        device_intervals(map, aw, false),
        device_intervals(map, aw, true),
    );
    let mut used = BTreeSet::new();
    let mut written = BTreeSet::new();
    let mut maxlen = 0;
    for b in &blocks {
        maxlen = maxlen.max(b.insts.len() + 1);
        for i in &b.insts {
            match i.op {
                Op::Get(r) => {
                    used.insert(r);
                }
                Op::Set(r, _) => {
                    used.insert(r);
                    written.insert(r);
                }
                _ => {}
            }
        }
    }
    let used: BTreeSet<Reg> = used
        .into_iter()
        .filter(|r: &Reg| r.index() < cx.regs.len())
        .collect();
    let written: BTreeSet<Reg> = written.intersection(&used).copied().collect();
    let inside =
        |t: u64| f.insts.contains_key(&t) && (t == f.entry || !cx.targets.contains_key(&t));
    let mut labels_pc: BTreeSet<u64> = BTreeSet::new();
    labels_pc.insert(f.entry);
    if cx.opts.exits == Exits::Chain {
        for b in &blocks {
            let mut add = |t: u64| {
                if inside(t) {
                    labels_pc.insert(t);
                }
            };
            match (b.exit.flow, b.exit.next) {
                (Flow::Call { ret }, _) => add(ret),
                (Flow::Trap(_), Next::Const(t)) => add(t),
                (_, Next::Const(t)) if t != b.pc + u64::from(b.len) => add(t),
                (_, Next::Const(_)) => {}
                (
                    _,
                    Next::Select {
                        taken, not_taken, ..
                    },
                ) => {
                    add(taken);
                    add(not_taken);
                }
                (_, Next::Dynamic(_)) => {
                    for t in f.tables.get(&b.pc).into_iter().flatten() {
                        add(*t);
                    }
                }
            }
            if b.insts.iter().any(|i| matches!(i.op, Op::Guard(..))) {
                add(b.pc + u64::from(b.len));
            }
        }
    }
    let pcs: Vec<u64> = f.insts.keys().copied().collect();
    if cx.opts.exits == Exits::Chain {
        for (k, pc) in pcs.iter().enumerate() {
            if let Some(Insn::Lifted { block, .. }) = f.insts.get(pc) {
                let nx = pc + u64::from(block.len);
                if inside(nx) && pcs.get(k + 1) != Some(&nx) {
                    labels_pc.insert(nx);
                }
            }
        }
    }
    let first_label = labels_pc.first().copied().unwrap_or(f.entry);
    let labels: BTreeMap<u64, u32> = labels_pc
        .iter()
        .enumerate()
        .map(|(i, pc)| (*pc, i as u32))
        .collect();
    let mut nloc = L_FIRST_FREE;
    let mut reg_locals = BTreeMap::new();
    for &r in &used {
        if cx.regs.width(r).bits() > 64 {
            reg_locals.insert(r, V::P(nloc, nloc + 1));
            nloc += 2;
        } else {
            reg_locals.insert(r, V::S(nloc));
            nloc += 1;
        }
    }
    let val_base = nloc;
    let nvals = 2 * maxlen as u32;
    let mut em = Em {
        out: Vec::new(),
        cx,
        reg_locals,
        val_base,
        regs: cx.regs,
        written,
        used,
        depth: 0,
        case: 0,
        ncases: labels.len() as u32,
        labels: labels.clone(),
        next_in_order: None,
        entry: f.entry,
        clock,
        dev_r,
        dev_w,
        addr_mask,
        clash,
        tables: &f.tables,
    };
    em.load_regs();
    let multi = em.ncases > 1;
    if multi {
        em.c32(labels[&f.entry] as i32);
        em.set(L_LABEL);
    }
    em.i(I::Loop(BlockType::Empty));
    for _ in 0..em.ncases {
        em.i(I::Block(BlockType::Empty));
    }
    if multi {
        em.get(L_LABEL);
        let ls: Vec<u32> = (0..em.ncases).collect();
        em.i(I::BrTable(ls.into(), 0));
    }
    em.i(I::End);
    let mut case = 0;
    let mut undefined = 0;
    let mut errors = Vec::new();
    for (k, (&pc, insn)) in f.insts.iter().enumerate() {
        match labels.get(&pc) {
            Some(&c) if c != case => {
                em.i(I::End);
                case = c;
            }
            Some(_) => {}
            None if pc < first_label => continue,
            None => {}
        }
        em.case = case;
        em.next_in_order = pcs.get(k + 1).copied();
        let mark = em.out.len();
        let result = match insn {
            Insn::Lifted { block, raw } => em.block(block, *raw).map_err(|m| (*raw, Some(m))),
            Insn::Failed(e) => Err((e.insn, None)),
        };
        if let Err((raw, message)) = result {
            em.out.truncate(mark);
            em.depth = 0;
            undefined += 1;
            if let Some(message) = message {
                errors.push(EmitError {
                    entry: em.entry,
                    pc,
                    message,
                });
            }
            em.undefined(pc, raw);
        }
    }
    for _ in case + 1..em.ncases {
        em.i(I::End);
    }
    em.i(I::End);
    em.i(I::Unreachable);
    em.i(I::End);
    let mut locals = vec![(2u32, ValType::I32), (N_T + 1, ValType::I64)];
    locals.push((nloc - L_FIRST_FREE + nvals, ValType::I64));
    let mut func = WasmFn::new(locals);
    for x in &em.out {
        func.instruction(x);
    }
    Emitted {
        func,
        undefined,
        errors,
    }
}

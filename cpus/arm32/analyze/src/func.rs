//! One function run: the code reachable from an entry without entering
//! another function, with the abstract state at every instruction.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::rc::Rc;
use std::sync::Arc;

use relift_cpu_arm32::decode::Encoding;
use relift_cpu_arm32::lift::{self, COND_FAIL, LiftError};
use relift_cpu_arm32::regs;
use relift_ir::{Block, Flow, Next, Op, Value};

use crate::eval::{self, DmaStart, Out};
use crate::image::Image;
use crate::loc::Loc;
use crate::platform::{BiosCall, Dma, Kind};
use crate::report::{CopyKind, Edge, EdgeKind, RamCopy, Swi, Table, Unresolved, UnresolvedKind};
use crate::val::{Abs, Base, LR, Psr, SP, State, Sym};

/// Registers a call to code the analysis cannot see is taken to keep:
/// `r4`-`r11` and the stack pointer (the procedure call standard).
pub(crate) const AAPCS_KEEP: u16 = 0x2FF0;

/// An effect of a callee that depends on its arguments, applied at each
/// call site with the caller's values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PEffect {
    /// A SWI with `r0`-`r2` in terms of the entry registers.
    Swi(u8, [Abs; 3]),
    /// A word store.
    Store(Abs, Abs),
}

/// What a function does for its callers, found by running it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Summary {
    /// Some path returns to the caller.
    pub returns: bool,
    /// Registers (bits 0-14) equal to their entry value at every return.
    pub keep: u16,
    /// The function jumps to the address in this entry register; with
    /// `true` the jump interworks (`BX`).
    pub via: Option<(u8, bool)>,
    /// Effects that depend on the arguments.
    pub effects: Rc<[PEffect]>,
    /// The function leaves through the word its caller left at this
    /// offset from the entry stack pointer: it ends the caller's frame (a
    /// `BL` to a shared epilogue).
    pub frame_exit: Option<i32>,
    /// No instruction it runs, nor any function it calls, writes the mode,
    /// the interrupt masks, an SPSR or a banked register: the caller keeps
    /// them (a SWI restores them before it returns).
    pub psr: bool,
    /// It, or a function it calls, both loads and stores through
    /// addresses neither constant nor on its stack: it may copy.
    pub writes: bool,
    /// It passes an argument on to a call of such a function: with
    /// constant arguments its run can learn more.
    pub spec: bool,
}

impl Summary {
    /// The assumption for a function not run yet: it never returns. A
    /// caller is run again once the callee's summary is known.
    pub(crate) fn unknown() -> Summary {
        Summary::new(false, 0x7FFF)
    }

    pub(crate) fn new(returns: bool, keep: u16) -> Summary {
        Summary {
            returns,
            keep,
            via: None,
            effects: Rc::from(Vec::new()),
            frame_exit: None,
            psr: true,
            writes: false,
            spec: false,
        }
    }

    /// The assumption for code the analysis cannot see: it returns and
    /// keeps what the procedure call standard keeps.
    pub(crate) fn opaque() -> Summary {
        Summary {
            psr: false,
            ..Summary::new(true, AAPCS_KEEP)
        }
    }
}

/// `a` with the entry registers replaced by their values in `st`.
fn subst(a: Abs, st: &State) -> Abs {
    match a {
        Abs::S(Sym {
            base: Base::Entry(r),
            off,
            align,
        }) => {
            let v = crate::val::add(st.reg(r), off);
            match v {
                Abs::K(x) => Abs::K(x & !((1u128 << align) - 1)),
                Abs::S(s) if align == 0 => Abs::S(s),
                Abs::S(s) => Abs::S(Sym {
                    align: s.align.max(align),
                    ..s
                }),
                _ => Abs::Top,
            }
        }
        Abs::K(_) => a,
        _ => Abs::Top,
    }
}

fn is_param(a: Abs) -> bool {
    matches!(
        a,
        Abs::S(Sym {
            base: Base::Entry(r),
            ..
        }) if r != SP
    )
}

pub(crate) type Lifted = Result<Rc<(Encoding, Block)>, LiftError>;

/// The registers `r0`-`r12` a call passes: every register the callee can
/// read, since code may keep to no calling convention.
pub(crate) type Args = [Abs; 13];

/// What a constant call wrote, by callee and arguments.
pub(crate) type Runs = HashMap<(Loc, Args), Option<Rc<crate::conc::Written>>>;

/// What a run on a caller's constants found: copies, constant stores,
/// RAM-to-RAM moves, IRQ handlers.
#[derive(Default)]
pub(crate) struct Special {
    copies: Vec<RamCopy>,
    ptr_stores: Vec<(u32, u32)>,
    moves: Vec<(u32, u32)>,
    irq: Vec<u32>,
}

/// The runs on callers' constants, by callee and arguments, with the
/// version of the facts they used.
pub(crate) type Specials = HashMap<(Loc, Args), (u64, Rc<Special>)>;

/// How deep runs of a function on its caller's constants nest.
const MAX_SPEC_DEPTH: u8 = 3;

pub(crate) struct Env<'a> {
    pub image: &'a Image<'a>,
    pub summaries: &'a HashMap<Loc, Summary>,
    pub entries: &'a HashSet<Loc>,
    pub ptrs: &'a HashMap<u32, BTreeSet<u32>>,
    pub cache: &'a RefCell<HashMap<(Loc, u32), Lifted>>,
    pub runs: &'a RefCell<Runs>,
    /// ROM addresses where a table ends: the bases of the tables
    /// discovered code reads that it also takes as constants, and the
    /// function entries.
    pub stops: &'a BTreeSet<u32>,
    /// How many runs on a caller's constants this run is inside.
    pub depth: u8,
    pub specials: &'a RefCell<Specials>,
    /// Changes whenever a summary, a copy or a RAM constant does.
    pub version: u64,
}

impl Env<'_> {
    pub(crate) fn lift(&self, loc: Loc, word: u32) -> Lifted {
        if let Some(l) = self.cache.borrow().get(&(loc, word)) {
            return l.clone();
        }
        let l = lift::lift_for(word, loc.addr, loc.thumb, self.image.platform.profile())
            .map(|l| Rc::new((l.enc, relift_ir::simplify(&l.block))));
        self.cache.borrow_mut().insert((loc, word), l.clone());
        l
    }

    fn summary(&self, l: Loc) -> Summary {
        self.summaries
            .get(&l)
            .cloned()
            .unwrap_or_else(Summary::unknown)
    }
}

#[derive(Default)]
pub(crate) struct FnRun {
    pub body: BTreeMap<Loc, u8>,
    pub edges: Vec<Edge>,
    /// The mode, masks, SPSRs and banked registers at each edge, in the
    /// order of `edges`.
    pub edge_psr: Vec<Psr>,
    pub unresolved: Vec<Unresolved>,
    pub data: Vec<(u32, u8)>,
    /// Constant words stored to constant RAM addresses.
    pub ptr_stores: Vec<(u32, u32)>,
    /// Constants stored to constant RAM addresses: the store, address, size
    /// and value.
    pub ram_stores: Vec<(Loc, u32, u8, u32)>,
    /// Words stored to constant RAM addresses that are another RAM word
    /// with one field set to a value not known ([`Base::Field`]): the
    /// store, address, the word's address and the bits it keeps.
    pub field_stores: Vec<(Loc, u32, u32, u32)>,
    pub irq: Vec<u32>,
    pub copies: Vec<RamCopy>,
    pub swis: Vec<Swi>,
    pub tables: Vec<Table>,
    /// RAM words whose stored constants this run jumped through.
    pub reads: BTreeSet<u32>,
    pub summary: Option<Summary>,
    pub peffects: Vec<PEffect>,
    /// Branches inside the function: from, to.
    pub branches: BTreeSet<(Loc, Loc)>,
    /// Jump tables whose check another branch bypasses.
    pub bad_tables: Vec<Loc>,
    /// Thumb code addresses (bit 0 set) read from ROM tables and stored.
    pub escaped: Vec<u32>,
    /// ROM addresses in the image read as constants from ROM words.
    pub refs: Vec<u32>,
    /// The ROM tables read: base and the address after the last entry
    /// read.
    pub tab_reads: Vec<(u32, u32)>,
    /// Some instruction run writes the mode, a mask, an SPSR or a banked
    /// register.
    pub writes_psr: bool,
    /// Computed jumps with the link register set to the next instruction.
    pub linked: BTreeSet<Loc>,
    /// Words a constant call copied from RAM to RAM: destination, source.
    pub moves: Vec<(u32, u32)>,
    /// Some store goes through an address neither constant nor on the
    /// stack.
    pub ptr_store: bool,
    /// Some load does.
    pub ptr_load: bool,
    /// A callee may copy.
    pub writes: bool,
    /// Some call passes an argument on to a callee that writes.
    pub spec: bool,
    /// Code addresses in RAM no copy holds that transfers lead to.
    pub outside: Vec<Loc>,
    /// Stores to constant image addresses: address and size.
    pub image_writes: Vec<(u32, u8)>,
}

struct Run<'a> {
    env: &'a Env<'a>,
    entry: Loc,
    out: FnRun,
    rets: Vec<State>,
    states: HashMap<(Loc, Flags), State>,
    merged: HashMap<Loc, State>,
    work: Vec<(Loc, Flags)>,
    no_tables: &'a BTreeSet<Loc>,
}

/// The condition flags N, Z, C and V: states with different known flags
/// are kept apart, so a run of instructions under one condition sees the
/// flags its first instruction proved.
type Flags = [Abs; 4];

const FLAG_REGS: [relift_ir::Reg; 4] = [regs::N, regs::Z, regs::C, regs::V];

fn flags(st: &State) -> Flags {
    FLAG_REGS.map(|r| st.regs[r.index()])
}

/// The flag a condition field tests and the value that makes it hold, for
/// the conditions on one flag.
fn cond_flag(c: u32) -> Option<(usize, bool)> {
    (c < 8)
        .then_some(((c >> 1) as usize, c & 1 == 0))
        .map(|(f, v)| {
            let i = [1, 2, 0, 3][f];
            (i, v)
        })
}

/// `st` with the flag of condition `c` set so that it holds (`holds`) or
/// fails; `None` when that contradicts a known flag.
fn refine(st: &State, c: u32, holds: bool) -> Option<State> {
    let Some((i, v)) = cond_flag(c) else {
        return Some(st.clone());
    };
    let want = Abs::K(u128::from(v == holds));
    let r = FLAG_REGS[i].index();
    match st.regs[r] {
        Abs::K(_) if st.regs[r] != want => None,
        _ => {
            let mut s = st.clone();
            s.regs[r] = want;
            Some(s)
        }
    }
}

/// The most entries a table without a bound check is read for.
const MAX_TABLE: u32 = 4096;

/// The most instructions that leave the flags between the compare that
/// bounds a jump table and the conditional instruction on it.
const MAX_COMPARE_GAP: usize = 8;

/// Whether `b` takes the state from bit 0 of the address it jumps to
/// (`BX`; on ARMv5 also `BLX` with a register and the loads of the PC):
/// it writes the Thumb bit with bit 0 of a value the target is computed
/// from.
pub(crate) fn interworks(b: &Block) -> bool {
    let Next::Dynamic(target) = b.exit.next else {
        return false;
    };
    let op = |v: Value| b.insts.get(v.index()).map(|i| &i.op);
    let Some(mut t) = b.insts.iter().rev().find_map(|i| match i.op {
        Op::Set(r, v) if r == regs::T => Some(v),
        _ => None,
    }) else {
        return false;
    };
    let kept = |v: Value| matches!(op(v), Some(Op::Get(r)) if *r == regs::T);
    loop {
        match op(t) {
            Some(Op::Extract(x, 0)) => return feeds(b, *x, target),
            Some(Op::Select(_, x, y)) if kept(*y) => t = *x,
            Some(Op::Select(_, x, y)) if kept(*x) => t = *y,
            _ => return false,
        }
    }
}

/// Whether `to` is computed from `from` in `b`.
fn feeds(b: &Block, from: Value, to: Value) -> bool {
    let mut seen = HashSet::new();
    let mut work = vec![to];
    while let Some(v) = work.pop() {
        if v == from {
            return true;
        }
        if seen.insert(v.index())
            && let Some(i) = b.insts.get(v.index())
        {
            work.extend(i.op.operands());
        }
    }
    false
}

/// Whether the instruction at `loc` opens a stack frame, as a function's
/// first instruction does: it saves the link register on the stack, or
/// lowers the stack pointer without storing.
pub(crate) fn opens_frame(image: &Image<'_>, loc: Loc) -> bool {
    let Some(word) = image.fetch(loc) else {
        return false;
    };
    let Ok(l) = lift::lift_for(word, loc.addr, loc.thumb, image.platform.profile()) else {
        return false;
    };
    let block = relift_ir::simplify(&l.block);
    let st = State::entry(loc.thumb);
    let out = eval::eval(&block, loc, &st, image, false, None);
    if out.may_skip || !out.may_run {
        return false;
    }
    let saves_lr = out.st.stack.values().any(|v| {
        *v == Abs::S(Sym {
            base: Base::Entry(LR),
            off: 0,
            align: 0,
        })
    });
    let lowers = out.st.reg(SP).stack_off().is_some_and(|o| o < 0);
    saves_lr || lowers && out.st.stack.is_empty()
}

/// Runs the function at `entry`; jumps whose table check failed (in
/// `no_tables`) stay unresolved.
pub(crate) fn run(
    env: &Env<'_>,
    entry: Loc,
    entry_state: State,
    no_tables: &BTreeSet<Loc>,
) -> FnRun {
    let mut r = Run {
        env,
        entry,
        out: FnRun::default(),
        rets: Vec::new(),
        states: HashMap::new(),
        merged: HashMap::new(),
        work: Vec::new(),
        no_tables,
    };
    r.add(entry, entry_state);
    r.explore();
    r.finish()
}

/// Where code at `addr` in `thumb` state is, reached from `from`.
pub(crate) fn locs(
    image: &Image<'_>,
    from: Loc,
    addr: u32,
    thumb: bool,
) -> Result<Vec<Loc>, UnresolvedKind> {
    match image.kind(addr) {
        Kind::Image if image.in_image(addr) => Ok(vec![Loc::rom(addr, thumb)]),
        Kind::Ram => {
            if let Some(c) = from.copy
                && image.holds(c, addr)
            {
                return Ok(vec![Loc {
                    addr,
                    thumb,
                    copy: Some(c),
                }]);
            }
            let cs = image.copies_at(addr);
            if cs.is_empty() {
                return Err(UnresolvedKind::RamNoCopy);
            }
            Ok(cs
                .into_iter()
                .map(|c| Loc {
                    addr,
                    thumb,
                    copy: Some(c),
                })
                .collect())
        }
        Kind::Bios => Err(UnresolvedKind::Bios),
        _ => Err(UnresolvedKind::OutsideCode),
    }
}

impl Run<'_> {
    fn unresolved(&mut self, at: Loc, kind: UnresolvedKind, detail: String) {
        self.out.unresolved.push(Unresolved { at, kind, detail });
    }

    fn succ(&mut self, from: Loc, to: Loc, st: State) {
        if to != Self::fall_loc(from) {
            self.out.branches.insert((from, to));
        }
        self.add(to, st);
    }

    fn add(&mut self, to: Loc, st: State) {
        match self.merged.get_mut(&to) {
            Some(m) => {
                m.merge(&st);
            }
            None => {
                self.merged.insert(to, st.clone());
            }
        }
        let key = (to, flags(&st));
        match self.states.get_mut(&key) {
            Some(old) => {
                if old.merge(&st) {
                    self.work.push(key);
                }
            }
            None => {
                self.states.insert(key, st);
                self.work.push(key);
            }
        }
    }

    fn fall_loc(loc: Loc) -> Loc {
        Loc {
            addr: loc.addr.wrapping_add(loc.size()),
            ..loc
        }
    }

    fn explore(&mut self) {
        while let Some(key) = self.work.pop() {
            let Some(st) = self.states.get(&key).cloned() else {
                continue;
            };
            self.step(key.0, &st);
        }
    }

    fn step(&mut self, loc: Loc, st: &State) {
        let Some(word) = self.env.image.fetch(loc) else {
            self.unresolved(loc, UnresolvedKind::OutsideCode, "no code bytes".to_owned());
            return;
        };
        let lifted = match self.env.lift(loc, word) {
            Ok(l) => l,
            Err(e) => {
                self.unresolved(loc, UnresolvedKind::Undefined, e.to_string());
                return;
            }
        };
        let (enc, block) = &*lifted;
        self.out.body.insert(loc, loc.size() as u8);
        let fall = Self::fall_loc(loc);
        if matches!(enc, Encoding::SVC_A1 | Encoding::SVC_T1) {
            let number = if loc.thumb {
                word & 0xFF
            } else {
                (word >> 16) & 0xFF
            } as u8;
            self.swi(loc, number, st);
            if self.env.image.platform.bios_call(number) != BiosCall::NoReturn {
                let mut s = st.clone();
                for r in 0..4 {
                    s.regs[r] = Abs::Top;
                }
                self.succ(loc, fall, s);
            }
            return;
        }
        let arm_cond = (!loc.thumb && word >> 28 < 0xE && cond_flag(word >> 28).is_some())
            .then_some(word >> 28);
        if let Some(c) = arm_cond {
            if let Some(skip) = refine(st, c, false) {
                self.succ(loc, fall, skip);
            }
            let Some(run) = refine(st, c, true) else {
                return;
            };
            return self.execute(loc, word, *enc, block, &run);
        }
        self.execute(loc, word, *enc, block, st);
    }

    fn execute(&mut self, loc: Loc, word: u32, enc: Encoding, block: &Block, st: &State) {
        let fall = Self::fall_loc(loc);
        let literal = eval::pc_relative(enc, word, loc.thumb);
        let swi = matches!(block.exit.flow, Flow::Call { .. })
            && block.exit.next == relift_ir::Next::Const(0x08);
        if !swi
            && block
                .insts
                .iter()
                .any(|i| matches!(i.op, relift_ir::Op::Set(r, _) if r.index() >= regs::M.index()))
        {
            self.out.writes_psr = true;
        }
        let out = eval::eval(block, loc, st, self.env.image, literal, None);
        if out.may_skip {
            self.succ(loc, fall, st.clone());
        }
        if !out.may_run {
            return;
        }
        self.effects(loc, &out);
        if enc == Encoding::B_T1
            && cond_flag((word >> 8) & 0xF).is_some()
            && let [Abs::K(taken), Abs::K(_)] = out.targets[..]
        {
            if let Some(s) = refine(&out.st, (word >> 8) & 0xF, true) {
                let to = Loc {
                    addr: taken as u32,
                    ..loc
                };
                self.succ(loc, to, s);
            }
            if let Some(s) = refine(&out.st, (word >> 8) & 0xF, false) {
                self.succ(loc, fall, s);
            }
            return;
        }
        let link = Self::linked(&out.st, loc);
        let bx = interworks(block);
        match out.flow {
            Flow::Call { .. } => {
                for t in out.targets.clone() {
                    self.call(loc, t, &out.st, EdgeKind::Call);
                }
            }
            _ => {
                for t in out.targets.clone() {
                    self.jump(loc, enc, bx, t, &out, link);
                }
            }
        }
    }

    /// Whether the link register holds the address after `loc` with the
    /// state bit of `loc`'s state, so a jump from `loc` is a call.
    fn linked(st: &State, loc: Loc) -> bool {
        let fall = loc.addr.wrapping_add(loc.size());
        st.reg(LR).k32() == Some(fall | u32::from(loc.thumb))
    }

    fn effects(&mut self, loc: Loc, out: &Out) {
        self.out.data.extend(out.data.iter().copied());
        for &(a, n) in &out.data {
            if n == 4
                && self.env.image.kind(a) == Kind::Image
                && let Some(v) = self.env.image.read(None, a, 4)
                && self.env.image.in_image(v)
            {
                self.out.refs.push(v);
            }
        }
        self.out.ptr_load |= out.ptr_load;
        for &(a, n, v) in &out.stores {
            if a.k().is_none() && a.stack_off().is_none() {
                self.out.ptr_store = true;
            }
            self.image_write(a, n);
            self.ram_store(loc, a, n, v);
            if n == 4 {
                self.store(a, v);
            }
        }
        for d in &out.dma {
            self.dma(loc, d);
        }
    }

    /// A store of `n` bytes at `a`, noted when `a` is a constant address
    /// in the image.
    fn image_write(&mut self, a: Abs, n: u8) {
        if let Some(a) = a.k32()
            && self.env.image.kind(a) == Kind::Image
        {
            self.out.image_writes.push((a, n));
        }
    }

    fn ram_store(&mut self, site: Loc, a: Abs, n: u8, v: Abs) {
        let Some(a) = a.k32().filter(|a| self.env.image.is_ram(*a)) else {
            return;
        };
        match v {
            Abs::K(_) => {
                if let Some(v) = v.k32() {
                    self.out.ram_stores.push((site, a, n, v));
                }
            }
            Abs::S(Sym {
                base: Base::Field { addr, keep },
                off: 0,
                align: 0,
            }) if n == 4 && a.is_multiple_of(4) => {
                self.out.field_stores.push((site, a, addr, keep));
            }
            _ => {}
        }
    }

    fn peffect(&mut self, e: PEffect) {
        if !self.out.peffects.contains(&e) {
            self.out.peffects.push(e);
        }
    }

    /// A word store of `v` at `a`.
    fn store(&mut self, a: Abs, v: Abs) {
        if let Abs::Tab { base, stride, n } = v
            && a.stack_off().is_none()
        {
            let es = self.table_entries(base, stride, n, None);
            self.out
                .escaped
                .extend(es.into_iter().filter(|e| e.1).map(|e| e.0 | 1));
            return;
        }
        match (a, v) {
            (Abs::K(a), Abs::K(v)) => {
                let (a, v) = (a as u32, v as u32);
                if self.env.image.is_ram(a) && a % 4 == 0 {
                    self.out.ptr_stores.push((a, v));
                    if self.env.image.platform.irq_vector(a) {
                        self.out.irq.push(v);
                    }
                } else if self.env.image.kind(a) == Kind::Image && a % 4 == 0 {
                    self.out.ptr_stores.push((a, v));
                }
            }
            (a, v) if (is_param(a) || a.k().is_some()) && (is_param(v) || v.k().is_some()) => {
                self.peffect(PEffect::Store(a, v));
            }
            _ => {}
        }
    }

    /// Applies a callee's argument-dependent effects at a call from
    /// `site` with state `st`.
    fn apply(&mut self, site: Loc, effects: &[PEffect], st: &State) {
        for e in effects {
            match *e {
                PEffect::Swi(n, args) => {
                    let args = args.map(|a| subst(a, st));
                    self.swi_args(site, n, args);
                }
                PEffect::Store(a, v) => {
                    let (a, v) = (subst(a, st), subst(v, st));
                    self.image_write(a, 4);
                    self.ram_store(site, a, 4, v);
                    self.store(a, v);
                }
            }
        }
    }

    fn dma(&mut self, loc: Loc, d: &DmaStart) {
        let dma = Dma {
            channel: d.channel,
            src: d.src.k32(),
            dst: d.dst.k32(),
            count: d.count.k32(),
            control: d.control,
        };
        if let Some((src, dst, len)) = self.env.image.platform.dma(&dma) {
            self.copy(loc, CopyKind::Dma(d.channel), src, dst, len);
        }
    }

    fn copy(&mut self, site: Loc, kind: CopyKind, src: u32, dst: u32, len: u32) {
        if len == 0 || !self.env.image.is_ram(dst) {
            return;
        }
        let Some(bytes) = self.env.image.source(src, len) else {
            return;
        };
        self.out.copies.push(RamCopy {
            site,
            kind,
            src: Some(src),
            dst,
            bytes: Arc::from(bytes),
        });
    }

    fn swi(&mut self, loc: Loc, number: u8, st: &State) {
        self.out.swis.push(Swi { site: loc, number });
        self.swi_args(loc, number, [st.reg(0), st.reg(1), st.reg(2)]);
    }

    fn swi_args(&mut self, loc: Loc, number: u8, args: [Abs; 3]) {
        let call = self.env.image.platform.bios_call(number);
        if !matches!(call, BiosCall::CpuSet | BiosCall::CpuFastSet) {
            return;
        }
        let (Some(src), Some(dst), Some(ctl)) = (args[0].k32(), args[1].k32(), args[2].k32())
        else {
            if args.iter().all(|a| is_param(*a) || a.k().is_some()) {
                self.peffect(PEffect::Swi(number, args));
            }
            return;
        };
        if ctl & (1 << 24) != 0 {
            return;
        }
        let count = ctl & 0x1F_FFFF;
        if call == BiosCall::CpuSet {
            let unit = if ctl & (1 << 26) != 0 { 4 } else { 2 };
            self.copy(
                loc,
                CopyKind::CpuSet,
                src & !(unit - 1),
                dst & !(unit - 1),
                count * unit,
            );
        } else {
            self.copy(
                loc,
                CopyKind::CpuFastSet,
                src & !3,
                dst & !3,
                count.div_ceil(8) * 32,
            );
        }
    }

    fn targets(&mut self, at: Loc, addr: u32, thumb: bool) -> Vec<Loc> {
        match locs(self.env.image, at, addr, thumb) {
            Ok(v) => v,
            Err(k) => {
                if k == UnresolvedKind::RamNoCopy {
                    self.out.outside.push(Loc::rom(addr, thumb));
                }
                self.unresolved(at, k, format!("{addr:#010x}"));
                Vec::new()
            }
        }
    }

    /// Continues after a call from `site` to a function with summary `s`.
    fn resume(&mut self, site: Loc, st: &State, s: Summary) {
        if !s.returns {
            return;
        }
        let mut after = st.clone();
        if self.env.image.options.reachable_locals {
            after.forget_reachable_locals();
        }
        after.clobber(s.keep, s.psr);
        let mut t = after.regs.clone();
        t[relift_cpu_arm32::regs::T.index()] = Abs::K(u128::from(site.thumb));
        after.regs = t;
        let fall = Self::fall_loc(site);
        self.succ(site, fall, after);
    }

    fn call(&mut self, site: Loc, t: Abs, st: &State, kind: EdgeKind) {
        let Abs::K(v) = t else {
            self.call_dynamic(site, t, st);
            return;
        };
        let Some(thumb) = st.thumb() else {
            self.unresolved(
                site,
                UnresolvedKind::UnknownState,
                format!("call to {v:#010x}"),
            );
            return;
        };
        let mut s = Summary::new(false, 0x7FFF);
        let callees = self.targets(site, v as u32, thumb);
        if callees.is_empty() {
            // A callee the run cannot reach is not proof that the call
            // never returns.
            s = if self.env.image.platform.external(v as u32) {
                Summary::new(true, AAPCS_KEEP)
            } else {
                Summary::opaque()
            };
        }
        for callee in callees {
            self.out.edges.push(Edge {
                site,
                target: callee,
                kind,
            });
            self.out.edge_psr.push(st.psr());
            let ran = self.constant_call(site, callee, st);
            let cs = self.env.summary(callee);
            self.out.writes |= cs.writes;
            let addr = |a: Abs| {
                is_param(a)
                    || a.k32()
                        .is_some_and(|k| self.env.image.is_ram(k) || self.env.image.is_source(k))
            };
            if (cs.writes || cs.spec)
                && (0..4).any(|r| is_param(st.reg(r)))
                && (0..4).filter(|r| addr(st.reg(*r))).count() >= 2
            {
                self.out.spec = true;
            }
            if cs.spec && !ran {
                self.special(callee, st);
            }
            let effects = cs.effects.clone();
            self.apply(site, &effects, st);
            if let Some(o) = cs.frame_exit {
                self.frame_exit(site, o, st);
            }
            let cs = match cs.via {
                Some((r, bx)) => self.via(site, callee, r, bx, st, cs),
                None => cs,
            };
            s.returns |= cs.returns;
            s.keep &= cs.keep;
            s.psr &= cs.psr;
        }
        self.resume(site, st, s);
    }

    /// The code addresses in the table at `base` with `stride`: the first
    /// `n` entries at most, from the first while each is a code address
    /// (zero entries are skipped), with the state from bit 0 when `thumb`
    /// is `None`. A ROM table also ends before the next stop (the base of
    /// another table, an address discovered code both takes as a constant
    /// and reads as a table, or a function entry). The ROM words read are
    /// data.
    fn table_entries(
        &mut self,
        base: u32,
        stride: u32,
        n: u32,
        thumb: Option<bool>,
    ) -> Vec<(u32, bool)> {
        let mut out = Vec::new();
        let ram = self.env.image.is_ram(base);
        let cut = if ram {
            u32::MAX
        } else {
            self.env
                .stops
                .range(base.saturating_add(1)..)
                .next()
                .copied()
                .unwrap_or(u32::MAX)
        };
        let mut end = base;
        for i in 0..MAX_TABLE.min(n) {
            let a = base.wrapping_add(i * stride);
            if !ram {
                end = a.saturating_add(4);
            }
            if a.checked_add(4).is_none_or(|e| e > cut) {
                break;
            }
            let mut vals: Vec<u32> = Vec::new();
            if ram {
                self.out.reads.insert(a);
                for c in self.env.image.copies_at(a) {
                    if let Some(v) = self.env.image.read(Some(c), a, 4) {
                        vals.push(v);
                    }
                }
                if let Some(p) = self.env.ptrs.get(&a) {
                    vals.extend(p.iter().copied());
                }
            } else if let Some(v) = self.env.image.read(None, a, 4) {
                vals.push(v);
            }
            if vals.is_empty() {
                break;
            }
            let mut ok = true;
            let mut here = Vec::new();
            for v in vals {
                if v == 0 {
                    continue;
                }
                let th = thumb.unwrap_or(v & 1 == 1);
                let addr = if th { v & !1 } else { v };
                let rom = self.env.image.platform.code_addr(addr);
                let in_ram =
                    self.env.image.is_ram(addr) && !self.env.image.copies_at(addr).is_empty();
                if addr % 4 != 0 && !th || !(rom || in_ram) {
                    ok = false;
                    break;
                }
                here.push((addr, th));
            }
            if !ok {
                break;
            }
            if !ram {
                self.out.data.push((a, 4));
            }
            out.extend(here);
        }
        if !ram {
            self.out.tab_reads.push((base, end));
        }
        out.sort_unstable();
        out.dedup();
        out
    }

    /// A transfer to a word of the ROM table at `base`.
    fn tab_transfer(
        &mut self,
        site: Loc,
        (base, stride, n): (u32, u32, u32),
        thumb: Option<bool>,
        st: &State,
        linked: bool,
    ) -> Summary {
        let es = self.table_entries(base, stride, n, thumb);
        if es.is_empty() {
            self.unresolved(
                site,
                UnresolvedKind::IndirectJump,
                format!("table {base:#010x} has no code address"),
            );
            return Summary::opaque();
        }
        let mut s = Summary::new(false, 0x7FFF);
        let mut reached = false;
        for (addr, th) in es {
            for t in self.targets(site, addr, th) {
                reached = true;
                self.out.edges.push(Edge {
                    site,
                    target: t,
                    kind: EdgeKind::Pointer,
                });
                self.out.edge_psr.push(st.psr());
                let ts = self.env.summary(t);
                s.returns |= ts.returns;
                s.keep &= ts.keep;
                s.psr &= ts.psr;
            }
        }
        if !reached {
            s = Summary::opaque();
        }
        if linked {
            self.resume(site, st, s.clone());
        }
        s
    }

    /// A call to a function that leaves through the word at offset `o`
    /// of its entry stack: that word is in this function's frame.
    fn frame_exit(&mut self, site: Loc, o: i32, st: &State) {
        let Some(p) = st.reg(SP).stack_off() else {
            self.unresolved(
                site,
                UnresolvedKind::IndirectJump,
                "epilogue with unknown stack".to_owned(),
            );
            return;
        };
        let slot = p + o;
        let v = match st.stack.get(&slot) {
            Some(v) => *v,
            None if slot >= 0 && !st.frame_dirty.contains(&slot) => Abs::sym(Base::Frame(slot), 0),
            None => Abs::Top,
        };
        let mut after = st.clone();
        after.clobber(0, false);
        match v {
            Abs::S(Sym {
                base: Base::Entry(LR),
                off: 0,
                ..
            }) => self.rets.push(after),
            Abs::S(Sym {
                base: Base::Frame(q),
                off: 0,
                ..
            }) => {
                let own = self.out.summary.get_or_insert(Summary::new(false, 0x7FFF));
                own.frame_exit.get_or_insert(q);
            }
            _ => self.unresolved(
                site,
                UnresolvedKind::IndirectJump,
                "epilogue target not known".to_owned(),
            ),
        }
    }

    /// A jump from `site` to the entry of another function `to`: that
    /// function returns to this function's caller, so its effects, its
    /// returns and the registers it keeps are this function's from here.
    fn tail(&mut self, site: Loc, to: Loc, st: &State) {
        self.out.edges.push(Edge {
            site,
            target: to,
            kind: EdgeKind::Tail,
        });
        self.out.edge_psr.push(st.psr());
        let cs = self.env.summary(to);
        let effects = cs.effects.clone();
        self.apply(site, &effects, st);
        if let Some(o) = cs.frame_exit {
            self.frame_exit(site, o, st);
        }
        let cs = match cs.via {
            Some((r, bx)) => self.via(site, to, r, bx, st, cs),
            None => cs,
        };
        if cs.returns {
            let mut after = st.clone();
            after.clobber(cs.keep, cs.psr);
            self.rets.push(after);
        }
    }

    /// A call whose registers name both a ROM and a RAM address is run
    /// with the constants in `r0`-`r12`; what it writes to RAM becomes
    /// copies.
    fn constant_call(&mut self, site: Loc, callee: Loc, st: &State) -> bool {
        let args: Args = std::array::from_fn(|i| st.reg(i as u8));
        let ks: Vec<u32> = args.iter().filter_map(|a| a.k32()).collect();
        let ram: BTreeSet<u32> = ks
            .iter()
            .copied()
            .filter(|a| self.env.image.is_ram(*a))
            .collect();
        let image = ks.iter().any(|a| self.env.image.is_source(*a));
        if !(self.env.image.options.image_calls && image)
            && (ram.is_empty() || !(image || ram.len() > 1))
        {
            return false;
        }
        let key = (callee, args);
        let cached = self.env.runs.borrow().get(&key).cloned();
        let got = match cached {
            Some(g) => g,
            None => {
                let g = crate::conc::run_call(self.env, callee, args).map(Rc::new);
                self.env.runs.borrow_mut().insert(key, g.clone());
                g
            }
        };
        let Some(w) = got else { return true };
        for (dst, bytes) in &w.ranges {
            if bytes.len() >= 4 {
                self.out.copies.push(RamCopy {
                    site,
                    kind: CopyKind::Run,
                    src: None,
                    dst: *dst,
                    bytes: Arc::from(bytes.as_slice()),
                });
            }
        }
        self.out.moves.extend(w.moves.iter().copied());
        true
    }

    /// A call that passes constants to a function that passes its
    /// arguments on to a writer: the callee is run again with them, and
    /// the copies and constant stores that run finds are this run's.
    fn special(&mut self, callee: Loc, st: &State) {
        if self.env.depth >= MAX_SPEC_DEPTH
            || !(0..13u8).any(|r| {
                st.reg(r)
                    .k32()
                    .is_some_and(|a| self.env.image.is_ram(a) || self.env.image.is_source(a))
            })
        {
            return;
        }
        let args: Args = std::array::from_fn(|i| match st.reg(i as u8) {
            a @ Abs::K(_) => a,
            _ => Abs::Top,
        });
        let key = (callee, args);
        let cached = self.env.specials.borrow().get(&key).cloned();
        let sp = match cached {
            Some((v, sp)) if v == self.env.version => sp,
            _ => {
                let mut entry = State::entry(callee.thumb);
                entry.set_psr(&st.psr());
                for (r, a) in args.iter().enumerate() {
                    if a.k().is_some() {
                        entry.regs[r] = *a;
                    }
                }
                let env = Env {
                    depth: self.env.depth + 1,
                    ..*self.env
                };
                let r = run(&env, callee, entry, self.no_tables);
                let sp = Rc::new(Special {
                    copies: r.copies,
                    ptr_stores: r.ptr_stores,
                    moves: r.moves,
                    irq: r.irq,
                });
                self.env
                    .specials
                    .borrow_mut()
                    .insert(key, (self.env.version, sp.clone()));
                sp
            }
        };
        self.out.copies.extend(sp.copies.iter().cloned());
        self.out.ptr_stores.extend(sp.ptr_stores.iter().copied());
        self.out.moves.extend(sp.moves.iter().copied());
        self.out.irq.extend(sp.irq.iter().copied());
    }

    /// A call to `callee`, which jumps to the address in entry register
    /// `r`: the call reaches that address. The summary of the pair.
    fn via(&mut self, site: Loc, callee: Loc, r: u8, bx: bool, st: &State, cs: Summary) -> Summary {
        let v = st.reg(r);
        match v {
            Abs::K(x) => {
                let x = x as u32;
                let thumb = if bx { x & 1 == 1 } else { site.thumb };
                let addr = if thumb { x & !1 } else { x & !3 };
                let mut s = Summary::new(cs.returns, cs.keep);
                s.psr = cs.psr;
                let ts = self.targets(site, addr, thumb);
                if ts.is_empty() {
                    return Summary::opaque();
                }
                for t in ts {
                    self.out.edges.push(Edge {
                        site,
                        target: t,
                        kind: EdgeKind::Via,
                    });
                    self.out.edge_psr.push(st.psr());
                    let ts = self.env.summary(t);
                    s.returns |= ts.returns;
                    s.keep &= ts.keep;
                    s.psr &= ts.psr;
                }
                s
            }
            Abs::S(Sym {
                base: Base::Entry(k),
                off: 0,
                ..
            }) => {
                let own = self.out.summary.get_or_insert(Summary::new(false, 0x7FFF));
                own.via.get_or_insert((k, bx));
                Summary::new(false, 0x7FFF)
            }
            Abs::S(Sym {
                base: Base::Mem(a),
                off: 0,
                ..
            }) => self.pointer(site, a, (!bx).then_some(site.thumb), st, false),
            Abs::Tab { base, stride, n } => self.tab_transfer(
                site,
                (base, stride, n),
                (!bx).then_some(site.thumb),
                st,
                false,
            ),
            _ => {
                self.unresolved(
                    site,
                    UnresolvedKind::IndirectCall,
                    format!("r{r} passed to {callee} not constant"),
                );
                Summary::opaque()
            }
        }
    }

    fn call_dynamic(&mut self, site: Loc, t: Abs, st: &State) {
        if let Abs::S(Sym {
            base: Base::Mem(a),
            off: 0,
            ..
        }) = t
        {
            let thumb = st.thumb();
            self.pointer(site, a, thumb, st, true);
            return;
        }
        self.unresolved(
            site,
            UnresolvedKind::IndirectCall,
            "target not constant".to_owned(),
        );
        self.resume(site, st, Summary::opaque());
    }

    /// A transfer through the RAM word at `a`, or the image variable at
    /// `a` ([`Image::variable`]): to every constant discovered code stores
    /// there and the variable's first value, but zero (a null pointer,
    /// which a program tests before it calls). `thumb` is the target
    /// state, `None` when the transfer interworks.
    fn pointer(
        &mut self,
        site: Loc,
        a: u32,
        thumb: Option<bool>,
        st: &State,
        linked: bool,
    ) -> Summary {
        self.out.reads.insert(a);
        let first = (self.env.image.kind(a) == Kind::Image)
            .then(|| self.env.image.read(None, a, 4))
            .flatten();
        let mut vals: Vec<u32> = self
            .env
            .ptrs
            .get(&a)
            .into_iter()
            .flatten()
            .copied()
            .chain(first)
            .filter(|v| *v != 0)
            .collect();
        vals.sort_unstable();
        vals.dedup();
        if vals.is_empty() {
            self.unresolved(site, UnresolvedKind::PointerNoStore, format!("[{a:#010x}]"));
            if linked {
                self.resume(site, st, Summary::opaque());
            }
            return Summary::opaque();
        }
        let mut s = Summary::new(false, 0x7FFF);
        let mut reached = false;
        for v in vals {
            let th = thumb.unwrap_or(v & 1 == 1);
            let addr = if th { v & !1 } else { v & !3 };
            for t in self.targets(site, addr, th) {
                reached = true;
                self.out.edges.push(Edge {
                    site,
                    target: t,
                    kind: EdgeKind::Pointer,
                });
                self.out.edge_psr.push(st.psr());
                let ts = self.env.summary(t);
                s.returns |= ts.returns;
                s.keep &= ts.keep;
                s.psr &= ts.psr;
            }
        }
        if !reached {
            s = Summary::opaque();
        }
        if linked {
            self.resume(site, st, s.clone());
        }
        s
    }

    fn jump(&mut self, site: Loc, enc: Encoding, bx: bool, t: Abs, out: &Out, link: bool) {
        let st = &out.st;
        if link && !matches!(t, Abs::K(v) if v as u32 == Self::fall_loc(site).addr) {
            self.out.linked.insert(site);
        }
        if !link
            && matches!(t, Abs::Tab { .. } | Abs::Top | Abs::B { .. })
            && !self.no_tables.contains(&site)
            && self.table(site, out)
        {
            return;
        }
        match t {
            Abs::K(v) => {
                let v = v as u32;
                let Some(thumb) = st.thumb() else {
                    self.unresolved(
                        site,
                        UnresolvedKind::UnknownState,
                        format!("jump to {v:#010x}"),
                    );
                    return;
                };
                let fall = Self::fall_loc(site);
                if v == fall.addr && thumb == site.thumb {
                    self.succ(site, fall, st.clone());
                    return;
                }
                if link {
                    self.call(site, t, st, EdgeKind::LinkedJump);
                    return;
                }
                for to in self.targets(site, v, thumb) {
                    if to != self.entry && self.env.entries.contains(&to) {
                        self.tail(site, to, st);
                    } else {
                        self.succ(site, to, st.clone());
                    }
                }
            }
            Abs::S(Sym {
                base: Base::Entry(LR),
                off: 0,
                ..
            }) if !link => self.rets.push(st.clone()),
            Abs::S(Sym {
                base: Base::Frame(o),
                off: 0,
                ..
            }) if !link => {
                let own = self.out.summary.get_or_insert(Summary::new(false, 0x7FFF));
                own.frame_exit.get_or_insert(o);
            }
            Abs::S(Sym {
                base: Base::Entry(r),
                off: 0,
                ..
            }) if !link => {
                let own = self.out.summary.get_or_insert(Summary::new(false, 0x7FFF));
                own.via.get_or_insert((r, bx));
            }
            Abs::Tab { base, stride, n } if link => {
                let thumb = if bx { None } else { Some(site.thumb) };
                self.tab_transfer(site, (base, stride, n), thumb, st, link);
            }
            Abs::Tab { base, stride, n } => {
                let thumb = if bx { None } else { Some(site.thumb) };
                let es = self.table_entries(base, stride, n, thumb);
                if es.is_empty() {
                    self.unresolved(
                        site,
                        UnresolvedKind::IndirectJump,
                        format!("table {base:#010x} has no code address"),
                    );
                }
                for (addr, th) in es {
                    for to in self.targets(site, addr, th) {
                        let mut s2 = st.clone();
                        s2.regs[regs::T.index()] = Abs::K(u128::from(th));
                        if to != self.entry && self.env.entries.contains(&to) {
                            self.tail(site, to, &s2);
                        } else {
                            self.succ(site, to, s2);
                        }
                    }
                }
            }
            Abs::S(Sym {
                base: Base::Mem(a),
                off: 0,
                ..
            }) => {
                let thumb = if bx { None } else { st.thumb() };
                if thumb.is_none() && !bx {
                    self.unresolved(site, UnresolvedKind::UnknownState, format!("[{a:#010x}]"));
                    return;
                }
                self.pointer(site, a, thumb, st, link);
            }
            _ if !link && self.env.image.options.switches && self.unbounded_index(site, st) => {}
            _ => {
                let detail = format!("{} target not constant", enc.name());
                if link {
                    self.unresolved(site, UnresolvedKind::IndirectCall, detail);
                    self.resume(site, st, Summary::opaque());
                } else {
                    self.unresolved(site, UnresolvedKind::IndirectJump, detail);
                }
            }
        }
    }

    /// The register and bound of a compare with an immediate at `l`.
    fn compare(&self, l: Loc) -> Option<(u8, u32)> {
        let w = self.env.image.fetch(l)?;
        let (enc, _) = &*self.env.lift(l, w).ok()?;
        match enc {
            Encoding::CMP_i_T1 => Some((((w >> 8) & 7) as u8, w & 0xFF)),
            Encoding::CMP_i_A1 if w >> 28 == 0xE => {
                let rot = (w >> 8) & 0xF;
                Some((((w >> 16) & 0xF) as u8, (w & 0xFF).rotate_right(2 * rot)))
            }
            _ => None,
        }
    }

    /// Whether the instruction at `l` leaves the flags as they are and goes
    /// on to the next one.
    fn passes_flags(&self, l: Loc) -> bool {
        let Some(w) = self.env.image.fetch(l) else {
            return false;
        };
        let Ok(lifted) = self.env.lift(l, w) else {
            return false;
        };
        let b = &lifted.1;
        b.exit.flow == Flow::Jump
            && b.exit.next == Next::Const(u64::from(Self::fall_loc(l).addr))
            && !b
                .insts
                .iter()
                .any(|i| matches!(i.op, Op::Set(r, _) if FLAG_REGS.contains(&r)))
    }

    /// The jump at `site` as an index into the code right after the address
    /// it reads as the program counter: that address and how far the
    /// target moves per index, when the target is that address plus an
    /// index the run does not bound (`add pc, pc, rX, lsl #n`) and reads
    /// no memory.
    fn index_jump(&self, site: Loc) -> Option<(u32, u32)> {
        let w = self.env.image.fetch(site)?;
        let lifted = self.env.lift(site, w).ok()?;
        let at = |k: u32| {
            let mut st = State::entry(site.thumb);
            for r in 0..15 {
                st.regs[r] = Abs::K(u128::from(k));
            }
            let out = eval::eval(&lifted.1, site, &st, self.env.image, false, None);
            match (out.targets.first(), out.data.is_empty() && !out.ptr_load) {
                (Some(Abs::K(t)), true) => u32::try_from(*t).ok(),
                _ => None,
            }
        };
        let base = site.addr.wrapping_add(2 * site.size());
        let (t0, t1) = (at(0)?, at(2)?);
        (t0 == base && t1 > t0).then_some((base, (t1 - t0) / 2))
    }

    /// An instruction at `l` that always branches to a constant address
    /// within the function (`B`).
    fn is_branch(&self, l: Loc) -> bool {
        let Some(w) = self.env.image.fetch(l) else {
            return false;
        };
        let Ok(lifted) = self.env.lift(l, w) else {
            return false;
        };
        let b = &lifted.1;
        let guarded = b
            .insts
            .iter()
            .any(|i| matches!(i.op, Op::Guard(_, COND_FAIL)));
        !guarded
            && b.exit.flow == Flow::Jump
            && matches!(b.exit.next, Next::Const(t) if t != u64::from(Self::fall_loc(l).addr))
    }

    /// A jump into the code after it by an index with no bound: every
    /// branch of a table of them there, or else the code there from its
    /// first instruction (an unrolled loop entered part way).
    fn unbounded_index(&mut self, site: Loc, st: &State) -> bool {
        let Some((base, _)) = self.index_jump(site) else {
            return false;
        };
        let mut n = 0;
        while n < MAX_TABLE {
            let l = Loc {
                addr: base.wrapping_add(n * site.size()),
                ..site
            };
            if !self.is_branch(l) {
                break;
            }
            self.succ(site, l, st.clone());
            n += 1;
        }
        if n == 0 {
            self.succ(site, Loc { addr: base, ..site }, st.clone());
        }
        true
    }

    /// The condition field of a conditional instruction at `l`.
    fn condition(&self, l: Loc) -> Option<u32> {
        let w = self.env.image.fetch(l)?;
        let (enc, _) = &*self.env.lift(l, w).ok()?;
        let c = match enc {
            Encoding::B_T1 => (w >> 8) & 0xF,
            _ if !l.thumb => w >> 28,
            _ => return None,
        };
        (c < 0xE).then_some(c)
    }

    /// Runs the code from `from` with register `r` = `i` until it leaves
    /// `from..=site` (`Ok(None)`) or takes the jump at `site`.
    fn walk(
        &self,
        from: Loc,
        site: Loc,
        r: u8,
        i: u32,
        data: &mut Vec<(u32, u8)>,
    ) -> Result<Option<(u32, bool)>, ()> {
        let mut st = self.merged.get(&from).ok_or(())?.clone();
        st.regs[usize::from(r)] = Abs::K(u128::from(i));
        let mut l = from;
        for _ in 0..32 {
            let w = self.env.image.fetch(l).ok_or(())?;
            let lifted = self.env.lift(l, w).map_err(|_| ())?;
            let literal = eval::pc_relative(lifted.0, w, l.thumb);
            let out = eval::eval(&lifted.1, l, &st, self.env.image, literal, None);
            if out.may_skip && out.may_run {
                return Err(());
            }
            data.extend(out.data.iter().copied());
            let next = if out.may_run {
                let [Abs::K(t)] = out.targets[..] else {
                    return Err(());
                };
                st = out.st;
                let thumb = st.thumb().ok_or(())?;
                if l == site {
                    return Ok(Some((t as u32, thumb)));
                }
                Loc {
                    addr: t as u32,
                    thumb,
                    copy: l.copy,
                }
            } else {
                if l == site {
                    return Ok(None);
                }
                Self::fall_loc(l)
            };
            if next.thumb != l.thumb || next.addr < from.addr || next.addr > site.addr {
                return Ok(None);
            }
            l = next;
        }
        Err(())
    }

    /// A computed jump bounded by a compare with an immediate and a
    /// conditional instruction on the C and Z flags right after it: the
    /// targets for every index that reaches the jump.
    fn table(&mut self, site: Loc, out: &Out) -> bool {
        let mut check = None;
        for k in 0..12u32 {
            let Some(addr) = site.addr.checked_sub(k * site.size()) else {
                break;
            };
            let l = Loc { addr, ..site };
            if !self.merged.contains_key(&l) {
                break;
            }
            if let Some(c) = self.condition(l) {
                check = Some((l, c));
                break;
            }
        }
        let Some((cond_at, cond)) = check else {
            return false;
        };
        if !matches!(cond, 0 | 1 | 2 | 3 | 8 | 9) {
            return false;
        }
        let Some(cmp_addr) = cond_at.addr.checked_sub(site.size()) else {
            return false;
        };
        let mut cmp_at = Loc {
            addr: cmp_addr,
            ..site
        };
        let mut cmp = self.compare(cmp_at);
        if self.env.image.options.switches {
            for _ in 0..MAX_COMPARE_GAP {
                if cmp.is_some() || !self.merged.contains_key(&cmp_at) || !self.passes_flags(cmp_at)
                {
                    break;
                }
                let Some(a) = cmp_at.addr.checked_sub(site.size()) else {
                    break;
                };
                cmp_at = Loc { addr: a, ..site };
                cmp = self.compare(cmp_at);
            }
        }
        let Some((r, max)) = cmp else {
            return false;
        };
        if max > 1024 || !self.merged.contains_key(&cmp_at) {
            return false;
        }
        let mut data = Vec::new();
        if !matches!(self.walk(cmp_at, site, r, max + 1, &mut data), Ok(None)) {
            return false;
        }
        let mut raw = Vec::new();
        for i in 0..=max {
            match self.walk(cmp_at, site, r, i, &mut data) {
                Ok(Some(t)) => raw.push(t),
                Ok(None) => {}
                Err(()) => return false,
            }
        }
        if raw.is_empty() {
            return false;
        }
        self.out.data.extend(data);
        let mut targets = Vec::new();
        for (addr, thumb) in raw {
            for t in self.targets(site, addr, thumb) {
                targets.push(t);
                self.succ(site, t, out.st.clone());
            }
        }
        self.out.tables.push(Table {
            site,
            check: cmp_at,
            max,
            targets,
        });
        true
    }

    fn finish(mut self) -> FnRun {
        for t in &self.out.tables {
            let inside = |l: &Loc| {
                l.thumb == t.site.thumb
                    && l.copy == t.site.copy
                    && l.addr >= t.check.addr
                    && l.addr <= t.site.addr
            };
            let bypass = self.out.branches.iter().any(|(from, to)| {
                !inside(from)
                    && to.thumb == t.site.thumb
                    && to.copy == t.site.copy
                    && to.addr > t.check.addr
                    && to.addr <= t.site.addr
            });
            if bypass {
                self.out.bad_tables.push(t.site);
            }
        }
        let mut s = self.out.summary.unwrap_or(Summary::new(false, 0x7FFF));
        let unresolved_exit = self.out.unresolved.iter().any(|u| {
            matches!(
                u.kind,
                UnresolvedKind::IndirectJump
                    | UnresolvedKind::PointerNoStore
                    | UnresolvedKind::UnknownState
            )
        });
        if !self.rets.is_empty() || unresolved_exit {
            s.returns = true;
        }
        if unresolved_exit {
            s.keep &= AAPCS_KEEP;
            s.psr = false;
        }
        if self.out.writes_psr {
            s.psr = false;
        }
        for st in &self.rets {
            for r in 0..15u8 {
                let same = st.reg(r)
                    == Abs::S(Sym {
                        base: Base::Entry(r),
                        off: 0,
                        align: 0,
                    });
                if !same {
                    s.keep &= !(1 << r);
                }
            }
        }
        s.effects = Rc::from(std::mem::take(&mut self.out.peffects));
        s.writes = self.out.writes || self.out.ptr_store && self.out.ptr_load;
        s.spec = self.out.spec;
        self.out.summary = Some(s);
        self.out
    }
}

//! One function run: the code reachable from an entry without entering
//! another function, with the abstract state at every instruction.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::rc::Rc;

use relift_cpu_arm32::decode::Encoding;
use relift_cpu_arm32::lift::{self, LiftError};
use relift_cpu_arm32::regs;
use relift_ir::{Block, Flow};

use crate::eval::{self, DmaStart, Out};
use crate::image::Image;
use crate::loc::{Loc, Region};
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

/// The RAM ranges a constant call wrote, by callee and arguments.
pub(crate) type Runs = HashMap<(Loc, Args), Option<Rc<Vec<(u32, Vec<u8>)>>>>;

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
}

impl Env<'_> {
    pub(crate) fn lift(&self, loc: Loc, word: u32) -> Lifted {
        if let Some(l) = self.cache.borrow().get(&(loc, word)) {
            return l.clone();
        }
        let l = lift::lift(word, loc.addr, loc.thumb)
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

const IRQ_VECTOR: u32 = 0x0300_7FFC;
const SWI_CPU_SET: u8 = 0x0B;
const SWI_CPU_FAST_SET: u8 = 0x0C;

fn is_bx(e: Encoding) -> bool {
    matches!(e, Encoding::BX_A1 | Encoding::BX_T1 | Encoding::BXV4_T1)
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
    let reg = Region::of(addr);
    match reg {
        Region::Rom if image.in_image(addr) => Ok(vec![Loc::rom(addr, thumb)]),
        Region::Ewram | Region::Iwram => {
            if let Some(c) = from.copy
                && image.copies_at(addr).contains(&c)
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
        Region::Bios => Err(UnresolvedKind::Bios),
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
            if !matches!(number, 0x00 | 0x26) {
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
        match out.flow {
            Flow::Call { .. } => {
                for t in out.targets.clone() {
                    self.call(loc, t, &out.st, EdgeKind::Call);
                }
            }
            _ => {
                for t in out.targets.clone() {
                    self.jump(loc, enc, t, &out, link);
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
                && Region::of(a) == Region::Rom
                && let Some(v) = self.env.image.read(None, a, 4)
                && self.env.image.in_image(v)
            {
                self.out.refs.push(v);
            }
        }
        for &(a, n, v) in &out.stores {
            if n == 4 {
                self.store(a, v);
            }
        }
        for d in &out.dma {
            self.dma(loc, d);
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
                if Region::of(a).is_ram() && a % 4 == 0 {
                    self.out.ptr_stores.push((a, v));
                    if a & 0x00FF_7FFF == IRQ_VECTOR & 0x00FF_7FFF && Region::of(a) == Region::Iwram
                    {
                        self.out.irq.push(v);
                    }
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
                PEffect::Store(a, v) => self.store(subst(a, st), subst(v, st)),
            }
        }
    }

    fn dma(&mut self, loc: Loc, d: &DmaStart) {
        let c = u32::from(d.control);
        let timing = (c >> 12) & 3;
        let src_ctl = (c >> 7) & 3;
        let dst_ctl = (c >> 5) & 3;
        if timing != 0 || src_ctl != 0 || !(dst_ctl == 0 || dst_ctl == 3) || d.channel == 0 {
            return;
        }
        let (Some(src), Some(dst), Some(count)) = (d.src.k32(), d.dst.k32(), d.count.k32()) else {
            return;
        };
        let count = match (count, d.channel) {
            (0, 3) => 0x1_0000,
            (0, _) => 0x4000,
            (n, _) => n,
        };
        let unit = if c & 0x400 != 0 { 4 } else { 2 };
        self.copy(loc, CopyKind::Dma(d.channel), src, dst, count * unit);
    }

    fn copy(&mut self, site: Loc, kind: CopyKind, src: u32, dst: u32, len: u32) {
        if len == 0 || !Region::of(dst).is_ram() {
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
            bytes: Rc::from(bytes),
        });
    }

    fn swi(&mut self, loc: Loc, number: u8, st: &State) {
        self.out.swis.push(Swi { site: loc, number });
        self.swi_args(loc, number, [st.reg(0), st.reg(1), st.reg(2)]);
    }

    fn swi_args(&mut self, loc: Loc, number: u8, args: [Abs; 3]) {
        if !matches!(number, SWI_CPU_SET | SWI_CPU_FAST_SET) {
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
        if number == SWI_CPU_SET {
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
        for callee in self.targets(site, v as u32, thumb) {
            self.out.edges.push(Edge {
                site,
                target: callee,
                kind,
            });
            self.out.edge_psr.push(st.psr());
            self.constant_call(site, callee, st);
            let cs = self.env.summary(callee);
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
        let ram = Region::of(base).is_ram();
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
                let rom = addr >> 24 == 0x08 && self.env.image.in_image(addr);
                let in_ram =
                    Region::of(addr).is_ram() && !self.env.image.copies_at(addr).is_empty();
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
        for (addr, th) in es {
            for t in self.targets(site, addr, th) {
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
    fn constant_call(&mut self, site: Loc, callee: Loc, st: &State) {
        let args: Args = std::array::from_fn(|i| st.reg(i as u8));
        let ks: Vec<u32> = args.iter().filter_map(|a| a.k32()).collect();
        if !ks.iter().any(|a| Region::of(*a).is_ram())
            || !ks.iter().any(|a| self.env.image.is_source(*a))
        {
            return;
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
        for (dst, bytes) in got.iter().flat_map(|v| v.iter()) {
            if bytes.len() >= 4 {
                self.out.copies.push(RamCopy {
                    site,
                    kind: CopyKind::Run,
                    src: None,
                    dst: *dst,
                    bytes: Rc::from(bytes.as_slice()),
                });
            }
        }
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
                for t in self.targets(site, addr, thumb) {
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

    /// A transfer through the RAM word at `a`: every constant discovered
    /// code stores there. `thumb` is the target state, `None` when the
    /// transfer interworks.
    fn pointer(
        &mut self,
        site: Loc,
        a: u32,
        thumb: Option<bool>,
        st: &State,
        linked: bool,
    ) -> Summary {
        self.out.reads.insert(a);
        let vals: Vec<u32> = self
            .env
            .ptrs
            .get(&a)
            .map(|s| s.iter().copied().collect())
            .unwrap_or_default();
        if vals.is_empty() {
            self.unresolved(site, UnresolvedKind::PointerNoStore, format!("[{a:#010x}]"));
            if linked {
                self.resume(site, st, Summary::opaque());
            }
            return Summary::opaque();
        }
        let mut s = Summary::new(false, 0x7FFF);
        for v in vals {
            let th = thumb.unwrap_or(v & 1 == 1);
            let addr = if th { v & !1 } else { v & !3 };
            for t in self.targets(site, addr, th) {
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
        if linked {
            self.resume(site, st, s.clone());
        }
        s
    }

    fn jump(&mut self, site: Loc, enc: Encoding, t: Abs, out: &Out, link: bool) {
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
                own.via.get_or_insert((r, is_bx(enc)));
            }
            Abs::Tab { base, stride, n } if link => {
                let thumb = if is_bx(enc) { None } else { Some(site.thumb) };
                self.tab_transfer(site, (base, stride, n), thumb, st, link);
            }
            Abs::Tab { base, stride, n } => {
                let thumb = if is_bx(enc) { None } else { Some(site.thumb) };
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
                let thumb = if is_bx(enc) { None } else { st.thumb() };
                if thumb.is_none() && !is_bx(enc) {
                    self.unresolved(site, UnresolvedKind::UnknownState, format!("[{a:#010x}]"));
                    return;
                }
                self.pointer(site, a, thumb, st, link);
            }
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
        let cmp_at = Loc {
            addr: cmp_addr,
            ..site
        };
        let Some((r, max)) = self.compare(cmp_at) else {
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
        self.out.summary = Some(s);
        self.out
    }
}

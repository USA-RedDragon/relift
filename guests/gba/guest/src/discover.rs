//! A small discovery of a ROM's code for the build until
//! `relift-gba-analyze` lands: from the reset entry, the instructions each
//! function reaches, run over values that are either known constants or
//! unknown.
//!
//! Each instruction is lifted with `relift-cpu-arm32` and its IR evaluated
//! with the registers known along the path: constants, `ADR`, literal pool
//! loads from ROM and what the code computes from them. So `BX` to a known
//! register interworks, and a jump that leaves the return address in LR
//! (`MOV LR, PC; BX Rn`) is a call. Stores of known values to known
//! addresses are read back along the path, and each function starts with a
//! stack pointer of its own, so `PUSH {LR}` then `POP {PC}` is followed. A store of a known word to the IRQ
//! vector (`0x03007FFC`) makes the word an IRQ handler entry. Where paths
//! meet, values that differ become unknown. A transfer whose target or
//! state is not known ends its path and is listed; at run time that code
//! is missed code.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::fmt;

use relift_cpu_arm32::lift::{self, COND_FAIL};
use relift_cpu_arm32::regs;
use relift_ir::interp::{eval_bin, eval_cmp, eval_un};
use relift_ir::{Block, Endian, ExtKind, Flow, Next, Op, Width};

/// The cartridge entry.
pub const RESET: u32 = 0x0800_0000;

/// Where the BIOS keeps the user IRQ handler's address.
pub const IRQ_VECTOR: u32 = 0x0300_7FFC;

/// The stack pointer every function starts with: an address of no memory,
/// so pushes and pops of the function's frame are followed without taking
/// it for a real one.
const FRAME: u128 = 0x0F80_0000;

/// The return address every function starts with in LR: a jump there is a
/// return.
const RET: u32 = 0x0E00_0000;

/// The most bytes of RAM a path remembers.
const MEM_BYTES: usize = 1 << 16;

/// The most instructions of a leaf run in place at its calls.
const INLINE: usize = 24;

/// The most instructions a function's start runs on known values before
/// the paths are met (a copy loop runs to its end).
const CONCRETE: usize = 1 << 15;

/// The most instruction visits per function, against runaway joins.
const VISITS: usize = 1 << 16;

/// Where an instruction is: its address and CPU state.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Loc {
    /// The address.
    pub addr: u32,
    /// Whether it runs in Thumb state.
    pub thumb: bool,
}

impl Loc {
    /// The instruction size in bytes.
    #[must_use]
    pub const fn size(self) -> u32 {
        if self.thumb { 2 } else { 4 }
    }
}

impl fmt::Display for Loc {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}:{:08X}",
            if self.thumb { 'T' } else { 'A' },
            self.addr
        )
    }
}

/// One function: its entry and the instructions it reaches.
#[derive(Clone, Debug, Default)]
pub struct Function {
    /// The instructions.
    pub instructions: BTreeSet<Loc>,
    /// The instructions whose computed jump is a call (LR holds the next
    /// instruction's address).
    pub calls: BTreeSet<Loc>,
    /// The known targets of computed jumps inside the function, by the
    /// address of the jump.
    pub jumps: BTreeMap<u32, BTreeSet<u32>>,
    /// Whether the function may return (a jump to its entry LR, or a
    /// computed jump the discovery cannot follow).
    pub returns: bool,
    /// Whether it makes a direct call.
    pub calls_out: bool,
}

/// A transfer the discovery could not follow.
#[derive(Clone, Debug)]
pub struct Unresolved {
    /// The instruction.
    pub at: Loc,
    /// Why.
    pub why: &'static str,
}

/// What the discovery found.
#[derive(Clone, Debug, Default)]
pub struct Report {
    /// The functions by entry.
    pub functions: BTreeMap<Loc, Function>,
    /// The IRQ handlers stored to the vector.
    pub irq_handlers: BTreeSet<Loc>,
    /// Transfers not followed.
    pub unresolved: Vec<Unresolved>,
    /// Instructions that do not lift, with the reason.
    pub failed: BTreeMap<Loc, String>,
    /// The bytes of work RAM that code runs from, as the ROM's copies left
    /// them where the path transferred there.
    pub ram: BTreeMap<u32, u8>,
}

impl Report {
    /// Every instruction of every function, once.
    #[must_use]
    pub fn instructions(&self) -> BTreeSet<Loc> {
        self.functions
            .values()
            .flat_map(|f| f.instructions.iter().copied())
            .collect()
    }
}

/// A value whose bits in `k` are known to be those of `v`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
struct Kv {
    v: u128,
    k: u128,
}

impl Kv {
    const UNKNOWN: Kv = Kv { v: 0, k: 0 };

    fn known(v: u128) -> Kv {
        Kv { v, k: u128::MAX }
    }

    fn of(x: Option<u128>) -> Kv {
        x.map_or(Kv::UNKNOWN, Kv::known)
    }

    /// The value, when every bit of width `w` is known.
    fn get(self, w: Width) -> Option<u128> {
        (self.k & w.mask() == w.mask()).then_some(self.v & w.mask())
    }

    fn masked(self, m: u128) -> Kv {
        Kv {
            v: self.v & m,
            k: self.k | !m,
        }
    }
}

type Regs = Vec<Kv>;

/// Bytes of RAM the path stored known values to.
type Mem = BTreeMap<u32, u8>;

/// Whether a load at `a` reads back what was stored there: work RAM and
/// the function's own frame, not I/O or the cartridge's backup.
fn remembers(a: u32) -> bool {
    matches!(a >> 24, 0x02 | 0x03) || a >> 24 == (FRAME >> 24) as u32
}

fn meet_mem(a: &mut Mem, b: &Mem) -> bool {
    let n = a.len();
    a.retain(|k, v| b.get(k) == Some(v));
    a.len() != n
}

fn unknown() -> Regs {
    vec![Kv::UNKNOWN; regs::regs().len()]
}

fn meet(a: &mut Regs, b: &Regs) -> bool {
    let mut changed = false;
    for (x, y) in a.iter_mut().zip(b) {
        let k = x.k & y.k & !(x.v ^ y.v);
        if k != x.k {
            x.k = k;
            x.v &= k;
            changed = true;
        }
    }
    changed
}

/// The ROM, mirrored at the three Game Pak windows.
struct Rom<'a>(&'a [u8]);

impl Rom<'_> {
    fn read(&self, addr: u32, n: usize) -> Option<u128> {
        if !(0x08..=0x0D).contains(&(addr >> 24)) {
            return None;
        }
        let off = (addr & 0x01FF_FFFF) as usize;
        let b = self.0.get(off..off + n)?;
        Some(b.iter().rev().fold(0u128, |v, &x| (v << 8) | u128::from(x)))
    }

    fn unit(&self, at: Loc) -> Option<u32> {
        self.read(at.addr, at.size() as usize).map(|v| v as u32)
    }
}

/// The outcome of one instruction over known values.
struct Step {
    /// The state after it when it runs.
    out: Regs,
    /// Whether its condition may fail (it may do nothing).
    may_skip: bool,
    /// Whether it may run.
    may_run: bool,
    /// The exit's possible targets, `None` for an unknown one.
    targets: Vec<Option<u32>>,
    /// Known word stores.
    stores: Vec<(u32, u32)>,
    /// RAM after it.
    mem: Mem,
}

fn eval(b: &Block, rom: &Rom<'_>, inp: &Regs, mem_in: &Mem) -> Step {
    use relift_ir::BinOp;
    let mut vals: Vec<Kv> = Vec::with_capacity(b.insts.len());
    let mut writes: Vec<(usize, Kv)> = Vec::new();
    let mut stores = Vec::new();
    let mut mem = mem_in.clone();
    let mut may_skip = false;
    let mut may_run = true;
    let width = |x: relift_ir::Value| b.width_of(x).unwrap_or(Width::W1);
    for inst in &b.insts {
        let w = inst.width.unwrap_or(Width::W1);
        let m = w.mask();
        let v = |x: relift_ir::Value| vals[x.index()];
        let g = |x: relift_ir::Value| vals[x.index()].get(width(x));
        let r = match &inst.op {
            Op::Const(c) => Kv::known(c.get()),
            Op::Get(reg) => inp[reg.index()].masked(m),
            Op::Set(reg, x) => {
                writes.push((reg.index(), v(*x)));
                Kv::UNKNOWN
            }
            Op::Un(op, a) => Kv::of(g(*a).map(|a| eval_un(*op, a, w))),
            Op::Bin(op, x, y) => {
                let (a, c) = (v(*x), v(*y));
                match (op, g(*x), g(*y)) {
                    (_, Some(p), Some(q)) => Kv::known(eval_bin(*op, p, q, w)),
                    (BinOp::And, ..) => {
                        let k = (a.k & c.k) | (a.k & !a.v) | (c.k & !c.v);
                        Kv {
                            v: a.v & c.v & k,
                            k,
                        }
                        .masked(m)
                    }
                    (BinOp::Or, ..) => {
                        let k = (a.k & c.k) | (a.k & a.v) | (c.k & c.v);
                        Kv {
                            v: (a.v | c.v) & k,
                            k,
                        }
                        .masked(m)
                    }
                    (BinOp::Xor, ..) => {
                        let k = a.k & c.k;
                        Kv {
                            v: (a.v ^ c.v) & k,
                            k,
                        }
                        .masked(m)
                    }
                    (BinOp::Shl, _, Some(n)) if n < u128::from(w.bits()) => Kv {
                        v: a.v << n,
                        k: (a.k << n) | ((1u128 << n) - 1),
                    }
                    .masked(m),
                    (BinOp::LShr, _, Some(n)) if n < u128::from(w.bits()) => Kv {
                        v: (a.v & m) >> n,
                        k: ((a.k & m) >> n) | !(m >> n),
                    }
                    .masked(m),
                    _ => Kv::UNKNOWN,
                }
            }
            Op::Cmp(op, x, y) => {
                let aw = width(*x);
                Kv::of(match (g(*x), g(*y)) {
                    (Some(a), Some(c)) => Some(u128::from(eval_cmp(*op, a, c, aw))),
                    _ => None,
                })
            }
            Op::Select(c, x, y) => match g(*c) {
                Some(0) => v(*y),
                Some(_) => v(*x),
                None => {
                    let (a, d) = (v(*x), v(*y));
                    let k = a.k & d.k & !(a.v ^ d.v);
                    Kv { v: a.v & k, k }
                }
            },
            Op::Ext(kind, a) => {
                let aw = width(*a);
                match kind {
                    ExtKind::Zero => v(*a).masked(aw.mask()).masked(m),
                    ExtKind::Sign => Kv::of(g(*a).map(|a| {
                        let sh = 128 - aw.bits();
                        ((((a << sh) as i128) >> sh) as u128) & m
                    })),
                }
            }
            Op::Extract(a, lo) => {
                let x = v(*a);
                Kv {
                    v: x.v >> lo,
                    k: x.k >> lo,
                }
                .masked(m)
            }
            Op::Concat(h, l) => {
                let lw = width(*l);
                let (hv, lv) = (v(*h).masked(width(*h).mask()), v(*l).masked(lw.mask()));
                Kv {
                    v: (hv.v << lw.bits()) | (lv.v & lw.mask()),
                    k: (hv.k << lw.bits()) | (lv.k & lw.mask()),
                }
                .masked(m)
            }
            Op::Load { addr, endian, .. } | Op::IoLoad { addr, endian, .. } => {
                let n = w.bits() / 8;
                Kv::of(
                    g(*addr)
                        .and_then(|a| {
                            let a = a as u32;
                            (0..n)
                                .rev()
                                .map(|i| mem.get(&a.wrapping_add(i)).copied())
                                .try_fold(0u128, |acc, x| x.map(|x| (acc << 8) | u128::from(x)))
                                .or_else(|| rom.read(a, n as usize))
                        })
                        .filter(|_| *endian == Endian::Little),
                )
            }
            Op::Store { addr, value, .. } | Op::IoStore { addr, value, .. } => {
                let n = width(*value).bits() / 8;
                match (g(*addr), g(*value)) {
                    (Some(a), Some(x)) => {
                        let a = a as u32;
                        if n == 4 {
                            stores.push((a, x as u32));
                        }
                        for i in 0..n {
                            if remembers(a) && mem.len() < MEM_BYTES {
                                mem.insert(a.wrapping_add(i), (x >> (8 * i)) as u8);
                            }
                        }
                    }
                    (Some(a), None) => {
                        for i in 0..n {
                            mem.remove(&(a as u32).wrapping_add(i));
                        }
                    }
                    (None, _) => mem.clear(),
                }
                Kv::UNKNOWN
            }
            Op::Guard(c, code) if *code == COND_FAIL => {
                match g(*c) {
                    Some(0) => {}
                    Some(_) => may_run = false,
                    None => may_skip = true,
                }
                Kv::UNKNOWN
            }
            _ => Kv::UNKNOWN,
        };
        vals.push(r);
    }
    if !may_run {
        may_skip = true;
    }
    let mut out = inp.clone();
    for (i, x) in writes {
        out[i] = x;
    }
    let g = |x: relift_ir::Value| vals[x.index()].get(width(x));
    let targets = match b.exit.next {
        Next::Const(t) => vec![Some(t as u32)],
        Next::Select {
            cond,
            taken,
            not_taken,
        } => match g(cond) {
            Some(0) => vec![Some(not_taken as u32)],
            Some(_) => vec![Some(taken as u32)],
            None => vec![Some(taken as u32), Some(not_taken as u32)],
        },
        Next::Dynamic(x) => vec![g(x).map(|t| t as u32)],
    };
    Step {
        out,
        may_skip,
        may_run,
        targets,
        stores,
        mem,
    }
}

/// The known value of slot `r`.
fn reg(r: &Regs, x: relift_ir::Reg, w: Width) -> Option<u128> {
    r[x.index()].get(w)
}

/// The modes the CPU can be in.
const MODES: [u128; 7] = [0x10, 0x11, 0x12, 0x13, 0x17, 0x1B, 0x1F];

/// `b` run once per mode for a state whose mode is unknown, when every run
/// knows the exit: the targets of all runs, and the state where the runs
/// agree.
fn by_mode(b: &Block, rom: &Rom<'_>, inp: &Regs, mem: &Mem) -> Option<Step> {
    let mut all: Option<Step> = None;
    for m in MODES {
        let mut r = inp.clone();
        r[regs::M.index()] = Kv::known(m);
        let mut s = eval(b, rom, &r, mem);
        if s.targets.contains(&None) || s.may_skip {
            return None;
        }
        s.out[regs::M.index()] = Kv::UNKNOWN;
        match &mut all {
            None => all = Some(s),
            Some(a) => {
                meet(&mut a.out, &s.out);
                meet_mem(&mut a.mem, &s.mem);
                for t in s.targets {
                    if !a.targets.contains(&t) {
                        a.targets.push(t);
                    }
                }
            }
        }
    }
    all
}

/// The copy or fill of `CpuSet` (0x0B) and `CpuFastSet` (0x0C) with known
/// operands, into the path's RAM.
fn bios_copy(rom: &Rom<'_>, number: u32, r: &Regs, mem: &mut Mem) {
    if number != 0x0B && number != 0x0C {
        return;
    }
    let get = |n: u32| reg(r, regs::r(n), Width::W32).map(|v| v as u32);
    let (Some(src), Some(dst), Some(ctl)) = (get(0), get(1), get(2)) else {
        if let Some(d) = get(1) {
            mem.retain(|a, _| *a < d);
        } else {
            mem.clear();
        }
        return;
    };
    let fill = ctl & (1 << 24) != 0;
    let (unit, count) = if number == 0x0C {
        (4u32, (ctl & 0x1F_FFFF).div_ceil(8) * 8)
    } else {
        (if ctl & (1 << 26) != 0 { 4 } else { 2 }, ctl & 0x1F_FFFF)
    };
    let (src, dst) = (src & !(unit - 1), dst & !(unit - 1));
    let byte = |a: u32, mem: &Mem| {
        mem.get(&a)
            .copied()
            .or_else(|| rom.read(a, 1).map(|v| v as u8))
    };
    for i in 0..count.min(0x10000) * unit {
        let from = if fill { src + i % unit } else { src + i };
        let to = dst + i;
        match byte(from, mem) {
            Some(b) if remembers(to) && mem.len() < MEM_BYTES => {
                mem.insert(to, b);
            }
            _ => {
                mem.remove(&to);
            }
        }
    }
}

/// The registers a call leaves unknown (the AAPCS scratch registers, the
/// flags and LR).
fn after_call(r: &mut Regs) {
    for n in [0, 1, 2, 3, 12, 14] {
        r[regs::r(n).index()] = Kv::UNKNOWN;
    }
    for f in [regs::N, regs::Z, regs::C, regs::V] {
        r[f.index()] = Kv::UNKNOWN;
    }
}

/// The CPU mode and masks an entry starts with, when known.
type Psr = [Kv; 3];

const PSR: [relift_ir::Reg; 3] = [regs::M, regs::I, regs::F];

fn psr(r: &Regs) -> Psr {
    PSR.map(|x| r[x.index()])
}

struct Walk<'a> {
    rom: Rom<'a>,
    entries: VecDeque<(Loc, Psr)>,
    busy: BTreeSet<Loc>,
    report: Report,
}

impl Walk<'_> {
    fn entry(&mut self, at: Loc, p: Psr) {
        if !self.report.functions.contains_key(&at) && !self.entries.iter().any(|e| e.0 == at) {
            self.entries.push_back((at, p));
        }
    }

    /// Whether a call of `at` returns, discovering it first when needed (a
    /// call into a function being discovered is taken to return).
    fn call(&mut self, at: Loc, p: Psr) -> bool {
        if let Some(f) = self.report.functions.get(&at) {
            return f.returns;
        }
        if self.busy.contains(&at) {
            return true;
        }
        self.busy.insert(at);
        let f = self.function(at, p);
        self.busy.remove(&at);
        let r = f.returns;
        self.report.functions.insert(at, f);
        r
    }

    /// The instruction unit at `at`: from the ROM, else from RAM the path
    /// stored known bytes to, else from the RAM image copies left.
    fn code(&self, at: Loc, mem: &Mem) -> Option<u32> {
        if let Some(w) = self.rom.unit(at) {
            return Some(w);
        }
        (0..at.size())
            .rev()
            .map(|i| {
                let a = at.addr.wrapping_add(i);
                mem.get(&a).or_else(|| self.report.ram.get(&a)).copied()
            })
            .try_fold(0u32, |acc, b| b.map(|b| (acc << 8) | u32::from(b)))
    }

    /// Keeps the work RAM `mem` holds as code that runs from RAM, for an
    /// entry the path transfers to there.
    fn keep_ram(&mut self, to: u32, mem: &Mem) {
        if !matches!(to >> 24, 0x02 | 0x03) {
            return;
        }
        for (&a, &v) in mem {
            if matches!(a >> 24, 0x02 | 0x03) {
                self.report.ram.entry(a).or_insert(v);
            }
        }
    }

    /// The entry `t` of a call or vector in the state `thumb`, made a
    /// function: whether it returns.
    fn callee(
        &mut self,
        at: Loc,
        t: Option<u32>,
        thumb: Option<bool>,
        out: &Regs,
        mem: &Mem,
    ) -> bool {
        match (t, thumb) {
            (Some(t), Some(th)) => {
                self.keep_ram(t, mem);
                self.call(
                    Loc {
                        addr: t & !1,
                        thumb: th,
                    },
                    psr(out),
                )
            }
            _ => {
                self.report.unresolved.push(Unresolved {
                    at,
                    why: "a call to an unknown target",
                });
                true
            }
        }
    }

    /// The state after a call of `callee` from `out`: the callee run in
    /// place when it is a small leaf (a SWI wrapper, an accessor), else the
    /// state a call leaves.
    fn after(&mut self, callee: Option<Loc>, out: &Regs, mem: &Mem) -> (Regs, Mem) {
        if let Some(c) = callee
            && let Some(f) = self.report.functions.get(&c)
            && f.calls.is_empty()
            && !f.calls_out
            && f.instructions.len() <= INLINE
            && let Some(r) = self.inline(c, out, mem)
        {
            return r;
        }
        let mut o = out.clone();
        after_call(&mut o);
        (o, Mem::new())
    }

    /// Runs the leaf at `entry` from the caller's state; the state at its
    /// returns, met.
    fn inline(&mut self, entry: Loc, out: &Regs, mem: &Mem) -> Option<(Regs, Mem)> {
        let mut init = out.clone();
        init[regs::r(14).index()] = Kv::known(u128::from(RET));
        init[regs::T.index()] = Kv::known(u128::from(entry.thumb));
        let mut scratch = Function::default();
        let mut state: BTreeMap<Loc, (Regs, Mem)> = BTreeMap::from([(entry, (init, mem.clone()))]);
        let mut work = VecDeque::from([entry]);
        let mut back: Option<(Regs, Mem)> = None;
        let mut visits = 0;
        while let Some(at) = work.pop_front() {
            visits += 1;
            if visits > 4 * INLINE {
                return None;
            }
            let (inp, min) = state[&at].clone();
            for (to, r, m) in self.step(&mut scratch, at, &inp, &min, true) {
                if to.addr == RET {
                    match &mut back {
                        None => back = Some((r, m)),
                        Some((br, bm)) => {
                            meet(br, &r);
                            meet_mem(bm, &m);
                        }
                    }
                    continue;
                }
                match state.get_mut(&to) {
                    None => {
                        state.insert(to, (r, m));
                        work.push_back(to);
                    }
                    Some((old, old_mem)) => {
                        let a = meet(old, &r);
                        if meet_mem(old_mem, &m) || a {
                            work.push_back(to);
                        }
                    }
                }
            }
        }
        back.map(|(mut r, m)| {
            r[regs::r(14).index()] = Kv::UNKNOWN;
            r[regs::T.index()] = out[regs::T.index()];
            (r, m)
        })
    }

    /// The successors of the instruction at `at` from the state `inp`,
    /// `min`; a return goes to [`RET`]. `quiet` reports nothing (a leaf run
    /// in place).
    fn step(
        &mut self,
        f: &mut Function,
        at: Loc,
        inp: &Regs,
        min: &Mem,
        quiet: bool,
    ) -> Vec<(Loc, Regs, Mem)> {
        let mut next: Vec<(Loc, Regs, Mem)> = Vec::new();
        let Some(word) = self.code(at, min) else {
            if !quiet {
                self.report.unresolved.push(Unresolved {
                    at,
                    why: "the code is outside the ROM and the RAM copies",
                });
            }
            return next;
        };
        f.instructions.insert(at);
        let l = match lift::lift(word, at.addr, at.thumb) {
            Ok(l) => l,
            Err(e) => {
                if !quiet {
                    self.report.failed.insert(at, e.to_string());
                }
                return next;
            }
        };
        let mut s = eval(&l.block, &self.rom, inp, min);
        if s.may_run && s.targets.contains(&None) && reg(inp, regs::M, Width::new(5)).is_none() {
            s = by_mode(&l.block, &self.rom, inp, min).unwrap_or(s);
        }
        for &(a, v) in &s.stores {
            if a == IRQ_VECTOR && !quiet {
                let h = Loc {
                    addr: v & !1,
                    thumb: v & 1 != 0,
                };
                self.keep_ram(h.addr, &s.mem);
                if self.report.irq_handlers.insert(h) {
                    self.entry(h, [Kv::known(0x12), Kv::known(1), Kv::UNKNOWN]);
                }
            }
        }
        let fall = at.addr.wrapping_add(at.size());
        if s.may_skip {
            next.push((Loc { addr: fall, ..at }, inp.clone(), min.clone()));
        }
        if !s.may_run {
            return next;
        }
        let thumb = reg(&s.out, regs::T, Width::W1).map(|t| t != 0);
        match l.block.exit.flow {
            Flow::Call { ret } if l.block.exit.next == Next::Const(0x08) => {
                let number = if at.thumb {
                    word & 0xFF
                } else {
                    (word >> 16) & 0xFF
                };
                let mut o = inp.clone();
                let mut m = min.clone();
                bios_copy(&self.rom, number, inp, &mut m);
                for n in 0..4 {
                    o[regs::r(n).index()] = Kv::UNKNOWN;
                }
                next.push((
                    Loc {
                        addr: ret as u32,
                        ..at
                    },
                    o,
                    m,
                ));
            }
            Flow::Call { ret } => {
                f.calls_out = true;
                let t = self.bl_pair(at, word).or(s.targets[0]);
                let back = quiet || self.callee(at, t, thumb, &s.out, &s.mem);
                let callee = t.zip(thumb).map(|(t, th)| Loc {
                    addr: t & !1,
                    thumb: th,
                });
                let (o, m) = if quiet {
                    let mut o = s.out.clone();
                    after_call(&mut o);
                    (o, Mem::new())
                } else {
                    self.after(callee, &s.out, &s.mem)
                };
                if back {
                    next.push((
                        Loc {
                            addr: ret as u32,
                            ..at
                        },
                        o,
                        m,
                    ));
                }
            }
            Flow::Jump | Flow::Return => {
                let lr = reg(&s.out, regs::r(14), Width::W32).map(|v| v as u32 & !1);
                let dynamic = matches!(l.block.exit.next, Next::Dynamic(_));
                if dynamic && lr == Some(fall) {
                    f.calls.insert(at);
                    let back = quiet || self.callee(at, s.targets[0], thumb, &s.out, &s.mem);
                    let mut o = s.out.clone();
                    after_call(&mut o);
                    o[regs::T.index()] = Kv::known(u128::from(at.thumb));
                    if back {
                        next.push((Loc { addr: fall, ..at }, o, Mem::new()));
                    }
                    return next;
                }
                for t in &s.targets {
                    if *t == Some(RET) {
                        f.returns = true;
                        next.push((
                            Loc {
                                addr: RET,
                                thumb: false,
                            },
                            s.out.clone(),
                            s.mem.clone(),
                        ));
                        continue;
                    }
                    if let (true, Some(t)) = (dynamic, t) {
                        f.jumps.entry(at.addr).or_default().insert(t & !1);
                    }
                    match (t, thumb) {
                        (Some(t), Some(th)) => {
                            if !quiet && (at.addr >> 24) != (t >> 24) {
                                self.keep_ram(*t, &s.mem);
                            }
                            next.push((
                                Loc {
                                    addr: t & !1,
                                    thumb: th,
                                },
                                s.out.clone(),
                                s.mem.clone(),
                            ));
                        }
                        (None, _) if dynamic => {
                            f.returns = true;
                            if !quiet {
                                self.report.unresolved.push(Unresolved {
                                    at,
                                    why: "a computed jump (a return, or missed code)",
                                });
                            }
                        }
                        _ => {
                            if !quiet {
                                self.report.unresolved.push(Unresolved {
                                    at,
                                    why: "a jump in an unknown state",
                                });
                            }
                        }
                    }
                }
            }
            Flow::Trap(_) => {
                next.push((Loc { addr: fall, ..at }, s.out.clone(), s.mem.clone()));
            }
        }
        next
    }

    fn function(&mut self, entry: Loc, p: Psr) -> Function {
        let mut f = Function::default();
        let mut init = unknown();
        init[regs::T.index()] = Kv::known(u128::from(entry.thumb));
        init[regs::r(13).index()] = Kv::known(FRAME);
        init[regs::r(14).index()] = Kv::known(u128::from(RET));
        for (r, v) in PSR.iter().zip(p) {
            init[r.index()] = v;
        }
        let (mut at, mut regs_now, mut mem_now) = (entry, init, Mem::new());
        let mut seen: BTreeSet<(Loc, u64)> = BTreeSet::new();
        for _ in 0..CONCRETE {
            let mut h = std::hash::DefaultHasher::new();
            std::hash::Hash::hash(&regs_now, &mut h);
            if !seen.insert((at, std::hash::Hasher::finish(&h))) {
                break;
            }
            let mut n = self.step(&mut f, at, &regs_now, &mem_now, false);
            if n.len() != 1 || n[0].0.addr == RET {
                break;
            }
            let (to, r, m) = n.remove(0);
            (at, regs_now, mem_now) = (to, r, m);
        }
        let mut state: BTreeMap<Loc, (Regs, Mem)> = BTreeMap::from([(at, (regs_now, mem_now))]);
        let mut work = VecDeque::from([at]);
        let mut visits = 0;
        while let Some(at) = work.pop_front() {
            visits += 1;
            if visits > VISITS {
                self.report.unresolved.push(Unresolved {
                    at,
                    why: "the function did not settle",
                });
                break;
            }
            let (inp, min) = state[&at].clone();
            for (to, r, m) in self.step(&mut f, at, &inp, &min, false) {
                if to.addr == RET {
                    continue;
                }
                match state.get_mut(&to) {
                    None => {
                        state.insert(to, (r, m));
                        work.push_back(to);
                    }
                    Some((old, old_mem)) => {
                        let a = meet(old, &r);
                        if meet_mem(old_mem, &m) || a {
                            work.push_back(to);
                        }
                    }
                }
            }
        }
        f
    }

    fn bl_pair(&self, at: Loc, word: u32) -> Option<u32> {
        if !at.thumb {
            return None;
        }
        let hi = self.rom.unit(Loc {
            addr: at.addr.wrapping_sub(2),
            thumb: true,
        })?;
        crate::frontend::bl_pair_target(hi, word, at.addr)
    }
}

/// Discovers the code of `rom` from its reset entry.
#[must_use]
pub fn discover(rom: &[u8]) -> Report {
    let mut w = Walk {
        rom: Rom(rom),
        entries: VecDeque::from([(
            Loc {
                addr: RESET,
                thumb: false,
            },
            [Kv::known(0x1F), Kv::known(0), Kv::known(0)],
        )]),
        busy: BTreeSet::new(),
        report: Report::default(),
    };
    while let Some((e, p)) = w.entries.pop_front() {
        w.call(e, p);
    }
    w.report
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rom(words: &[(u32, &[u8])]) -> Vec<u8> {
        let mut r = vec![0u8; 0x400];
        for (off, b) in words {
            r[*off as usize..*off as usize + b.len()].copy_from_slice(b);
        }
        r
    }

    fn arm(w: &[u32]) -> Vec<u8> {
        w.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    fn thumb(w: &[u16]) -> Vec<u8> {
        w.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    #[test]
    fn adr_and_bx_enter_thumb_code_and_bl_calls_a_function() {
        let r = rom(&[
            (0, &arm(&[0xE28F_0001, 0xE12F_FF10])),
            (0x8, &thumb(&[0xF000, 0xF803, 0xE7FE])),
            (0x12, &thumb(&[0x4770])),
        ]);
        let rep = discover(&r);
        let t = Loc {
            addr: RESET + 8,
            thumb: true,
        };
        let main = &rep.functions[&Loc {
            addr: RESET,
            thumb: false,
        }];
        assert!(main.instructions.contains(&t), "{rep:?}");
        assert_eq!(main.jumps[&(RESET + 4)], BTreeSet::from([RESET + 8]));
        assert!(main.instructions.contains(&Loc {
            addr: RESET + 0xC,
            thumb: true
        }));
        assert!(rep.functions.contains_key(&Loc {
            addr: RESET + 0x12,
            thumb: true
        }));
    }

    #[test]
    fn a_literal_pool_store_to_the_vector_is_an_irq_handler() {
        let r = rom(&[
            (
                0,
                &arm(&[
                    0xE59F_0008,
                    0xE59F_1008,
                    0xE581_0000,
                    0xEAFF_FFFE,
                    0x0800_0021,
                    0x0300_7FFC,
                ]),
            ),
            (0x20, &thumb(&[0x4770])),
        ]);
        let rep = discover(&r);
        let h = Loc {
            addr: RESET + 0x20,
            thumb: true,
        };
        assert!(rep.irq_handlers.contains(&h), "{rep:?}");
        assert!(rep.functions.contains_key(&h));
    }

    #[test]
    fn mov_lr_pc_then_bx_is_a_call() {
        let r = rom(&[
            (
                0,
                &arm(&[
                    0xE59F_100C,
                    0xE1A0_E00F,
                    0xE12F_FF11,
                    0xEAFF_FFFE,
                    0,
                    0x0800_0040,
                ]),
            ),
            (0x40, &arm(&[0xE12F_FF1E])),
        ]);
        let rep = discover(&r);
        let main = &rep.functions[&Loc {
            addr: RESET,
            thumb: false,
        }];
        assert!(main.calls.contains(&Loc {
            addr: RESET + 8,
            thumb: false
        }));
        assert!(main.instructions.contains(&Loc {
            addr: RESET + 0xC,
            thumb: false
        }));
        assert!(rep.functions.contains_key(&Loc {
            addr: RESET + 0x40,
            thumb: false
        }));
    }
}

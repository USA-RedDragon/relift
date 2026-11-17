//! What the runtime must know of a Switch 2 program's NVN2 driver (its SDK
//! module) to run the driver's own code for a texture's storage class.
//!
//! The runtime keeps its own device state and never runs the driver's
//! `nvnDeviceInitialize`, so the driver's device object holds none of the
//! fields `nvnTextureBuilderGetStorageClass` reads. This reads the driver
//! itself, without running it:
//!
//! * [`proc_table`] finds the driver's proc table (sorted names, their
//!   offsets into a string pool, and a pointer array) from the constants
//!   `nvnBootstrapLoaderInternal`'s lookup uses;
//! * [`device_layout`] runs the lifted code of the procs it needs over
//!   abstract values ([`Abstract`]): the device offsets the storage class
//!   getter reads, the one it hands `NvRmGpuDeviceChooseKind`, the flags
//!   word `nvnDeviceGetFlags` returns, and the constant the driver's
//!   `nvnDeviceInitialize` leaves in each other field it reads on the paths
//!   that return true.
//!
//! The abstract run assumes calls it cannot follow (through a register the
//! run does not know) write no memory it tracks; a field such a call could
//! set would read here as the constant before it.

use std::collections::{BTreeMap, HashMap, VecDeque};

use relift_cpu_arm64::frontend::Arm64;
use relift_guest::Frontend;
use relift_ir::interp::{eval_bin, eval_cmp, eval_un};
use relift_ir::{BinOp, Block, ExtKind, Flow, Next, Op, Reg, Width};
use relift_switch_formats::nso::Nso;

use crate::program::{Reloc, relocs};

/// Instructions one abstract run may step before it gives up.
const BUDGET: usize = 8_000_000;
/// Calls deep a run follows into the module's own functions.
const DEPTH: u32 = 4;
const X30: u32 = 30;
/// States kept apart at one place.
const PARTITIONS: usize = 8;
const SP: Reg = Reg(31);

/// A word the loader fills: a relative pointer (an image offset) or a symbol.
#[derive(Clone, Debug, PartialEq, Eq)]
enum Word {
    Value(u64),
    Import(String),
}

/// The driver module as the analysis reads it: its image, read-only extent,
/// relocated words and symbols, all as image offsets.
pub struct Image {
    img: Vec<u8>,
    rodata: u64,
    read_only: u64,
    words: HashMap<u64, Word>,
    symbols: HashMap<String, u64>,
    fe: Arm64,
    blocks: std::cell::RefCell<HashMap<u64, Option<std::rc::Rc<Block>>>>,
}

impl Image {
    /// The analysis view of `nso`, lifted for the CPU `fe`.
    ///
    /// # Errors
    ///
    /// No MOD0 or unreadable symbols.
    pub fn new(nso: &Nso, fe: Arm64) -> Result<Image, String> {
        let m = nso
            .mod0()
            .map_err(|e| e.to_string())?
            .ok_or("nvn driver: the module has no MOD0")?;
        let syms = nso.symbols().map_err(|e| e.to_string())?;
        let mut words = HashMap::new();
        for r in relocs(&nso.image, m.dynamic as usize) {
            match r {
                Reloc::Value { at, value } => {
                    words.insert(at, Word::Value(value));
                }
                Reloc::Symbol { at, sym, kind, .. } => {
                    if let Some(s) = sym.checked_sub(1).and_then(|k| syms.get(k)) {
                        let w = if s.is_defined() && kind != crate::program::SlotKind::Plt {
                            Word::Value(s.value)
                        } else {
                            Word::Import(s.name.clone())
                        };
                        words.insert(at, w);
                    }
                }
            }
        }
        let rodata = &nso.segments[1];
        Ok(Image {
            img: nso.image.clone(),
            rodata: u64::from(rodata.memory_offset),
            read_only: u64::from(rodata.memory_offset) + u64::from(rodata.size),
            words,
            symbols: syms
                .iter()
                .filter(|s| s.is_defined())
                .map(|s| (s.name.clone(), s.value))
                .collect(),
            fe,
            blocks: Default::default(),
        })
    }

    fn u32_at(&self, o: u64) -> Option<u32> {
        let o = usize::try_from(o).ok()?;
        Some(u32::from_le_bytes(self.img.get(o..o + 4)?.try_into().ok()?))
    }

    fn cstr(&self, o: u64) -> Option<&str> {
        let o = usize::try_from(o).ok()?;
        let rest = self.img.get(o..)?;
        let end = rest.iter().position(|&b| b == 0)?;
        std::str::from_utf8(&rest[..end]).ok()
    }

    /// The printable string a pointer into .rodata points to the start of.
    fn rodata_str(&self, o: u64) -> Option<&str> {
        if o <= self.rodata || o >= self.read_only || self.img.get(o as usize - 1) != Some(&0) {
            return None;
        }
        self.cstr(o)
            .filter(|t| t.len() >= 2 && t.bytes().all(|c| (0x20..0x7f).contains(&c)))
    }

    fn block(&self, pc: u64) -> Option<std::rc::Rc<Block>> {
        if let Some(b) = self.blocks.borrow().get(&pc) {
            return b.clone();
        }
        let b = usize::try_from(pc)
            .ok()
            .and_then(|o| self.img.get(o..))
            .filter(|_| pc < self.read_only)
            .and_then(|bytes| self.fe.lift(bytes, pc).ok())
            .map(std::rc::Rc::new);
        self.blocks.borrow_mut().insert(pc, b.clone());
        b
    }

    /// The constant a load of `bytes` at image offset `at` gives: read-only
    /// bytes, or a relocated word.
    fn load_const(&self, at: u64, bytes: u32) -> Option<V> {
        if let Some(w) = self.words.get(&at).cloned() {
            return (bytes == 8).then(|| match w {
                Word::Value(v) => V::K(u128::from(v)),
                Word::Import(n) => V::Import(n),
            });
        }
        if at.checked_add(u64::from(bytes))? > self.read_only {
            return None;
        }
        let o = usize::try_from(at).ok()?;
        let b = self.img.get(o..o + bytes as usize)?;
        let mut v = 0u128;
        for (i, x) in b.iter().enumerate() {
            v |= u128::from(*x) << (8 * i);
        }
        Some(V::K(v))
    }
}

/// An object a pointer points into.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Obj {
    /// What argument register `n` pointed to at the entry of the run.
    Arg(u32),
    /// The stack.
    Stack,
    /// What the pointer at byte `off` of an object pointed to.
    At(Box<Obj>, i64),
}

/// An abstract value.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum V {
    /// A constant.
    K(u128),
    /// A pointer into an object.
    P(Obj, i64),
    /// The address of an imported symbol.
    Import(String),
    /// A constant plus an index the run does not know.
    Idx(u64),
    /// The word the program keeps at this image offset in writable memory
    /// (a hook the program installs at run time).
    Slot(u64),
    /// An entry of a table at `tab` of `elem`-byte entries (sign-extended
    /// when `sign`), shifted left by `shift`, plus `base`: a jump table's
    /// target.
    Tab {
        tab: u64,
        elem: u32,
        sign: bool,
        shift: u32,
        base: u64,
    },
    /// Anything.
    U,
}

/// A run of bytes in an object.
#[derive(Clone, Debug, PartialEq, Eq)]
enum Cell {
    Bytes(Vec<u8>),
    Val(V, u32),
}

impl Cell {
    fn len(&self) -> u32 {
        match self {
            Cell::Bytes(b) => b.len() as u32,
            Cell::Val(_, n) => *n,
        }
    }
}

/// The bytes an object holds, by offset; untouched bytes hold what they
/// held at the entry of the run (unknown for the stack).
type Mem = BTreeMap<i64, Cell>;

/// An abstract machine state.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct State {
    regs: Vec<V>,
    mem: BTreeMap<Obj, Mem>,
}

fn carve(m: &mut Mem, off: i64, len: u32) {
    let end = off + i64::from(len);
    let keys: Vec<i64> = m
        .range(..end)
        .filter(|(k, c)| **k + i64::from(c.len()) > off)
        .map(|(k, _)| *k)
        .collect();
    for k in keys {
        let c = m.remove(&k).unwrap_or(Cell::Val(V::U, 0));
        let cend = k + i64::from(c.len());
        match c {
            Cell::Bytes(b) => {
                if k < off {
                    m.insert(k, Cell::Bytes(b[..(off - k) as usize].to_vec()));
                }
                if cend > end {
                    m.insert(end, Cell::Bytes(b[(end - k) as usize..].to_vec()));
                }
            }
            Cell::Val(..) => {
                if k < off {
                    m.insert(k, Cell::Val(V::U, (off - k) as u32));
                }
                if cend > end {
                    m.insert(end, Cell::Val(V::U, (cend - end) as u32));
                }
            }
        }
    }
}

fn mem_store(m: &mut Mem, off: i64, len: u32, v: &V) {
    carve(m, off, len);
    let cell = match v {
        V::K(k) => Cell::Bytes((0..len).map(|i| (k >> (8 * i)) as u8).collect()),
        _ => Cell::Val(v.clone(), len),
    };
    m.insert(off, cell);
}

/// What a load of `len` bytes at `off` reads: `None` when nothing stored
/// touches the bytes (they hold the entry content).
fn mem_load(m: &Mem, off: i64, len: u32) -> Option<V> {
    let end = off + i64::from(len);
    let touching: Vec<(&i64, &Cell)> = m
        .range(..end)
        .filter(|(k, c)| **k + i64::from(c.len()) > off)
        .collect();
    if touching.is_empty() {
        return None;
    }
    if let [(k, Cell::Val(v, n))] = touching[..]
        && *k == off
        && *n == len
    {
        return Some(v.clone());
    }
    let mut v = 0u128;
    let mut covered = 0;
    for (k, c) in touching {
        let Cell::Bytes(b) = c else {
            return Some(V::U);
        };
        for (i, x) in b.iter().enumerate() {
            let at = k + i as i64;
            if at >= off && at < end && len <= 16 {
                v |= u128::from(*x) << (8 * (at - off));
                covered += 1;
            }
        }
    }
    Some(if covered == len { V::K(v) } else { V::U })
}

fn merge_mem(a: &Mem, b: &Mem) -> Mem {
    if a == b {
        return a.clone();
    }
    let mut cuts: Vec<i64> = Vec::new();
    for (k, c) in a.iter().chain(b.iter()) {
        cuts.push(*k);
        cuts.push(k + i64::from(c.len()));
    }
    cuts.sort_unstable();
    cuts.dedup();
    let piece = |m: &Mem, lo: i64, hi: i64| -> Option<Cell> {
        let (k, c) = m.range(..hi).next_back()?;
        if k + i64::from(c.len()) <= lo {
            return None;
        }
        Some(match c {
            Cell::Bytes(bytes) => Cell::Bytes(bytes[(lo - k) as usize..(hi - k) as usize].to_vec()),
            Cell::Val(v, n) if *k == lo && i64::from(*n) == hi - lo => Cell::Val(v.clone(), *n),
            Cell::Val(..) => Cell::Val(V::U, (hi - lo) as u32),
        })
    };
    let mut out = Mem::new();
    for w in cuts.windows(2) {
        let (lo, hi) = (w[0], w[1]);
        match (piece(a, lo, hi), piece(b, lo, hi)) {
            (None, None) => {}
            (Some(x), Some(y)) if x == y => {
                out.insert(lo, x);
            }
            _ => {
                out.insert(lo, Cell::Val(V::U, (hi - lo) as u32));
            }
        }
    }
    out
}

impl State {
    /// The state at a function's entry: argument registers point into
    /// [`Obj::Arg`] objects, the stack pointer to the stack, the rest
    /// unknown.
    pub fn entry(nregs: usize) -> State {
        let mut regs = vec![V::U; nregs];
        for n in 0..8 {
            regs[n as usize] = V::P(Obj::Arg(n), 0);
        }
        regs[SP.index()] = V::P(Obj::Stack, 0);
        State {
            regs,
            mem: BTreeMap::new(),
        }
    }

    /// Whether no register holds a different constant in `o`.
    fn agrees(&self, o: &State) -> bool {
        self.regs
            .iter()
            .zip(&o.regs)
            .all(|(a, b)| !matches!((a, b), (V::K(x), V::K(y)) if x != y))
    }

    fn merge(&self, o: &State) -> State {
        let regs = self
            .regs
            .iter()
            .zip(&o.regs)
            .map(|(a, b)| if a == b { a.clone() } else { V::U })
            .collect();
        let mut mem = BTreeMap::new();
        for k in self.mem.keys().chain(o.mem.keys()) {
            if mem.contains_key(k) {
                continue;
            }
            let empty = Mem::new();
            let a = self.mem.get(k).unwrap_or(&empty);
            let b = o.mem.get(k).unwrap_or(&empty);
            mem.insert(k.clone(), merge_mem(a, b));
        }
        State { regs, mem }
    }

    /// The value of register `r`.
    pub fn reg(&self, r: u32) -> &V {
        &self.regs[r as usize]
    }

    /// What a load of `len` bytes at byte `off` of `obj` gives.
    pub fn load(&self, obj: &Obj, off: i64, len: u32) -> V {
        if let Some(v) = self.mem.get(obj).and_then(|m| mem_load(m, off, len)) {
            return v;
        }
        match obj {
            Obj::Stack => V::U,
            _ => V::P(Obj::At(Box::new(obj.clone()), off), 0),
        }
    }

    fn store(&mut self, obj: &Obj, off: i64, len: u32, v: &V) {
        mem_store(self.mem.entry(obj.clone()).or_default(), off, len, v);
    }

    fn clobber_call(&mut self) {
        for (i, r) in self.regs.iter_mut().enumerate() {
            let keep = (19..=29).contains(&i) || i == SP.index();
            if !keep {
                *r = V::U;
            }
        }
    }
}

/// A memory access of a run: the object, offset and bytes.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Access {
    /// The object.
    pub obj: Obj,
    /// Its offset.
    pub off: i64,
    /// The bytes.
    pub len: u32,
}

/// What an abstract run saw.
#[derive(Default)]
pub struct Trace {
    /// Loads from objects.
    pub loads: Vec<Access>,
    /// Calls to imports: the name and the argument registers x0..x7.
    pub imports: Vec<(String, Vec<V>)>,
    /// Constants the run computed or loaded.
    pub consts: Vec<u64>,
    /// Image offsets of the relocated words the run loaded.
    pub word_loads: Vec<u64>,
    /// Calls and jumps through registers the run did not know.
    pub unknown_targets: usize,
    /// Where jumps through registers the run did not know ended a path.
    pub lost: Vec<u64>,
    /// Hook calls given a C string (the hook's word and the string): the
    /// driver's settings lookups, which the run takes to find nothing.
    pub settings: Vec<(u64, String)>,
    steps: usize,
}

/// Where a block goes.
enum Succ {
    /// These image offsets.
    Pcs(Vec<u64>),
    /// An imported function.
    Import(String),
    /// The function a writable word of the program holds.
    Slot(u64),
    /// Somewhere the run does not know.
    Unknown,
}

/// The abstract interpreter over a driver [`Image`].
pub struct Abstract<'a> {
    image: &'a Image,
    nregs: usize,
    /// The hook taken for the settings lookup (a call of it with a name
    /// finds nothing); `None` takes every hook given a name for one.
    settings_hook: Option<u64>,
}

fn width_of(b: &Block, v: relift_ir::Value) -> Width {
    b.insts[v.index()].width.unwrap_or(Width::W64)
}

fn sext(v: u128, from: Width, to: Width) -> u128 {
    let bits = from.bits();
    if bits < 128 && v >> (bits - 1) & 1 == 1 {
        (v | !from.mask()) & to.mask()
    } else {
        v
    }
}

impl<'a> Abstract<'a> {
    /// An interpreter over `image`.
    pub fn new(image: &'a Image) -> Abstract<'a> {
        Abstract {
            image,
            nregs: image.fe.regs().len(),
            settings_hook: None,
        }
    }

    /// A fresh entry state.
    pub fn entry(&self) -> State {
        State::entry(self.nregs)
    }

    /// Runs the function at image offset `pc` from `state`; the merge of
    /// the states it returns with, each with the returned value in x0 (none
    /// when no path returns).
    ///
    /// # Errors
    ///
    /// The budget ran out.
    pub fn run(&self, pc: u64, state: State, trace: &mut Trace) -> Result<Vec<State>, String> {
        self.run_at(pc, state, trace, 0)
    }

    fn run_at(
        &self,
        entry: u64,
        state: State,
        trace: &mut Trace,
        depth: u32,
    ) -> Result<Vec<State>, String> {
        let mut stored: HashMap<u64, Vec<State>> = HashMap::new();
        let mut queue: VecDeque<(u64, usize)> = VecDeque::new();
        let mut exits: Vec<State> = Vec::new();
        let entry_sp = state.regs[SP.index()].clone();
        stored.insert(entry, vec![state]);
        queue.push_back((entry, 0));
        while let Some((start, idx)) = queue.pop_front() {
            let Some(mut s) = stored.get(&start).and_then(|v| v.get(idx)).cloned() else {
                continue;
            };
            let mut pc = start;
            loop {
                trace.steps += 1;
                if trace.steps > BUDGET {
                    return Err(format!(
                        "the abstract run from 0x{entry:x} took more than {BUDGET} steps"
                    ));
                }
                let Some(b) = self.image.block(pc) else {
                    break;
                };
                let next = self.step(&b, &mut s, trace);
                let mut to: Vec<(u64, State)> = Vec::new();
                let mut fall: Option<u64> = None;
                match (b.exit.flow, next) {
                    (Flow::Return, _) => {
                        exits.push(s.clone());
                        break;
                    }
                    (Flow::Trap(_), _) => break,
                    (Flow::Call { ret }, target) => {
                        let mut more = self.call(target, &mut s, trace, depth)?;
                        to.extend(more.drain(..).map(|m| (ret, m)));
                        fall = Some(ret);
                    }
                    (Flow::Jump, Succ::Pcs(t)) if t.len() == 1 && t[0] == pc + u64::from(b.len) => {
                        fall = Some(t[0])
                    }
                    (Flow::Jump, Succ::Pcs(t)) => to.extend(t.into_iter().map(|t| (t, s.clone()))),
                    (Flow::Jump, target) => {
                        if s.regs[SP.index()] == entry_sp {
                            let more = self.call(target, &mut s, trace, depth)?;
                            exits.push(s.clone());
                            exits.extend(more);
                        } else {
                            trace.unknown_targets += 1;
                            trace.lost.push(pc);
                        }
                        break;
                    }
                }
                if let Some(f) = fall {
                    if stored.contains_key(&f) || !to.is_empty() {
                        to.push((f, s.clone()));
                    } else {
                        pc = f;
                        continue;
                    }
                }
                for (t, st) in to {
                    Self::merge_into(&mut stored, &mut queue, t, st);
                }
                break;
            }
        }
        Ok(exits)
    }

    /// Adds `s` to the states kept at `t`: merged into one that agrees with
    /// it on every register both know, else kept apart (up to
    /// [`PARTITIONS`]), so paths that return different constants stay
    /// apart.
    fn merge_into(
        stored: &mut HashMap<u64, Vec<State>>,
        queue: &mut VecDeque<(u64, usize)>,
        t: u64,
        s: State,
    ) {
        let list = stored.entry(t).or_default();
        let at = list
            .iter()
            .position(|o| o.agrees(&s))
            .or_else(|| (list.len() >= PARTITIONS).then(|| list.len() - 1));
        match at {
            Some(i) => {
                let m = list[i].merge(&s);
                if m != list[i] {
                    list[i] = m;
                    queue.push_back((t, i));
                }
            }
            None => {
                list.push(s);
                queue.push_back((t, list.len() - 1));
            }
        }
    }

    /// Applies a call to `s`; the states of further return paths that
    /// return other constants (see [`Abstract::merge_into`]).
    fn call(
        &self,
        target: Succ,
        s: &mut State,
        trace: &mut Trace,
        depth: u32,
    ) -> Result<Vec<State>, String> {
        let args: Vec<V> = (0..8).map(|i| s.regs[i].clone()).collect();
        let t = match target {
            Succ::Pcs(t) if t.len() == 1 => t[0],
            Succ::Import(name) => {
                self.import(&name, &args, s);
                trace.imports.push((name, args));
                s.clobber_call();
                return Ok(Vec::new());
            }
            Succ::Slot(slot) => {
                let key = match &args[0] {
                    V::K(k) => u64::try_from(*k)
                        .ok()
                        .and_then(|k| self.image.rodata_str(k)),
                    _ => None,
                };
                s.clobber_call();
                if let Some(key) = key
                    && self.settings_hook.is_none_or(|h| h == slot)
                {
                    trace.settings.push((slot, key.to_string()));
                    s.regs[0] = V::K(0);
                } else {
                    trace.unknown_targets += 1;
                }
                return Ok(Vec::new());
            }
            _ => {
                trace.unknown_targets += 1;
                s.clobber_call();
                return Ok(Vec::new());
            }
        };
        if let Some(name) = self.stub_import(t) {
            self.import(&name, &args, s);
            trace.imports.push((name, args));
            s.clobber_call();
            return Ok(Vec::new());
        }
        if depth >= DEPTH {
            s.clobber_call();
            return Ok(Vec::new());
        }
        let mut callee = s.clone();
        callee.regs[X30 as usize] = V::U;
        let lost = trace.lost.len();
        let exits = self.run_at(t, callee, trace, depth + 1)?;
        let partial = trace.lost.len() > lost;
        let mut groups: Vec<State> = Vec::new();
        for e in exits {
            match groups.iter().position(|g| g.reg(0) == e.reg(0)) {
                Some(i) => groups[i] = groups[i].merge(&e),
                None if groups.len() < PARTITIONS => groups.push(e),
                None => {
                    let last = groups.len() - 1;
                    groups[last] = groups[last].merge(&e);
                }
            }
        }
        if groups.is_empty() {
            s.clobber_call();
            return Ok(Vec::new());
        }
        let sp = s.regs[SP.index()].clone();
        let saved: Vec<V> = (19..=29).map(|i| s.regs[i].clone()).collect();
        let fix = |mut g: State| {
            g.regs[SP.index()] = sp.clone();
            for (k, v) in saved.iter().enumerate() {
                g.regs[19 + k] = v.clone();
            }
            if partial {
                g.clobber_call();
            }
            g
        };
        let mut it = groups.into_iter().map(fix);
        *s = it.next().unwrap_or_else(|| s.clone());
        Ok(it.collect())
    }

    /// The targets of a jump table: entries while they land in code near
    /// `base` (the run does not know the index's bound, so a few entries
    /// past the table may add paths that cannot happen; such paths can make
    /// a value unknown, never a wrong constant).
    fn jump_table(&self, tab: u64, elem: u32, sign: bool, shift: u32, base: u64) -> Succ {
        let mut out = Vec::new();
        for k in 0..256u64 {
            let Some(V::K(e)) = self.image.load_const(tab + k * u64::from(elem), elem) else {
                break;
            };
            let bits = 8 * elem;
            let e = if sign && e >> (bits - 1) & 1 == 1 {
                e as i64 - (1i64 << bits)
            } else {
                e as i64
            };
            let t = base.wrapping_add((e << shift) as u64);
            if t.abs_diff(base) > 0x1_0000 || !t.is_multiple_of(4) || self.image.block(t).is_none()
            {
                break;
            }
            if !out.contains(&t) {
                out.push(t);
            }
        }
        if out.is_empty() {
            Succ::Unknown
        } else {
            Succ::Pcs(out)
        }
    }

    /// The import a PLT stub at `t` jumps to.
    fn stub_import(&self, t: u64) -> Option<String> {
        let mut s = self.entry();
        let mut trace = Trace::default();
        let mut pc = t;
        for _ in 0..4 {
            let b = self.image.block(pc)?;
            if b.exit.flow != Flow::Jump {
                return None;
            }
            match self.step(&b, &mut s, &mut trace) {
                Succ::Pcs(n) if n.len() == 1 && n[0] == pc + 4 => pc = n[0],
                Succ::Import(n) => return Some(n),
                _ => return None,
            }
        }
        None
    }

    fn import(&self, name: &str, args: &[V], s: &mut State) {
        if matches!(name, "memcpy" | "memmove")
            && let (V::P(dobj, doff), V::K(n)) = (&args[0], &args[2])
            && *n <= 0x10_0000
        {
            let n = *n as u32;
            let copied: Vec<(i64, Cell)> = match &args[1] {
                V::P(sobj, soff) => s
                    .mem
                    .get(sobj)
                    .map(|m| {
                        m.range(..soff + i64::from(n))
                            .filter(|(k, c)| **k + i64::from(c.len()) > *soff)
                            .filter_map(|(k, c)| match c {
                                Cell::Bytes(b) => {
                                    let lo = (*soff).max(*k);
                                    let hi = (soff + i64::from(n)).min(k + b.len() as i64);
                                    Some((
                                        lo - soff,
                                        Cell::Bytes(
                                            b[(lo - k) as usize..(hi - k) as usize].to_vec(),
                                        ),
                                    ))
                                }
                                Cell::Val(..) => None,
                            })
                            .collect()
                    })
                    .unwrap_or_default(),
                _ => Vec::new(),
            };
            let m = s.mem.entry(dobj.clone()).or_default();
            carve(m, *doff, n);
            m.insert(*doff, Cell::Val(V::U, n));
            for (rel, c) in copied {
                let len = c.len();
                carve(m, doff + rel, len);
                m.insert(doff + rel, c);
            }
            return;
        }
        if name == "memset"
            && let (V::P(obj, off), V::K(c), V::K(n)) = (&args[0], &args[1], &args[2])
            && *n <= 0x10_0000
        {
            let n = *n as u32;
            carve(s.mem.entry(obj.clone()).or_default(), *off, n);
            s.mem
                .entry(obj.clone())
                .or_default()
                .insert(*off, Cell::Bytes(vec![*c as u8; n as usize]));
        }
    }

    /// Evaluates `b`'s instructions on `s` (writing its registers and
    /// memory); the values.
    fn eval(&self, b: &Block, s: &mut State, trace: &mut Trace) -> Vec<V> {
        let mut vals: Vec<V> = Vec::with_capacity(b.insts.len());
        let mut sets: Vec<(Reg, V)> = Vec::new();
        for inst in &b.insts {
            let w = inst.width.unwrap_or(Width::W64);
            let get = |v: relift_ir::Value, vals: &Vec<V>| vals[v.index()].clone();
            let r = match &inst.op {
                Op::Const(k) => {
                    let v = k.get() & w.mask();
                    if let Ok(x) = u64::try_from(v) {
                        trace.consts.push(x);
                    }
                    V::K(v)
                }
                Op::Get(reg) => s.regs[reg.index()].clone(),
                Op::Set(reg, v) => {
                    sets.push((*reg, get(*v, &vals)));
                    V::U
                }
                Op::Un(op, a) => match get(*a, &vals) {
                    V::K(x) => V::K(eval_un(*op, x, w)),
                    _ => V::U,
                },
                Op::Bin(op, a, c) => {
                    let (x, y) = (get(*a, &vals), get(*c, &vals));
                    match (op, x, y) {
                        (_, V::K(x), V::K(y)) => {
                            let r = eval_bin(*op, x, y, w);
                            if let Ok(v) = u64::try_from(r) {
                                trace.consts.push(v);
                            }
                            V::K(r)
                        }
                        (BinOp::Add, V::P(Obj::At(..), _), V::K(k))
                        | (BinOp::Add, V::K(k), V::P(Obj::At(..), _))
                            if k > 0x4_0000 && (k as u64) < self.image.read_only =>
                        {
                            V::Idx(k as u64)
                        }
                        (BinOp::Add, V::P(o, off), V::K(k))
                        | (BinOp::Add, V::K(k), V::P(o, off)) => {
                            V::P(o, off.wrapping_add(k as u64 as i64))
                        }
                        (BinOp::Sub, V::P(o, off), V::K(k)) => {
                            V::P(o, off.wrapping_sub(k as u64 as i64))
                        }
                        (
                            BinOp::Shl,
                            V::Tab {
                                tab,
                                elem,
                                sign,
                                shift,
                                base: 0,
                            },
                            V::K(k),
                        ) if k < 8 => V::Tab {
                            tab,
                            elem,
                            sign,
                            shift: shift + k as u32,
                            base: 0,
                        },
                        (
                            BinOp::Add,
                            V::K(k),
                            V::Tab {
                                tab,
                                elem,
                                sign,
                                shift,
                                base: 0,
                            },
                        )
                        | (
                            BinOp::Add,
                            V::Tab {
                                tab,
                                elem,
                                sign,
                                shift,
                                base: 0,
                            },
                            V::K(k),
                        ) => V::Tab {
                            tab,
                            elem,
                            sign,
                            shift,
                            base: k as u64,
                        },
                        (BinOp::Add, V::K(k), V::U) | (BinOp::Add, V::U, V::K(k))
                            if k > 0x4_0000 && (k as u64) < self.image.read_only =>
                        {
                            V::Idx(k as u64)
                        }
                        (BinOp::Or | BinOp::Xor | BinOp::Add, p, V::K(0))
                        | (BinOp::Or | BinOp::Xor | BinOp::Add, V::K(0), p) => p,
                        (
                            BinOp::Shl | BinOp::LShr | BinOp::AShr | BinOp::RotR | BinOp::Sub,
                            p,
                            V::K(0),
                        ) => p,
                        (BinOp::And, p, V::K(m)) | (BinOp::And, V::K(m), p)
                            if m & w.mask() == w.mask() =>
                        {
                            p
                        }
                        (BinOp::Sub, V::P(o1, a1), V::P(o2, a2)) if o1 == o2 => {
                            V::K(u128::from(a1.wrapping_sub(a2) as u64) & w.mask())
                        }
                        _ => V::U,
                    }
                }
                Op::Cmp(op, a, c) => match (get(*a, &vals), get(*c, &vals)) {
                    (V::K(x), V::K(y)) => V::K(u128::from(eval_cmp(*op, x, y, width_of(b, *a)))),
                    _ => V::U,
                },
                Op::Select(c, a, d) => match get(*c, &vals) {
                    V::K(k) => get(if k & 1 == 1 { *a } else { *d }, &vals),
                    _ => {
                        let (x, y) = (get(*a, &vals), get(*d, &vals));
                        if x == y { x } else { V::U }
                    }
                },
                Op::Ext(kind, a) => match get(*a, &vals) {
                    V::K(x) => V::K(match kind {
                        ExtKind::Zero => x,
                        ExtKind::Sign => sext(x, width_of(b, *a), w),
                    }),
                    p @ V::P(..) if *kind == ExtKind::Zero => p,
                    V::Tab {
                        tab,
                        elem,
                        shift,
                        base,
                        ..
                    } if base == 0 && shift == 0 => V::Tab {
                        tab,
                        elem,
                        sign: *kind == ExtKind::Sign,
                        shift,
                        base,
                    },
                    t @ V::Tab { .. } if *kind == ExtKind::Zero => t,
                    p @ V::Import(_) => p,
                    _ => V::U,
                },
                Op::Extract(a, lo) => match get(*a, &vals) {
                    V::K(x) => V::K((x >> *lo) & w.mask()),
                    p @ (V::P(..) | V::Import(_) | V::Tab { .. }) if *lo == 0 && w.bits() >= 32 => {
                        p
                    }
                    _ => V::U,
                },
                Op::Concat(hi, lo) => match (get(*hi, &vals), get(*lo, &vals)) {
                    (V::K(h), V::K(l)) => V::K((h << width_of(b, *lo).bits()) | l),
                    _ => V::U,
                },
                Op::Load { addr, .. } => {
                    let len = w.bytes().unwrap_or(8);
                    match get(*addr, &vals) {
                        V::K(a) => match u64::try_from(a).ok().and_then(|a| {
                            if self.image.words.contains_key(&a) {
                                trace.word_loads.push(a);
                            }
                            self.image.load_const(a, len)
                        }) {
                            Some(v) => {
                                if let V::K(x) = &v
                                    && let Ok(x) = u64::try_from(*x)
                                {
                                    trace.consts.push(x);
                                }
                                v
                            }
                            None if len == 8 && a >= u128::from(self.image.read_only) => {
                                V::Slot(a as u64)
                            }
                            None => V::U,
                        },
                        V::Idx(tab) if len <= 4 => V::Tab {
                            tab,
                            elem: len,
                            sign: false,
                            shift: 0,
                            base: 0,
                        },
                        V::P(obj, off) => {
                            trace.loads.push(Access {
                                obj: obj.clone(),
                                off,
                                len,
                            });
                            s.load(&obj, off, len)
                        }
                        _ => V::U,
                    }
                }
                Op::Store { addr, value, .. } => {
                    let len = width_of(b, *value).bytes().unwrap_or(8);
                    if let V::P(obj, off) = get(*addr, &vals) {
                        let v = get(*value, &vals);
                        s.store(&obj, off, len, &v);
                    }
                    V::U
                }
                Op::StoreExclusive { addr, .. }
                | Op::AtomicRmw { addr, .. }
                | Op::CompareSwap { addr, .. } => {
                    if let V::P(obj, off) = get(*addr, &vals) {
                        s.store(&obj, off, w.bytes().unwrap_or(8), &V::U);
                    }
                    V::U
                }
                _ => V::U,
            };
            vals.push(r);
        }
        for (reg, v) in sets {
            s.regs[reg.index()] = v;
        }
        vals
    }

    /// Steps block `b`; the known successors (none when the target is not
    /// known).
    fn step(&self, b: &Block, s: &mut State, trace: &mut Trace) -> Succ {
        let vals = self.eval(b, s, trace);
        match b.exit.next {
            Next::Const(t) => Succ::Pcs(vec![t]),
            Next::Select {
                cond,
                taken,
                not_taken,
            } => match &vals[cond.index()] {
                V::K(k) => Succ::Pcs(vec![if k & 1 == 1 { taken } else { not_taken }]),
                _ => Succ::Pcs(vec![taken, not_taken]),
            },
            Next::Dynamic(v) => match &vals[v.index()] {
                V::K(t) => u64::try_from(*t).map_or(Succ::Unknown, |t| Succ::Pcs(vec![t])),
                V::Tab {
                    tab,
                    elem,
                    sign,
                    shift,
                    base,
                } if *base != 0 => self.jump_table(*tab, *elem, *sign, *shift, *base),
                V::Import(n) => Succ::Import(n.clone()),
                V::Slot(a) => Succ::Slot(*a),
                _ => Succ::Unknown,
            },
        }
    }
}

/// The driver's proc table: each proc's name (with its `nvn` prefix) and
/// image offset, in the table's (sorted) order.
///
/// # Errors
///
/// No `nvnBootstrapLoaderInternal`, or no table among the constants its
/// lookup uses.
pub fn proc_table(image: &Image) -> Result<Vec<(String, u64)>, String> {
    let loader = *image
        .symbols
        .get("nvnBootstrapLoaderInternal")
        .ok_or("nvn driver: the module defines no nvnBootstrapLoaderInternal")?;
    let a = Abstract::new(image);
    let mut trace = Trace::default();
    a.run(loader, a.entry(), &mut trace)?;
    let mut consts = trace.consts.clone();
    consts.sort_unstable();
    consts.dedup();
    let consts: Vec<u64> = consts
        .into_iter()
        .filter(|c| *c < image.img.len() as u64)
        .collect();
    let code =
        |at: u64| matches!(image.words.get(&at), Some(Word::Value(v)) if *v < image.read_only);
    for &offs in &consts {
        for &pool in &consts {
            let names = table_names(image, offs, pool);
            if names.len() < 16 {
                continue;
            }
            for &slot in &trace.word_loads {
                let Some(Word::Value(start)) = image.words.get(&slot) else {
                    continue;
                };
                let start = *start;
                if start < image.read_only || !(0..names.len() as u64).all(|k| code(start + 8 * k))
                {
                    continue;
                }
                return Ok(names
                    .into_iter()
                    .zip((0..).map(|k: u64| start + 8 * k))
                    .filter_map(|(n, at)| match image.words.get(&at) {
                        Some(Word::Value(v)) => Some((format!("nvn{n}"), *v)),
                        _ => None,
                    })
                    .collect());
            }
        }
    }
    Err(
        "nvn driver: no proc table among the constants of nvnBootstrapLoaderInternal's lookup"
            .into(),
    )
}

/// The names a table of u32 offsets at `offs` into a string pool at `pool`
/// gives, as long as they stay sorted, distinct and non-empty.
fn table_names(image: &Image, offs: u64, pool: u64) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for k in 0.. {
        let Some(o) = image.u32_at(offs + 4 * k) else {
            break;
        };
        let Some(name) = image
            .cstr(pool + u64::from(o))
            .filter(|n| !n.is_empty() && n.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_'))
        else {
            break;
        };
        if out.last().is_some_and(|l| l.as_str() >= name) {
            break;
        }
        out.push(name.to_string());
    }
    out
}

/// A device field and the bytes the driver's `nvnDeviceInitialize` leaves
/// in it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Field {
    /// Its offset in the device object.
    pub off: u32,
    /// The bytes.
    pub bytes: Vec<u8>,
}

/// What the runtime writes into a device object before it runs the
/// driver's `nvnTextureBuilderGetStorageClass` on a builder of it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeviceLayout {
    /// The image offset of `nvnTextureBuilderGetStorageClass`.
    pub storage_class: u64,
    /// The offset of the device pointer in a texture builder.
    pub builder_device: u32,
    /// The offset of the device's flags word (`nvnDeviceGetFlags`).
    pub flags: u32,
    /// The offset of the `NvRmGpuDevice` pointer the getter hands
    /// `NvRmGpuDeviceChooseKind`.
    pub gpu_device: u32,
    /// The other fields the getter reads, with their values.
    pub fields: Vec<Field>,
    /// The image offset of the word holding the hook `nvnDeviceInitialize`
    /// reads its settings through (a function of the setting's name that
    /// returns its text, or null), if it reads any.
    pub settings_hook: Option<u64>,
    /// The names of those settings; the values above hold only when none is
    /// set.
    pub settings: Vec<String>,
}

fn proc_addr(procs: &[(String, u64)], name: &str) -> Result<u64, String> {
    procs
        .iter()
        .find(|(n, _)| n == name)
        .map(|(_, a)| *a)
        .ok_or_else(|| format!("nvn driver: the proc table has no {name}"))
}

/// The offset of the object pointer a getter of one pointer argument
/// returns from it.
fn getter_offset(a: &Abstract<'_>, pc: u64, name: &str) -> Result<i64, String> {
    let mut t = Trace::default();
    let exits = a.run(pc, a.entry(), &mut t)?;
    let offs: Vec<i64> = exits
        .iter()
        .map(|s| match s.reg(0) {
            V::P(Obj::At(o, off), 0) if **o == Obj::Arg(0) => Some(*off),
            _ => None,
        })
        .collect::<Option<Vec<i64>>>()
        .ok_or_else(|| format!("nvn driver: {name} does not return a field of its argument"))?;
    match offs.as_slice() {
        [first, rest @ ..] if rest.iter().all(|o| o == first) => Ok(*first),
        _ => Err(format!("nvn driver: {name} returns no single field")),
    }
}

/// Reads the device layout of the driver in `image`.
///
/// # Errors
///
/// A proc missing from the table, or code of a shape this does not read
/// (named in the message).
pub fn device_layout(image: &Image) -> Result<DeviceLayout, String> {
    let procs = proc_table(image)?;
    let a = Abstract::new(image);
    let storage_class = proc_addr(&procs, "nvnTextureBuilderGetStorageClass")?;
    let builder_device = getter_offset(
        &a,
        proc_addr(&procs, "nvnTextureBuilderGetDevice")?,
        "nvnTextureBuilderGetDevice",
    )?;
    let flags = getter_offset(
        &a,
        proc_addr(&procs, "nvnDeviceGetFlags")?,
        "nvnDeviceGetFlags",
    )?;
    let device = Obj::At(Box::new(Obj::Arg(0)), builder_device);

    let mut t = Trace::default();
    a.run(storage_class, a.entry(), &mut t)?;
    let gpu: Vec<i64> = t
        .imports
        .iter()
        .filter(|(n, _)| n == "NvRmGpuDeviceChooseKind")
        .map(|(_, args)| match &args[0] {
            V::P(Obj::At(o, off), 0) if **o == device => Some(*off),
            _ => None,
        })
        .collect::<Option<Vec<i64>>>()
        .ok_or("nvn driver: nvnTextureBuilderGetStorageClass hands NvRmGpuDeviceChooseKind no device field")?;
    let gpu_device = match gpu.as_slice() {
        [first, rest @ ..] if rest.iter().all(|o| o == first) => *first,
        [] => return Err("nvn driver: nvnTextureBuilderGetStorageClass never calls NvRmGpuDeviceChooseKind".into()),
        _ => return Err("nvn driver: nvnTextureBuilderGetStorageClass hands NvRmGpuDeviceChooseKind several fields".into()),
    };
    let mut wanted: BTreeMap<i64, u32> = BTreeMap::new();
    for l in &t.loads {
        if l.obj != device {
            continue;
        }
        let in_flags = l.off >= flags && l.off + i64::from(l.len) <= flags + 4;
        if in_flags || l.off == gpu_device {
            continue;
        }
        let e = wanted.entry(l.off).or_insert(0);
        *e = (*e).max(l.len);
    }

    let init = proc_addr(&procs, "nvnDeviceInitialize")?;
    let mut probe = Trace::default();
    a.run(init, a.entry(), &mut probe)?;
    let mut count: BTreeMap<u64, std::collections::BTreeSet<String>> = BTreeMap::new();
    for (h, k) in probe.settings {
        count.entry(h).or_default().insert(k);
    }
    let settings_hook = count
        .iter()
        .max_by_key(|(_, keys)| keys.len())
        .map(|(h, _)| *h);
    let a = Abstract {
        settings_hook: settings_hook.or(Some(u64::MAX)),
        ..a
    };
    let mut ti = Trace::default();
    let exits = a.run(init, a.entry(), &mut ti)?;
    let ok: Vec<State> = exits
        .into_iter()
        .filter(|s| !matches!(s.reg(0), V::K(k) if k & 0xff == 0))
        .collect();
    let mut it = ok.into_iter();
    let first = it
        .next()
        .ok_or("nvn driver: no path of nvnDeviceInitialize returns true")?;
    let done = it.fold(first, |x, y| x.merge(&y));
    let dev = Obj::Arg(0);
    let mut fields = Vec::new();
    for (off, len) in wanted {
        match done.load(&dev, off, len) {
            V::K(k) => fields.push(Field {
                off: off as u32,
                bytes: (0..len).map(|i| (k >> (8 * i)) as u8).collect(),
            }),
            _ => {
                return Err(format!(
                    "nvn driver: nvnTextureBuilderGetStorageClass reads device field 0x{off:x} ({len} bytes), \
                     which nvnDeviceInitialize leaves no constant in ({} calls it could not follow)",
                    ti.unknown_targets
                ));
            }
        }
    }
    let mut settings: Vec<String> = ti.settings.iter().map(|(_, k)| k.clone()).collect();
    settings.sort();
    settings.dedup();
    Ok(DeviceLayout {
        storage_class,
        settings_hook,
        settings,
        builder_device: builder_device as u32,
        flags: flags as u32,
        gpu_device: gpu_device as u32,
        fields,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_switch_formats::nso::{NsoSpec, build};

    const RODATA: usize = 0x1000;
    const DATA: usize = 0x2000;
    const LOADER: u32 = 0x100;
    const FILLER: u32 = 0x140;
    const GET_DEVICE: u32 = 0x180;
    const GET_FLAGS: u32 = 0x1c0;
    const STORAGE_CLASS: u32 = 0x200;
    const INIT: u32 = 0x280;
    const STUB: u32 = 0x300;
    const POOL: u32 = 0x1600;
    const OFFS: u32 = 0x1800;
    const SETTING: u32 = 0x1901;
    const DYNSTR: usize = 0x100;
    const DYNSYM: usize = 0x200;
    const RELA: usize = 0xa00;
    const TABLE_WORD: u32 = 0x2100;
    const CHOOSE_GOT: u32 = 0x2180;
    const HOOK: u32 = 0x2190;
    const TABLE: u32 = 0x2200;

    fn movz_x(rd: u32, imm: u32) -> u32 {
        0xd280_0000 | (imm << 5) | rd
    }
    fn movz_w(rd: u32, imm: u32) -> u32 {
        0x5280_0000 | (imm << 5) | rd
    }
    fn ldr_x(rt: u32, rn: u32, off: u32) -> u32 {
        0xf940_0000 | ((off / 8) << 10) | (rn << 5) | rt
    }
    fn ldr_w(rt: u32, rn: u32, off: u32) -> u32 {
        0xb940_0000 | ((off / 4) << 10) | (rn << 5) | rt
    }
    fn ldrb(rt: u32, rn: u32, off: u32) -> u32 {
        0x3940_0000 | (off << 10) | (rn << 5) | rt
    }
    fn str_w(rt: u32, rn: u32, off: u32) -> u32 {
        0xb900_0000 | ((off / 4) << 10) | (rn << 5) | rt
    }
    fn strb(rt: u32, rn: u32, off: u32) -> u32 {
        0x3900_0000 | (off << 10) | (rn << 5) | rt
    }
    fn bl(from: u32, to: u32) -> u32 {
        0x9400_0000 | ((to.wrapping_sub(from) / 4) & 0x3ff_ffff)
    }
    const RET: u32 = 0xd65f_03c0;
    const MOV_X19_X0: u32 = 0xaa00_03f3;
    const BLR_X8: u32 = 0xd63f_0100;
    const BR_X17: u32 = 0xd61f_0220;

    fn put(b: &mut [u8], at: usize, v: &[u8]) {
        b[at..at + v.len()].copy_from_slice(v);
    }
    fn code(t: &mut [u8], at: u32, words: &[u32]) {
        for (k, w) in words.iter().enumerate() {
            put(t, at as usize + 4 * k, &w.to_le_bytes());
        }
    }

    /// A driver whose nvnDeviceInitialize stores the byte at 0x104 only
    /// when `stores_flag`.
    fn driver(stores_flag: bool) -> Image {
        let mut text = vec![0u8; 0x1000];
        code(&mut text, 0, &[RET]);
        put(&mut text, 4, &(RODATA as u32).to_le_bytes());
        code(
            &mut text,
            LOADER,
            &[
                movz_x(0, OFFS),
                movz_x(1, POOL),
                movz_x(8, TABLE_WORD),
                ldr_x(2, 8, 0),
                RET,
            ],
        );
        code(&mut text, FILLER, &[RET]);
        code(&mut text, GET_DEVICE, &[ldr_x(0, 0, 8), RET]);
        code(&mut text, GET_FLAGS, &[ldr_w(0, 0, 8), RET]);
        code(
            &mut text,
            STORAGE_CLASS,
            &[
                ldr_x(9, 0, 8),
                ldr_w(10, 9, 0x100),
                ldrb(11, 9, 0x104),
                ldr_x(0, 9, 0x200),
                bl(STORAGE_CLASS + 16, STUB),
                RET,
            ],
        );
        let mut init = vec![
            MOV_X19_X0,
            movz_x(0, SETTING),
            movz_x(8, HOOK),
            ldr_x(8, 8, 0),
            BLR_X8,
            movz_w(8, 6),
            str_w(8, 19, 0x100),
        ];
        if stores_flag {
            init.extend([movz_w(8, 1), strb(8, 19, 0x104)]);
        }
        init.extend([movz_w(0, 1), RET]);
        code(&mut text, INIT, &init);
        code(
            &mut text,
            STUB,
            &[movz_x(16, CHOOSE_GOT), ldr_x(17, 16, 0), BR_X17],
        );

        let mut names: Vec<(String, u32)> =
            (0..16).map(|k| (format!("Aaa{k:02}"), FILLER)).collect();
        names.extend([
            ("DeviceGetFlags".to_string(), GET_FLAGS),
            ("DeviceInitialize".to_string(), INIT),
            ("TextureBuilderGetDevice".to_string(), GET_DEVICE),
            ("TextureBuilderGetStorageClass".to_string(), STORAGE_CLASS),
        ]);
        let mut rodata = vec![0u8; 0x1000];
        put(&mut rodata, 0, b"MOD0");
        for (k, v) in [(DATA - RODATA) as i32, 0, 0, 0, 0, 0].iter().enumerate() {
            put(&mut rodata, 4 + 4 * k, &v.to_le_bytes());
        }
        let mut pool = 0usize;
        for (k, (n, _)) in names.iter().enumerate() {
            put(&mut rodata, POOL as usize - RODATA + pool, n.as_bytes());
            put(
                &mut rodata,
                OFFS as usize - RODATA + 4 * k,
                &(pool as u32).to_le_bytes(),
            );
            pool += n.len() + 1;
        }
        put(&mut rodata, SETTING as usize - RODATA, b"TestSetting");
        let dynstr = b"\0nvnBootstrapLoaderInternal\0NvRmGpuDeviceChooseKind\0";
        put(&mut rodata, DYNSTR, dynstr);
        let sym = |name: u32, shndx: u16, value: u64| {
            let mut e = [0u8; 24];
            e[0..4].copy_from_slice(&name.to_le_bytes());
            e[4] = 0x12;
            e[6..8].copy_from_slice(&shndx.to_le_bytes());
            e[8..16].copy_from_slice(&value.to_le_bytes());
            e
        };
        put(&mut rodata, DYNSYM + 24, &sym(1, 1, u64::from(LOADER)));
        put(&mut rodata, DYNSYM + 48, &sym(28, 0, 0));
        let mut rela: Vec<(u64, u64, u64)> = vec![(u64::from(TABLE_WORD), 1027, u64::from(TABLE))];
        for (k, (_, a)) in names.iter().enumerate() {
            rela.push((u64::from(TABLE) + 8 * k as u64, 1027, u64::from(*a)));
        }
        rela.push((u64::from(CHOOSE_GOT), (2 << 32) | 1026, 0));
        for (k, (o, i, a)) in rela.iter().enumerate() {
            let at = RELA + 24 * k;
            put(&mut rodata, at, &o.to_le_bytes());
            put(&mut rodata, at + 8, &i.to_le_bytes());
            put(&mut rodata, at + 16, &a.to_le_bytes());
        }
        let mut data = vec![0u8; 0x1000];
        let dynamic = [
            (7u64, (RODATA + RELA) as u64),
            (8, 24 * rela.len() as u64),
            (0, 0),
        ];
        for (k, (t, v)) in dynamic.iter().enumerate() {
            put(&mut data, 16 * k, &t.to_le_bytes());
            put(&mut data, 16 * k + 8, &v.to_le_bytes());
        }
        let nso = Nso::parse(&build(&NsoSpec {
            text,
            rodata,
            data,
            dyn_extents: [(DYNSTR as u32, dynstr.len() as u32), (DYNSYM as u32, 72)],
            ..NsoSpec::default()
        }))
        .expect("the fixture parses");
        let cpu = crate::cpu::cpu(None, Some(crate::cpu::Console::Switch2)).expect("a CPU");
        Image::new(&nso, Arm64 { cpu }).expect("the fixture has a MOD0")
    }

    #[test]
    fn the_proc_table_comes_from_the_loaders_constants() {
        let img = driver(true);
        let t = proc_table(&img).expect("a table");
        assert_eq!(t.len(), 20);
        assert_eq!(t[0], ("nvnAaa00".to_string(), u64::from(FILLER)));
        assert!(t.contains(&(
            "nvnTextureBuilderGetStorageClass".to_string(),
            u64::from(STORAGE_CLASS)
        )));
    }

    #[test]
    fn the_layout_names_the_fields_the_getter_reads_with_the_inits_values() {
        let l = device_layout(&driver(true)).expect("a layout");
        assert_eq!(l.storage_class, u64::from(STORAGE_CLASS));
        assert_eq!((l.builder_device, l.flags, l.gpu_device), (8, 8, 0x200));
        assert_eq!(
            l.fields,
            [
                Field {
                    off: 0x100,
                    bytes: vec![6, 0, 0, 0]
                },
                Field {
                    off: 0x104,
                    bytes: vec![1]
                },
            ]
        );
        assert_eq!(l.settings_hook, Some(u64::from(HOOK)));
        assert_eq!(l.settings, ["TestSetting"]);
    }

    #[test]
    fn a_field_the_init_leaves_unknown_is_named() {
        let e = device_layout(&driver(false)).expect_err("the byte at 0x104 is not set");
        assert!(e.contains("0x104"), "{e}");
    }
}

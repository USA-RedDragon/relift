//! The microcodes' own SP DMAs: every RSP program the CPU code loads, run
//! over abstract values from where it starts, gives the IMEM layouts its
//! DMAs to IMEM make (overlays over its own code) and the IMEM ranges those
//! DMAs may overwrite.
//!
//! A start is RSP code the CPU code loads into IMEM: the bytes of an SP DMA
//! with constant operands, or, for an SP DMA to a constant IMEM address from
//! a computed RDRAM address, each candidate that is RSP code. A candidate is
//! a constant address the CPU code stores, passes or builds in a register;
//! the candidates of one CPU function are partners. A world is an IMEM
//! layout (which bytes are at each IMEM word) with the candidates chosen to
//! reach it. Each world runs to a fixed point over every path: registers are
//! small sets of constants, ranges, a word loaded from a DMEM range, or an
//! initial DMEM word plus a constant; DMEM is a map of known bytes; the
//! states of the code a jump goes to are kept apart by the constants the
//! registers that code reads held at the jump (a DMA routine's operands, as
//! each call site set them).
//!
//! An SP DMA (an `MTC0` to the length register) with constant operands
//! copies bytes. Into DMEM they become known; into IMEM the paths after it
//! go on in the world of the new layout. An SP DMA to IMEM from an RDRAM
//! address the run does not know takes each candidate that is RSP code of
//! the DMA's length (the microcode's text, whose address the CPU code wrote
//! into the task in DMEM); when a copy into DMEM from an unknown address
//! came before it (the microcode's data), the world forks once more, one
//! world per partner of the text, run again from its start with the data's
//! bytes known. A computed jump through a halfword or word table in DMEM
//! goes to every entry of the table's range that is code.
//!
//! The run finds layouts and ranges only; the runtime checks the bytes of
//! every layout it enters. Stores to addresses the run does not know are
//! not modeled.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::rc::Rc;

use relift_cpu_rsp::decode::{self, Encoding};

use crate::image::{Image, Source};

/// Words of IMEM.
const WORDS: usize = 1024;
/// The most constants a value holds.
const SET: usize = 8;
/// The most return addresses whose states a word keeps apart.
const MAX_CTX: usize = 16;
/// The most worlds of one start.
const MAX_WORLDS: usize = 256;
/// The most instructions one start runs.
const MAX_STEPS: usize = 4_000_000;
/// The most data candidates one world forks to.
const MAX_FORK: usize = 64;
/// The most entries of a jump table.
const MAX_TABLE: i64 = 512;
/// The physical cartridge ROM addresses.
const CART: u32 = 0x1000_0000;
const CART_END: u32 = 0x1FC0_0000;

/// One IMEM word of a layout: the word and where it comes from.
type Word = Option<(u32, Source, u32)>;

/// A run of IMEM words of a layout from one place of a source.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct RspPart {
    /// The IMEM address of the first word.
    pub imem: u32,
    /// The source of the bytes.
    pub source: Source,
    /// Their offset in the source.
    pub offset: u32,
    /// The length in bytes.
    pub len: u32,
}

/// An IMEM layout a microcode makes: the bytes it holds at their IMEM
/// addresses, and the IMEM the microcode's DMAs may overwrite while it is
/// in the layout or in one it goes on to.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RspLayout {
    /// The known words, in runs; the other words are unknown.
    pub parts: Vec<RspPart>,
    /// The IMEM ranges, `[start, end)`, those DMAs may write.
    pub ranges: Vec<(u32, u32)>,
    /// Whether the analysis could not follow every path or DMA, so that
    /// any IMEM may be overwritten.
    pub unknown: bool,
}

/// A candidate: the segment of the CPU code that holds it and its physical
/// address (RDRAM, or a cartridge address).
pub(crate) type Cand = (u32, u32);

/// The physical address of a kseg0 or kseg1 constant, or the constant.
pub(crate) fn cand_addr(c: u32) -> u32 {
    if c >> 30 == 2 { c & 0x1FFF_FFFF } else { c }
}

/// What the CPU side gives the microcode runs.
pub(crate) struct Cpu<'a> {
    pub image: &'a Image<'a>,
    /// Per candidate, its partners (itself among them).
    pub partners: HashMap<Cand, BTreeSet<Cand>>,
    /// SP DMAs to IMEM with constant operands: segment, IMEM address,
    /// RDRAM address, length word.
    pub constant: Vec<(u32, u32, u32, u32)>,
    /// IMEM addresses of SP DMAs from a computed RDRAM address.
    pub computed: BTreeSet<u32>,
    /// The runs of RSP code the scan found: source, start and end offsets.
    pub code: Vec<(Source, u32, u32)>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum V {
    /// One of `n` constants, sorted.
    K(u8, [u32; SET]),
    /// A value from `lo` to `hi` (signed), a multiple of `step` apart.
    R(i64, i64, u32),
    /// The `n`-byte value at a DMEM address from `lo` to `hi`, `step`
    /// apart.
    T(u16, u16, u16, u8),
    /// The initial DMEM word at the address, plus the constant.
    S(u16, u32),
    Top,
}

fn gcd(a: u32, b: u32) -> u32 {
    if b == 0 { a } else { gcd(b, a % b) }
}

impl V {
    fn k(x: u32) -> V {
        let mut a = [0; SET];
        a[0] = x;
        V::K(1, a)
    }

    fn of(xs: &[u32]) -> V {
        let mut v: Vec<u32> = xs.to_vec();
        v.sort_unstable();
        v.dedup();
        if v.is_empty() || v.len() > SET {
            return V::Top;
        }
        let mut a = [0; SET];
        a[..v.len()].copy_from_slice(&v);
        V::K(v.len() as u8, a)
    }

    fn range(lo: i64, hi: i64, step: u32) -> V {
        if lo > hi || lo < -(1 << 31) || hi >= 1 << 32 || step == 0 {
            V::Top
        } else if lo == hi {
            V::k(lo as u32)
        } else {
            V::R(lo, hi, step)
        }
    }

    fn vals(&self) -> Option<&[u32]> {
        match self {
            V::K(n, a) => Some(&a[..usize::from(*n)]),
            _ => None,
        }
    }

    fn one(&self) -> Option<u32> {
        match self {
            V::K(1, a) => Some(a[0]),
            _ => None,
        }
    }

    /// The value as a range, for a constant set or a range.
    fn as_range(&self) -> Option<(i64, i64, u32)> {
        match *self {
            V::R(lo, hi, s) => Some((lo, hi, s)),
            V::K(..) => {
                let a = self.vals()?;
                let (lo, hi) = (i64::from(a[0]), i64::from(*a.last()?));
                let s = a.windows(2).fold(0, |g, w| gcd(g, w[1] - w[0]));
                Some((lo, hi, s.max(1)))
            }
            _ => None,
        }
    }

    fn join(self, o: V) -> V {
        if self == o {
            return self;
        }
        match (self, o) {
            (V::K(..), V::K(..)) => {
                V::of(&[self.vals().unwrap_or(&[]), o.vals().unwrap_or(&[])].concat())
            }
            (V::R(lo, hi, s), V::K(..)) | (V::K(..), V::R(lo, hi, s)) => {
                let k = if matches!(self, V::K(..)) { self } else { o };
                let inside = k.vals().unwrap_or(&[]).iter().all(|x| {
                    let x = i64::from(*x);
                    (lo..=hi).contains(&x) && (x - lo) % i64::from(s) == 0
                });
                if inside { V::R(lo, hi, s) } else { V::Top }
            }
            _ => V::Top,
        }
    }

    fn map(self, f: impl Fn(u32) -> u32) -> V {
        match self.vals() {
            Some(a) => V::of(&a.iter().map(|x| f(*x)).collect::<Vec<_>>()),
            None => V::Top,
        }
    }

    fn map2(self, o: V, f: impl Fn(u32, u32) -> u32) -> V {
        match (self.vals(), o.vals()) {
            (Some(a), Some(b)) if a.len() * b.len() <= SET => {
                let mut v = Vec::with_capacity(a.len() * b.len());
                for x in a {
                    for y in b {
                        v.push(f(*x, *y));
                    }
                }
                V::of(&v)
            }
            _ => V::Top,
        }
    }

    fn add(self, o: V) -> V {
        match (self, o) {
            (V::S(a, k), _) if o.one().is_some() => V::S(a, k.wrapping_add(o.one().unwrap_or(0))),
            (_, V::S(a, k)) if self.one().is_some() => {
                V::S(a, k.wrapping_add(self.one().unwrap_or(0)))
            }
            (V::R(lo, hi, s), _) | (_, V::R(lo, hi, s)) => {
                let k = if matches!(self, V::R(..)) { o } else { self };
                match k.one() {
                    Some(x) => {
                        let x = i64::from(x as i32);
                        V::range(lo + x, hi + x, s)
                    }
                    None => V::Top,
                }
            }
            _ => self.map2(o, u32::wrapping_add),
        }
    }

    fn sub(self, o: V) -> V {
        match (self, o.one()) {
            (V::S(a, k), Some(x)) => V::S(a, k.wrapping_sub(x)),
            (V::R(..), Some(x)) => self.add(V::k(x.wrapping_neg())),
            _ => self.map2(o, u32::wrapping_sub),
        }
    }

    fn and(self, o: V) -> V {
        match (self, o.one()) {
            (V::K(..), _) if o.vals().is_some() => self.map2(o, |a, b| a & b),
            (V::K(..) | V::R(..) | V::Top | V::T(..) | V::S(..), Some(m)) if m != 0 => {
                if let Some(x) = self.one() {
                    return V::k(x & m);
                }
                V::range(0, i64::from(m), 1 << m.trailing_zeros())
            }
            _ => match (self.one(), o) {
                (Some(m), V::R(..) | V::Top | V::T(..) | V::S(..)) if m != 0 => {
                    V::range(0, i64::from(m), 1 << m.trailing_zeros())
                }
                _ => V::Top,
            },
        }
    }

    fn sll(self, sa: u32) -> V {
        match self {
            V::R(lo, hi, s) => V::range(lo << sa, hi << sa, s << sa),
            _ => self.map(|a| a << sa),
        }
    }

    fn srl(self, sa: u32) -> V {
        match self {
            V::K(..) => self.map(|a| a >> sa),
            V::R(lo, hi, s) if lo >= 0 && sa < 32 => V::range(lo >> sa, hi >> sa, (s >> sa).max(1)),
            _ if sa > 0 && sa < 32 => V::range(0, i64::from(u32::MAX >> sa), 1),
            _ => V::Top,
        }
    }

    fn sra(self, sa: u32) -> V {
        match self {
            V::K(..) => self.map(|a| ((a as i32) >> sa) as u32),
            V::R(lo, hi, s) if sa < 32 => {
                let sx = |x: i64| i64::from((x as u32 as i32) >> sa);
                if hi < 1 << 31 {
                    V::range(sx(lo), sx(hi), (s >> sa).max(1))
                } else {
                    V::Top
                }
            }
            _ if sa > 0 && sa < 32 => V::range(-(1i64 << (31 - sa)), (1i64 << (31 - sa)) - 1, 1),
            _ => V::Top,
        }
    }
}

/// A DMEM byte the run knows, or one it does not; `None` for a byte that
/// holds what it held when the microcode started.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum B {
    Known(u8),
    Unk,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct St {
    r: [V; 32],
    dmem: Rc<Vec<Option<B>>>,
    mem: V,
    dram: V,
    /// The DMEM address and length (none when not known) of the last copy
    /// into DMEM from an RDRAM address the run does not know.
    pending: Option<(u16, Option<u16>)>,
    /// The context of the state: the constants the registers the code
    /// reads held at the last jump (so the states of a routine are kept
    /// apart by what each call site set up), or none.
    ctx: Option<u32>,
    /// The state is the target of a jump; its context is set next.
    jumped: bool,
}

impl St {
    fn new() -> St {
        let mut r = [V::Top; 32];
        r[0] = V::k(0);
        St {
            r,
            dmem: Rc::new(vec![None; 0x1000]),
            mem: V::Top,
            dram: V::Top,
            pending: None,
            ctx: None,
            jumped: false,
        }
    }

    fn get(&self, n: u32) -> V {
        if n == 0 { V::k(0) } else { self.r[n as usize] }
    }

    fn set(&mut self, n: u32, v: V) {
        if n != 0 {
            self.r[n as usize] = v;
        }
    }

    /// Joins `o` in; whether the state changed.
    fn join(&mut self, o: &St) -> bool {
        let mut changed = false;
        for (f, g) in self
            .r
            .iter_mut()
            .chain([&mut self.mem, &mut self.dram])
            .zip(o.r.iter().chain([&o.mem, &o.dram]))
        {
            let j = f.join(*g);
            changed |= j != *f;
            *f = j;
        }
        if self.pending.is_some() && self.pending != o.pending {
            self.pending = None;
            changed = true;
        }
        if !Rc::ptr_eq(&self.dmem, &o.dmem) && self.dmem != o.dmem {
            let differ = |a: &Option<B>, b: &Option<B>| a != b && *a != Some(B::Unk);
            if self
                .dmem
                .iter()
                .zip(o.dmem.iter())
                .any(|(a, b)| differ(a, b))
            {
                let m = Rc::make_mut(&mut self.dmem);
                for (a, b) in m.iter_mut().zip(o.dmem.iter()) {
                    if differ(a, b) {
                        *a = Some(B::Unk);
                    }
                }
                changed = true;
            }
        }
        changed
    }

    /// The end of the pending copy into DMEM: its length when known, else
    /// the first initial word read at or after it.
    fn pending_end(&self, inits: &BTreeSet<u16>) -> Option<(u32, u32)> {
        let (at, len) = self.pending?;
        let end = match len {
            Some(n) => u32::from(at) + u32::from(n),
            None => inits.range(at..).next().map_or(0x1000, |x| u32::from(*x)),
        };
        Some((u32::from(at), end))
    }

    /// The `n` bytes at DMEM address `at`: known, or the initial word, or
    /// not known.
    fn read(&self, at: u32, n: u32, signed: bool, inits: &BTreeSet<u16>) -> Result<u32, bool> {
        let bytes: Vec<Option<B>> = (0..n)
            .map(|i| self.dmem[((at + i) & 0xFFF) as usize])
            .collect();
        if bytes.iter().all(Option::is_none) {
            if let Some((p, e)) = self.pending_end(inits)
                && (p..e).contains(&at)
            {
                return Err(false);
            }
            return Err(n == 4 && at.is_multiple_of(4));
        }
        let mut v = 0u32;
        for b in bytes {
            match b {
                Some(B::Known(x)) => v = v << 8 | u32::from(x),
                _ => return Err(false),
            }
        }
        if signed && n < 4 {
            let s = 32 - 8 * n;
            v = ((v << s) as i32 >> s) as u32;
        }
        Ok(v)
    }

    fn load(&self, a: V, n: u32, signed: bool, inits: &mut BTreeSet<u16>) -> V {
        if let Some((lo, hi, s)) = a.as_range()
            && a.vals().is_none_or(|v| v.len() > 1)
            && hi - lo <= 0x1000
            && (hi - lo) / i64::from(s) <= MAX_TABLE
        {
            let lo = (lo as u32 & 0xFFF) as u16;
            let hi = lo + (hi - lo as i64) as u16;
            if let Some(vs) = a.vals() {
                let mut out = Vec::new();
                for &at in vs {
                    match self.read(at & 0xFFF, n, signed, inits) {
                        Ok(v) => out.push(v),
                        Err(_) => return V::T(lo, hi, s.min(0xFFFF) as u16, n as u8),
                    }
                }
                return V::of(&out);
            }
            return V::T(lo, hi, s.min(0xFFFF) as u16, n as u8);
        }
        let Some(at) = a.one() else {
            return V::Top;
        };
        let at = at & 0xFFF;
        match self.read(at, n, signed, inits) {
            Ok(v) => V::k(v),
            Err(true) => {
                inits.insert(at as u16);
                V::S(at as u16, 0)
            }
            Err(false) => V::Top,
        }
    }

    fn store_bytes(&mut self, at: u32, bytes: &[Option<u8>]) {
        let m = Rc::make_mut(&mut self.dmem);
        for (i, b) in bytes.iter().enumerate() {
            m[((at + i as u32) & 0xFFF) as usize] = Some(b.map_or(B::Unk, B::Known));
        }
    }

    fn store(&mut self, a: V, n: u32, v: V) {
        let Some(addrs) = a.vals() else {
            return;
        };
        let value = if addrs.len() == 1 { v.one() } else { None };
        for &at in addrs {
            let bytes: Vec<Option<u8>> = (0..n)
                .map(|i| value.map(|x| (x >> (8 * (n - 1 - i))) as u8))
                .collect();
            self.store_bytes(at, &bytes);
        }
    }
}

/// An SP DMA from RDRAM a run starts.
#[derive(Clone, Copy, Debug)]
struct Dma {
    mem: V,
    dram: V,
    len: V,
}

/// A copy from RDRAM to SP memory: `rows` rows of `row` bytes `skip` apart
/// from physical address `pa`, read in segment `seg`, to SP address `at`
/// (IMEM when `imem`).
#[derive(Clone, Copy)]
struct Copy {
    imem: bool,
    at: u32,
    pa: u32,
    rows: u32,
    row: u32,
    skip: u32,
    seg: u32,
}

/// A layout: the word at each IMEM word.
type Layout = Rc<Vec<Word>>;

struct World {
    layout: Layout,
    /// The candidates chosen, in order: a text, then maybe its data.
    chose: Vec<Cand>,
    /// The segment RDRAM addresses the microcode computes are read in.
    seg: u32,
    /// The first state and word the world was entered with.
    seed: Option<(u32, St)>,
    /// Whether the world forked to its data candidates.
    forked: bool,
    states: HashMap<(u16, Option<u32>), St>,
    ctxs: HashMap<u16, usize>,
    /// Per word, the registers its code reads before it writes them.
    live: HashMap<u16, u32>,
    work: Vec<(u16, Option<u32>)>,
    /// The IMEM ranges its DMAs write.
    ranges: BTreeSet<(u32, u32)>,
    /// It lost a path or a DMA.
    unknown: bool,
    /// The worlds its DMAs go on to.
    next: BTreeSet<usize>,
}

struct Run<'a> {
    cpu: &'a Cpu<'a>,
    /// The candidates whose bytes are RSP code.
    codes: &'a [Cand],
    /// The candidate the start's bytes are, for an SP DMA from a computed
    /// RDRAM address.
    start: Option<Cand>,
    worlds: Vec<World>,
    index: HashMap<(Layout, Vec<Cand>), usize>,
    steps: usize,
    /// A limit was reached.
    limited: bool,
    /// The DMEM words runs read as the CPU code left them.
    inits: BTreeSet<u16>,
}

/// Whether `w` is RSP code: it decodes, or is zero.
fn is_code(w: u32) -> bool {
    w == 0 || decode::decode(w).is_some()
}

fn simm(w: u32) -> u32 {
    i32::from(w as u16 as i16) as u32
}

/// The words at physical address `pa`, read in segment `seg` (or, for a
/// cartridge address, in the ROM): `rows` rows of `row` bytes `skip` apart,
/// with where each comes from.
fn read(cpu: &Cpu<'_>, seg: u32, pa: u32, rows: u32, row: u32, skip: u32) -> Option<Vec<Word>> {
    let image = cpu.image;
    let mut out = Vec::new();
    for k in 0..rows {
        let start = pa.wrapping_add(k * (row + skip));
        let (source, off) = if (CART..CART_END).contains(&start) {
            (Source::Rom, start - CART)
        } else {
            let (_, s, rel) = image.read(seg, start, 4)?;
            let sg = &image.segs[s as usize];
            if rel + row > sg.len {
                return None;
            }
            (sg.source, sg.offset + rel)
        };
        let bytes = image.src.bytes(source);
        for i in (0..row).step_by(4) {
            let at = (off + i) as usize;
            let w = u32::from_be_bytes(bytes.get(at..at + 4)?.try_into().ok()?);
            out.push(Some((w, source, off + i)));
        }
    }
    Some(out)
}

/// The address a candidate's bytes are read at: a cartridge address, or
/// an RDRAM one.
fn phys(c: Cand) -> u32 {
    if (CART..CART_END).contains(&c.1) {
        c.1 & !7
    } else {
        c.1 & 0x00FF_FFF8
    }
}

/// Whether the words are RSP code: all are known, one is not zero, and at
/// most one in 16 of the others does not decode (data in the code).
fn code_run(ws: &[Word]) -> bool {
    let Some(ws) = ws
        .iter()
        .map(|w| w.map(|w| w.0))
        .collect::<Option<Vec<u32>>>()
    else {
        return false;
    };
    let nonzero = ws.iter().filter(|w| **w != 0).count();
    let bad = ws.iter().filter(|w| !is_code(**w)).count();
    nonzero > 0 && bad * 16 <= nonzero
}

impl Run<'_> {
    fn read(&self, seg: u32, pa: u32, rows: u32, row: u32, skip: u32) -> Option<Vec<Word>> {
        read(self.cpu, seg, pa, rows, row, skip)
    }

    fn world(&mut self, layout: Layout, chose: Vec<Cand>, seg: u32) -> Option<usize> {
        if let Some(&i) = self.index.get(&(layout.clone(), chose.clone())) {
            return Some(i);
        }
        if self.worlds.len() >= MAX_WORLDS {
            self.limited = true;
            return None;
        }
        let i = self.worlds.len();
        self.index.insert((layout.clone(), chose.clone()), i);
        self.worlds.push(World {
            layout,
            chose,
            seg,
            seed: None,
            forked: false,
            states: HashMap::new(),
            ctxs: HashMap::new(),
            live: HashMap::new(),
            work: Vec::new(),
            ranges: BTreeSet::new(),
            unknown: false,
            next: BTreeSet::new(),
        });
        Some(i)
    }

    fn add(&mut self, wi: usize, pc: u32, st: St) {
        let w = &mut self.worlds[wi];
        if w.seed.is_none() {
            w.seed = Some((pc, st.clone()));
        }
        let pc = ((pc & 0xFFC) >> 2) as u16;
        let mut st = st;
        if st.jumped {
            st.jumped = false;
            let live = match w.live.get(&pc) {
                Some(m) => *m,
                None => {
                    let m = live_in(&w.layout, pc);
                    w.live.insert(pc, m);
                    m
                }
            };
            let mut h = 0x811C_9DC5u32;
            for r in (1..32).filter(|r| live >> r & 1 == 1) {
                let x = st.r[r].one().unwrap_or(0xFFFF_FFFF);
                h = (h ^ r as u32 ^ x).wrapping_mul(0x0100_0193);
            }
            st.ctx = Some(h);
            if !w.states.contains_key(&(pc, st.ctx)) {
                let n = w.ctxs.entry(pc).or_insert(0);
                if *n >= MAX_CTX {
                    st.ctx = None;
                } else {
                    *n += 1;
                }
            }
        }
        let key = (pc, st.ctx);
        let changed = match w.states.get_mut(&key) {
            Some(old) => old.join(&st),
            None => {
                w.states.insert(key, st);
                true
            }
        };
        if changed {
            w.work.push(key);
        }
    }

    fn run(&mut self) {
        while let Some(wi) = self.worlds.iter().position(|w| !w.work.is_empty()) {
            while let Some(key) = self.worlds[wi].work.pop() {
                self.steps += 1;
                if self.steps > MAX_STEPS {
                    self.limited = true;
                    return;
                }
                let st = self.worlds[wi].states[&key].clone();
                self.step(wi, key.0, st);
            }
        }
    }

    fn word(&self, wi: usize, i: u16) -> Option<u32> {
        self.worlds[wi].layout[usize::from(i) % WORDS].map(|w| w.0)
    }

    /// The targets of a computed jump to `v` in world `wi`: its constants,
    /// or the entries of its DMEM table that are code.
    fn computed(&mut self, wi: usize, st: &St, v: V) -> Vec<u32> {
        if let Some(a) = v.vals() {
            return a.iter().map(|x| x & 0xFFC).collect();
        }
        let V::T(lo, hi, step, n) = v else {
            self.worlds[wi].unknown = true;
            return Vec::new();
        };
        let mut out = BTreeSet::new();
        let mut at = u32::from(lo);
        while at <= u32::from(hi) {
            if let Ok(x) = st.read(at, u32::from(n), false, &self.inits) {
                let t = x & 0xFFC;
                if x < 0x1000 && self.word(wi, (t >> 2) as u16).is_some_and(|w| w != 0) {
                    out.insert(t);
                }
            }
            at += u32::from(step.max(1));
        }
        if out.is_empty() {
            self.worlds[wi].unknown = true;
        }
        out.into_iter().collect()
    }

    fn step(&mut self, wi: usize, i: u16, mut st: St) {
        let Some(w) = self.word(wi, i) else {
            self.worlds[wi].unknown = true;
            return;
        };
        let Some(enc) = decode::decode(w) else {
            return;
        };
        let pc = u32::from(i) * 4;
        if !enc.has_delay_slot() {
            if enc == Encoding::Break {
                return;
            }
            let dma = exec(&mut st, w, enc, &mut self.inits);
            self.go(wi, st, &[(pc + 4) & 0xFFF], dma);
            return;
        }
        let rs = st.get(w >> 21 & 31);
        let rt = st.get(w >> 16 & 31);
        let next = (pc + 8) & 0xFFF;
        let rel = (pc + 4).wrapping_add(simm(w) << 2) & 0xFFF;
        let sign = |v: V, f: fn(i32) -> bool| -> Option<bool> {
            let a = v.vals()?;
            let t = a.iter().filter(|x| f(**x as i32)).count();
            (t == a.len() || t == 0).then_some(t == a.len())
        };
        let eq = |a: V, b: V| -> Option<bool> {
            match (a.one(), b.one()) {
                (Some(x), Some(y)) => Some(x == y),
                _ => None,
            }
        };
        let both = |c: Option<bool>| match c {
            Some(true) => vec![rel],
            Some(false) => vec![next],
            None => vec![rel, next],
        };
        let targets: Vec<u32> = match enc {
            Encoding::J | Encoding::Jal => vec![(w << 2) & 0xFFC],
            Encoding::Jr | Encoding::Jalr => self.computed(wi, &st, rs),
            Encoding::Beq => both(eq(rs, rt)),
            Encoding::Bne => both(eq(rs, rt).map(|x| !x)),
            Encoding::Bltz | Encoding::Bltzal => both(sign(rs, |x| x < 0)),
            Encoding::Bgez | Encoding::Bgezal => both(sign(rs, |x| x >= 0)),
            Encoding::Bgtz => both(sign(rs, |x| x > 0)),
            Encoding::Blez => both(sign(rs, |x| x <= 0)),
            _ => Vec::new(),
        };
        let link = match enc {
            Encoding::Jal | Encoding::Bltzal | Encoding::Bgezal => Some(31),
            Encoding::Jalr => Some(w >> 11 & 31),
            _ => None,
        };
        if let Some(r) = link {
            st.set(r, V::k(next));
        }
        st.jumped = matches!(
            enc,
            Encoding::J | Encoding::Jal | Encoding::Jr | Encoding::Jalr
        );

        let Some(s) = self.word(wi, (i + 1) % WORDS as u16) else {
            self.worlds[wi].unknown = true;
            return;
        };
        let dma = match decode::decode(s) {
            Some(e) if !e.has_delay_slot() && e != Encoding::Break => {
                exec(&mut st, s, e, &mut self.inits)
            }
            _ => return,
        };
        self.go(wi, st, &targets, dma);
    }

    /// Goes on to `targets` with `st`, after the SP DMA `dma` when there is
    /// one.
    fn go(&mut self, wi: usize, st: St, targets: &[u32], dma: Option<Dma>) {
        let Some(d) = dma else {
            for &t in targets {
                self.add(wi, t, st.clone());
            }
            return;
        };
        for (to, st) in self.dma(wi, st, d) {
            if to != wi {
                self.worlds[wi].next.insert(to);
            }
            for &t in targets {
                self.add(to, t, st.clone());
            }
        }
    }

    fn range(&mut self, wi: usize, at: u32, len: u32) {
        let end = at + len;
        let r = &mut self.worlds[wi].ranges;
        if end > 0x1000 {
            r.insert((at, 0x1000));
            r.insert((0, end - 0x1000));
        } else {
            r.insert((at, end));
        }
    }

    /// Whether world `wi` is a text's, entered after a copy into DMEM from
    /// an unknown address: its data candidates may tell what it does not
    /// know.
    fn can_fork(&self, wi: usize) -> bool {
        let w = &self.worlds[wi];
        w.chose.len() == 1 && w.seed.as_ref().is_some_and(|s| s.1.pending.is_some())
    }

    /// The worlds and states after the SP DMA `d` of world `wi`. A DMA to
    /// IMEM, or to an SP address it does not know, in a world that can
    /// fork to its data ([`Run::can_fork`]) forks instead.
    fn dma(&mut self, wi: usize, st: St, d: Dma) -> Vec<(usize, St)> {
        let imem = d
            .mem
            .vals()
            .is_none_or(|m| m.iter().any(|x| x & 0x1000 != 0));
        if imem && d.dram.vals().is_none() && self.can_fork(wi) {
            if !self.worlds[wi].forked {
                self.worlds[wi].forked = true;
                let text = self.worlds[wi].chose[0];
                self.fork(wi, text);
            }
            return Vec::new();
        }
        if d.mem.vals().is_none() {
            self.worlds[wi].unknown = true;
            return vec![(wi, st)];
        }
        let mut out = Vec::new();
        for (m, len, dram) in triples(d) {
            let imem = m & 0x1000 != 0;
            let at = m & 0xFF8;
            let Some(l) = len else {
                if imem {
                    self.range(wi, at, 0x1000 - at);
                    out.extend(self.load_code(wi, &st, dram, at, 0x1000 - at));
                } else {
                    let mut s2 = st.clone();
                    s2.pending = Some((at as u16, None));
                    out.push((wi, s2));
                }
                continue;
            };
            let rows = (l >> 12 & 0xFF) + 1;
            let row = (l & 0xFF8) + 8;
            let skip = l >> 20 & 0xFF8;
            let total = (rows * row).min(0x1000);
            if imem {
                self.range(wi, at, total);
            }
            let Some(pa) = dram.one() else {
                if imem {
                    out.extend(self.load_code(wi, &st, dram, at, total));
                } else {
                    let mut s2 = st.clone();
                    s2.store_bytes(at, &vec![None; total as usize]);
                    s2.pending = Some((at as u16, Some(total as u16)));
                    out.push((wi, s2));
                }
                continue;
            };
            let c = Copy {
                imem,
                at,
                pa: pa & 0x00FF_FFF8,
                rows,
                row,
                skip,
                seg: self.worlds[wi].seg,
            };
            let chose = self.worlds[wi].chose.clone();
            if let Some(r) = self.copy(wi, &st, c, chose) {
                out.push(r);
            }
        }
        out
    }

    /// The worlds and states after a copy of `len` bytes into IMEM at `at`
    /// from an RDRAM address `dram` the run does not know (or knows only as
    /// an initial DMEM word plus a constant): in a world of no text, one
    /// per text candidate that is RSP code of that length, with the DMEM
    /// word the address came from set to it.
    fn load_code(&mut self, wi: usize, st: &St, dram: V, at: u32, len: u32) -> Vec<(usize, St)> {
        let w = &self.worlds[wi];
        let texts: Vec<Cand> = if matches!(dram, V::S(..)) && w.chose.is_empty() {
            self.texts(len)
        } else {
            Vec::new()
        };
        if texts.is_empty() {
            self.worlds[wi].unknown = true;
            let mut lay = (*self.worlds[wi].layout).clone();
            for i in (0..len).step_by(4) {
                lay[(((at + i) & 0xFFF) >> 2) as usize] = None;
            }
            let (chose, seg) = (self.worlds[wi].chose.clone(), self.worlds[wi].seg);
            return self
                .world(Rc::new(lay), chose, seg)
                .map(|to| (to, st.clone()))
                .into_iter()
                .collect();
        }
        let mut out = Vec::new();
        for t in texts {
            let mut s1 = st.clone();
            if let V::S(a, k) = dram {
                let init = phys(t).wrapping_sub(k);
                s1.store_bytes(u32::from(a), &init.to_be_bytes().map(Some));
            }
            let c = Copy {
                imem: true,
                at,
                pa: phys(t),
                rows: 1,
                row: len,
                skip: 0,
                seg: t.0,
            };
            if let Some(r) = self.copy(wi, &s1, c, vec![t]) {
                out.push(r);
            }
        }
        out
    }

    /// Starts the data worlds of world `wi`, whose text is `text`: from the
    /// world's first state, with the pending copy into DMEM made from each
    /// partner of the text that is not code.
    fn fork(&mut self, wi: usize, text: Cand) {
        let Some((pc, seed)) = self.worlds[wi].seed.clone() else {
            return;
        };
        let Some((pat, end)) = seed.pending_end(&self.inits) else {
            return;
        };
        let n = end.saturating_sub(pat).min(0x1000);
        let datas: Vec<Cand> = self
            .cpu
            .partners
            .get(&text)
            .into_iter()
            .flatten()
            .copied()
            .filter(|c| *c != text && !self.codes.contains(c))
            .take(MAX_FORK)
            .collect();
        for dc in datas {
            let Some(ws) = self.read(dc.0, phys(dc), 1, n.next_multiple_of(8), 0) else {
                continue;
            };
            let bytes: Vec<Option<u8>> = ws
                .iter()
                .flat_map(|w| w.map_or([None; 4], |w| w.0.to_be_bytes().map(Some)))
                .take(n as usize)
                .collect();
            let mut s = seed.clone();
            s.store_bytes(pat, &bytes);
            s.pending = None;
            let (lay, seg) = (self.worlds[wi].layout.clone(), self.worlds[wi].seg);
            if let Some(to) = self.world(lay, vec![text, dc], seg) {
                self.worlds[wi].next.insert(to);
                self.add(to, pc, s);
            }
        }
    }

    /// The text candidates that are RSP code of `len` bytes.
    fn texts(&self, len: u32) -> Vec<Cand> {
        self.codes
            .iter()
            .copied()
            .filter(|c| Some(*c) != self.start)
            .filter(|c| {
                self.read(c.0, phys(*c), 1, len.min(0x1000), 0)
                    .is_some_and(|ws| code_run(&ws))
            })
            .collect()
    }

    /// The world, with the candidates `chose`, and the state after the
    /// copy `c` in world `wi`.
    fn copy(&mut self, wi: usize, st: &St, c: Copy, chose: Vec<Cand>) -> Option<(usize, St)> {
        let Copy {
            imem,
            at,
            pa,
            rows,
            row,
            skip,
            seg,
        } = c;
        let words = self.read(seg, pa, rows, row, skip);
        let total = (rows * row).min(0x1000);
        if imem {
            let mut lay = (*self.worlds[wi].layout).clone();
            let n = (total / 4) as usize;
            let known = words.filter(|ws| code_run(ws));
            if known.is_none() {
                self.worlds[wi].unknown = true;
            }
            for k in 0..n {
                lay[((at >> 2) as usize + k) % WORDS] = known.as_ref().and_then(|ws| ws[k]);
            }
            let to = self.world(Rc::new(lay), chose, seg)?;
            return Some((to, st.clone()));
        }
        let mut s2 = st.clone();
        let bytes: Vec<Option<u8>> = match words {
            Some(ws) => ws
                .iter()
                .flat_map(|w| w.map_or([None; 4], |w| w.0.to_be_bytes().map(Some)))
                .take(total as usize)
                .collect(),
            None => vec![None; total as usize],
        };
        s2.store_bytes(at, &bytes);
        let to = if chose == self.worlds[wi].chose {
            wi
        } else {
            let lay = self.worlds[wi].layout.clone();
            self.world(lay, chose, seg)?
        };
        Some((to, s2))
    }
}

/// The registers each word reads (`.0`) and writes (`.1`), as masks.
fn uses(w: u32) -> (u32, u32) {
    let (op, rs, rt, rd) = (w >> 26, w >> 21 & 31, w >> 16 & 31, w >> 11 & 31);
    let b = |r: u32| 1u32 << r;
    let (r, wr) = match op {
        0 => match w & 63 {
            0 | 2 | 3 => (b(rt), b(rd)),
            8 => (b(rs), 0),
            9 => (b(rs), b(rd)),
            13 => (0, 0),
            _ => (b(rs) | b(rt), b(rd)),
        },
        1 => (b(rs), if rt & 0x10 != 0 { b(31) } else { 0 }),
        2 => (0, 0),
        3 => (0, b(31)),
        4 | 5 => (b(rs) | b(rt), 0),
        6 | 7 => (b(rs), 0),
        15 => (0, b(rt)),
        8..=14 | 32..=39 => (b(rs), b(rt)),
        40..=47 => (b(rs) | b(rt), 0),
        16 | 18 => match rs {
            0..=2 => (0, b(rt)),
            4..=6 => (b(rt), 0),
            _ => (0, 0),
        },
        50 | 58 => (b(rs), 0),
        _ => (0, 0),
    };
    (r & !1, wr & !1)
}

/// The registers the code of layout `lay` at word `pc` may read before it
/// writes them, along its paths up to the next jump.
fn live_in(lay: &[Word], pc: u16) -> u32 {
    let mut live = 0u32;
    let mut seen = BTreeSet::new();
    let mut work = vec![(pc, 0u32)];
    while let Some((i, written)) = work.pop() {
        if seen.len() > 256 || !seen.insert((i, written)) {
            continue;
        }
        let Some(w) = lay[usize::from(i) % WORDS].map(|w| w.0) else {
            continue;
        };
        let Some(enc) = decode::decode(w) else {
            continue;
        };
        let (r, wr) = uses(w);
        live |= r & !written;
        let mut written = written | wr;
        let next = (i + 1) % WORDS as u16;
        if !enc.has_delay_slot() {
            if enc != Encoding::Break {
                work.push((next, written));
            }
            continue;
        }
        if let Some(s) = lay[usize::from(next)].map(|w| w.0) {
            let (r2, w2) = uses(s);
            live |= r2 & !written;
            written |= w2;
        }
        let after = (next + 1) % WORDS as u16;
        let rel = (i32::from(i) + 1 + i32::from(w as u16 as i16)).rem_euclid(WORDS as i32) as u16;
        match enc {
            Encoding::J | Encoding::Jal | Encoding::Jr | Encoding::Jalr => {}
            _ => {
                work.push((rel, written));
                work.push((after, written));
            }
        }
    }
    live
}

/// The SP address, length word (when known) and RDRAM address of each DMA
/// `d` may be.
fn triples(d: Dma) -> Vec<(u32, Option<u32>, V)> {
    let lens: Vec<Option<u32>> = match d.len.vals() {
        Some(a) => a.iter().map(|x| Some(*x)).collect(),
        None => vec![None],
    };
    let drams: Vec<V> = match d.dram.vals() {
        Some(a) => a.iter().map(|x| V::k(*x)).collect(),
        None => vec![d.dram],
    };
    let mut out = Vec::new();
    for &m in d.mem.vals().unwrap_or(&[]) {
        for &l in &lens {
            for &r in &drams {
                out.push((m, l, r));
            }
        }
    }
    out
}

/// Runs the instruction `w` (`enc`, no branch) on `st`; the SP DMA it
/// starts, if any.
fn exec(st: &mut St, w: u32, enc: Encoding, inits: &mut BTreeSet<u16>) -> Option<Dma> {
    use Encoding as E;
    let rs = w >> 21 & 31;
    let rt = w >> 16 & 31;
    let rd = w >> 11 & 31;
    let sa = w >> 6 & 31;
    let imm = w & 0xFFFF;
    let s = st.get(rs);
    let t = st.get(rt);
    let k = V::k;
    let v = match enc {
        E::Add | E::Addu => Some((rd, s.add(t))),
        E::Sub | E::Subu => Some((rd, s.sub(t))),
        E::Addi | E::Addiu => Some((rt, s.add(k(simm(w))))),
        E::And => Some((rd, s.and(t))),
        E::Or => Some((rd, s.map2(t, |a, b| a | b))),
        E::Xor => Some((rd, s.map2(t, |a, b| a ^ b))),
        E::Nor => Some((rd, s.map2(t, |a, b| !(a | b)))),
        E::Andi => Some((rt, s.and(k(imm)))),
        E::Ori if imm == 0 => Some((rt, s)),
        E::Ori => Some((rt, s.map(|a| a | imm))),
        E::Xori => Some((rt, s.map(|a| a ^ imm))),
        E::Lui => Some((rt, k(imm << 16))),
        E::Sll => Some((rd, t.sll(sa))),
        E::Srl => Some((rd, t.srl(sa))),
        E::Sra => Some((rd, t.sra(sa))),
        E::Sllv => Some((rd, t.map2(s, |a, b| a << (b & 31)))),
        E::Srlv => Some((rd, t.map2(s, |a, b| a >> (b & 31)))),
        E::Srav => Some((rd, t.map2(s, |a, b| ((a as i32) >> (b & 31)) as u32))),
        E::Slt => Some((rd, s.map2(t, |a, b| u32::from((a as i32) < (b as i32))))),
        E::Sltu => Some((rd, s.map2(t, |a, b| u32::from(a < b)))),
        E::Slti => Some((rt, s.map(|a| u32::from((a as i32) < (simm(w) as i32))))),
        E::Sltiu => Some((rt, s.map(|a| u32::from(a < simm(w))))),
        E::Lb | E::Lbu | E::Lh | E::Lhu | E::Lw | E::Lwu => {
            let n = match enc {
                E::Lb | E::Lbu => 1,
                E::Lh | E::Lhu => 2,
                _ => 4,
            };
            let a = s.add(k(simm(w)));
            Some((rt, st.load(a, n, matches!(enc, E::Lb | E::Lh), inits)))
        }
        E::Sb | E::Sh | E::Sw => {
            let n = match enc {
                E::Sb => 1,
                E::Sh => 2,
                _ => 4,
            };
            st.store(s.add(k(simm(w))), n, t);
            None
        }
        E::Mfc0 | E::Mfc2 | E::Cfc2 => Some((rt, V::Top)),
        E::Mtc0 => {
            match rd & 15 {
                0 => st.mem = t,
                1 => st.dram = t,
                2 => {
                    return Some(Dma {
                        mem: st.mem,
                        dram: st.dram,
                        len: t,
                    });
                }
                _ => {}
            }
            None
        }
        _ => {
            if enc.class() == "vmem" && w >> 29 == 7 {
                let size = match (w >> 11) & 31 {
                    0 => 1,
                    1 => 2,
                    2 => 4,
                    3 => 8,
                    _ => 16,
                };
                let off = (((w & 0x7F) << 25) as i32 >> 25) as u32;
                if let Some(addrs) = s.add(k(off.wrapping_mul(size))).vals() {
                    for &x in addrs {
                        st.store_bytes(x, &vec![None; size as usize]);
                    }
                }
            }
            None
        }
    };
    if let Some((r, x)) = v {
        st.set(r, x);
    }
    None
}

/// The IMEM ranges the DMAs of a world and of those it goes on to write,
/// and whether one of them could not tell.
type Reach = (BTreeSet<(u32, u32)>, bool);

/// The layouts of every start, merged by their words.
pub(crate) fn layouts(cpu: &Cpu<'_>) -> Vec<RspLayout> {
    let in_code = |w: &Word| {
        w.is_some_and(|(_, s, o)| {
            cpu.code
                .iter()
                .any(|&(cs, a, b)| cs == s && (a..b).contains(&o))
        })
    };
    let mut all: Vec<Cand> = cpu.partners.keys().copied().collect();
    all.sort_unstable();
    let codes: Vec<Cand> = all
        .into_iter()
        .filter(|c| {
            read(cpu, c.0, phys(*c), 1, 64, 0).is_some_and(|ws| in_code(&ws[0]) && code_run(&ws))
        })
        .collect();
    let mut starts: Vec<(u32, Vec<Word>, u32, Option<Cand>)> = Vec::new();
    for &(seg, imem, dram, lw) in &cpu.constant {
        let rows = (lw >> 12 & 0xFF) + 1;
        let row = (lw & 0xFF8) + 8;
        let skip = lw >> 20 & 0xFF8;
        if let Some(ws) = read(cpu, seg, dram & 0x00FF_FFF8, rows, row, skip) {
            starts.push((imem & 0xFF8, ws, seg, None));
        }
    }
    for &imem in &cpu.computed {
        let at = imem & 0xFF8;
        for &c in &codes {
            let Some(ws) = read(cpu, c.0, phys(c), 1, 0x1000 - at, 0) else {
                continue;
            };
            let n = ws
                .iter()
                .take_while(|w| w.is_some_and(|w| is_code(w.0)))
                .count();
            starts.push((at, ws[..n].to_vec(), c.0, Some(c)));
        }
    }
    let mut merged: BTreeMap<Vec<RspPart>, Reach> = BTreeMap::new();
    for (at, ws, seg, start) in starts {
        let mut lay = vec![None; WORDS];
        for (k, w) in ws.into_iter().enumerate().take(WORDS) {
            lay[((at >> 2) as usize + k) % WORDS] = w;
        }
        let mut run = Run {
            cpu,
            codes: &codes,
            start,
            worlds: Vec::new(),
            index: HashMap::new(),
            steps: 0,
            limited: false,
            inits: BTreeSet::new(),
        };
        let Some(w0) = run.world(Rc::new(lay), Vec::new(), seg) else {
            continue;
        };
        run.add(w0, at, St::new());
        run.run();
        if start.is_some() && run.worlds.len() < 2 {
            continue;
        }
        let n = run.worlds.len();
        let mut reach: Vec<Reach> = run
            .worlds
            .iter()
            .map(|w| (w.ranges.clone(), w.unknown || run.limited))
            .collect();
        loop {
            let mut changed = false;
            for i in 0..n {
                for &j in &run.worlds[i].next {
                    let (r, u) = reach[j].clone();
                    let before = (reach[i].0.len(), reach[i].1);
                    reach[i].0.extend(r);
                    reach[i].1 |= u;
                    changed |= before != (reach[i].0.len(), reach[i].1);
                }
            }
            if !changed {
                break;
            }
        }
        for (w, (r, u)) in run.worlds.iter().zip(reach) {
            if w.states.is_empty() || w.forked {
                continue;
            }
            let e = merged.entry(parts(&w.layout)).or_default();
            e.0.extend(r);
            e.1 |= u;
        }
    }
    merged
        .into_iter()
        .filter(|(p, _)| !p.is_empty())
        .map(|(parts, (ranges, unknown))| RspLayout {
            parts,
            ranges: ranges.into_iter().collect(),
            unknown,
        })
        .collect()
}

/// The runs of words of `lay` from one place of a source.
fn parts(lay: &[Word]) -> Vec<RspPart> {
    let mut out: Vec<RspPart> = Vec::new();
    for (i, w) in lay.iter().enumerate() {
        let Some((_, source, offset)) = *w else {
            continue;
        };
        let imem = (i * 4) as u32;
        match out.last_mut() {
            Some(p)
                if p.imem + p.len == imem && p.source == source && p.offset + p.len == offset =>
            {
                p.len += 4;
            }
            _ => out.push(RspPart {
                imem,
                source,
                offset,
                len: 4,
            }),
        }
    }
    out
}

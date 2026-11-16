//! The driver: the boot segments and the PI DMA fixed point, the roots,
//! the source-wide scan and its regions, the sweep, the units and the RSP
//! images.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet, VecDeque};
use std::fmt;

use relift_cpu_mips::decode::{self as mdecode, Encoding};
use relift_cpu_mips::lift;
use relift_ir::Next;
use relift_n64_formats::{Cic, Rom};

use crate::boot;
use crate::eval;
use crate::func::{self, DmaConst, Env, FnRun, Loc, Summary, valid};
use crate::image::{FLAT_BASE, Image, Segment, SegmentOrigin, Source, Sources};
use crate::report::{
    Function, Origin, PiDma, Report, RspImage, RspOrigin, Stats, Unit, Unresolved, fnv,
};
use crate::scan::{self, Words};
use crate::val::{PHYS, SLOTS, State, sext32};

/// Why the analysis did not run.
#[derive(Debug)]
pub enum AnalyzeError {
    /// The image is not a ROM in a known byte order.
    Format(relift_n64_formats::Error),
    /// No segment holds the entry point.
    NoEntry(u32),
}

impl fmt::Display for AnalyzeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AnalyzeError::Format(e) => write!(f, "{e}"),
            AnalyzeError::NoEntry(e) => {
                write!(f, "no loaded segment holds the entry point {e:#010x}")
            }
        }
    }
}

impl std::error::Error for AnalyzeError {}

/// What the caller can choose.
#[derive(Clone, Copy, Debug, Default)]
pub struct Options {
    /// Boot as this variant instead of the one the boot code hash names.
    pub cic: Option<Cic>,
}

/// The exception vectors with `Status.BEV` clear: TLB refill, 64-bit TLB
/// refill, cache error and the general vector.
const VECTORS: [u32; 4] = [0x8000_0000, 0x8000_0080, 0xA000_0100, 0x8000_0180];

/// The most rounds of the PI DMA fixed point.
const MAX_ROUNDS: u32 = 8;
/// The most rounds of address-taken roots.
const MAX_TAKEN_ROUNDS: u32 = 8;
/// The most runs of one function.
const MAX_RUNS: u32 = 6;
/// Run words a region needs before the sweep takes all its words.
const SWEEP_SUPPORT: usize = 64;
/// Physical addresses below this are RDRAM; code linked above it must be
/// moved before it runs, so its own address fields are relocations.
const RDRAM_END: u32 = 0x0080_0000;

/// Analyzes a ROM image in any byte order.
///
/// # Errors
///
/// The image is no ROM, or no segment holds the entry point.
pub fn analyze(rom: &[u8]) -> Result<Report, AnalyzeError> {
    analyze_with(rom, Options::default())
}

/// [`analyze`] with options.
///
/// # Errors
///
/// As [`analyze`].
pub fn analyze_with(rom: &[u8], opts: Options) -> Result<Report, AnalyzeError> {
    let rom = Rom::new(rom).map_err(AnalyzeError::Format)?;
    let cic = opts.cic.or_else(|| rom.cic());
    let (boot_kind, entry, boot_segs) = boot::boot(&rom, cic);
    let streams: Vec<_> = rom
        .streams()
        .into_iter()
        .filter_map(|s| {
            let d = s.kind.decode(&rom.bytes[s.offset..]).ok()?;
            Some((s, d))
        })
        .collect();
    let src = Sources {
        rom: &rom.bytes,
        streams,
    };
    let mut stats = Stats::default();

    let mut known = boot_segs;
    known.extend(tables(&rom.bytes));
    let mut taken: BTreeSet<Loc> = BTreeSet::new();
    let rom_len = rom.bytes.len() as u32;
    loop {
        stats.rounds += 1;
        let mut d = Driver::new(Image {
            src: &src,
            segs: known.clone(),
        });
        d.start(entry, &taken, &[])?;
        taken.extend(d.taken());
        let mut new: Vec<Segment> = vector_copies(&d.image)
            .into_iter()
            .filter(|v| !known.contains(v))
            .collect();
        for (loc, run) in d.runs() {
            for (site, dma) in &run.dma {
                if let DmaConst::Pi { dram, cart, len } = *dma
                    && let Some(off) = cart.checked_sub(0x1000_0000)
                    && off < rom_len
                    && dram < RDRAM_END
                {
                    let seg = Segment {
                        source: Source::Rom,
                        offset: off,
                        len: len.min(rom_len - off) & !3,
                        link: Some(0x8000_0000 | dram),
                        origin: SegmentOrigin::PiDma { site: *site },
                    };
                    let _ = loc;
                    let dup = known
                        .iter()
                        .chain(&new)
                        .any(|k| k.offset == seg.offset && k.len == seg.len && k.link == seg.link);
                    if !dup && seg.len > 0 {
                        new.push(seg);
                    }
                }
            }
        }
        if new.is_empty() || stats.rounds >= MAX_ROUNDS {
            break;
        }
        known.extend(new);
    }

    let words: Vec<Words> = src.all().map(|s| Words::new(src.bytes(s))).collect();
    let mut segs = known.clone();
    let mut areas: Vec<(u32, usize, usize)> = Vec::new();
    let mut moved: Vec<Segment> = Vec::new();
    for (si, s) in src.all().enumerate() {
        let w = &words[si];
        let from = if s == Source::Rom {
            boot::BOOT_OFFSET as usize / 4
        } else {
            0
        };
        for r in scan::regions(w, from) {
            stats.regions += 1;
            let mut pieces = vec![r];
            let tables_first = known
                .iter()
                .enumerate()
                .filter(|(_, k)| matches!(k.origin, SegmentOrigin::Table { .. }))
                .chain(
                    known
                        .iter()
                        .enumerate()
                        .filter(|(_, k)| !matches!(k.origin, SegmentOrigin::Table { .. })),
                );
            for (ki, k) in tables_first {
                if k.source != s {
                    continue;
                }
                let (ka, kb) = (k.offset as usize / 4, (k.offset + k.len) as usize / 4);
                let mut rest = Vec::new();
                for (a, b) in pieces {
                    let (ia, ib) = (a.max(ka), b.min(kb));
                    if ia < ib {
                        areas.push((ki as u32, ia - ka, ib - ka));
                        if matches!(k.origin, SegmentOrigin::Table { .. }) {
                            rest.push((a, b));
                            continue;
                        }
                        let here = k.addr(4 * (ia - ka) as u32);
                        let st = scan::starts(w, (ia, ib), here.wrapping_sub(4 * ia as u32));
                        if let Some((v, votes)) = scan::vote(w, (ia, ib), &st)
                            && v != here
                        {
                            stats.linked_regions += 1;
                            moved.push(Segment {
                                source: s,
                                offset: 4 * ia as u32,
                                len: 4 * (ib - ia) as u32,
                                link: Some(v),
                                origin: SegmentOrigin::Region { votes },
                            });
                        }
                        if a < ia {
                            rest.push((a, ia));
                        }
                        if ib < b {
                            rest.push((ib, b));
                        }
                    } else {
                        rest.push((a, b));
                    }
                }
                pieces = rest;
            }
            for p in pieces {
                if p.1 - p.0 < 16 {
                    continue;
                }
                let st = scan::starts(w, p, FLAT_BASE.wrapping_sub(4 * p.0 as u32));
                let link = scan::vote(w, p, &st);
                if link.is_some() {
                    stats.linked_regions += 1;
                }
                segs.push(Segment {
                    source: s,
                    offset: 4 * p.0 as u32,
                    len: 4 * (p.1 - p.0) as u32,
                    link: link.map(|l| l.0),
                    origin: SegmentOrigin::Region {
                        votes: link.map_or(0, |l| l.1),
                    },
                });
            }
        }
    }
    segs.extend(moved);
    let nknown = known.len();
    let mut prologues = Vec::new();
    for (i, s) in segs.iter().enumerate() {
        let si = source_index(s.source);
        let w = &words[si];
        let a = s.offset as usize / 4;
        let ranges: Vec<(usize, usize)> = if i < nknown {
            areas
                .iter()
                .filter(|x| x.0 as usize == i)
                .map(|x| (a + x.1, a + x.2))
                .collect()
        } else {
            vec![(a, a + s.len as usize / 4)]
        };
        for r in ranges {
            let base = s.base().wrapping_sub(4 * a as u32);
            for st in scan::starts(w, r, base) {
                prologues.push((i as u32, base.wrapping_add(4 * st as u32)));
            }
        }
    }
    let mut d = Driver::new(Image { src: &src, segs });
    d.start(entry, &taken, &prologues)?;
    d.reject_data();
    stats.runs = d.nruns;
    stats.rejected = d.rejected.len() as u32;
    Ok(d.report(boot_kind, entry, &words, &areas, nknown, stats))
}

fn source_index(s: Source) -> usize {
    match s {
        Source::Rom => 0,
        Source::Stream(i) => i as usize + 1,
    }
}

struct Func {
    origin: Origin,
    run: Option<FnRun>,
    callers: Vec<Loc>,
    runs: u32,
}

struct Driver<'a> {
    image: Image<'a>,
    funcs: HashMap<Loc, Func>,
    summaries: HashMap<Loc, Summary>,
    entries: HashSet<Loc>,
    queue: VecDeque<Loc>,
    queued: HashSet<Loc>,
    rejected: HashSet<Loc>,
    nruns: u64,
}

impl<'a> Driver<'a> {
    fn new(image: Image<'a>) -> Driver<'a> {
        Driver {
            image,
            funcs: HashMap::new(),
            summaries: HashMap::new(),
            entries: HashSet::new(),
            queue: VecDeque::new(),
            queued: HashSet::new(),
            rejected: HashSet::new(),
            nruns: 0,
        }
    }

    /// Runs from the entry, the exception trampolines, the address-taken
    /// roots of an earlier round and `extra`, then from the constant code
    /// addresses found, to a fixed point.
    fn start(
        &mut self,
        entry: u32,
        taken: &BTreeSet<Loc>,
        extra: &[Loc],
    ) -> Result<(), AnalyzeError> {
        let segs: Vec<u32> = (0..self.image.segs.len() as u32)
            .filter(|i| {
                let s = &self.image.segs[*i as usize];
                s.shared() && s.holds(entry)
            })
            .collect();
        if segs.is_empty() {
            return Err(AnalyzeError::NoEntry(entry));
        }
        for s in segs {
            self.root((s, entry), Origin::Entry);
        }
        for i in 0..self.image.segs.len() as u32 {
            if self.image.segs[i as usize].shared() {
                for v in VECTORS {
                    if self.image.segs[i as usize].holds(v)
                        && self.image.word(i, v).is_some_and(|w| w != 0 && valid(w))
                    {
                        self.root((i, v), Origin::Exception);
                    }
                }
                for (_, _, t) in trampolines(&self.image, i) {
                    for s in self.image.targets(i, t) {
                        self.root((s, t), Origin::Exception);
                    }
                }
            }
        }
        for l in taken {
            if (l.0 as usize) < self.image.segs.len() {
                self.root(*l, Origin::AddressTaken);
            }
        }
        self.drain();
        for l in extra {
            self.root(*l, Origin::Prologue);
        }
        self.drain();
        for _ in 0..MAX_TAKEN_ROUNDS {
            if self.address_taken() == 0 {
                break;
            }
            self.drain();
        }
        Ok(())
    }

    fn root(&mut self, l: Loc, origin: Origin) {
        if self.rejected.contains(&l) || self.funcs.contains_key(&l) {
            return;
        }
        self.funcs.insert(
            l,
            Func {
                origin,
                run: None,
                callers: Vec::new(),
                runs: 0,
            },
        );
        self.entries.insert(l);
        if self.queued.insert(l) {
            self.queue.push_back(l);
        }
    }

    fn reject(&mut self, l: Loc) {
        self.funcs.remove(&l);
        self.entries.remove(&l);
        self.rejected.insert(l);
    }

    fn drain(&mut self) {
        while let Some(f) = self.queue.pop_front() {
            self.queued.remove(&f);
            let Some(func) = self.funcs.get_mut(&f) else {
                continue;
            };
            func.runs += 1;
            let first = func.run.is_none();
            let origin = func.origin;
            let env = Env {
                image: &self.image,
                summaries: &self.summaries,
                entries: &self.entries,
            };
            let mut r = func::run(&env, f);
            self.nruns += 1;
            let speculative = matches!(origin, Origin::AddressTaken | Origin::Prologue);
            let zero_entry = self.image.word(f.0, f.1) == Some(0);
            if first && speculative && (r.invalid || r.body.is_empty() || zero_entry) {
                self.reject(f);
                continue;
            }
            let callees: Vec<(Loc, Origin)> = r
                .calls
                .iter()
                .map(|c| (c.1, Origin::Call))
                .chain(r.tails.iter().map(|t| (*t, Origin::Tail)))
                .collect();
            for (c, o) in callees {
                self.root(c, o);
                if let Some(cf) = self.funcs.get_mut(&c)
                    && !cf.callers.contains(&f)
                {
                    cf.callers.push(f);
                }
            }
            let s = r.summary.take().unwrap_or_else(Summary::assumed);
            let prev = self
                .summaries
                .get(&f)
                .cloned()
                .unwrap_or_else(Summary::assumed);
            self.summaries.insert(f, s.clone());
            let callers = match self.funcs.get_mut(&f) {
                Some(func) => {
                    func.run = Some(r);
                    func.callers.clone()
                }
                None => Vec::new(),
            };
            if prev != s {
                for c in callers {
                    let again = self.funcs.get(&c).is_some_and(|cf| cf.runs < MAX_RUNS);
                    if again && self.queued.insert(c) {
                        self.queue.push_back(c);
                    }
                }
            }
        }
    }

    fn runs(&self) -> impl Iterator<Item = (Loc, &FnRun)> {
        self.funcs
            .iter()
            .filter_map(|(l, f)| f.run.as_ref().map(|r| (*l, r)))
    }

    fn found(&self) -> HashSet<Loc> {
        let mut out = HashSet::new();
        for (l, r) in self.runs() {
            for &(pc, len) in &r.body {
                out.insert((l.0, pc));
                if len == 8 {
                    out.insert((l.0, pc.wrapping_add(4)));
                }
            }
        }
        out
    }

    /// Roots at the constant code addresses the runs computed that no run
    /// reached; how many.
    fn address_taken(&mut self) -> usize {
        let found = self.found();
        let bases: HashSet<u32> = self
            .runs()
            .flat_map(|(_, r)| r.bases.iter().copied())
            .collect();
        let mut new = BTreeSet::new();
        for (l, r) in self.runs() {
            for &c in &r.consts {
                if bases.contains(&(c & PHYS)) {
                    continue;
                }
                for s in self.image.targets(l.0, c) {
                    let loc = (s, c);
                    if !found.contains(&loc)
                        && !self.funcs.contains_key(&loc)
                        && !self.rejected.contains(&loc)
                        && self.image.word(s, c).is_some_and(valid)
                    {
                        new.insert(loc);
                    }
                }
            }
        }
        let n = new.len();
        for l in new {
            self.root(l, Origin::AddressTaken);
        }
        n
    }

    fn taken(&self) -> Vec<Loc> {
        self.funcs
            .iter()
            .filter(|(_, f)| f.origin == Origin::AddressTaken && f.run.is_some())
            .map(|(l, _)| *l)
            .collect()
    }

    /// Rejects the speculative roots whose code overlaps words read as
    /// data.
    fn reject_data(&mut self) {
        let mut data: HashSet<(u32, u32)> = HashSet::new();
        for (_, r) in self.runs() {
            for &(s, rel, n) in &r.data {
                let mut o = rel & !3;
                while o < rel + u32::from(n) {
                    data.insert((s, o));
                    o += 4;
                }
            }
        }
        let bad: Vec<Loc> = self
            .funcs
            .iter()
            .filter(|(_, f)| matches!(f.origin, Origin::AddressTaken | Origin::Prologue))
            .filter(|(l, f)| {
                let seg = &self.image.segs[l.0 as usize];
                f.run.as_ref().is_some_and(|r| {
                    r.body.iter().any(|&(pc, _)| {
                        seg.offset_of(pc, false)
                            .is_some_and(|rel| data.contains(&(l.0, rel)))
                    })
                })
            })
            .map(|(l, _)| *l)
            .collect();
        for l in bad {
            self.reject(l);
        }
    }

    fn report(
        &self,
        boot: crate::report::Boot,
        entry: u32,
        words: &[Words],
        areas: &[(u32, usize, usize)],
        nknown: usize,
        mut stats: Stats,
    ) -> Report {
        let segs = &self.image.segs;
        let mut found: Vec<Vec<u8>> = segs.iter().map(|s| vec![0u8; s.len as usize / 4]).collect();
        for (l, r) in self.runs() {
            let s = &segs[l.0 as usize];
            for &(pc, len) in &r.body {
                for k in 0..u32::from(len) / 4 {
                    if let Some(rel) = s.offset_of(pc.wrapping_add(4 * k), false)
                        && let Some(f) = found[l.0 as usize].get_mut(rel as usize / 4)
                    {
                        *f = 1;
                    }
                }
            }
        }
        for (i, s) in segs.iter().enumerate() {
            let w = &words[source_index(s.source)];
            let a = s.offset as usize / 4;
            let n = s.len as usize / 4;
            let ranges: Vec<(usize, usize)> = if i < nknown {
                areas
                    .iter()
                    .filter(|x| x.0 as usize == i)
                    .map(|x| (x.1, x.2))
                    .collect()
            } else {
                vec![(0, n)]
            };
            for (ra, rb) in ranges {
                let support = found[i][ra..rb].iter().filter(|f| **f != 0).count();
                if support < SWEEP_SUPPORT.min(rb - ra) {
                    continue;
                }
                let (ga, gb) = scan::grow(w, (a + ra, a + rb), a, a + n);
                for j in ga..gb {
                    if w.valid(j) && found[i][j - a] == 0 {
                        found[i][j - a] = 2;
                    }
                }
            }
        }
        let mut units = Vec::new();
        for (i, s) in segs.iter().enumerate() {
            let w = &words[source_index(s.source)];
            let a = s.offset as usize / 4;
            let f = &found[i];
            let masks = match s.link {
                Some(l) if l & PHYS >= RDRAM_END => scan::reloc_masks(&w.w[a..a + f.len()], l),
                _ => vec![0; f.len()],
            };
            let mut j = 0;
            while j < f.len() {
                if f[j] == 0 {
                    j += 1;
                    continue;
                }
                let k0 = j;
                while j < f.len() && f[j] != 0 {
                    j += 1;
                }
                let run = k0..j;
                stats.run_words += f[run.clone()].iter().filter(|x| **x == 1).count() as u64;
                let swept = f[run.clone()].iter().filter(|x| **x == 2).count() as u32;
                stats.swept_words += u64::from(swept);
                let hash = fnv(run
                    .clone()
                    .flat_map(|k| (w.w[a + k] & !masks[k]).to_be_bytes()));
                units.push(Unit {
                    segment: i as u32,
                    source: s.source,
                    offset: s.offset + 4 * k0 as u32,
                    len: 4 * (j - k0) as u32,
                    link: s.link.map(|l| l.wrapping_add(4 * k0 as u32)),
                    hash,
                    reloc: run.clone().filter(|k| masks[*k] != 0).count() as u32,
                    swept,
                });
            }
        }
        let mut functions: Vec<Function> = self
            .funcs
            .iter()
            .filter(|(_, f)| f.run.is_some())
            .map(|(l, f)| Function {
                segment: l.0,
                addr: l.1,
                origin: f.origin,
            })
            .collect();
        functions.sort_by_key(|f| (f.segment, f.addr));
        let mut unres: BTreeMap<(u32, u32, crate::report::UnresolvedKind), String> =
            BTreeMap::new();
        let mut pi = BTreeSet::new();
        let mut sp = BTreeSet::new();
        for (l, r) in self.runs() {
            for (site, kind, detail) in &r.unresolved {
                unres
                    .entry((l.0, *site, *kind))
                    .or_insert_with(|| detail.clone());
            }
            for (site, d) in &r.dma {
                match *d {
                    DmaConst::Pi { dram, cart, len } => {
                        pi.insert((l.0, *site, dram, cart, len));
                    }
                    DmaConst::Sp { mem, dram, len } => {
                        sp.insert((l.0, *site, mem, dram, len));
                    }
                }
            }
        }
        let unresolved = unres
            .into_iter()
            .map(|((segment, site, kind), detail)| Unresolved {
                segment,
                site,
                kind,
                detail,
            })
            .collect();
        let pi_dma = pi
            .into_iter()
            .map(|(segment, site, dram, cart, len)| PiDma {
                segment,
                site,
                dram,
                cart,
                len,
            })
            .collect();
        let rsp = self.rsp(&sp, words);
        Report {
            boot,
            entry,
            streams: self.image.src.streams.iter().map(|s| s.0).collect(),
            segments: segs.clone(),
            units,
            functions,
            unresolved,
            pi_dma,
            rsp,
            stats,
        }
    }

    fn rsp(&self, sp: &BTreeSet<(u32, u32, u32, u32, u32)>, words: &[Words]) -> Vec<RspImage> {
        let mut out = Vec::new();
        for &(seg, site, mem, dram, lw) in sp {
            if mem & 0x1000 == 0 {
                continue;
            }
            let len = ((lw & 0xFFF) + 1) * ((lw >> 12 & 0xFF) + 1);
            let Some((_, s, rel)) = self.image.read(seg, dram, 4) else {
                continue;
            };
            let sg = &self.image.segs[s as usize];
            let len = len.min(sg.len - rel);
            let bytes = self.image.src.bytes(sg.source);
            let at = (sg.offset + rel) as usize;
            out.push(RspImage {
                source: sg.source,
                offset: sg.offset + rel,
                len,
                imem: Some(mem & 0xFFF),
                hash: fnv(bytes[at..at + len as usize].iter().copied()),
                origin: RspOrigin::SpDma { site },
            });
        }
        for (si, s) in self.image.src.all().enumerate() {
            let bytes = self.image.src.bytes(s);
            for (a, b) in scan::rsp_regions(&words[si]) {
                let (o, e) = (4 * a, 4 * b);
                out.push(RspImage {
                    source: s,
                    offset: o as u32,
                    len: (e - o) as u32,
                    imem: None,
                    hash: fnv(bytes[o..e].iter().copied()),
                    origin: RspOrigin::Scan,
                });
            }
        }
        out
    }
}

/// The largest segment a segment table entry may give.
const TABLE_MAX: u32 = 0x40_0000;
/// The most segments the segment tables give.
const TABLE_CAP: usize = 4096;

/// The segments of the ROM's segment tables: three words in a row, the
/// start and end of a ROM range of code (at least 24 of its first 32 words
/// decode) and a kseg0 address in RDRAM to link it at, where at least one
/// `JAL` of the range's code reaches a function start of the range and the
/// vote of [`scan::vote`] links the range nowhere else.
fn tables(rom: &[u8]) -> Vec<Segment> {
    let n = rom.len() / 4;
    let word =
        |i: usize| u32::from_be_bytes([rom[4 * i], rom[4 * i + 1], rom[4 * i + 2], rom[4 * i + 3]]);
    let len = rom.len() as u32;
    let words = Words::new(rom);
    let mut out: Vec<Segment> = Vec::new();
    for i in 0..n.saturating_sub(2) {
        let (a, b, l) = (word(i), word(i + 1), word(i + 2));
        if a < boot::BOOT_OFFSET
            || a % 4 != 0
            || b <= a
            || b > len
            || b - a > TABLE_MAX
            || b - a < 0x40
            || !(0x8000_0400..0x8000_0000 + RDRAM_END).contains(&l)
            || l % 4 != 0
        {
            continue;
        }
        let first = a as usize / 4;
        let k = (first..first + 32).filter(|&j| valid(word(j))).count();
        if k < 24 {
            continue;
        }
        let r = (first, b as usize / 4);
        let base = l.wrapping_sub(4 * first as u32);
        let starts_at: Vec<usize> = scan::starts(&words, r, base);
        let starts: HashSet<u32> = starts_at
            .iter()
            .map(|&s| base.wrapping_add(4 * s as u32))
            .collect();
        let linked = (r.0..r.1).any(|j| {
            let x = word(j);
            x >> 26 == 3 && starts.contains(&(0x8000_0000 | (x & 0x03FF_FFFF) << 2))
        });
        let other = scan::vote(&words, r, &starts_at).is_some_and(|(v, _)| v != l);
        if !linked || other {
            continue;
        }
        let seg = Segment {
            source: Source::Rom,
            offset: a,
            len: (b - a) & !3,
            link: Some(l),
            origin: SegmentOrigin::Table { at: 4 * i as u32 },
        };
        if !out
            .iter()
            .any(|s| s.offset == seg.offset && s.len == seg.len && s.link == seg.link)
        {
            out.push(seg);
            if out.len() == TABLE_CAP {
                break;
            }
        }
    }
    out
}

/// The trampolines of segment `seg` as (address, length in bytes, target):
/// a run of up to four instructions from a `LUI` that writes only `k0` and
/// `k1` and ends in a jump through a register to a constant, with its delay
/// slot, the shape of an exception vector that a program copies to the
/// vector addresses.
fn trampolines(image: &Image<'_>, seg: u32) -> Vec<(u32, u32, u32)> {
    let s = &image.segs[seg as usize];
    let mut out = Vec::new();
    for rel in (0..s.len).step_by(4) {
        let pc0 = s.addr(rel);
        let Some(w0) = image.word(seg, pc0) else {
            continue;
        };
        if mdecode::decode(w0) != Some(Encoding::Lui) || !matches!(w0 >> 16 & 31, 26 | 27) {
            continue;
        }
        let mut st = State::entry();
        let mut pc = pc0;
        for _ in 0..4 {
            let (Some(w), slot) = (image.word(seg, pc), image.word(seg, pc.wrapping_add(4))) else {
                break;
            };
            let Ok(l) = lift::lift(sext32(pc) as u64, w, slot.unwrap_or(0)) else {
                break;
            };
            let o = eval::eval(&l.block, &st, &mut |_, _| None);
            let only_k = (1..SLOTS).all(|i| i == 26 || i == 27 || o.st.regs[i] == st.regs[i]);
            if !only_k || o.end != eval::End::Exit {
                break;
            }
            match l.block.exit.next {
                Next::Dynamic(_) => {
                    if let [crate::val::Abs::K(t)] = o.targets[..] {
                        out.push((pc0, pc.wrapping_add(8).wrapping_sub(pc0), t as u32));
                    }
                    break;
                }
                Next::Const(n) if n == sext32(pc.wrapping_add(l.block.len)) as u64 => {
                    st = o.st;
                    pc = pc.wrapping_add(l.block.len);
                }
                _ => break,
            }
        }
    }
    out.sort_unstable();
    out.dedup();
    out
}

/// The segments that place each trampoline of the shared segments of
/// `image` at every exception vector, for the copies a program makes.
fn vector_copies(image: &Image<'_>) -> Vec<Segment> {
    let mut out: Vec<Segment> = Vec::new();
    for i in 0..image.segs.len() as u32 {
        let s = &image.segs[i as usize];
        if !s.shared() || matches!(s.origin, SegmentOrigin::Vector { .. }) {
            continue;
        }
        for (at, len, _) in trampolines(image, i) {
            let Some(offset) = s.source_offset(at) else {
                continue;
            };
            for v in VECTORS {
                let seg = Segment {
                    source: s.source,
                    offset,
                    len,
                    link: Some(v & !0x2000_0000),
                    origin: SegmentOrigin::Vector { from: at },
                };
                if !out.contains(&seg) {
                    out.push(seg);
                }
            }
        }
    }
    out
}

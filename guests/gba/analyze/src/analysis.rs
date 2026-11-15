//! The driver: roots, the fixed point over function runs and the report.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fmt;
use std::rc::Rc;

use relift_gba_formats::header::{MAX_ROM, MULTIBOOT_BASE, MULTIBOOT_ENTRY};
use relift_gba_formats::{Error as FormatError, Header};

use crate::func::{self, Env, FnRun, Lifted, Summary};
use crate::image::Image;
use crate::loc::Loc;
use crate::report::{CopyKind, Function, Origin, RamCopy, Report, UnresolvedKind};
use relift_cpu_arm32::regs::{self, Mode};

use crate::val::{Abs, State};

/// Why an image cannot be analyzed.
#[derive(Debug)]
pub enum AnalyzeError {
    /// The header does not parse.
    Format(FormatError),
    /// The image is larger than the cartridge space.
    TooLarge(usize),
    /// The image has no multiboot entry, or is larger than work RAM.
    NotMultiboot,
}

impl fmt::Display for AnalyzeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AnalyzeError::Format(e) => write!(f, "{e}"),
            AnalyzeError::TooLarge(n) => {
                write!(f, "image of {n} bytes exceeds the cartridge space")
            }
            AnalyzeError::NotMultiboot => {
                f.write_str("no multiboot entry branch at 0xC0, or larger than work RAM")
            }
        }
    }
}

impl std::error::Error for AnalyzeError {}

/// The reset entry.
pub const RESET: u32 = 0x0800_0000;
/// Discovery rounds before the table stops are taken as they are.
const MAX_ROUNDS: usize = 8;
/// Function runs per function before the fixed point gives up.
const MAX_RUNS_PER_FUNCTION: usize = 1024;

/// Find the functions of the image `rom` run as a cartridge, from the
/// reset entry at `0x08000000`.
///
/// # Errors
///
/// The header does not parse, or the image is larger than 32 MiB.
pub fn analyze(rom: &[u8]) -> Result<Report, AnalyzeError> {
    if rom.len() > MAX_ROM {
        return Err(AnalyzeError::TooLarge(rom.len()));
    }
    Header::parse(rom).map_err(AnalyzeError::Format)?;
    Ok(discover(Image::new(rom), Loc::rom(RESET, false)))
}

/// Find the functions of the multiboot image `rom` as the BIOS runs it:
/// loaded to `0x02000000` in external work RAM and entered at its
/// entry word at `0xC0`, with no cartridge. The image is copy 0 of the
/// report, of kind [`crate::CopyKind::Load`].
///
/// # Errors
///
/// The header does not parse, or the image has no multiboot entry branch
/// or does not fit work RAM.
pub fn analyze_multiboot(rom: &[u8]) -> Result<Report, AnalyzeError> {
    let h = Header::parse(rom).map_err(AnalyzeError::Format)?;
    if h.multiboot_entry.is_none() {
        return Err(AnalyzeError::NotMultiboot);
    }
    let mut image = Image::new(&[]);
    let site = Loc {
        addr: MULTIBOOT_BASE + MULTIBOOT_ENTRY as u32,
        thumb: false,
        copy: Some(0),
    };
    image.copies.push(RamCopy {
        site,
        kind: CopyKind::Load,
        src: None,
        dst: MULTIBOOT_BASE,
        bytes: Rc::from(rom),
    });
    Ok(discover(image, site))
}

/// Runs discovery to its fixed point with the table stops known so far.
/// A ROM table runs at most to the next stop, and the stops (the bases of
/// other tables taken as constants, function entries) are only known once
/// the code is: when one found falls inside a table that was read past it, the
/// discovery starts again with it known, until no table is read past a
/// stop.
fn discover(image: Image<'_>, entry: Loc) -> Report {
    let mut stops = BTreeSet::new();
    let mut cache = RefCell::new(HashMap::new());
    let mut conc = RefCell::new(HashMap::new());
    for _ in 0..MAX_ROUNDS {
        let mut d = Driver::new(image.clone(), stops, cache, conc);
        d.add_root(entry, Origin::Entry);
        loop {
            d.solve();
            if !d.address_taken() {
                break;
            }
        }
        let found = d.found_stops();
        let cut = d
            .runs
            .values()
            .flat_map(|r| r.tab_reads.iter())
            .chain(d.scans.iter())
            .any(|&(base, end)| {
                found
                    .range(base.saturating_add(1)..end)
                    .any(|s| !d.stops.contains(s))
            });
        if !cut {
            return d.report();
        }
        stops = std::mem::take(&mut d.stops);
        stops.extend(found);
        cache = std::mem::take(&mut d.cache);
        conc = std::mem::take(&mut d.conc);
    }
    let mut d = Driver::new(image, stops, cache, conc);
    d.add_root(entry, Origin::Entry);
    loop {
        d.solve();
        if !d.address_taken() {
            break;
        }
    }
    d.report()
}

struct Driver<'a> {
    image: Image<'a>,
    roots: BTreeMap<Loc, Origin>,
    origins: HashMap<Loc, Origin>,
    entries: HashSet<Loc>,
    runs: HashMap<Loc, FnRun>,
    run_count: HashMap<Loc, usize>,
    summaries: HashMap<Loc, Summary>,
    ptrs: HashMap<u32, BTreeSet<u32>>,
    callers: HashMap<Loc, BTreeSet<Loc>>,
    /// The mode, masks, SPSRs and banked registers at each call site of a
    /// function, by function and site.
    site_psr: HashMap<Loc, BTreeMap<Loc, crate::val::Psr>>,
    /// Their join: what a function reached only through discovered edges
    /// starts with.
    entry_psr: HashMap<Loc, crate::val::Psr>,
    readers: HashMap<u32, BTreeSet<Loc>>,
    irq: BTreeSet<u32>,
    queue: BTreeSet<Loc>,
    cache: RefCell<HashMap<(Loc, u32), Lifted>>,
    conc: RefCell<crate::func::Runs>,
    no_tables: BTreeSet<Loc>,
    rejected: BTreeSet<Loc>,
    stops: BTreeSet<u32>,
    /// The address-taken tables found: base and the address after the last
    /// word read.
    scans: Vec<(u32, u32)>,
}

impl<'a> Driver<'a> {
    fn new(
        image: Image<'a>,
        stops: BTreeSet<u32>,
        cache: RefCell<HashMap<(Loc, u32), Lifted>>,
        conc: RefCell<crate::func::Runs>,
    ) -> Driver<'a> {
        Driver {
            image,
            roots: BTreeMap::new(),
            origins: HashMap::new(),
            entries: HashSet::new(),
            runs: HashMap::new(),
            run_count: HashMap::new(),
            summaries: HashMap::new(),
            ptrs: HashMap::new(),
            callers: HashMap::new(),
            site_psr: HashMap::new(),
            entry_psr: HashMap::new(),
            readers: HashMap::new(),
            irq: BTreeSet::new(),
            queue: BTreeSet::new(),
            cache,
            conc,
            no_tables: BTreeSet::new(),
            rejected: BTreeSet::new(),
            stops,
            scans: Vec::new(),
        }
    }

    /// The table stops the discovered code gives: the ROM addresses it
    /// both reads as a constant word and reads a table at (a base that is
    /// an offset into another table is no stop), and the ROM function
    /// entries.
    fn found_stops(&self) -> BTreeSet<u32> {
        let refs: HashSet<u32> = self
            .runs
            .values()
            .flat_map(|r| r.refs.iter().copied())
            .collect();
        let mut s: BTreeSet<u32> = self
            .runs
            .values()
            .flat_map(|r| r.tab_reads.iter().map(|t| t.0))
            .filter(|b| refs.contains(b))
            .chain(self.scans.iter().map(|t| t.0))
            .collect();
        s.extend(
            self.entries
                .iter()
                .filter(|l| l.copy.is_none())
                .map(|l| l.addr),
        );
        s
    }

    fn add_root(&mut self, l: Loc, o: Origin) {
        self.roots.entry(l).or_insert(o);
        self.add_entry(l, o);
    }

    fn add_entry(&mut self, l: Loc, o: Origin) {
        self.origins.entry(l).or_insert(o);
        if self.entries.insert(l) {
            self.queue.insert(l);
            for (g, r) in &self.runs {
                if *g != l && r.body.contains_key(&l) {
                    self.queue.insert(*g);
                }
            }
        }
    }

    fn resolve_irq(&mut self) {
        let addrs: Vec<u32> = self.irq.iter().copied().collect();
        for v in addrs {
            if let Ok(ls) = func::locs(&self.image, Loc::rom(RESET, false), v & !3, false) {
                for l in ls {
                    self.add_root(l, Origin::Irq);
                }
            }
        }
    }

    /// The code addresses discovered code takes: odd words in the image
    /// that discovered code reads as constants (Thumb functions), and the
    /// tables of such words a constant read points at. A candidate becomes
    /// an entry when its own run lifts without an undefined instruction,
    /// stays in code bytes and overlaps no constant read as data and no
    /// instruction found in the other state or at another alignment.
    fn address_taken(&mut self) -> bool {
        let mut data: HashSet<u32> = HashSet::new();
        let mut words: BTreeSet<u32> = BTreeSet::new();
        for r in self.runs.values() {
            for &(a, n) in &r.data {
                for i in 0..u32::from(n) {
                    data.insert(a.wrapping_add(i));
                }
                if n == 4 {
                    words.insert(a);
                }
            }
        }
        let mut code: HashMap<u32, (u32, bool)> = HashMap::new();
        for r in self.runs.values() {
            for (l, n) in &r.body {
                if l.copy.is_none() {
                    for i in 0..u32::from(*n) {
                        code.insert(l.addr.wrapping_add(i), (l.addr, l.thumb));
                    }
                }
            }
        }
        let mut cands: BTreeSet<u32> = BTreeSet::new();
        for r in self.runs.values() {
            cands.extend(r.escaped.iter().copied());
            for &(a, n) in &r.data {
                if n == 4 {
                    words.insert(a);
                }
            }
        }
        let word = |a: u32| self.image.read(None, a, 4);
        let mut scans = Vec::new();
        for &a in &words {
            let Some(v) = word(a) else { continue };
            if v & 1 == 1 && v >> 24 == 0x08 && self.image.in_image(v & !1) {
                cands.insert(v);
            }
            if v & 3 == 0 && self.image.in_image(v) {
                let mut t = v;
                while let Some(e) = word(t) {
                    if !(e & 1 == 1 && e >> 24 == 0x08 && self.image.in_image(e & !1))
                        || data.contains(&t) && !words.contains(&t)
                        || t > v && self.stops.range(v + 1..=t + 3).next().is_some()
                    {
                        break;
                    }
                    cands.insert(e);
                    t += 4;
                }
                if t > v {
                    scans.push((v, t + 4));
                }
            }
        }
        self.scans.extend(scans);
        let mut added = false;
        for v in cands {
            let l = Loc::rom(v & !1, true);
            if self.entries.contains(&l)
                || self.rejected.contains(&l)
                || code.get(&l.addr) == Some(&(l.addr, true))
            {
                continue;
            }
            let run = {
                let env = Env {
                    image: &self.image,
                    summaries: &self.summaries,
                    entries: &self.entries,
                    ptrs: &self.ptrs,
                    cache: &self.cache,
                    runs: &self.conc,
                    stops: &self.stops,
                };
                func::run(&env, l, State::entry(true), &self.no_tables)
            };
            let bad_insn = run.unresolved.iter().any(|u| {
                matches!(
                    u.kind,
                    UnresolvedKind::Undefined | UnresolvedKind::OutsideCode
                )
            });
            let overlap = run.body.iter().any(|(b, n)| {
                (0..u32::from(*n)).any(|i| {
                    let a = b.addr.wrapping_add(i);
                    data.contains(&a) || code.get(&a).is_some_and(|c| *c != (b.addr, b.thumb))
                })
            });
            if bad_insn || overlap {
                self.rejected.insert(l);
                continue;
            }
            self.add_root(l, Origin::AddressTaken);
            added = true;
        }
        added
    }

    fn solve(&mut self) {
        while let Some(f) = self.queue.pop_first() {
            let n = self.run_count.entry(f).or_insert(0);
            *n += 1;
            if *n > MAX_RUNS_PER_FUNCTION {
                continue;
            }
            let run = {
                let env = Env {
                    image: &self.image,
                    summaries: &self.summaries,
                    entries: &self.entries,
                    ptrs: &self.ptrs,
                    cache: &self.cache,
                    runs: &self.conc,
                    stops: &self.stops,
                };
                let mut st = State::entry(f.thumb);
                let edges_only =
                    matches!(self.origins.get(&f), Some(Origin::Called | Origin::Jumped));
                if let Some(p) = self.entry_psr.get(&f)
                    && edges_only
                    && !self.roots.contains_key(&f)
                {
                    st.set_psr(p);
                }
                match self.roots.get(&f) {
                    Some(Origin::Irq) => {
                        st.regs[regs::M.index()] = Abs::K(u128::from(Mode::Irq.bits()))
                    }
                    Some(Origin::Entry) => {
                        st.regs[regs::M.index()] = Abs::K(u128::from(Mode::Sys.bits()))
                    }
                    _ => {}
                }
                func::run(&env, f, st, &self.no_tables)
            };
            self.absorb(f, run);
        }
    }

    fn absorb(&mut self, f: Loc, run: FnRun) {
        if !run.bad_tables.is_empty() {
            self.no_tables.extend(run.bad_tables.iter().copied());
            self.queue.insert(f);
            return;
        }
        let sum = run.summary.clone().unwrap_or_else(Summary::unknown);
        if self.summaries.get(&f) != Some(&sum) {
            self.summaries.insert(f, sum);
            if let Some(cs) = self.callers.get(&f) {
                self.queue.extend(cs.iter().copied());
            }
        }
        for (e, p) in run.edges.iter().zip(&run.edge_psr) {
            let site_psr = self.site_psr.entry(e.target).or_default();
            if site_psr.insert(e.site, p.clone()) != Some(p.clone()) {
                let joined = site_psr
                    .values()
                    .cloned()
                    .reduce(|a, b| a.iter().zip(&b).map(|(x, y)| x.join(*y)).collect());
                if let Some(j) = joined
                    && self.entry_psr.get(&e.target) != Some(&j)
                {
                    self.entry_psr.insert(e.target, j);
                    self.queue.insert(e.target);
                }
            }
        }
        for e in &run.edges {
            self.callers.entry(e.target).or_default().insert(f);
            let o = if e.kind == crate::report::EdgeKind::Tail {
                Origin::Jumped
            } else {
                Origin::Called
            };
            self.add_entry(e.target, o);
        }
        for &a in &run.reads {
            self.readers.entry(a).or_default().insert(f);
        }
        for &(a, v) in &run.ptr_stores {
            if self.ptrs.entry(a).or_default().insert(v)
                && let Some(rs) = self.readers.get(&a)
            {
                self.queue.extend(rs.iter().copied());
            }
        }
        let mut new_copy = false;
        for c in &run.copies {
            if !self
                .image
                .copies
                .iter()
                .any(|o| o.dst == c.dst && o.bytes == c.bytes)
            {
                self.image.copies.push(c.clone());
                new_copy = true;
                for (i, w) in c.bytes.as_chunks::<4>().0.iter().enumerate() {
                    let a = c.dst + 4 * i as u32;
                    let v = u32::from_le_bytes([w[0], w[1], w[2], w[3]]);
                    if a.is_multiple_of(4)
                        && self.ptrs.entry(a).or_default().insert(v)
                        && let Some(rs) = self.readers.get(&a)
                    {
                        self.queue.extend(rs.iter().copied());
                    }
                }
            }
        }
        let mut new_irq = false;
        for &v in &run.irq {
            new_irq |= self.irq.insert(v);
        }
        self.runs.insert(f, run);
        if new_copy {
            let waiting: Vec<Loc> = self
                .runs
                .iter()
                .filter(|(_, r)| {
                    r.unresolved
                        .iter()
                        .any(|u| u.kind == UnresolvedKind::RamNoCopy)
                })
                .map(|(l, _)| *l)
                .collect();
            self.queue.extend(waiting);
        }
        if new_copy || new_irq {
            self.resolve_irq();
        }
    }

    fn report(&self) -> Report {
        let mut seen: BTreeSet<Loc> = BTreeSet::new();
        let mut work: Vec<Loc> = self.roots.keys().copied().collect();
        while let Some(l) = work.pop() {
            if !seen.insert(l) {
                continue;
            }
            if let Some(r) = self.runs.get(&l) {
                work.extend(r.edges.iter().map(|e| e.target));
            }
        }
        let mut rep = Report {
            copies: self.image.copies.clone(),
            ..Report::default()
        };
        let mut irq: BTreeSet<Loc> = BTreeSet::new();
        let mut unresolved = BTreeSet::new();
        let mut swis = BTreeSet::new();
        for l in &seen {
            let Some(r) = self.runs.get(l) else { continue };
            let mut edges = r.edges.clone();
            edges.sort_unstable();
            edges.dedup();
            rep.functions.insert(
                *l,
                Function {
                    entry: *l,
                    origin: self
                        .roots
                        .get(l)
                        .or(self.origins.get(l))
                        .copied()
                        .unwrap_or(Origin::Called),
                    instructions: r.body.clone(),
                    edges,
                    tables: r.tables.clone(),
                    returns: self.summaries.get(l).is_some_and(|s| s.returns),
                },
            );
            unresolved.extend(r.unresolved.iter().cloned());
            swis.extend(r.swis.iter().copied());
            rep.data.extend(r.data.iter().copied());
            if self.roots.get(l) == Some(&Origin::Irq) {
                irq.insert(*l);
            }
        }
        rep.unresolved = unresolved.into_iter().collect();
        rep.swis = swis.into_iter().collect();
        rep.irq_handlers = irq.into_iter().collect();
        let mut bytes: HashSet<u32> = HashSet::new();
        for &(a, n) in &rep.data {
            for i in 0..u32::from(n) {
                bytes.insert(a.wrapping_add(i));
            }
        }
        rep.conflicts = rep
            .instructions()
            .into_iter()
            .filter(|(l, n)| (0..u32::from(*n)).any(|i| bytes.contains(&l.addr.wrapping_add(i))))
            .map(|(l, _)| l)
            .collect();
        rep
    }
}

//! The driver: roots, the fixed point over function runs and the report.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::sync::Arc;

use crate::config::Config;
use crate::eval;
use crate::func::{self, Env, FnRun, Lifted, Summary};
use crate::image::Image;
use crate::loc::Loc;
use crate::platform::Platform;
use crate::report::{CopyKind, Function, Origin, RamCopy, Report, UnresolvedKind};
use relift_cpu_arm32::regs::{self, Mode};

use crate::val::{self, Abs, State};

/// Discovery rounds before the table stops are taken as they are.
const MAX_ROUNDS: usize = 8;
/// Function runs per function before the fixed point gives up.
const MAX_RUNS_PER_FUNCTION: usize = 1024;

/// Finds the functions of the code on `platform` reachable from the
/// roots of `config`, and from its seeds: entries when they open a stack
/// frame, else candidates checked as code addresses taken are.
#[must_use]
pub fn discover(platform: &dyn Platform, config: &Config) -> Report {
    let mut image = Image::new(platform, config.options);
    image.copies.clone_from(&config.loads);
    let mut roots = config.roots.clone();
    for &l in &config.seeds {
        if roots.contains(&l) {
            continue;
        }
        if func::opens_frame(&image, l) {
            roots.push(l);
        } else if l.copy.is_none() {
            image.seeds.push(l);
        }
    }
    run(image, &roots, &config.hints)
}

/// A code address candidate: the address, the state, the RAM copy that
/// holds it, whether only the scan of every image word gives it, and
/// whether that word is in another segment than the address.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
struct Cand {
    addr: u32,
    thumb: bool,
    copy: Option<u16>,
    scanned: bool,
    far: bool,
}

impl Cand {
    fn taken(addr: u32, thumb: bool) -> Cand {
        Cand {
            addr,
            thumb,
            copy: None,
            scanned: false,
            far: false,
        }
    }

    fn loc(self) -> Loc {
        Loc {
            addr: self.addr,
            thumb: self.thumb,
            copy: self.copy,
        }
    }
}

/// The field stores of one run that keep the same bits: the first store,
/// the bits kept and each store's address and source word.
type FieldGroup = (Loc, u32, Vec<(u32, u32)>);

/// Runs discovery to its fixed point with the table stops known so far.
/// An image table runs at most to the next stop, and the stops (the bases
/// of other tables taken as constants, function entries) are only known
/// once the code is: when one found falls inside a table that was read
/// past it, the discovery starts again with it known, until no table is
/// read past a stop.
fn run(mut image: Image<'_>, roots: &[Loc], hints: &[Loc]) -> Report {
    let mut stops = BTreeSet::new();
    let mut cache = RefCell::new(HashMap::new());
    let mut conc = RefCell::new(HashMap::new());
    for _ in 0..MAX_ROUNDS {
        let mut d = Driver::new(image.clone(), stops, cache, conc);
        for &r in roots {
            d.add_root(r, Origin::Entry);
        }
        for &h in hints {
            d.add_root(h, Origin::Hint);
        }
        d.settle();
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
        image.written = std::mem::take(&mut d.image.written);
        cache = std::mem::take(&mut d.cache);
        conc = std::mem::take(&mut d.conc);
    }
    let mut d = Driver::new(image, stops, cache, conc);
    for &r in roots {
        d.add_root(r, Origin::Entry);
    }
    for &h in hints {
        d.add_root(h, Origin::Hint);
    }
    d.settle();
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
    /// RAM words code also stores values to that are not constant.
    open: HashSet<u32>,
    /// RAM words copied to other RAM words: source to destinations.
    moves: HashMap<u32, BTreeSet<u32>>,
    specials: RefCell<func::Specials>,
    boots: RefCell<func::Boots>,
    /// Counts the changes of summaries, copies and RAM constants.
    version: u64,
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
            open: HashSet::new(),
            moves: HashMap::new(),
            specials: RefCell::new(HashMap::new()),
            boots: RefCell::new(HashMap::new()),
            version: 0,
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
        let mut addrs: Vec<u32> = self.irq.iter().copied().collect();
        if let Some((e, m)) = self.image.platform.irq_entry() {
            self.mirror_vectors(e, m);
            addrs.push(e);
        }
        for v in addrs {
            if let Ok(ls) = func::locs(&self.image, Loc::rom(v & !3, false), v & !3, false) {
                for l in ls {
                    self.add_root(l, Origin::Irq);
                }
            }
        }
    }

    /// The code addresses discovered code takes: odd words in the image
    /// or in a RAM copy (as the copy's own code reads them) that
    /// discovered code reads as constants (Thumb functions, in the image or
    /// in the copies that hold them), and the
    /// tables of such words a constant read points at; with
    /// [`crate::Options::arm_pointers`] the even words it reads too (ARM
    /// functions), and with [`crate::Options::scan_words`] every word of
    /// the image. A candidate becomes an entry when its own run lifts
    /// without an undefined instruction, stays in code bytes and overlaps
    /// no constant read as data and no instruction found in the other
    /// state or at another alignment.
    fn address_taken(&mut self) -> bool {
        let opts = self.image.options;
        let platform = self.image.platform;
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
        let mut cands: BTreeSet<Cand> = BTreeSet::new();
        for r in self.runs.values() {
            cands.extend(r.escaped.iter().map(|&v| Cand::taken(v & !1, true)));
        }
        cands.extend(
            self.image
                .seeds
                .iter()
                .map(|l| Cand::taken(l.addr, l.thumb)),
        );
        let word = |a: u32| self.image.read(None, a, 4);
        let mut scans = Vec::new();
        for &a in &words {
            for (c, v) in self.copy_words(a) {
                for (t, thumb) in [(v & !1, v & 1 == 1), (v, false)] {
                    if !(thumb || opts.arm_pointers && v & 3 == 0) {
                        continue;
                    }
                    if platform.code_addr(t) {
                        cands.insert(Cand::taken(t, thumb));
                    } else {
                        cands.extend(self.copy_cands(Some(c), t, thumb));
                    }
                }
            }
            let Some(v) = word(a) else { continue };
            if v & 1 == 1 && !platform.code_addr(v & !1) {
                cands.extend(self.copy_cands(None, v & !1, true));
            }
            if opts.arm_pointers && v & 3 == 0 && !platform.code_addr(v) {
                cands.extend(self.copy_cands(None, v, false));
            }
            if v & 1 == 1 && platform.code_addr(v & !1) {
                cands.insert(Cand::taken(v & !1, true));
            }
            if opts.arm_pointers && v & 3 == 0 && platform.code_addr(v) {
                cands.insert(Cand::taken(v, false));
            }
            if v & 3 == 0 && self.image.in_image(v) {
                let mut t = v;
                while let Some(e) = word(t) {
                    if !(e & 1 == 1 && platform.code_addr(e & !1))
                        || data.contains(&t) && !words.contains(&t)
                        || t > v && self.stops.range(v + 1..=t + 3).next().is_some()
                    {
                        break;
                    }
                    cands.insert(Cand::taken(e & !1, true));
                    t += 4;
                }
                if t > v {
                    scans.push((v, t + 4));
                }
            }
        }
        if opts.scan_words {
            let segments = platform.segments();
            let segment = |a: u32| {
                segments
                    .iter()
                    .rposition(|(b, d)| a.wrapping_sub(*b) < d.len() as u32)
            };
            for &(base, bytes) in &segments {
                for (i, w) in bytes.as_chunks::<4>().0.iter().enumerate() {
                    let a = base + 4 * i as u32;
                    if code.contains_key(&a) && !data.contains(&a) {
                        continue;
                    }
                    let v = u32::from_le_bytes(*w);
                    let (t, thumb) = match v & 3 {
                        1 | 3 => (v & !1, true),
                        0 if opts.arm_pointers => (v, false),
                        _ => continue,
                    };
                    if !platform.code_addr(t) || cands.contains(&Cand::taken(t, thumb)) {
                        continue;
                    }
                    cands.insert(Cand {
                        addr: t,
                        thumb,
                        copy: None,
                        scanned: true,
                        far: segment(a) != segment(t),
                    });
                }
            }
        }
        self.scans.extend(scans);
        let mut added = false;
        let mut seen: HashSet<Loc> = HashSet::new();
        let reached = self.reached();
        for c in cands {
            let l = c.loc();
            if !seen.insert(l) || !platform.owns(c.addr) {
                continue;
            }
            if self.entries.contains(&l) && !reached.contains(&l) && self.runs.contains_key(&l) {
                self.add_root(l, Origin::AddressTaken);
                added = true;
                continue;
            }
            if self.entries.contains(&l)
                || self.rejected.contains(&l)
                || l.copy.is_none() && code.get(&l.addr) == Some(&(l.addr, c.thumb))
                || l.copy.is_some() && self.runs.values().any(|r| r.body.contains_key(&l))
            {
                continue;
            }
            if !c.thumb && !self.always_runs(l) {
                self.rejected.insert(l);
                continue;
            }
            if c.far && !func::opens_frame(&self.image, l) {
                continue;
            }
            let run = {
                let env = Env {
                    image: &self.image,
                    summaries: &self.summaries,
                    entries: &self.entries,
                    ptrs: &self.ptrs,
                    open: &self.open,
                    boot: &self.roots,
                    cache: &self.cache,
                    runs: &self.conc,
                    stops: &self.stops,
                    depth: 0,
                    specials: &self.specials,
                    boots: &self.boots,
                    version: self.version,
                };
                func::run(&env, l, State::entry(c.thumb), &self.no_tables)
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
            if bad_insn || overlap || c.scanned && c.thumb && self.covers_arm(&run) {
                self.rejected.insert(l);
                continue;
            }
            self.add_root(l, Origin::AddressTaken);
            added = true;
        }
        added
    }

    /// The exception vectors at the platform's IRQ entry `e` that copies
    /// put at its mirror `m`: each copy that holds `m` gets a twin at
    /// `e`, from the copy's bytes at `m - e` on, so the vectors are lifted
    /// where the CPU runs them (a boot stub and the code that replaces it
    /// each give one).
    fn mirror_vectors(&mut self, e: u32, m: u32) {
        let back = m.wrapping_sub(e);
        let mut twins = Vec::new();
        for c in self.image.copies_at(m) {
            let src = &self.image.copies[usize::from(c)];
            let from = back.max(src.dst);
            let bytes = &src.bytes[(from - src.dst) as usize..];
            let dst = from.wrapping_sub(back);
            if !self
                .image
                .copies
                .iter()
                .any(|o| o.dst == dst && *o.bytes == *bytes)
            {
                twins.push(RamCopy {
                    site: src.site,
                    kind: CopyKind::Run,
                    src: Some(from),
                    dst,
                    bytes: Arc::from(bytes),
                });
            }
        }
        for t in twins {
            self.image.copies.push(t);
            self.version += 1;
        }
    }

    /// The words the copies that hold `a` keep there, as their own code
    /// reads them.
    fn copy_words(&self, a: u32) -> Vec<(u16, u32)> {
        if !self.image.is_ram(a) {
            return Vec::new();
        }
        self.image
            .copies_at(a)
            .into_iter()
            .filter_map(|c| Some((c, self.image.read(Some(c), a, 4)?)))
            .collect()
    }

    /// The candidates for a code address `t` in RAM read from copy `c` or
    /// from the image: in `c` when it holds `t`, else in every copy that
    /// does.
    fn copy_cands(&self, c: Option<u16>, t: u32, thumb: bool) -> Vec<Cand> {
        if !self.image.is_ram(t) {
            return Vec::new();
        }
        let copies = match c {
            Some(c) if self.image.holds(c, t) => vec![c],
            _ => self.image.copies_at(t),
        };
        copies
            .into_iter()
            .map(|c| Cand {
                copy: Some(c),
                ..Cand::taken(t, thumb)
            })
            .collect()
    }

    /// The functions the roots reach through edges: those a report lists.
    fn reached(&self) -> BTreeSet<Loc> {
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
        seen
    }

    fn env(&self) -> Env<'_> {
        Env {
            image: &self.image,
            summaries: &self.summaries,
            entries: &self.entries,
            ptrs: &self.ptrs,
            open: &self.open,
            boot: &self.roots,
            cache: &self.cache,
            runs: &self.conc,
            stops: &self.stops,
            depth: 0,
            specials: &self.specials,
            boots: &self.boots,
            version: self.version,
        }
    }

    /// Whether the instruction at `l` runs whatever the flags are, as a
    /// function's first instruction does.
    fn always_runs(&self, l: Loc) -> bool {
        let env = self.env();
        let Some(w) = self.image.fetch(l) else {
            return false;
        };
        let Ok(lifted) = env.lift(l, w) else {
            return false;
        };
        let out = eval::eval(
            &lifted.1,
            l,
            &State::entry(l.thumb),
            &self.image,
            false,
            None,
        );
        out.may_run && !out.may_skip
    }

    /// Whether a Thumb run covers ARM code: half or more of its halfwords
    /// at 2 mod 4 (at least four) are the upper half of a word that lifts
    /// as an ARM instruction that runs whatever the flags are. In Thumb
    /// code few halfwords are; in ARM code nearly every word is one.
    fn covers_arm(&self, run: &FnRun) -> bool {
        let tops: Vec<Loc> = run
            .body
            .keys()
            .filter(|b| b.thumb && b.addr % 4 == 2)
            .map(|b| Loc::rom(b.addr - 2, false))
            .collect();
        let arm = tops.iter().filter(|l| self.always_runs(**l)).count();
        tops.len() >= 4 && 2 * arm >= tops.len()
    }

    /// Solves until no address-taken entry and no rewritten instruction is
    /// new.
    fn settle(&mut self) {
        loop {
            self.solve();
            if !self.address_taken() && !self.rewritten() {
                break;
            }
        }
    }

    /// The code in RAM that may hold other bytes than the copy it was found
    /// in. Every constant stored to a constant address that changes an
    /// instruction found there in some copy makes a rewrite of that copy
    /// (its bytes with the store) and an entry at the instruction in both,
    /// so code that finds either unit there goes on from it. Rewrites start
    /// from their copy, never from each other: an instruction that finds
    /// another rewrite in place leaves for that one's entry. Where copies
    /// at one address hold different instructions, each copy gets an entry
    /// at the first of each run of them and at those a branch reaches.
    /// A run that stores words which are another word of RAM with one
    /// field set to a value not known ([`val::Base::Field`]) is taken to
    /// set the same value in each, and makes a rewrite for every value the
    /// field can hold.
    fn rewritten(&mut self) -> bool {
        let stores: BTreeSet<(u32, u8, u32, Loc)> = self
            .runs
            .values()
            .flat_map(|r| r.ram_stores.iter().map(|&(s, a, n, v)| (a, n, v, s)))
            .collect();
        let mut code: BTreeMap<u32, BTreeSet<(bool, u16)>> = BTreeMap::new();
        for r in self.runs.values() {
            for l in r.body.keys() {
                if let Some(c) = l.copy
                    && !self.image.is_table(c)
                {
                    code.entry(l.addr)
                        .or_default()
                        .insert((l.thumb, self.image.base(c)));
                }
            }
        }
        let mut roots = Vec::new();
        let targets: HashSet<(u32, bool)> = self
            .runs
            .values()
            .flat_map(|r| r.branches.iter().map(|(_, to)| (to.addr, to.thumb)))
            .collect();
        let units = |at: u32, thumb: bool, ls: &BTreeSet<(bool, u16)>| -> BTreeMap<u16, u32> {
            ls.iter()
                .filter(|l| l.0 == thumb)
                .filter_map(|&(_, b)| {
                    let w = self.image.fetch(Loc {
                        addr: at,
                        thumb,
                        copy: Some(b),
                    })?;
                    Some((b, w))
                })
                .collect()
        };
        let differ = |u: &BTreeMap<u16, u32>| u.values().collect::<BTreeSet<_>>().len() > 1;
        for (&at, ls) in &code {
            for thumb in [false, true] {
                let u = units(at, thumb, ls);
                if !differ(&u) {
                    continue;
                }
                let size = if thumb { 2 } else { 4 };
                let before = code
                    .get(&at.wrapping_sub(size))
                    .map(|p| units(at.wrapping_sub(size), thumb, p))
                    .unwrap_or_default();
                let same_run = differ(&before) && before.keys().eq(u.keys());
                if same_run && !targets.contains(&(at, thumb)) {
                    continue;
                }
                roots.extend(u.keys().map(|&b| Loc {
                    addr: at,
                    thumb,
                    copy: Some(b),
                }));
            }
        }
        for &(a, n, v, site) in &stores {
            for (&at, ls) in code.range(a.saturating_sub(3)..a.saturating_add(u32::from(n))) {
                for &(thumb, b) in ls {
                    let size = if thumb { 2 } else { 4 };
                    if at + size <= a {
                        continue;
                    }
                    let Some(p) = self.rewrite(b, &[(a, n, v)], site) else {
                        continue;
                    };
                    for c in [b, p] {
                        roots.push(Loc {
                            addr: at,
                            thumb,
                            copy: Some(c),
                        });
                    }
                }
            }
        }
        let groups: BTreeSet<FieldGroup> = self
            .runs
            .values()
            .flat_map(|r| {
                let mut by: BTreeMap<u32, (Loc, Vec<(u32, u32)>)> = BTreeMap::new();
                for &(s, a, src, keep) in &r.field_stores {
                    let g = by.entry(keep).or_insert((s, Vec::new()));
                    g.0 = g.0.min(s);
                    g.1.push((a, src));
                }
                by.into_iter().map(|(k, (s, mut v))| {
                    v.sort_unstable();
                    v.dedup();
                    (s, k, v)
                })
            })
            .collect();
        for (site, keep, stores) in &groups {
            let Some((lo, bits)) = val::field(*keep) else {
                continue;
            };
            let hit = |a: u32| -> Vec<(u32, bool, u16)> {
                code.range(a.saturating_sub(3)..a + 4)
                    .flat_map(|(&at, ls)| ls.iter().map(move |&(t, b)| (at, t, b)))
                    .filter(|&(at, t, _)| at + if t { 2 } else { 4 } > a)
                    .collect()
            };
            let bases: BTreeSet<u16> = stores
                .iter()
                .flat_map(|&(a, _)| hit(a))
                .map(|h| h.2)
                .collect();
            for b in bases {
                for v in 0..1u32 << bits {
                    let writes: Vec<(u32, u8, u32)> = stores
                        .iter()
                        .filter_map(|&(a, src)| {
                            let w = self.image.read(Some(b), src, 4)?;
                            Some((a, 4, (w & keep) | (v << lo)))
                        })
                        .collect();
                    let Some(p) = self.rewrite(b, &writes, *site) else {
                        continue;
                    };
                    for &(a, _) in stores {
                        for (at, thumb, hb) in hit(a) {
                            if hb != b {
                                continue;
                            }
                            for c in [b, p] {
                                roots.push(Loc {
                                    addr: at,
                                    thumb,
                                    copy: Some(c),
                                });
                            }
                        }
                    }
                }
            }
        }
        let mut added = false;
        for l in roots {
            if !self.roots.contains_key(&l) {
                self.add_root(l, Origin::Rewritten);
                added = true;
            }
        }
        added
    }

    /// The copy that holds copy `b` with each `(a, n, v)` of `writes`
    /// stored (`v` at `a`, `n` bytes), made when no copy does; `None` when
    /// they change no byte of `b`.
    fn rewrite(&mut self, b: u16, writes: &[(u32, u8, u32)], site: Loc) -> Option<u16> {
        let c = self.image.copies.get(usize::from(b))?;
        let mut bytes = c.bytes.to_vec();
        for &(a, n, v) in writes {
            for i in 0..u32::from(n) {
                let off = a.wrapping_add(i).wrapping_sub(c.dst) as usize;
                if let Some(x) = bytes.get_mut(off) {
                    *x = (v >> (8 * i)) as u8;
                }
            }
        }
        if *bytes == *c.bytes {
            return None;
        }
        let dst = c.dst;
        if let Some(i) = self
            .image
            .copies
            .iter()
            .position(|o| o.dst == dst && *o.bytes == *bytes)
        {
            return u16::try_from(i).ok();
        }
        let i = u16::try_from(self.image.copies.len()).ok()?;
        self.image.copies.push(RamCopy {
            site,
            kind: CopyKind::Rewrite(b),
            src: None,
            dst,
            bytes: Arc::from(bytes),
        });
        self.version += 1;
        Some(i)
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
                    open: &self.open,
                    boot: &self.roots,
                    cache: &self.cache,
                    runs: &self.conc,
                    stops: &self.stops,
                    depth: 0,
                    specials: &self.specials,
                    boots: &self.boots,
                    version: self.version,
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

    fn absorb(&mut self, f: Loc, mut run: FnRun) {
        if !run.bad_tables.is_empty() {
            self.no_tables.extend(run.bad_tables.iter().copied());
            self.queue.insert(f);
            return;
        }
        let sum = run.summary.clone().unwrap_or_else(Summary::unknown);
        if self.summaries.get(&f) != Some(&sum) {
            self.version += 1;
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
        let mut learned: Vec<(u32, u32)> = run.ptr_stores.clone();
        for &(d, s) in &run.moves {
            if self.moves.entry(s).or_default().insert(d) {
                learned.extend(self.ptrs.get(&s).into_iter().flatten().map(|v| (d, *v)));
            }
        }
        let mut new_copy = false;
        let mut new_ranges = Vec::new();
        let mut table_fns = Vec::new();
        for c in &run.copies {
            if !self
                .image
                .copies
                .iter()
                .any(|o| o.dst == c.dst && o.bytes == c.bytes)
            {
                if let (CopyKind::Table(thumb), Ok(i)) =
                    (c.kind, u16::try_from(self.image.copies.len()))
                {
                    table_fns.push(Loc {
                        addr: c.dst,
                        thumb,
                        copy: Some(i),
                    });
                }
                self.image.copies.push(c.clone());
                self.version += 1;
                new_copy = true;
                new_ranges.push((c.dst, c.len()));
                for (i, w) in c.bytes.as_chunks::<4>().0.iter().enumerate() {
                    let a = c.dst + 4 * i as u32;
                    if a.is_multiple_of(4) {
                        learned.push((a, u32::from_le_bytes([w[0], w[1], w[2], w[3]])));
                    }
                }
            }
        }
        self.learn(learned);
        for l in table_fns {
            self.add_root(l, Origin::AddressTaken);
        }
        for &a in &run.ptr_open {
            if self.open.insert(a) {
                self.version += 1;
                if let Some(rs) = self.readers.get(&a) {
                    self.queue.extend(rs.iter().copied());
                }
            }
        }
        let mut new_irq = false;
        for &v in &run.irq {
            new_irq |= self.irq.insert(v);
        }
        let writes = std::mem::take(&mut run.image_writes);
        self.runs.insert(f, run);
        self.image_writes(&writes);
        if new_copy {
            let waiting: Vec<Loc> = self
                .runs
                .iter()
                .filter(|(_, r)| {
                    r.unresolved
                        .iter()
                        .any(|u| u.kind == UnresolvedKind::RamNoCopy)
                        || r.edges.iter().any(|e| {
                            new_ranges
                                .iter()
                                .any(|&(d, n)| e.target.addr.wrapping_sub(d) < n)
                        })
                })
                .map(|(l, _)| *l)
                .collect();
            self.queue.extend(waiting);
        }
        if new_copy || new_irq {
            self.resolve_irq();
        }
    }

    /// Takes the image bytes `writes` store to as variables; runs again the
    /// functions that read any new one as a constant.
    fn image_writes(&mut self, writes: &[(u32, u8)]) {
        let mut new: HashSet<u32> = HashSet::new();
        for &(a, n) in writes {
            for i in 0..u32::from(n) {
                let b = a.wrapping_add(i);
                if self.image.written.insert(b) {
                    new.insert(b);
                }
            }
        }
        if new.is_empty() {
            return;
        }
        self.version += 1;
        let readers: Vec<Loc> = self
            .runs
            .iter()
            .filter(|(_, r)| {
                r.data
                    .iter()
                    .any(|&(a, n)| (0..u32::from(n)).any(|i| new.contains(&a.wrapping_add(i))))
            })
            .map(|(l, _)| *l)
            .collect();
        self.queue.extend(readers);
    }

    /// Adds constant words stored in RAM, and what they reach through the
    /// RAM-to-RAM copies; runs the readers of every word that changes.
    fn learn(&mut self, mut work: Vec<(u32, u32)>) {
        while let Some((a, v)) = work.pop() {
            if !self.ptrs.entry(a).or_default().insert(v) {
                continue;
            }
            self.version += 1;
            if let Some(rs) = self.readers.get(&a) {
                self.queue.extend(rs.iter().copied());
            }
            if let Some(ds) = self.moves.get(&a) {
                work.extend(ds.iter().map(|d| (*d, v)));
            }
        }
    }

    fn report(&self) -> Report {
        let seen = self.reached();
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
                    linked: r.linked.clone(),
                },
            );
            unresolved.extend(r.unresolved.iter().cloned());
            rep.outside.extend(r.outside.iter().copied());
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

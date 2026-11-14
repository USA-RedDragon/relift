//! Which functions must raise exact FPSR flags: the flags-live regions.
//!
//! FPSR's cumulative flags are sticky, so a flag raised anywhere since the
//! last FPSR write on a thread is visible at a read on that thread. The
//! region of a live read is every instruction that can execute on some path
//! from the last FPSR write (an `MSR FPSR`, or thread start, where the
//! runtime clears the host flags) to the read. A function must raise exact
//! flags when one of its FP instructions lies in some region, or when it
//! holds a live read (whose `MRS FPSR` must see the host flags). Every other
//! function, including a function in a region that does no FP, can use the
//! flag-free fast path: what it raises is either never observed or nothing.
//!
//! The walk goes backward from each read over the program's control flow,
//! one thread at a time (a call and its return stay matched, so a region
//! follows only the reading thread):
//!
//! - an `MSR FPSR` ends a path, and so does the entry of a thread start;
//! - a call before a point puts the callee's tail (from its returns back to
//!   its own FPSR writes) in the region, and the path goes on before the
//!   call unless the callee writes FPSR on every path (`must_write`);
//! - reaching the entry of the reading function (or of a function the
//!   region climbed into) goes on before every call site and tail-call
//!   site of it.
//!
//! A region is unbounded, and the program falls back to producing flags in
//! every function, when it meets code the walk cannot follow: an indirect
//! call or computed jump with unknown targets, the entry of a function that
//! code the analysis cannot see may call (its address is taken), or a point
//! entered from such code (a resume point after `setjmp`).

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

/// The possible targets of a call or a transfer out of a function.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Targets {
    /// These function entries (none: the transfer cannot happen).
    Known(Vec<u64>),
    /// Code the analysis cannot name.
    Unknown,
}

/// What the last instruction of a block does besides going to the block's
/// successors in the function.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum End {
    /// Nothing: the block only goes to its successors.
    Fall,
    /// A call; the successor is the return point.
    Call(Targets),
    /// A tail transfer to another function, which returns for this one; a
    /// conditional one also has successors.
    Exit(Targets),
    /// A return.
    Return,
}

/// A straight-line run of a function's instructions.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Bb {
    /// The address of the first instruction.
    pub start: u64,
    /// Whether an instruction of the block may raise FP exception flags.
    pub fp: bool,
    /// Whether one after the block's last FPSR write may.
    pub fp_tail: bool,
    /// Whether the block writes FPSR.
    pub writes: bool,
    /// The successor blocks in the function, by index.
    pub succs: Vec<usize>,
    /// How the block ends.
    pub end: End,
    /// Whether code the analysis cannot see may enter the block (a resume
    /// point after `setjmp`, a landing pad).
    pub foreign: bool,
    /// Whether the block cannot run when its function is entered as the
    /// program start, with the registers the runtime sets there.
    pub start_dead: bool,
}

/// One function: its blocks (the first is the entry) and the blocks that
/// start with a live `MRS FPSR`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Func {
    /// The entry address.
    pub entry: u64,
    /// The blocks; `blocks[0]` starts at the entry.
    pub blocks: Vec<Bb>,
    /// The blocks that start with a live read, with the read's address.
    pub reads: Vec<(u64, usize)>,
}

/// The whole program as the analysis sees it.
#[derive(Clone, Debug, Default)]
pub struct Program {
    /// Every function of every module.
    pub fns: Vec<Func>,
    /// Entries that code the analysis cannot see may call: address-taken
    /// functions, loader-resolved symbols it does not follow. A function
    /// that is neither escaped nor called runs only as a thread start
    /// (where the flags are clear) or never.
    pub escaped: BTreeSet<u64>,
    /// The program start: entries the runtime enters once, with the
    /// registers it sets there, which mark the blocks that cannot then
    /// run ([`Bb::start_dead`]). When nothing else may enter a start, its
    /// dead blocks never run and a function entered only from them is
    /// never entered.
    pub starts: BTreeSet<u64>,
}

/// Why a function must raise exact flags.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Reason {
    /// It holds a live read.
    Read,
    /// FP instructions of it lie in the region of a read.
    Region,
}

/// The region of one read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Region {
    /// The read's address.
    pub read: u64,
    /// The entry of the function holding it.
    pub func: u64,
    /// The functions with FP instructions in the region.
    pub fp: BTreeSet<u64>,
    /// How many functions the region touches, FP or not.
    pub touched: usize,
    /// Why the region could not be bounded, if it could not.
    pub unbounded: Option<String>,
    /// Whether the read can run: a read in a function that cannot be
    /// entered, or in a dead block of a start, has no region.
    pub entered: bool,
}

/// The analysis result.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Decision {
    /// The functions that must raise exact flags and why (the strongest
    /// reason when there are several).
    pub flagged: BTreeMap<u64, Reason>,
    /// Every read's region.
    pub regions: Vec<Region>,
}

impl Decision {
    /// Whether some region is unbounded, so every function must produce
    /// flags.
    pub fn program_wide(&self) -> bool {
        self.regions.iter().any(|r| r.unbounded.is_some())
    }
}

/// One step of the backward walk.
#[derive(Clone, Copy, Debug)]
enum Item {
    /// Block `b` of function `f` runs in the region up to its end; `up`
    /// says whether reaching the entry goes on into the callers.
    Whole(usize, usize, bool),
    /// The tail of the function with this entry.
    Tail(u64),
}

struct Walker<'a> {
    p: &'a Program,
    index: &'a HashMap<u64, usize>,
    preds: &'a [Vec<Vec<usize>>],
    callers: &'a HashMap<u64, Vec<(usize, usize)>>,
    live: &'a Live,
    must: &'a HashSet<u64>,
    seen: HashSet<(usize, usize, bool)>,
    tails: HashSet<u64>,
    climbed: HashSet<usize>,
    fp: BTreeSet<u64>,
    touched: BTreeSet<u64>,
    work: Vec<Item>,
}

impl Walker<'_> {
    /// Pushes the paths into block `b` of `f` from its predecessors.
    fn preds(&mut self, f: usize, b: usize, up: bool) -> Result<(), String> {
        let func = &self.p.fns[f];
        let blk = &func.blocks[b];
        if blk.foreign {
            return Err(format!(
                "{:#x} may be entered from code the analysis cannot see",
                blk.start
            ));
        }
        if b == 0 && up {
            self.climb(f)?;
        }
        for &q in &self.preds[f][b] {
            let qb = &func.blocks[q];
            match &qb.end {
                End::Call(t) => {
                    if self.through(t, qb.start)? {
                        self.work.push(Item::Whole(f, q, up));
                    }
                }
                _ => self.work.push(Item::Whole(f, q, up)),
            }
        }
        Ok(())
    }

    /// Puts the tails of the targets in the region; whether the path goes
    /// on before the transfer (some target may not write FPSR).
    fn through(&mut self, t: &Targets, at: u64) -> Result<bool, String> {
        let Targets::Known(ts) = t else {
            return Err(format!(
                "the block at {at:#x} calls or jumps to unknown code"
            ));
        };
        let mut on = false;
        for &e in ts {
            if !self.index.contains_key(&e) {
                return Err(format!(
                    "the block at {at:#x} reaches {e:#x}, which is no function"
                ));
            }
            self.work.push(Item::Tail(e));
            on |= !self.must.contains(&e);
        }
        Ok(on)
    }

    /// Goes on before every call site of function `f`.
    fn climb(&mut self, f: usize) -> Result<(), String> {
        if !self.climbed.insert(f) {
            return Ok(());
        }
        let e = self.p.fns[f].entry;
        if self.p.escaped.contains(&e) {
            return Err(format!(
                "{e:#x} may be called from code the analysis cannot see"
            ));
        }
        for &(g, b) in self.callers.get(&e).into_iter().flatten() {
            if self.live.runs(self.p, g, b) {
                self.work.push(Item::Whole(g, b, true));
            }
        }
        Ok(())
    }

    fn run(&mut self) -> Result<(), String> {
        while let Some(item) = self.work.pop() {
            match item {
                Item::Whole(f, b, up) => {
                    if self.seen.contains(&(f, b, true)) || !self.seen.insert((f, b, up)) {
                        continue;
                    }
                    let func = &self.p.fns[f];
                    let blk = &func.blocks[b];
                    self.touched.insert(func.entry);
                    if blk.fp_tail || (blk.fp && !blk.writes) {
                        self.fp.insert(func.entry);
                    }
                    if !blk.writes {
                        self.preds(f, b, up)?;
                    }
                }
                Item::Tail(e) => {
                    if !self.tails.insert(e) {
                        continue;
                    }
                    let f = self.index[&e];
                    for (b, blk) in self.p.fns[f].blocks.iter().enumerate() {
                        match &blk.end {
                            End::Return => self.work.push(Item::Whole(f, b, false)),
                            End::Exit(t) => {
                                if self.through(t, blk.start)? {
                                    self.work.push(Item::Whole(f, b, false));
                                }
                            }
                            End::Fall | End::Call(_) => {}
                        }
                    }
                }
            }
        }
        Ok(())
    }
}

/// The entries of the functions that write FPSR on every path from their
/// entry to a return or tail transfer (a function that never returns
/// counts). Computed as the greatest fixpoint, so recursion that ends
/// counts by its ending paths.
fn must_write(p: &Program, index: &HashMap<u64, usize>) -> HashSet<u64> {
    // Only functions that can reach a write are candidates.
    let mut can: HashSet<usize> = HashSet::new();
    let mut rev: HashMap<usize, Vec<usize>> = HashMap::new();
    for (i, f) in p.fns.iter().enumerate() {
        if f.blocks.iter().any(|b| b.writes) {
            can.insert(i);
        }
        for b in &f.blocks {
            if let End::Call(Targets::Known(ts)) | End::Exit(Targets::Known(ts)) = &b.end {
                for t in ts {
                    if let Some(&j) = index.get(t) {
                        rev.entry(j).or_default().push(i);
                    }
                }
            }
        }
    }
    let mut todo: Vec<usize> = can.iter().copied().collect();
    while let Some(j) = todo.pop() {
        for &i in rev.get(&j).into_iter().flatten() {
            if can.insert(i) {
                todo.push(i);
            }
        }
    }
    let mut must: HashSet<u64> = can.iter().map(|&i| p.fns[i].entry).collect();
    let all = |ts: &Targets, must: &HashSet<u64>| match ts {
        Targets::Known(ts) => !ts.is_empty() && ts.iter().all(|t| must.contains(t)),
        Targets::Unknown => false,
    };
    loop {
        let mut changed = false;
        for &i in &can {
            let f = &p.fns[i];
            if !must.contains(&f.entry) {
                continue;
            }
            // Forward must-analysis: written on every path into a block.
            let n = f.blocks.len();
            let mut inn = vec![true; n];
            inn[0] = false;
            for (b, blk) in f.blocks.iter().enumerate() {
                if blk.foreign {
                    inn[b] = false;
                }
            }
            let mut preds: Vec<Vec<usize>> = vec![Vec::new(); n];
            for (b, blk) in f.blocks.iter().enumerate() {
                for &s in &blk.succs {
                    preds[s].push(b);
                }
            }
            let out = |b: usize, inn: &[bool], must: &HashSet<u64>| {
                let blk = &f.blocks[b];
                inn[b] || blk.writes || matches!(&blk.end, End::Call(t) if all(t, must))
            };
            let mut stable = false;
            while !stable {
                stable = true;
                for b in 1..n {
                    if f.blocks[b].foreign || !inn[b] {
                        continue;
                    }
                    if preds[b].iter().any(|&q| !out(q, &inn, &must)) {
                        inn[b] = false;
                        stable = false;
                    }
                }
            }
            let ok = f.blocks.iter().enumerate().all(|(b, blk)| match &blk.end {
                End::Return => inn[b] || blk.writes,
                End::Exit(t) => inn[b] || blk.writes || all(t, &must),
                End::Fall | End::Call(_) => true,
            });
            if !ok {
                must.remove(&f.entry);
                changed = true;
            }
        }
        if !changed {
            return must;
        }
    }
}

/// Which functions can be entered and which of their blocks can run.
struct Live {
    /// By function: whether anything may enter it.
    entered: Vec<bool>,
    /// By function: whether it is a start nothing else enters, so its
    /// [`Bb::start_dead`] blocks never run.
    start_only: Vec<bool>,
}

impl Live {
    /// A function is entered when code the analysis cannot see may call it
    /// (it escapes), when it is a start or has no call site (a thread
    /// start), or from a block that runs in an entered function. This
    /// relies on what the walk relies on: the runtime enters guest code
    /// only at a start or at an address the code computes or stores, and
    /// such an address escapes.
    fn new(
        p: &Program,
        index: &HashMap<u64, usize>,
        callers: &HashMap<u64, Vec<(usize, usize)>>,
    ) -> Live {
        let start_only: Vec<bool> = p
            .fns
            .iter()
            .map(|f| {
                p.starts.contains(&f.entry)
                    && !p.escaped.contains(&f.entry)
                    && !callers.contains_key(&f.entry)
            })
            .collect();
        let mut live = Live {
            entered: vec![false; p.fns.len()],
            start_only,
        };
        let mut work = Vec::new();
        for (i, f) in p.fns.iter().enumerate() {
            if p.escaped.contains(&f.entry)
                || p.starts.contains(&f.entry)
                || !callers.contains_key(&f.entry)
            {
                live.entered[i] = true;
                work.push(i);
            }
        }
        while let Some(i) = work.pop() {
            for (b, blk) in p.fns[i].blocks.iter().enumerate() {
                if !live.runs(p, i, b) {
                    continue;
                }
                if let End::Call(Targets::Known(ts)) | End::Exit(Targets::Known(ts)) = &blk.end {
                    for t in ts {
                        if let Some(&j) = index.get(t)
                            && !live.entered[j]
                        {
                            live.entered[j] = true;
                            work.push(j);
                        }
                    }
                }
            }
        }
        live
    }

    /// Whether block `b` of function `f` can run.
    fn runs(&self, p: &Program, f: usize, b: usize) -> bool {
        self.entered[f] && !(self.start_only[f] && p.fns[f].blocks[b].start_dead)
    }
}

/// Finds the functions of `p` that must raise exact flags.
pub fn decide(p: &Program) -> Decision {
    let index: HashMap<u64, usize> = p
        .fns
        .iter()
        .enumerate()
        .map(|(i, f)| (f.entry, i))
        .collect();
    let preds: Vec<Vec<Vec<usize>>> = p
        .fns
        .iter()
        .map(|f| {
            let mut v = vec![Vec::new(); f.blocks.len()];
            for (b, blk) in f.blocks.iter().enumerate() {
                for &s in &blk.succs {
                    v[s].push(b);
                }
            }
            v
        })
        .collect();
    let mut callers: HashMap<u64, Vec<(usize, usize)>> = HashMap::new();
    for (i, f) in p.fns.iter().enumerate() {
        for (b, blk) in f.blocks.iter().enumerate() {
            if let End::Call(Targets::Known(ts)) | End::Exit(Targets::Known(ts)) = &blk.end {
                for &t in ts {
                    callers.entry(t).or_default().push((i, b));
                }
            }
        }
    }
    let must = must_write(p, &index);
    let live = Live::new(p, &index, &callers);
    let mut d = Decision::default();
    for (i, f) in p.fns.iter().enumerate() {
        for &(read, b) in &f.reads {
            if !live.runs(p, i, b) {
                d.regions.push(Region {
                    read,
                    func: f.entry,
                    fp: BTreeSet::new(),
                    touched: 0,
                    unbounded: None,
                    entered: false,
                });
                continue;
            }
            d.flagged.insert(f.entry, Reason::Read);
            let mut w = Walker {
                p,
                index: &index,
                preds: &preds,
                callers: &callers,
                live: &live,
                must: &must,
                seen: HashSet::new(),
                tails: HashSet::new(),
                climbed: HashSet::new(),
                fp: BTreeSet::new(),
                touched: BTreeSet::from([f.entry]),
                work: Vec::new(),
            };
            let r = w.preds(i, b, true).and_then(|()| w.run());
            for &e in &w.fp {
                d.flagged.entry(e).or_insert(Reason::Region);
            }
            d.regions.push(Region {
                read,
                func: f.entry,
                fp: w.fp,
                touched: w.touched.len(),
                unbounded: r.err(),
                entered: true,
            });
        }
    }
    d
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A block: `fp` (and after a write too, when `writes`), successors and end.
    fn bb(start: u64, fp: bool, writes: bool, succs: &[usize], end: End) -> Bb {
        Bb {
            start,
            fp,
            fp_tail: fp && !writes,
            writes,
            succs: succs.to_vec(),
            end,
            foreign: false,
            start_dead: false,
        }
    }

    fn call(t: u64) -> End {
        End::Call(Targets::Known(vec![t]))
    }

    fn func(entry: u64, blocks: Vec<Bb>, reads: &[usize]) -> Func {
        let reads = reads.iter().map(|&b| (blocks[b].start, b)).collect();
        Func {
            entry,
            blocks,
            reads,
        }
    }

    /// A leaf that does FP (or not) and returns.
    fn leaf(entry: u64, fp: bool) -> Func {
        func(entry, vec![bb(entry, fp, false, &[], End::Return)], &[])
    }

    fn flagged(d: &Decision) -> Vec<(u64, Reason)> {
        d.flagged.iter().map(|(&e, &r)| (e, r)).collect()
    }

    #[test]
    fn a_region_ends_at_the_last_write() {
        // 0x100: fp; msr; call 0x200 | call 0x300 | mrs (read) | call 0x400; ret
        let f = func(
            0x100,
            vec![
                Bb {
                    fp_tail: false,
                    ..bb(0x100, true, true, &[1], call(0x200))
                },
                bb(0x110, false, false, &[2], call(0x300)),
                bb(0x120, false, false, &[3], call(0x400)),
                bb(0x130, false, false, &[], End::Return),
            ],
            &[2],
        );
        let p = Program {
            fns: vec![f, leaf(0x200, true), leaf(0x300, false), leaf(0x400, true)],
            escaped: BTreeSet::new(),
            starts: BTreeSet::new(),
        };
        let d = decide(&p);
        assert!(!d.program_wide());
        // 0x100's FP runs before its write; 0x300 is in the region but does
        // no FP; 0x400 runs after the read.
        assert_eq!(
            flagged(&d),
            vec![(0x100, Reason::Read), (0x200, Reason::Region)]
        );
        assert_eq!(d.regions[0].touched, 3);
    }

    #[test]
    fn a_callee_that_always_writes_bounds_the_region() {
        // 0x100: call 0x200 (fp) | call 0x300 (writes on every path) | read
        let f = func(
            0x100,
            vec![
                bb(0x100, false, false, &[1], call(0x200)),
                bb(0x110, false, false, &[2], call(0x300)),
                bb(0x120, false, false, &[], End::Return),
            ],
            &[2],
        );
        // 0x300: fp; branch to a write either way (one through a tail call
        // to 0x400, which writes), then fp after the write.
        let w = func(
            0x300,
            vec![
                bb(0x300, true, false, &[1, 2], End::Fall),
                Bb {
                    fp_tail: true,
                    ..bb(0x310, true, true, &[], End::Return)
                },
                bb(
                    0x320,
                    false,
                    false,
                    &[],
                    End::Exit(Targets::Known(vec![0x400])),
                ),
            ],
            &[],
        );
        let p = Program {
            fns: vec![
                f,
                leaf(0x200, true),
                w,
                func(0x400, vec![bb(0x400, false, true, &[], End::Return)], &[]),
            ],
            escaped: BTreeSet::new(),
            starts: BTreeSet::new(),
        };
        let d = decide(&p);
        assert!(!d.program_wide());
        assert_eq!(
            flagged(&d),
            vec![(0x100, Reason::Read), (0x300, Reason::Region)]
        );
    }

    /// The program start at 0x100, whose block 1 (dead at the start when
    /// `dead`) tail-calls 0x200, a handler that makes an unknown call and
    /// then reads.
    fn start_and_handler(dead: bool) -> Program {
        let start = func(
            0x100,
            vec![
                bb(0x100, false, false, &[1, 2], End::Fall),
                Bb {
                    start_dead: dead,
                    ..bb(
                        0x110,
                        false,
                        false,
                        &[],
                        End::Exit(Targets::Known(vec![0x200])),
                    )
                },
                bb(0x120, false, false, &[], End::Return),
            ],
            &[],
        );
        let handler = func(
            0x200,
            vec![
                bb(0x200, false, false, &[1], End::Call(Targets::Unknown)),
                bb(0x210, false, false, &[], End::Return),
            ],
            &[1],
        );
        Program {
            fns: vec![start, handler],
            escaped: BTreeSet::new(),
            starts: BTreeSet::from([0x100]),
        }
    }

    #[test]
    fn a_function_entered_only_from_dead_start_code_never_runs() {
        let d = decide(&start_and_handler(true));
        assert!(!d.program_wide());
        assert!(d.flagged.is_empty());
        assert!(!d.regions[0].entered);
    }

    #[test]
    fn a_read_in_dead_start_code_never_runs() {
        let mut p = start_and_handler(true);
        p.fns[0].reads = vec![(0x110, 1)];
        p.fns[0].blocks[0].end = End::Call(Targets::Unknown);
        p.fns[1].reads.clear();
        let d = decide(&p);
        assert!(!d.program_wide());
        assert!(d.regions.iter().all(|r| !r.entered));
        p.fns[0].blocks[1].start_dead = false;
        assert!(decide(&p).program_wide());
    }

    #[test]
    fn a_handler_entered_another_way_keeps_its_region() {
        // The branch to it is not proven dead.
        assert!(decide(&start_and_handler(false)).program_wide());
        // The start may be entered from elsewhere, so its dead blocks may run.
        let mut p = start_and_handler(true);
        p.escaped.insert(0x100);
        assert!(decide(&p).program_wide());
        let mut p = start_and_handler(true);
        p.fns.push(func(
            0x300,
            vec![bb(
                0x300,
                false,
                false,
                &[],
                End::Exit(Targets::Known(vec![0x100])),
            )],
            &[],
        ));
        assert!(decide(&p).program_wide());
        // Code the analysis cannot see may call the handler.
        let mut p = start_and_handler(true);
        p.escaped.insert(0x200);
        assert!(decide(&p).program_wide());
        // A live call site reaches it.
        let mut p = start_and_handler(true);
        p.fns.push(func(
            0x300,
            vec![
                bb(0x300, false, false, &[1], call(0x200)),
                bb(0x310, false, false, &[], End::Return),
            ],
            &[],
        ));
        assert!(decide(&p).program_wide());
        // Dead blocks count only in a start.
        let mut p = start_and_handler(true);
        p.starts.clear();
        assert!(decide(&p).program_wide());
    }

    #[test]
    fn an_indirect_call_or_an_escaped_entry_is_unbounded() {
        let p = Program {
            fns: vec![func(
                0x100,
                vec![
                    bb(0x100, false, false, &[1], End::Call(Targets::Unknown)),
                    bb(0x110, false, false, &[], End::Return),
                ],
                &[1],
            )],
            escaped: BTreeSet::new(),
            starts: BTreeSet::new(),
        };
        let d = decide(&p);
        assert!(d.program_wide());
        assert!(
            d.regions[0]
                .unbounded
                .as_deref()
                .unwrap()
                .contains("unknown code")
        );
        // The read at the entry of a function whose address is taken.
        let p = Program {
            fns: vec![func(
                0x100,
                vec![bb(0x100, false, false, &[], End::Return)],
                &[0],
            )],
            escaped: BTreeSet::from([0x100]),
            starts: BTreeSet::new(),
        };
        assert!(decide(&p).program_wide());
        // A write before the indirect call bounds it.
        let p = Program {
            fns: vec![func(
                0x100,
                vec![
                    bb(0x100, false, true, &[1], End::Fall),
                    bb(0x104, false, false, &[2], End::Fall),
                    bb(0x110, false, false, &[], End::Return),
                ],
                &[2],
            )],
            escaped: BTreeSet::from([0x100]),
            starts: BTreeSet::new(),
        };
        assert!(!decide(&p).program_wide());
        // A resume point after setjmp may be entered from any longjmp.
        let mut f = func(
            0x100,
            vec![
                bb(0x100, false, false, &[1], call(0x200)),
                bb(0x110, false, false, &[], End::Return),
            ],
            &[1],
        );
        f.blocks[1].foreign = true;
        let p = Program {
            fns: vec![f, leaf(0x200, false)],
            escaped: BTreeSet::new(),
            starts: BTreeSet::new(),
        };
        assert!(decide(&p).program_wide());
    }

    #[test]
    fn recursion_ends() {
        // 0x200: fp; if c { call 0x200 }; ret. 0x100: call 0x200; read.
        let r = func(
            0x200,
            vec![
                bb(0x200, true, false, &[1, 2], End::Fall),
                bb(0x210, false, false, &[2], call(0x200)),
                bb(0x220, false, false, &[], End::Return),
            ],
            &[],
        );
        let f = func(
            0x100,
            vec![
                bb(0x100, false, false, &[1], call(0x200)),
                bb(0x110, false, false, &[], End::Return),
            ],
            &[1],
        );
        // 0x300 writes, then recurses or not: it writes on every path.
        let w = func(
            0x300,
            vec![
                bb(0x300, false, true, &[1, 2], End::Fall),
                bb(0x310, false, false, &[2], call(0x300)),
                bb(0x320, false, false, &[], End::Return),
            ],
            &[],
        );
        // 0x400 recurses before writing on one path only.
        let n = func(
            0x400,
            vec![
                bb(0x400, false, false, &[1, 2], End::Fall),
                bb(0x410, false, false, &[], call(0x400)),
                bb(0x420, false, true, &[], End::Return),
            ],
            &[],
        );
        let p = Program {
            fns: vec![f, r, w, n],
            escaped: BTreeSet::new(),
            starts: BTreeSet::new(),
        };
        let d = decide(&p);
        assert_eq!(
            flagged(&d),
            vec![(0x100, Reason::Read), (0x200, Reason::Region)]
        );
        let idx: HashMap<u64, usize> = p
            .fns
            .iter()
            .enumerate()
            .map(|(i, f)| (f.entry, i))
            .collect();
        let must = must_write(&p, &idx);
        assert!(must.contains(&0x300));
        // 0x400's recursive path never returns, so its returning path writes.
        assert!(must.contains(&0x400));
    }

    #[test]
    fn a_read_in_a_leaf_climbs_into_its_callers() {
        // 0x100 (read at entry) is called by 0x200 after FP and by 0x300
        // after a write; 0x200 is called by 0x400 after FP.
        let l = func(0x100, vec![bb(0x100, false, false, &[], End::Return)], &[0]);
        let a = func(
            0x200,
            vec![
                bb(0x200, true, false, &[1], call(0x100)),
                bb(0x210, false, false, &[], End::Return),
            ],
            &[],
        );
        let b = func(
            0x300,
            vec![
                Bb {
                    fp_tail: false,
                    ..bb(0x300, true, true, &[1], call(0x100))
                },
                bb(0x310, false, false, &[], End::Return),
            ],
            &[],
        );
        let c = func(
            0x400,
            vec![
                bb(0x400, true, false, &[1], call(0x200)),
                bb(0x410, true, false, &[], End::Return),
            ],
            &[],
        );
        let p = Program {
            fns: vec![l, a, b, c],
            escaped: BTreeSet::new(),
            starts: BTreeSet::new(),
        };
        let d = decide(&p);
        assert!(!d.program_wide());
        assert_eq!(
            flagged(&d),
            vec![
                (0x100, Reason::Read),
                (0x200, Reason::Region),
                (0x400, Reason::Region)
            ]
        );
    }

    #[test]
    fn reads_on_two_threads_see_only_their_own_thread() {
        // Thread 0x100: call 0x500 (fp); read; call 0x700 (fp).
        // Thread 0x200: call 0x600 (fp); read.
        let t1 = func(
            0x100,
            vec![
                bb(0x100, false, false, &[1], call(0x500)),
                bb(0x110, false, false, &[2], call(0x700)),
                bb(0x120, false, false, &[], End::Return),
            ],
            &[1],
        );
        let t2 = func(
            0x200,
            vec![
                bb(0x200, false, false, &[1], call(0x600)),
                bb(0x210, false, false, &[], End::Return),
            ],
            &[1],
        );
        let p = Program {
            fns: vec![
                t1,
                t2,
                leaf(0x500, true),
                leaf(0x600, true),
                leaf(0x700, true),
            ],
            escaped: BTreeSet::new(),
            starts: BTreeSet::new(),
        };
        let d = decide(&p);
        assert!(!d.program_wide());
        assert_eq!(d.regions[0].fp, BTreeSet::from([0x500]));
        assert_eq!(d.regions[1].fp, BTreeSet::from([0x600]));
        assert!(!d.flagged.contains_key(&0x700));
    }
}

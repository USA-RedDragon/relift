//! One function run: the instructions reached from an entry, run over
//! abstract values until the states at every instruction stop changing.

use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use relift_cpu_mips::lift;
use relift_ir::Block;

use crate::eval::{self, Dma, End};
use crate::image::Image;
use crate::report::UnresolvedKind;
use crate::val::{ABI_KEEP, Abs, Base, EPC, ERROR_EPC, PHYS, RA, SLOTS, SP, State, Sym, sext32};

/// A code location: segment and kseg0 address.
pub(crate) type Loc = (u32, u32);

/// What a caller needs to know of a function.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Summary {
    /// Some path returns to the entry return address.
    pub returns: bool,
    /// The slots every return leaves as they were at entry.
    pub keep: u64,
    /// Words of the caller's frame it may write.
    pub frame_dirty: u64,
    /// DMAs whose operands depend on the entry values.
    pub effects: Rc<[Dma]>,
}

impl Summary {
    /// What a caller assumes of a callee not analyzed yet.
    pub(crate) fn assumed() -> Summary {
        Summary {
            returns: true,
            keep: ABI_KEEP,
            frame_dirty: 0xF,
            effects: Rc::from(Vec::new()),
        }
    }
}

/// A DMA with constant operands.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum DmaConst {
    /// Cartridge address to RDRAM physical address, length in bytes.
    Pi { dram: u32, cart: u32, len: u32 },
    /// RDRAM physical address to SP memory offset (`0x1000` and up is
    /// IMEM), the length word.
    Sp { mem: u32, dram: u32, len: u32 },
}

/// What the analysis keeps of one run.
#[derive(Default)]
pub(crate) struct FnRun {
    /// The blocks reached: address and length (8 for a branch with its
    /// delay slot).
    pub body: Vec<(u32, u8)>,
    /// Calls: site and callee.
    pub calls: Vec<(u32, Loc)>,
    /// Jumps to the entries of other functions.
    pub tails: Vec<Loc>,
    /// Transfers and DMAs left unknown: site, kind, detail.
    pub unresolved: Vec<(u32, UnresolvedKind, String)>,
    /// Constant reads: segment, offset, size.
    pub data: Vec<(u32, u32, u8)>,
    /// Constant code addresses the code stores or passes to a call.
    pub consts: Vec<u32>,
    /// Physical addresses loads and stores use as a base.
    pub bases: Vec<u32>,
    /// DMAs with constant operands: site and DMA.
    pub dma: Vec<(u32, DmaConst)>,
    /// Jump tables: site, physical base, entries followed.
    pub tables: Vec<(u32, u32, u32)>,
    /// A path reached a word that does not lift.
    pub invalid: bool,
    /// The run stopped at its step limit.
    pub aborted: bool,
    /// The summary.
    pub summary: Option<Summary>,
}

/// What a run reads besides the image.
pub(crate) struct Env<'a> {
    pub image: &'a Image<'a>,
    pub summaries: &'a HashMap<Loc, Summary>,
    pub entries: &'a HashSet<Loc>,
}

impl Env<'_> {
    fn summary(&self, l: Loc) -> Summary {
        self.summaries
            .get(&l)
            .cloned()
            .unwrap_or_else(Summary::assumed)
    }
}

/// The step limit of one run.
const MAX_STEPS: usize = 400_000;
/// The most entries a jump table gives.
const MAX_TABLE: u32 = 256;
/// The most zero words a path runs through.
const MAX_ZEROS: u8 = 16;

struct Run<'a> {
    env: &'a Env<'a>,
    seg: u32,
    entry: u32,
    states: HashMap<u32, State>,
    work: Vec<u32>,
    blocks: HashMap<u32, Option<Rc<Block>>>,
    returns: Vec<(State, u64)>,
    open: bool,
    dirty: u64,
    effects: Vec<Dma>,
    out: FnRun,
}

/// Whether `w` decodes to a VR4300 instruction with a template.
pub(crate) fn valid(w: u32) -> bool {
    relift_cpu_mips::decode::decode(w).is_some_and(lift::has_template)
}

/// Runs the function at `entry`.
pub(crate) fn run(env: &Env<'_>, entry: Loc) -> FnRun {
    let mut r = Run {
        env,
        seg: entry.0,
        entry: entry.1,
        states: HashMap::new(),
        work: vec![entry.1],
        blocks: HashMap::new(),
        returns: Vec::new(),
        open: false,
        dirty: 0,
        effects: Vec::new(),
        out: FnRun::default(),
    };
    r.states.insert(entry.1, State::entry());
    let mut steps = 0;
    while let Some(pc) = r.work.pop() {
        steps += 1;
        if steps > MAX_STEPS {
            r.out.aborted = true;
            break;
        }
        r.step(pc);
    }
    r.finish()
}

/// The value `a` of a callee's entry terms, in the caller's state `st`.
pub(crate) fn subst(a: Abs, st: &State) -> Abs {
    let Abs::S(s) = a else {
        return a;
    };
    let v = match s.base {
        Base::Entry(i) => st.regs[usize::from(i)],
        Base::Frame(o) => match st.regs[SP].stack_off() {
            Some(d) => eval::stack_value(st, d + o),
            None => Abs::Top,
        },
    };
    offset(v, s.off, s.phys)
}

fn offset(v: Abs, off: u32, phys: bool) -> Abs {
    match v {
        Abs::K(x) => {
            let y = (x as u32).wrapping_add(off);
            Abs::K(if phys {
                u128::from(y & PHYS)
            } else {
                sext32(y)
            })
        }
        Abs::S(t) if !t.phys || off == 0 => Abs::S(Sym {
            off: t.off.wrapping_add(off),
            phys: t.phys || phys,
            ..t
        }),
        _ => Abs::Top,
    }
}

fn subst_dma(d: Dma, st: &State) -> Dma {
    match d {
        Dma::Pi { dram, cart, len } => Dma::Pi {
            dram: subst(dram, st),
            cart: subst(cart, st),
            len: subst(len, st),
        },
        Dma::Sp { mem, dram, len } => Dma::Sp {
            mem: subst(mem, st),
            dram: subst(dram, st),
            len: subst(len, st),
        },
    }
}

fn is_entry_term(a: Abs) -> bool {
    matches!(a, Abs::K(_) | Abs::S(_))
}

impl Run<'_> {
    fn block(&mut self, pc: u32) -> Option<Rc<Block>> {
        if let Some(b) = self.blocks.get(&pc) {
            return b.clone();
        }
        let img = self.env.image;
        let b = img.word(self.seg, pc).and_then(|w| {
            let slot = img.word(self.seg, pc.wrapping_add(4)).unwrap_or(0);
            lift::lift(sext32(pc) as u64, w, slot)
                .ok()
                .map(|l| Rc::new(l.block))
        });
        self.blocks.insert(pc, b.clone());
        b
    }

    fn add(&mut self, to: u32, st: State) {
        match self.states.get_mut(&to) {
            Some(old) => {
                if old.merge(&st) {
                    self.work.push(to);
                }
            }
            None => {
                self.states.insert(to, st);
                self.work.push(to);
            }
        }
    }

    fn unresolved(&mut self, at: u32, kind: UnresolvedKind, detail: String) {
        self.out.unresolved.push((at, kind, detail));
    }

    fn step(&mut self, pc: u32) {
        let Some(mut st) = self.states.get(&pc).cloned() else {
            return;
        };
        let Some(word) = self.env.image.word(self.seg, pc) else {
            self.unresolved(pc, UnresolvedKind::NoCode, String::new());
            return;
        };
        if word == 0 {
            st.zeros += 1;
            if st.zeros > MAX_ZEROS {
                return;
            }
        } else {
            st.zeros = 0;
        }
        let Some(block) = self.block(pc) else {
            self.out.invalid = true;
            return;
        };
        self.out.body.push((pc, block.len as u8));
        let (seg, image) = (self.seg, self.env.image);
        let mut data = Vec::new();
        let out = {
            let mut read = |pa: u32, n: u32| {
                let (v, s, rel) = image.read(seg, pa, n)?;
                data.push((s, rel, n as u8));
                Some(v)
            };
            eval::eval(&block, &st, &mut read)
        };
        self.out.data.extend(data);
        self.out.consts.extend(&out.consts);
        self.out.bases.extend(&out.bases);
        self.dirty |= out.st.frame_dirty;
        for d in &out.dma {
            self.dma(pc, *d);
        }
        let after = pc.wrapping_add(block.len);
        match out.end {
            End::Raise => return,
            End::Syscall => {
                let mut s = out.st;
                s.clobber(ABI_KEEP);
                self.add(after, s);
                return;
            }
            End::Exit => {}
        }
        let call = out.link.is_some();
        if call {
            for a in &out.st.regs[4..8] {
                if let Some(c) = a.k().and_then(eval::code_like) {
                    self.out.consts.push(c);
                }
            }
        }
        for t in out.targets.clone() {
            match t {
                Abs::K(v) if v >> 32 == 0xFFFF_FFFF || v >> 32 == 0 => {
                    if call {
                        self.call(pc, after, v as u32, &out.st);
                    } else {
                        self.jump(pc, v as u32, &out.st);
                    }
                }
                Abs::S(Sym {
                    base: Base::Entry(r),
                    off: 0,
                    phys: false,
                }) if !call && usize::from(r) == RA => {
                    self.returns.push((out.st.clone(), u64::MAX));
                }
                Abs::S(Sym {
                    base: Base::Entry(r),
                    off: 0,
                    ..
                }) if usize::from(r) == EPC || usize::from(r) == ERROR_EPC => {}
                Abs::Tab { .. } => self.table(pc, after, t, call, &out.st),
                other => {
                    let kind = if call {
                        UnresolvedKind::Call
                    } else {
                        UnresolvedKind::Jump
                    };
                    self.unresolved(pc, kind, format!("{other:?}"));
                    if call {
                        let mut s = out.st.clone();
                        s.clobber(ABI_KEEP);
                        self.add(after, s);
                    } else {
                        self.open = true;
                    }
                }
            }
        }
    }

    fn dma(&mut self, pc: u32, d: Dma) {
        let k = |a: Abs| a.k32();
        let c = match d {
            Dma::Pi { dram, cart, len } => match (k(dram), k(cart), k(len)) {
                (Some(d), Some(c), Some(l)) => Some(DmaConst::Pi {
                    dram: d & PHYS,
                    cart: c,
                    len: (l & 0x00FF_FFFF) + 1,
                }),
                _ => None,
            },
            Dma::Sp { mem, dram, len } => match (k(mem), k(dram), k(len)) {
                (Some(m), Some(d), Some(l)) => Some(DmaConst::Sp {
                    mem: m & 0x1FFF,
                    dram: d & 0x00FF_FFFF,
                    len: l,
                }),
                _ => None,
            },
        };
        match c {
            Some(c) => self.out.dma.push((pc, c)),
            None => {
                let ops = match d {
                    Dma::Pi { dram, cart, len }
                    | Dma::Sp {
                        mem: dram,
                        dram: cart,
                        len,
                    } => [dram, cart, len],
                };
                if ops.iter().all(|a| is_entry_term(*a)) {
                    if !self.effects.contains(&d) {
                        self.effects.push(d);
                    }
                } else {
                    self.unresolved(pc, UnresolvedKind::Dma, format!("{d:?}"));
                }
            }
        }
    }

    /// The state after a call returns: the callee's summary applied.
    fn resume(&mut self, site: u32, after: u32, st: &State, s: &Summary) {
        for e in s.effects.iter() {
            self.dma(site, subst_dma(*e, st));
        }
        if !s.returns {
            return;
        }
        let mut post = st.clone();
        post.clobber(s.keep);
        match st.regs[SP].stack_off() {
            Some(d) => {
                for i in 0..crate::val::FRAME_WORDS {
                    if s.frame_dirty >> i & 1 == 1 {
                        let o = d + 4 * i;
                        post.stack
                            .retain(|e| e.0 + i32::from(e.1) <= o || e.0 >= o + 4);
                        post.frame_dirty |= crate::val::frame_bits(o, 4);
                    }
                }
            }
            None => post.stack.clear(),
        }
        self.add(after, post);
    }

    fn callees(&mut self, site: u32, addr: u32) -> Vec<Loc> {
        let segs = self.env.image.targets(self.seg, addr);
        if segs.is_empty() && self.env.image.segs[self.seg as usize].shared() {
            self.unresolved(site, UnresolvedKind::Outside, format!("{addr:#010x}"));
        }
        segs.into_iter().map(|s| (s, addr)).collect()
    }

    fn call(&mut self, site: u32, after: u32, addr: u32, st: &State) {
        let callees = self.callees(site, addr);
        let mut sum: Option<Summary> = None;
        for c in callees {
            self.out.calls.push((site, c));
            let s = self.env.summary(c);
            sum = Some(match sum {
                None => s,
                Some(a) => Summary {
                    returns: a.returns || s.returns,
                    keep: a.keep & s.keep,
                    frame_dirty: a.frame_dirty | s.frame_dirty,
                    effects: a.effects.iter().chain(s.effects.iter()).copied().collect(),
                },
            });
        }
        let s = sum.unwrap_or_else(Summary::assumed);
        self.resume(site, after, st, &s);
    }

    fn tail(&mut self, to: Loc, st: &State) {
        self.out.tails.push(to);
        let s = self.env.summary(to);
        for e in s.effects.iter() {
            let d = subst_dma(*e, st);
            self.dma(to.1, d);
        }
        if s.returns {
            self.returns.push((st.clone(), s.keep));
        }
    }

    fn jump(&mut self, site: u32, addr: u32, st: &State) {
        let here = &self.env.image.segs[self.seg as usize];
        if here.holds(addr) {
            let loc = (self.seg, addr);
            if addr != self.entry && self.env.entries.contains(&loc) {
                self.tail(loc, st);
            } else {
                self.add(addr, st.clone());
            }
            return;
        }
        let others = self.callees(site, addr);
        if others.is_empty() {
            self.open = true;
        }
        for o in others {
            self.tail(o, st);
        }
    }

    fn table(&mut self, site: u32, after: u32, t: Abs, call: bool, st: &State) {
        let Abs::Tab { base, stride, n } = t else {
            return;
        };
        let img = self.env.image;
        let mut count = 0;
        for i in 0..n.min(MAX_TABLE) {
            let pa = base.wrapping_add(i.wrapping_mul(stride));
            let Some((v, s, rel)) = img.read(self.seg, pa & PHYS, 4) else {
                break;
            };
            let t = v as u32;
            let ok = if call {
                !img.targets(self.seg, t).is_empty()
            } else {
                img.segs[self.seg as usize].holds(t)
            };
            let seg_for = if call { None } else { Some(self.seg) };
            let word_ok = match seg_for {
                Some(sg) => img.word(sg, t).is_some_and(valid),
                None => img
                    .targets(self.seg, t)
                    .iter()
                    .any(|sg| img.word(*sg, t).is_some_and(valid)),
            };
            if !ok || !word_ok || t & 3 != 0 {
                break;
            }
            self.out.data.push((s, rel, 4));
            count += 1;
            if call {
                self.call(site, after, t, st);
            } else {
                self.jump(site, t, st);
            }
        }
        self.out.tables.push((site, base, count));
        if count == 0 {
            let kind = if call {
                UnresolvedKind::Call
            } else {
                UnresolvedKind::Jump
            };
            self.unresolved(site, kind, format!("table at {base:#x}"));
            if !call {
                self.open = true;
            }
        }
    }

    fn finish(mut self) -> FnRun {
        let mut keep = if self.returns.is_empty() {
            ABI_KEEP
        } else {
            u64::MAX
        };
        for (st, k) in &self.returns {
            let mut bits = 0u64;
            for i in 0..SLOTS {
                if st.regs[i] == Abs::entry(i) {
                    bits |= 1 << i;
                }
            }
            keep &= bits & k;
        }
        keep |= 1;
        self.out.summary = Some(Summary {
            returns: !self.returns.is_empty() || self.open,
            keep,
            frame_dirty: self.dirty,
            effects: Rc::from(std::mem::take(&mut self.effects)),
        });
        self.out
    }
}

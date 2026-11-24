//! Whole workgroups: every invocation of a workgroup with its barriers,
//! subgroup shuffles and shared memory, and whole dispatches of
//! workgroups.
//!
//! A workgroup's invocations run on one thread, a subgroup (warp) at a
//! time, its lanes in lockstep as NVIDIA hardware runs a warp: no lane
//! runs past an access to memory other invocations see (global or
//! workgroup), a shuffle, a call or return, or a jump back, before every
//! lane at the same place has run it. Lanes at different places
//! (divergent flow) go lowest place first ([`Inv::pos_cmp`]): the earlier
//! path in the code runs first, a loop runs until its lanes leave, and
//! lanes meet again where their paths join. A shuffle runs with the lanes
//! that reach it together as the active ones. Warps run one after another
//! up to a barrier, which releases when every invocation that has not
//! returned waits at one.
//! What a device would leave undefined or a translation
//! would rely on is written down as a [`Note`]: a barrier some invocations
//! returned before, invocations that wait at different barriers, a shuffle
//! run by part of its subgroup, a shuffle that reads an inactive lane. An
//! invocation that runs longer than its step budget, or touches memory
//! outside every region, stops the whole workgroup with a [`Failure`] that
//! names the block each invocation was in.
//!
//! [`run_dispatch`] runs the workgroups of one dispatch on several threads;
//! they share only global memory ([`SharedMemory`]), where loads and
//! stores take no lock and atomic instructions take one lock for the
//! whole dispatch.

use std::collections::BTreeMap;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};

use crate::arith::Fx;
use crate::code::Program;
use crate::exec::{Event, Inv, Mem, Plain, find, le};
use crate::{Env, Memory, Module};

/// Where workgroup variables are placed in the shared memory.
pub const WORKGROUP_BASE: u64 = 0x7f00_0000_0000_0000;

/// Something a device may do differently, seen while running.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Note {
    /// A barrier released while these invocations had already returned.
    BarrierAfterReturn { block: u32, returned: u32 },
    /// Invocations waited at barriers in different blocks.
    BarrierBlocks { blocks: Vec<u32> },
    /// A shuffle in `block` ran with `active` of its subgroup's lanes.
    PartialShuffle { block: u32, active: u32 },
    /// A shuffle read a lane that was not active (the result is garbage).
    InactiveLane { block: u32 },
}

/// Why a workgroup did not finish.
#[derive(Debug)]
pub struct Failure {
    /// The first invocation that stopped, and why.
    pub invocation: u32,
    pub message: String,
    /// (function, block) each invocation was last in.
    pub at: Vec<(u32, u32)>,
    /// Steps each invocation had run.
    pub steps: Vec<u64>,
}

/// What a finished workgroup leaves.
pub struct Done {
    pub mem: Memory,
    pub notes: BTreeMap<Note, u64>,
    /// Steps each invocation ran.
    pub steps: Vec<u64>,
}

#[derive(Clone, Copy, PartialEq)]
enum Wait {
    Run,
    /// Stopped before an instruction its warp runs in lockstep.
    At,
    Barrier(u32),
    Shuffle {
        block: u32,
        value: u64,
        lane: u32,
    },
    Done,
}

struct State {
    wait: Vec<Wait>,
    out: Vec<u64>,
    notes: BTreeMap<Note, u64>,
    subgroup: usize,
}

/// Memory many workgroups read and write at once: [`Memory`]'s regions as
/// atomic words accessed with relaxed order. A load or store is not one
/// atomic access, as on a GPU, where only atomic instructions are; each
/// byte a store writes changes on its own, never its neighbors.
pub struct SharedMemory {
    regions: Vec<(u64, Words)>,
    /// Every atomic read, modify and write holds this.
    atomic: Mutex<()>,
}

struct Words {
    len: u64,
    w: Box<[AtomicU32]>,
}

impl From<Memory> for SharedMemory {
    fn from(m: Memory) -> SharedMemory {
        SharedMemory {
            regions: m
                .regions
                .into_iter()
                .map(|(base, b)| {
                    let w = b
                        .chunks(4)
                        .map(|c| {
                            let mut x = [0u8; 4];
                            x[..c.len()].copy_from_slice(c);
                            AtomicU32::new(u32::from_le_bytes(x))
                        })
                        .collect();
                    (
                        base,
                        Words {
                            len: b.len() as u64,
                            w,
                        },
                    )
                })
                .collect(),
            atomic: Mutex::new(()),
        }
    }
}

impl SharedMemory {
    fn get(&self, addr: u64, n: u64, hint: &mut usize) -> Option<u64> {
        let (r, o) = find(&self.regions, |w| w.len, addr, n, hint)?;
        let w = &self.regions[r].1.w;
        if o % 4 == 0 && n == 4 {
            return Some(u64::from(w[o / 4].load(Ordering::Relaxed)));
        }
        let mut v = 0u64;
        for k in 0..n as usize {
            let b = (w[(o + k) / 4].load(Ordering::Relaxed) >> (8 * ((o + k) % 4))) & 0xff;
            v |= u64::from(b) << (8 * k);
        }
        Some(v)
    }

    fn put(&self, addr: u64, n: u64, v: u64, hint: &mut usize) -> bool {
        let Some((r, o)) = find(&self.regions, |w| w.len, addr, n, hint) else {
            return false;
        };
        let w = &self.regions[r].1.w;
        if o % 4 == 0 && n == 4 {
            w[o / 4].store(v as u32, Ordering::Relaxed);
            return true;
        }
        for k in 0..n as usize {
            let sh = 8 * ((o + k) % 4);
            let b = ((v >> (8 * k)) & 0xff) as u32;
            let _ = w[(o + k) / 4].fetch_update(Ordering::Relaxed, Ordering::Relaxed, |x| {
                Some((x & !(0xff << sh)) | (b << sh))
            });
        }
        true
    }

    /// The `n` bytes at `addr`, little-endian.
    pub fn read(&self, addr: u64, n: u64) -> u64 {
        self.get(addr, n, &mut 0)
            .unwrap_or_else(|| panic!("access to unmapped memory {addr:#x}+{n}"))
    }

    /// Writes the low `n` bytes of `v` at `addr`, little-endian.
    pub fn write(&self, addr: u64, n: u64, v: u64) {
        if !self.put(addr, n, v, &mut 0) {
            panic!("access to unmapped memory {addr:#x}+{n}");
        }
    }

    /// Back to plain memory.
    pub fn into_memory(self) -> Memory {
        Memory {
            regions: self
                .regions
                .into_iter()
                .map(|(base, w)| {
                    let mut b: Vec<u8> =
                        w.w.into_vec()
                            .into_iter()
                            .flat_map(|x| x.into_inner().to_le_bytes())
                            .collect();
                    b.truncate(w.len as usize);
                    (base, b)
                })
                .collect(),
        }
    }
}

/// Locks a mutex, taking it over from a thread that panicked holding it.
fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// The memory behind a workgroup's own: one workgroup's [`Memory`], or a
/// dispatch's [`SharedMemory`].
trait Global {
    fn get(&mut self, addr: u64, n: u64) -> Option<u64>;
    fn put(&mut self, addr: u64, n: u64, v: u64) -> bool;
}

impl Global for Plain<'_> {
    fn get(&mut self, addr: u64, n: u64) -> Option<u64> {
        Plain::get(self, addr, n)
    }
    fn put(&mut self, addr: u64, n: u64, v: u64) -> bool {
        Plain::put(self, addr, n, v)
    }
}

struct SharedRef<'a> {
    m: &'a SharedMemory,
    hint: usize,
}

impl Global for SharedRef<'_> {
    fn get(&mut self, addr: u64, n: u64) -> Option<u64> {
        self.m.get(addr, n, &mut self.hint)
    }
    fn put(&mut self, addr: u64, n: u64, v: u64) -> bool {
        self.m.put(addr, n, v, &mut self.hint)
    }
}

/// What a workgroup's invocations see: the read-only uniform blocks first,
/// then the global memory, then the workgroup's own shared memory.
struct GroupMem<'a, G> {
    uniform: Plain<'a>,
    global: G,
    local: Option<(u64, &'a mut Vec<u8>)>,
    /// What atomic instructions hold (a dispatch's).
    atomic: Option<&'a Mutex<()>>,
}

impl<G: Global> Mem for GroupMem<'_, G> {
    fn read(&mut self, addr: u64, n: u64) -> u64 {
        if let Some(v) = self.uniform.get(addr, n) {
            return v;
        }
        if let Some(v) = self.global.get(addr, n) {
            return v;
        }
        if let Some((base, b)) = &self.local
            && addr >= *base
            && addr + n <= *base + b.len() as u64
        {
            let o = (addr - *base) as usize;
            return le(&b[o..o + n as usize]);
        }
        panic!("access to unmapped memory {addr:#x}+{n}")
    }

    fn write(&mut self, addr: u64, n: u64, v: u64) {
        if self.global.put(addr, n, v) {
            return;
        }
        if let Some((base, b)) = &mut self.local
            && addr >= *base
            && addr + n <= *base + b.len() as u64
        {
            let o = (addr - *base) as usize;
            b[o..o + n as usize].copy_from_slice(&v.to_le_bytes()[..n as usize]);
            return;
        }
        panic!("access to unmapped memory {addr:#x}+{n}")
    }

    fn atomic<R>(&mut self, f: impl FnOnce(&mut Self) -> R) -> R {
        let _held = self.atomic.map(lock);
        f(self)
    }
}

fn message(p: &(dyn std::any::Any + Send)) -> String {
    p.downcast_ref::<String>()
        .cloned()
        .or_else(|| p.downcast_ref::<&str>().map(|s| (*s).to_string()))
        .unwrap_or_else(|| "panic".into())
}

impl State {
    /// Runs the shuffle the lanes `active` of subgroup `s` wait at, at
    /// the same place.
    fn shuffle(&mut self, s: usize, active: &[usize]) {
        let base = s * self.subgroup;
        let Wait::Shuffle { block, .. } = self.wait[active[0]] else {
            unreachable!()
        };
        if active.len() != self.subgroup {
            *self
                .notes
                .entry(Note::PartialShuffle {
                    block,
                    active: active.len() as u32,
                })
                .or_default() += 1;
        }
        let mut vals = vec![None; self.subgroup];
        for &k in active {
            if let Wait::Shuffle { value, .. } = self.wait[k] {
                vals[k - base] = Some(value);
            }
        }
        for &k in active {
            if let Wait::Shuffle { lane, .. } = self.wait[k] {
                let v = vals.get(lane as usize).copied().flatten();
                if v.is_none() {
                    *self.notes.entry(Note::InactiveLane { block }).or_default() += 1;
                }
                self.out[k] = v.unwrap_or(crate::GARBAGE);
                self.wait[k] = Wait::Run;
            }
        }
    }

    /// Every live invocation waits at a barrier: releases them; false when
    /// some do not.
    fn release_barrier(&mut self) -> bool {
        let n = self.wait.len();
        let at_barrier: Vec<u32> = self
            .wait
            .iter()
            .filter_map(|w| match w {
                Wait::Barrier(b) => Some(*b),
                _ => None,
            })
            .collect();
        let live = self.wait.iter().filter(|w| **w != Wait::Done).count();
        if at_barrier.is_empty() || at_barrier.len() != live {
            return false;
        }
        let returned = (n - live) as u32;
        let mut blocks = at_barrier;
        blocks.sort_unstable();
        blocks.dedup();
        if returned > 0 {
            *self
                .notes
                .entry(Note::BarrierAfterReturn {
                    block: blocks[0],
                    returned,
                })
                .or_default() += 1;
        }
        if blocks.len() > 1 {
            *self
                .notes
                .entry(Note::BarrierBlocks { blocks })
                .or_default() += 1;
        }
        for w in &mut self.wait {
            if matches!(w, Wait::Barrier(_)) {
                *w = Wait::Run;
            }
        }
        true
    }
}

/// What every workgroup of a run shares.
struct Setup<'a> {
    p: &'a Program,
    vars: &'a BTreeMap<u32, u64>,
    size: [u32; 3],
    subgroup: u32,
    max_steps: u64,
    /// Set when another workgroup failed.
    stop: Option<&'a AtomicBool>,
}

/// One workgroup over `mem`.
fn run_group<G: Global>(
    su: &Setup<'_>,
    env: &Env,
    mut mem: GroupMem<'_, G>,
) -> Result<(BTreeMap<Note, u64>, Vec<u64>), Failure> {
    let Setup {
        p,
        vars,
        size,
        subgroup,
        max_steps,
        stop,
    } = *su;
    let n = (size[0] * size[1] * size[2]) as usize;
    let sg = (subgroup as usize).max(1);
    let fx = Fx::of(env);
    let mut invs: Vec<Option<Inv>> = Vec::with_capacity(n);
    let fail = |invs: &[Option<Inv>], i: usize, message: String| Failure {
        invocation: i as u32,
        message,
        at: invs
            .iter()
            .map(|v| v.as_ref().map_or((0, 0), |v| v.at))
            .chain(std::iter::repeat((0, 0)))
            .take(n)
            .collect(),
        steps: invs
            .iter()
            .map(|v| v.as_ref().map_or(0, |v| v.steps))
            .chain(std::iter::repeat(0))
            .take(n)
            .collect(),
    };
    for i in 0..n {
        let mut e = env.clone();
        let i32_ = i as u32;
        e.local_id = [
            i32_ % size[0],
            (i32_ / size[0]) % size[1],
            i32_ / (size[0] * size[1]),
        ];
        e.local_index = i32_;
        e.subgroup_size = subgroup;
        match catch_unwind(AssertUnwindSafe(|| Inv::new(p, &e, Some(vars)))) {
            Ok(mut v) => {
                v.max_steps = max_steps;
                invs.push(Some(v));
            }
            Err(e) => {
                invs.push(None);
                return Err(fail(&invs, i, message(&*e)));
            }
        }
    }
    let mut st = State {
        wait: vec![Wait::Run; n],
        out: vec![0; n],
        notes: BTreeMap::new(),
        subgroup: sg,
    };
    let mut resume = vec![false; n];
    for v in invs.iter_mut().flatten() {
        v.lockstep = true;
    }
    loop {
        for s in 0..n.div_ceil(sg) {
            let lanes = s * sg..((s + 1) * sg).min(n);
            loop {
                for i in lanes.clone() {
                    if st.wait[i] != Wait::Run {
                        continue;
                    }
                    let inv = invs[i].as_mut().unwrap_or_else(|| unreachable!());
                    if resume[i] {
                        inv.resume(st.out[i]);
                        resume[i] = false;
                    }
                    match catch_unwind(AssertUnwindSafe(|| inv.run(p, &fx, env, &mut mem, true))) {
                        Ok(Event::Done) => st.wait[i] = Wait::Done,
                        Ok(Event::Barrier(b)) => st.wait[i] = Wait::Barrier(b),
                        Ok(Event::At) => st.wait[i] = Wait::At,
                        Ok(Event::Shuffle { block, value, lane }) => {
                            st.wait[i] = Wait::Shuffle { block, value, lane };
                            resume[i] = true;
                        }
                        Err(e) => return Err(fail(&invs, i, message(&*e))),
                    }
                    if stop.is_some_and(|s| s.load(Ordering::Relaxed)) {
                        return Err(fail(&invs, i, "workgroup stopped".into()));
                    }
                }
                let stopped = |w: &Wait| matches!(w, Wait::At | Wait::Shuffle { .. });
                let pos = |i: usize| invs[i].as_ref().unwrap_or_else(|| unreachable!());
                let Some(first) = lanes
                    .clone()
                    .filter(|&i| stopped(&st.wait[i]))
                    .min_by(|&a, &b| pos(a).pos_cmp(pos(b)).then(a.cmp(&b)))
                else {
                    break;
                };
                let active: Vec<usize> = lanes
                    .clone()
                    .filter(|&i| stopped(&st.wait[i]) && pos(i).pos_cmp(pos(first)).is_eq())
                    .collect();
                if matches!(st.wait[first], Wait::Shuffle { .. }) {
                    st.shuffle(s, &active);
                } else {
                    for &i in &active {
                        invs[i].as_mut().unwrap_or_else(|| unreachable!()).pass = true;
                        st.wait[i] = Wait::Run;
                    }
                }
            }
        }
        if st.wait.iter().all(|w| *w == Wait::Done) {
            break;
        }
        if !st.release_barrier() {
            return Err(fail(&invs, 0, "no invocation can go on".into()));
        }
    }
    let steps = invs
        .iter()
        .map(|v| v.as_ref().map_or(0, |v| v.steps))
        .collect();
    Ok((st.notes, steps))
}

/// The workgroup variables of `m`, placed from [`WORKGROUP_BASE`], and
/// their zeroed bytes.
fn workgroup_vars(m: &Module) -> (BTreeMap<u32, u64>, Vec<u8>) {
    let mut vars = BTreeMap::new();
    let mut at = WORKGROUP_BASE;
    let mut bytes = Vec::new();
    for &(id, ty, class, _) in &m.globals {
        if class != 4 {
            continue;
        }
        let crate::Ty::Ptr(pointee) = *m.ty(ty) else {
            panic!("variable type")
        };
        let sz = m.size(pointee);
        vars.insert(id, at);
        bytes.resize(bytes.len() + sz.next_multiple_of(16) as usize, 0);
        at += sz.next_multiple_of(16);
    }
    (vars, bytes)
}

/// Runs one workgroup of `size` invocations (x fastest) of `words` with
/// subgroups of `subgroup` invocations. `env` is every invocation's
/// environment but for its local id; `uniform` is memory each invocation
/// only reads (uniform blocks, which stores never reach) and sees before
/// anything else; `mem` is the memory all of them see, to which the
/// workgroup variables are added at [`WORKGROUP_BASE`] (zeroed, which a
/// device need not do). Each invocation may run `max_steps` instructions.
pub fn run_workgroup(
    words: &[u32],
    env: &Env,
    uniform: &Memory,
    mem: Memory,
    size: [u32; 3],
    subgroup: u32,
    max_steps: u64,
) -> Result<Done, Failure> {
    let mut mem = mem;
    let m = Module::parse(words);
    let (vars, bytes) = workgroup_vars(&m);
    if !bytes.is_empty() {
        mem.regions.push((WORKGROUP_BASE, bytes));
    }
    let p = m.program();
    let mut uni = uniform.clone();
    let gm = GroupMem {
        uniform: Plain {
            mem: &mut uni,
            hint: 0,
        },
        global: Plain {
            mem: &mut mem,
            hint: 0,
        },
        local: None,
        atomic: None,
    };
    let su = Setup {
        p,
        vars: &vars,
        size,
        subgroup,
        max_steps,
        stop: None,
    };
    let (notes, steps) = run_group(&su, env, gm)?;
    Ok(Done { mem, notes, steps })
}

/// What one workgroup of a dispatch leaves.
pub struct GroupDone {
    pub group: [u32; 3],
    pub notes: BTreeMap<Note, u64>,
    /// Steps each invocation ran.
    pub steps: Vec<u64>,
}

/// What a finished dispatch leaves.
pub struct DispatchDone {
    pub mem: Memory,
    /// Each workgroup, in the order given.
    pub groups: Vec<GroupDone>,
}

/// The shape of a dispatch.
#[derive(Clone, Debug)]
pub struct Dispatch {
    /// Workgroup size (x fastest).
    pub size: [u32; 3],
    /// The workgroup ids to run.
    pub groups: Vec<[u32; 3]>,
    /// Invocations per subgroup.
    pub subgroup: u32,
    /// The most instructions each invocation may run.
    pub max_steps: u64,
    /// Threads to run workgroups on.
    pub threads: usize,
}

/// Runs the workgroups of dispatch `d` of `words` on its threads, each as
/// [`run_workgroup`] runs one, over one global memory `mem`. Each
/// workgroup gets its own zeroed workgroup variables at
/// [`WORKGROUP_BASE`]. The failure is that of the first workgroup in
/// `d.groups` that failed (with its index); the others stop.
pub fn run_dispatch(
    words: &[u32],
    env: &Env,
    uniform: &Memory,
    mem: Memory,
    d: &Dispatch,
) -> Result<DispatchDone, (usize, Failure)> {
    let groups = &d.groups[..];
    let threads = d.threads;
    let m = Module::parse(words);
    let (vars, bytes) = workgroup_vars(&m);
    let p = m.program();
    let shared = SharedMemory::from(mem);
    let next = AtomicUsize::new(0);
    let stop = AtomicBool::new(false);
    type Slot = Option<Result<GroupDone, Failure>>;
    let results: Mutex<Vec<Slot>> = Mutex::new((0..groups.len()).map(|_| None).collect());
    std::thread::scope(|sc| {
        for _ in 0..threads.clamp(1, groups.len().max(1)) {
            sc.spawn(|| {
                let mut uni = uniform.clone();
                loop {
                    let k = next.fetch_add(1, Ordering::Relaxed);
                    if k >= groups.len() || stop.load(Ordering::Relaxed) {
                        break;
                    }
                    let mut e = env.clone();
                    e.group_id = groups[k];
                    let mut local = bytes.clone();
                    let gm = GroupMem {
                        uniform: Plain {
                            mem: &mut uni,
                            hint: 0,
                        },
                        global: SharedRef {
                            m: &shared,
                            hint: 0,
                        },
                        local: (!local.is_empty()).then_some((WORKGROUP_BASE, &mut local)),
                        atomic: Some(&shared.atomic),
                    };
                    let su = Setup {
                        p,
                        vars: &vars,
                        size: d.size,
                        subgroup: d.subgroup,
                        max_steps: d.max_steps,
                        stop: Some(&stop),
                    };
                    let r = run_group(&su, &e, gm);
                    let r = r.map(|(notes, steps)| GroupDone {
                        group: groups[k],
                        notes,
                        steps,
                    });
                    if r.is_err() {
                        stop.store(true, Ordering::Relaxed);
                    }
                    lock(&results)[k] = Some(r);
                }
            });
        }
    });
    let results = results
        .into_inner()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let mut done = Vec::with_capacity(groups.len());
    let mut first_fail = None;
    for (k, r) in results.into_iter().enumerate() {
        match r {
            Some(Ok(g)) => done.push(g),
            Some(Err(f)) if f.message != "workgroup stopped" => {
                first_fail.get_or_insert((k, f));
            }
            _ => {}
        }
    }
    if let Some(f) = first_fail {
        return Err(f);
    }
    Ok(DispatchDone {
        mem: shared.into_memory(),
        groups: done,
    })
}

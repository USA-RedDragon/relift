//! A whole workgroup: every invocation on its own thread, with the
//! workgroup's barriers, subgroup shuffles and shared memory, over one
//! memory all invocations see.
//!
//! Memory is [`SharedMemory`]: loads and stores take no lock, so the
//! invocations run in parallel; atomic instructions take one lock.
//! Invocations run freely between synchronization points. A barrier
//! releases when every invocation that has not returned waits at one; a
//! shuffle releases when every invocation of its subgroup waits at it, or,
//! when nothing else can run, with the invocations of the subgroup that
//! wait at it as the active ones (divergent flow). What a device would
//! leave undefined or a translation would rely on is written down as a
//! [`Note`]: a barrier some invocations returned before, invocations that
//! wait at different barriers, a shuffle run by part of its subgroup, a
//! shuffle that reads an inactive lane. An invocation that runs longer than
//! its step budget, or touches memory outside every region, stops the
//! whole workgroup with a [`Failure`] that names the block each invocation
//! was in.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};

use super::Run;
use crate::{Env, Memory, Module};

/// What an invocation stopped by another one's failure unwinds with.
const STOPPED: &str = "workgroup stopped";

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
    Barrier(u32),
    Shuffle { block: u32, value: u64, lane: u32 },
    Done,
}

struct State {
    wait: Vec<Wait>,
    out: Vec<u64>,
    notes: BTreeMap<Note, u64>,
    failed: Option<(u32, String)>,
}

/// The synchronization state of one workgroup.
pub struct Group {
    n: usize,
    subgroup: usize,
    state: Mutex<State>,
    cv: Condvar,
    /// The memory every invocation sees, each byte an atomic, so loads and
    /// stores take no lock.
    pub(crate) mem: SharedMemory,
    /// Every atomic read, modify and write holds this.
    pub(crate) atomic: Mutex<()>,
    /// Workgroup variable id -> its address.
    pub(crate) vars: BTreeMap<u32, u64>,
    func: Vec<AtomicU32>,
    block: Vec<AtomicU32>,
    stop: AtomicBool,
    steps: Vec<AtomicU64>,
}

/// Memory many invocations read and write at once: [`Memory`]'s regions,
/// each byte an atomic accessed with relaxed order. A load or store of
/// several bytes is not one atomic access, as on a GPU, where only atomic
/// instructions are.
pub struct SharedMemory {
    regions: Vec<(u64, Box<[AtomicU8]>)>,
}

impl From<Memory> for SharedMemory {
    fn from(m: Memory) -> SharedMemory {
        SharedMemory {
            regions: m
                .regions
                .into_iter()
                .map(|(base, b)| (base, b.into_iter().map(AtomicU8::new).collect()))
                .collect(),
        }
    }
}

impl SharedMemory {
    fn at(&self, addr: u64, n: u64) -> &[AtomicU8] {
        for (base, b) in &self.regions {
            if addr >= *base && addr + n <= *base + b.len() as u64 {
                let o = (addr - *base) as usize;
                return &b[o..o + n as usize];
            }
        }
        panic!("access to unmapped memory {addr:#x}+{n}");
    }

    /// The `n` bytes at `addr`, little-endian.
    pub fn read(&self, addr: u64, n: u64) -> u64 {
        self.at(addr, n).iter().enumerate().fold(0, |v, (k, b)| {
            v | u64::from(b.load(Ordering::Relaxed)) << (8 * k)
        })
    }

    /// Writes the low `n` bytes of `v` at `addr`, little-endian.
    pub fn write(&self, addr: u64, n: u64, v: u64) {
        for (k, b) in self.at(addr, n).iter().enumerate() {
            b.store((v >> (8 * k)) as u8, Ordering::Relaxed);
        }
    }

    /// Back to plain memory.
    pub fn into_memory(self) -> Memory {
        Memory {
            regions: self
                .regions
                .into_iter()
                .map(|(base, b)| {
                    (
                        base,
                        b.into_vec().into_iter().map(AtomicU8::into_inner).collect(),
                    )
                })
                .collect(),
        }
    }
}

/// Locks a mutex, taking it over from a thread that panicked holding it.
pub(crate) fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

impl Group {
    /// Notes where invocation `i` is (each block it enters).
    pub(crate) fn enter(&self, i: usize, func: u32, block: u32) {
        self.func[i].store(func, Ordering::Relaxed);
        self.block[i].store(block, Ordering::Relaxed);
    }

    /// Notes invocation `i`'s step count, and stops it when another one
    /// failed.
    pub(crate) fn count(&self, i: usize, steps: u64) {
        self.steps[i].store(steps, Ordering::Relaxed);
        if self.stop.load(Ordering::Relaxed) {
            std::panic::resume_unwind(Box::new(STOPPED));
        }
    }

    fn check(&self, st: &State) {
        if st.failed.is_some() {
            // Unwinds without the panic hook: the first failure is the report.
            std::panic::resume_unwind(Box::new(STOPPED));
        }
    }

    /// Waits at a barrier in `block`.
    pub(crate) fn barrier(&self, i: usize, block: u32) {
        let mut st = lock(&self.state);
        st.wait[i] = Wait::Barrier(block);
        self.release(&mut st);
        while st.wait[i] != Wait::Run {
            self.check(&st);
            st = self
                .cv
                .wait(st)
                .unwrap_or_else(std::sync::PoisonError::into_inner);
        }
    }

    /// Runs a shuffle in `block`: gives `value`, reads lane `lane`'s.
    pub(crate) fn shuffle(&self, i: usize, block: u32, value: u64, lane: u32) -> u64 {
        let mut st = lock(&self.state);
        st.wait[i] = Wait::Shuffle { block, value, lane };
        self.release(&mut st);
        while st.wait[i] != Wait::Run {
            self.check(&st);
            st = self
                .cv
                .wait(st)
                .unwrap_or_else(std::sync::PoisonError::into_inner);
        }
        st.out[i]
    }

    fn finish(&self, i: usize) {
        let mut st = lock(&self.state);
        st.wait[i] = Wait::Done;
        self.release(&mut st);
    }

    fn fail(&self, i: usize, message: String) {
        let mut st = lock(&self.state);
        if st.failed.is_none() {
            st.failed = Some((i as u32, message));
        }
        self.stop.store(true, Ordering::Relaxed);
        st.wait[i] = Wait::Done;
        self.cv.notify_all();
    }

    /// Releases what can go on; called with every change of `st.wait`.
    fn release(&self, st: &mut State) {
        let mut any = false;
        for s in 0..self.n.div_ceil(self.subgroup) {
            any |= self.release_shuffle(st, s, false);
        }
        let running = st.wait.contains(&Wait::Run);
        if !any && !running {
            let at_barrier: Vec<u32> = st
                .wait
                .iter()
                .filter_map(|w| match w {
                    Wait::Barrier(b) => Some(*b),
                    _ => None,
                })
                .collect();
            let live = st.wait.iter().filter(|w| **w != Wait::Done).count();
            if !at_barrier.is_empty() && at_barrier.len() == live {
                let returned = (self.n - live) as u32;
                let mut blocks = at_barrier.clone();
                blocks.sort_unstable();
                blocks.dedup();
                if returned > 0 {
                    *st.notes
                        .entry(Note::BarrierAfterReturn {
                            block: blocks[0],
                            returned,
                        })
                        .or_default() += 1;
                }
                if blocks.len() > 1 {
                    *st.notes.entry(Note::BarrierBlocks { blocks }).or_default() += 1;
                }
                for w in &mut st.wait {
                    if matches!(w, Wait::Barrier(_)) {
                        *w = Wait::Run;
                    }
                }
                any = true;
            } else if live > 0 {
                // Nothing can run: a subgroup's shuffle goes with the lanes
                // that wait at it.
                for s in 0..self.n.div_ceil(self.subgroup) {
                    if self.release_shuffle(st, s, true) {
                        any = true;
                        break;
                    }
                }
                if !any {
                    st.failed = Some((0, "no invocation can go on".into()));
                    self.stop.store(true, Ordering::Relaxed);
                }
            }
        }
        if any || st.failed.is_some() {
            self.cv.notify_all();
        }
    }

    /// Releases subgroup `s`'s shuffle when all its live lanes wait at the
    /// same one, or (`partial`) the lowest block's lanes.
    fn release_shuffle(&self, st: &mut State, s: usize, partial: bool) -> bool {
        let lanes = s * self.subgroup..((s + 1) * self.subgroup).min(self.n);
        let mut blocks: Vec<u32> = Vec::new();
        let mut live = 0;
        for k in lanes.clone() {
            match st.wait[k] {
                Wait::Done => {}
                Wait::Shuffle { block, .. } => {
                    live += 1;
                    blocks.push(block);
                }
                _ => live += 1,
            }
        }
        if blocks.is_empty() {
            return false;
        }
        blocks.sort_unstable();
        let block = blocks[0];
        let active = blocks.iter().filter(|b| **b == block).count();
        if !partial && active != live {
            return false;
        }
        if active != self.subgroup {
            *st.notes
                .entry(Note::PartialShuffle {
                    block,
                    active: active as u32,
                })
                .or_default() += 1;
        }
        let vals: Vec<Option<u64>> = lanes
            .clone()
            .map(|k| match st.wait[k] {
                Wait::Shuffle {
                    block: b, value, ..
                } if b == block => Some(value),
                _ => None,
            })
            .collect();
        for k in lanes {
            if let Wait::Shuffle { block: b, lane, .. } = st.wait[k]
                && b == block
            {
                let v = vals.get(lane as usize).copied().flatten();
                if v.is_none() {
                    *st.notes.entry(Note::InactiveLane { block }).or_default() += 1;
                }
                st.out[k] = v.unwrap_or(crate::GARBAGE);
                st.wait[k] = Wait::Run;
            }
        }
        true
    }
}

/// Runs one workgroup of `size` invocations (x fastest) of `words` with
/// subgroups of `subgroup` invocations. `env` is every invocation's
/// environment but for its local id; `uniform` is memory each invocation
/// gets a copy of and only reads (uniform blocks, which stores never
/// reach); `mem` is the memory all of them see,
/// to which the workgroup variables are added at [`WORKGROUP_BASE`] (zeroed,
/// which a device need not do). Each invocation may run `max_steps`
/// instructions.
pub fn run_workgroup(
    words: &[u32],
    env: &Env,
    uniform: &Memory,
    mem: Memory,
    size: [u32; 3],
    subgroup: u32,
    max_steps: u64,
) -> Result<Done, Failure> {
    let n = (size[0] * size[1] * size[2]) as usize;
    let mut mem = mem;
    let mut vars = BTreeMap::new();
    {
        let m = Module::parse(words);
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
        if !bytes.is_empty() {
            mem.regions.push((WORKGROUP_BASE, bytes));
        }
    }
    let g = Arc::new(Group {
        n,
        subgroup: subgroup as usize,
        state: Mutex::new(State {
            wait: vec![Wait::Run; n],
            out: vec![0; n],
            notes: BTreeMap::new(),
            failed: None,
        }),
        cv: Condvar::new(),
        mem: SharedMemory::from(mem),
        atomic: Mutex::new(()),
        vars,
        func: (0..n).map(|_| AtomicU32::new(0)).collect(),
        block: (0..n).map(|_| AtomicU32::new(0)).collect(),
        stop: AtomicBool::new(false),
        steps: (0..n).map(|_| AtomicU64::new(0)).collect(),
    });
    std::thread::scope(|sc| {
        for i in 0..n {
            let g = Arc::clone(&g);
            let mut e = env.clone();
            let i32_ = i as u32;
            e.local_id = [
                i32_ % size[0],
                (i32_ / size[0]) % size[1],
                i32_ / (size[0] * size[1]),
            ];
            e.local_index = i32_;
            e.subgroup_size = subgroup;
            std::thread::Builder::new()
                .stack_size(64 << 20)
                .spawn_scoped(sc, move || {
                    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                        let m = Module::parse(words);
                        let mut run = Run::new(&m, &e, uniform.clone());
                        run.group = Some((Arc::clone(&g), i));
                        run.max_steps = max_steps;
                        run.main();
                        g.steps[i].store(run.steps, Ordering::Relaxed);
                    }));
                    match r {
                        Ok(()) => g.finish(i),
                        Err(p) => {
                            let msg = p
                                .downcast_ref::<String>()
                                .cloned()
                                .or_else(|| p.downcast_ref::<&str>().map(|s| (*s).to_string()))
                                .unwrap_or_else(|| "panic".into());
                            g.fail(i, msg);
                        }
                    }
                })
                .expect("spawn an invocation");
        }
    });
    let g = Arc::into_inner(g).expect("the group outlives its invocations");
    let st = g
        .state
        .into_inner()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let steps = g.steps.iter().map(|s| s.load(Ordering::Relaxed)).collect();
    if let Some((invocation, message)) = st.failed {
        return Err(Failure {
            invocation,
            message,
            at: g
                .func
                .iter()
                .zip(&g.block)
                .map(|(f, b)| (f.load(Ordering::Relaxed), b.load(Ordering::Relaxed)))
                .collect(),
            steps,
        });
    }
    Ok(Done {
        mem: g.mem.into_memory(),
        notes: st.notes,
        steps,
    })
}

//! Runs SPIR-V compute programs under both interpreters, the compiled one
//! and the tree-walking reference, with seeded synthetic inputs, and
//! reports every program where they end differently: other memory, other
//! step counts, other notes or another failure message. A failure's
//! addresses are left out: which invocation of a workgroup fails first is
//! up to the reference's thread order.
//!
//! Every uniform or storage block gets 64 KiB whose 8-byte slots hold, at
//! random, small integers, floats, or addresses into one 4 MiB region of
//! random bytes, so loads through addresses from constant banks land in
//! memory. Each program runs as one invocation and as one workgroup.
//!
//! ```text
//! spvidiff [--seeds N] [--steps N] [--size N] [--subgroup N] [--jobs N] FILE.spv|DIR...
//! ```

use std::collections::HashMap;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use relift_gpu_spvi::{Denorm, Env, Memory, Module, Run, group, reference, specialize};

const DATA: u64 = 0x0000_1000_0000_0000;
const DATA_BYTES: usize = 4 << 20;
/// Each buffer's bytes; buffers lie back to back.
const BUF_BYTES: usize = 0x4_0000;
const BUFS: u64 = 0x0000_2000_0000_0000;

/// Floats a translation must keep exact: -0, quiet and signaling NaNs
/// with payloads, the smallest denormals of each sign, infinity, one.
const SPECIAL: [u32; 8] = [
    0x8000_0000,
    0x7fc0_0000,
    0xffd5_a5a5,
    0x7f80_0001,
    0x0000_0001,
    0x8040_0000,
    0x7f80_0000,
    0x3f80_0000,
];

fn special(r: &mut Rng) -> u64 {
    u64::from(SPECIAL[(r.next() % 8) as usize]) | u64::from(SPECIAL[(r.next() % 8) as usize]) << 32
}

/// A small deterministic generator (SplitMix64).
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }
}

struct Inputs {
    env: Env,
    uniform: Memory,
    mem: Memory,
}

fn inputs(m: &Module, seed: u64) -> Inputs {
    let mut r = Rng(seed);
    let mut data = vec![0u8; DATA_BYTES];
    for c in data.chunks_mut(8) {
        c.copy_from_slice(&r.next().to_le_bytes());
    }
    let mut uniforms = HashMap::new();
    let mut arrays = HashMap::new();
    let mut uniform = Memory::default();
    let mut at = BUFS;
    let buf = |r: &mut Rng| {
        let mut b = vec![0u8; BUF_BYTES];
        for c in b.chunks_mut(8) {
            if r.next().is_multiple_of(5) {
                c.copy_from_slice(&special(r).to_le_bytes());
                continue;
            }
            let v = match r.next() % 4 {
                0 => DATA + (r.next() % (DATA_BYTES as u64 / 2)) / 16 * 16,
                1 => (r.next() % 64) | ((r.next() % 4) << 32),
                2 => {
                    u64::from(((r.next() % 2000) as f32 / 100.0 - 10.0).to_bits())
                        | (r.next() % 64) << 32
                }
                _ => r.next(),
            };
            c.copy_from_slice(&v.to_le_bytes());
        }
        b
    };
    let mut bindings = m.buffers();
    bindings.sort_unstable();
    bindings.dedup();
    for b in bindings {
        if m.is_array(b) {
            for k in 0..8 {
                arrays.insert((b.0, b.1, k), at);
                uniform.regions.push((at, buf(&mut r)));
                at += BUF_BYTES as u64;
            }
        } else {
            uniforms.insert(b, at);
            uniform.regions.push((at, buf(&mut r)));
            at += BUF_BYTES as u64;
        }
    }
    let denorm = [Denorm::Preserve, Denorm::FlushBefore, Denorm::FlushAfter][(seed % 3) as usize];
    Inputs {
        env: Env {
            denorm,
            canonical_nan: seed % 5 == 1,
            mul_zero_positive: seed % 7 == 2,
            uniforms,
            arrays,
            local_id: [(seed % 3) as u32, 0, 0],
            group_id: [(seed % 2) as u32, 0, 0],
            local_index: (seed % 3) as u32,
            subgroup_size: 32,
            fold_identity: seed.is_multiple_of(2),
        },
        uniform,
        mem: Memory {
            regions: vec![(DATA, data)],
        },
    }
}

type Outcome = Result<(Vec<(u64, Vec<u8>)>, String, Vec<u64>), String>;

fn caught<T>(f: impl FnOnce() -> T) -> Result<T, String> {
    catch_unwind(AssertUnwindSafe(f)).map_err(|p| {
        p.downcast_ref::<String>()
            .cloned()
            .or_else(|| p.downcast_ref::<&str>().map(|s| (*s).to_string()))
            .unwrap_or_else(|| "panic".into())
    })
}

/// One invocation, with the uniform blocks as plain memory it may also
/// write.
fn single(words: &[u32], inp: &Inputs, steps: u64, old: bool) -> Outcome {
    let m = Module::parse(words);
    let mut mem = inp.mem.clone();
    mem.regions.extend(inp.uniform.regions.iter().cloned());
    let ended = |r: Result<(), String>, regions: Vec<(u64, Vec<u8>)>, steps: u64| match r {
        Ok(()) => Ok((regions, String::new(), vec![steps])),
        Err(e) => Err(format!("{e} (memory {:016x})", hash(&regions))),
    };
    if old {
        let mut r = reference::Run::new(&m, &inp.env, mem);
        r.max_steps = steps;
        let res = caught(|| r.main());
        ended(res, r.mem.regions, r.steps)
    } else {
        let mut r = Run::new(&m, &inp.env, mem);
        r.max_steps = steps;
        let res = caught(|| r.main());
        ended(res, r.mem.regions, r.steps)
    }
}

/// FNV-1a of every region's base and bytes.
fn hash(regions: &[(u64, Vec<u8>)]) -> u64 {
    regions.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, (b, v)| {
        v.iter().fold(h ^ b, |h, &x| {
            (h ^ u64::from(x)).wrapping_mul(0x100_0000_01b3)
        })
    })
}

fn workgroup(
    words: &[u32],
    inp: &Inputs,
    steps: u64,
    size: u32,
    subgroup: u32,
    old: bool,
) -> Outcome {
    let mut words = words.to_vec();
    for (k, v) in [size, 1, 1].iter().enumerate() {
        specialize(&mut words, k as u32, *v);
    }
    let sz = [size, 1, 1];
    let mem = inp.mem.clone();
    let r = caught(|| {
        if old {
            reference::group::run_workgroup(
                &words,
                &inp.env,
                &inp.uniform,
                mem,
                sz,
                subgroup,
                steps,
            )
            .map(|d| (d.mem.regions, format!("{:?}", d.notes), d.steps))
            .map_err(|f| f.message)
        } else {
            group::run_workgroup(&words, &inp.env, &inp.uniform, mem, sz, subgroup, steps)
                .map(|d| (d.mem.regions, format!("{:?}", d.notes), d.steps))
                .map_err(|f| f.message)
        }
    });
    r.and_then(|x| x)
}

fn summary(o: &Outcome) -> String {
    match o {
        Ok((regions, notes, steps)) => {
            let h = hash(regions);
            format!(
                "ok mem {h:016x} steps {:?}..{:?} notes {notes}",
                steps.iter().min(),
                steps.iter().max()
            )
        }
        Err(e) => {
            let e: Vec<&str> = e
                .split(' ')
                .map(|w| if w.starts_with("0x") { "0x" } else { w })
                .collect();
            format!("failed: {}", e.join(" "))
        }
    }
}

fn files(args: &[String]) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for a in args {
        let p = Path::new(a);
        if p.is_dir() {
            let mut stack = vec![p.to_path_buf()];
            while let Some(d) = stack.pop() {
                let Ok(rd) = std::fs::read_dir(&d) else {
                    continue;
                };
                for e in rd.flatten() {
                    let q = e.path();
                    if q.is_dir() {
                        stack.push(q);
                    } else if q.extension().is_some_and(|x| x == "spv") {
                        out.push(q);
                    }
                }
            }
        } else {
            out.push(p.to_path_buf());
        }
    }
    out.sort();
    out
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (mut seeds, mut steps, mut size, mut subgroup, mut jobs) =
        (2u64, 200_000u64, 32u32, 32u32, 8usize);
    let mut paths = Vec::new();
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut num = || -> u64 {
            it.next()
                .unwrap_or_else(|| panic!("{a} wants a value"))
                .parse()
                .expect("a number")
        };
        match a.as_str() {
            "--seeds" => seeds = num(),
            "--steps" => steps = num(),
            "--size" => size = num() as u32,
            "--subgroup" => subgroup = num() as u32,
            "--jobs" => jobs = num() as usize,
            _ => paths.push(a.clone()),
        }
    }
    if std::env::var_os("SPVIDIFF_PANICS").is_none() {
        std::panic::set_hook(Box::new(|_| {}));
    }
    let files = files(&paths);
    let next = AtomicUsize::new(0);
    let counts = Mutex::new(HashMap::<String, u64>::new());
    let bump = |k: &str| {
        *counts
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .entry(k.into())
            .or_default() += 1
    };
    std::thread::scope(|sc| {
        for _ in 0..jobs.max(1) {
            sc.spawn(|| {
                loop {
                    let k = next.fetch_add(1, Ordering::Relaxed);
                    let Some(f) = files.get(k) else { break };
                    let Ok(bytes) = std::fs::read(f) else {
                        continue;
                    };
                    let words: Vec<u32> = bytes
                        .as_chunks::<4>()
                        .0
                        .iter()
                        .map(|c| u32::from_le_bytes(*c))
                        .collect();
                    let Ok(m) = caught(|| Module::parse(&words)) else {
                        bump("unparsed");
                        continue;
                    };
                    for seed in 0..seeds {
                        let name = f
                            .file_name()
                            .map(|n| n.as_encoded_bytes().to_vec())
                            .unwrap_or_default();
                        let h = name.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, &b| {
                            (h ^ u64::from(b)).wrapping_mul(0x100_0000_01b3)
                        });
                        let inp = inputs(&m, seed.wrapping_mul(0x1234_5678_9abc) ^ h);
                        let (n, o) = (
                            single(&words, &inp, steps, false),
                            single(&words, &inp, steps, true),
                        );
                        let (sn, so) = (summary(&n), summary(&o));
                        match &o {
                            Ok(_) => bump("single ok"),
                            Err(e) => bump(&format!(
                                "single failed: {}",
                                e.chars().take(48).collect::<String>()
                            )),
                        }
                        if sn != so {
                            bump("single DIFFER");
                            println!(
                                "DIFFER single {} seed {seed}\n  new {sn}\n  old {so}",
                                f.display()
                            );
                        }
                        let n = workgroup(&words, &inp, steps, size, subgroup, false);
                        let o = workgroup(&words, &inp, steps, size, subgroup, true);
                        let (sn, so) = (summary(&n), summary(&o));
                        match &o {
                            Ok(_) => bump("group ok"),
                            Err(e) => bump(&format!(
                                "group failed: {}",
                                e.chars().take(48).collect::<String>()
                            )),
                        }
                        if sn != so {
                            let o2 = summary(&workgroup(&words, &inp, steps, size, subgroup, true));
                            if o2 != so {
                                bump("group racy");
                                continue;
                            }
                            bump("group DIFFER");
                            println!(
                                "DIFFER group {} seed {seed}\n  new {sn}\n  old {so}",
                                f.display()
                            );
                        }
                    }
                    let done = k + 1;
                    if done.is_multiple_of(500) {
                        eprintln!("{done}/{}", files.len());
                    }
                }
            });
        }
    });
    let mut c: Vec<_> = counts
        .into_inner()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .into_iter()
        .collect();
    c.sort();
    println!("{} programs, {seeds} seeds", files.len());
    for (k, v) in c {
        println!("  {k}: {v}");
    }
}

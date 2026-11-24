//! Runs workgroups of one compute translation on the CPU and reports how
//! each ended: the steps of every invocation, the notes of
//! `relift_gpu_spvi::group` (barriers after a return, partial shuffles,
//! reads of inactive lanes) or the failure (a runaway loop and the block it
//! is in, memory outside every region).
//!
//! The release profile aborts on a panic, which loses the report: build
//! it with `cargo build -p relift-gpu-sm86 --example wgrun` (with
//! `CARGO_PROFILE_DEV_OPT_LEVEL=3` for speed).
//!
//! ```text
//! wgrun PROGRAM.spv [options]
//!   --size X,Y,Z          workgroup size (sets SpecIds 0, 1 and 2)
//!   --group X,Y,Z         workgroup id (repeatable; default 0,0,0)
//!   --subgroup N          invocations per subgroup (default 32)
//!   --spec ID=VALUE       a specialization constant (repeatable)
//!   --uniform S,B=FILE    a uniform block's bytes, from FILE (repeatable)
//!   --uniform S,B=zero:N  a uniform block of N zero bytes
//!   --region BASE=FILE    memory at BASE (hex) from FILE (repeatable)
//!   --region BASE=zero:N  N zero bytes at BASE
//!   --base OFF=START      a compute base: the storage buffer of the address
//!                         at c[0] byte OFF (hex) starts at START (hex), and
//!                         the base table says so (gpus/nv mem::slots)
//!   --steps N             the most instructions per invocation
//!   --dump BASE,N=FILE    write N bytes at BASE after the last workgroup
//! ```

use std::collections::HashMap;

use relift_gpu_nv::translate::{COMPUTE_BASE_TABLE_BINDING, DRIVER_CBUF_SET, compute_base_binding};
use relift_gpu_spvi::group::run_workgroup;
use relift_gpu_spvi::{Denorm, Env, Memory, specialize};

fn triple(s: &str) -> [u32; 3] {
    let v: Vec<u32> = s.split(',').map(|x| x.parse().expect("a number")).collect();
    [
        v[0],
        v.get(1).copied().unwrap_or(1),
        v.get(2).copied().unwrap_or(1),
    ]
}

fn hex(s: &str) -> u64 {
    u64::from_str_radix(s.trim_start_matches("0x"), 16).unwrap_or_else(|e| panic!("{s}: {e}"))
}

fn bytes(src: &str) -> Vec<u8> {
    match src.strip_prefix("zero:") {
        Some(n) => vec![0; n.parse().expect("a byte count")],
        None => std::fs::read(src).unwrap_or_else(|e| panic!("{src}: {e}")),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut words: Vec<u32> = std::fs::read(&args[0])
        .unwrap_or_else(|e| panic!("{}: {e}", args[0]))
        .chunks(4)
        .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect();
    let (mut size, mut groups, mut subgroup, mut steps) =
        ([1, 1, 1], Vec::new(), 32, 50_000_000u64);
    let mut mem = Memory::default();
    let mut uniform = Memory::default();
    let mut uniforms = HashMap::new();
    let mut dumps = Vec::new();
    let mut table = vec![0u8; 0x10000];
    let mut next_uniform = 0x7e00_0000_0000_0000u64;
    let mut it = args[1..].iter();
    while let Some(a) = it.next() {
        let v = it.next().unwrap_or_else(|| panic!("{a} wants a value"));
        match a.as_str() {
            "--size" => size = triple(v),
            "--group" => groups.push(triple(v)),
            "--subgroup" => subgroup = v.parse().expect("a number"),
            "--steps" => steps = v.parse().expect("a number"),
            "--spec" => {
                let (id, val) = v.split_once('=').expect("ID=VALUE");
                let ok = specialize(
                    &mut words,
                    id.parse().expect("an id"),
                    val.parse().expect("a value"),
                );
                assert!(ok, "no specialization constant {id}");
            }
            "--uniform" => {
                let (sb, src) = v.split_once('=').expect("S,B=SOURCE");
                let (s, b) = sb.split_once(',').expect("S,B");
                let mut b8 = bytes(src);
                b8.resize(b8.len().max(0x10000), 0);
                uniforms.insert(
                    (s.parse().expect("a set"), b.parse().expect("a binding")),
                    next_uniform,
                );
                uniform.regions.push((next_uniform, b8));
                next_uniform += 0x100_0000;
            }
            "--region" => {
                let (base, src) = v.split_once('=').expect("BASE=SOURCE");
                mem.regions.push((hex(base), bytes(src)));
            }
            "--base" => {
                let (off, start) = v.split_once('=').expect("OFF=START");
                let (off, start) = (hex(off) as u32, hex(start));
                uniforms.insert((DRIVER_CBUF_SET, compute_base_binding(off)), start);
                let w = 4 * (off / 8) as usize;
                table[w..w + 4].copy_from_slice(&(start as u32).to_le_bytes());
            }
            "--dump" => {
                let (span, out) = v.split_once('=').expect("BASE,N=FILE");
                let (base, n) = span.split_once(',').expect("BASE,N");
                dumps.push((
                    hex(base),
                    n.parse::<u64>().expect("a count"),
                    out.to_string(),
                ));
            }
            _ => panic!("unknown option {a}"),
        }
    }
    uniforms.insert((DRIVER_CBUF_SET, COMPUTE_BASE_TABLE_BINDING), next_uniform);
    uniform.regions.push((next_uniform, table));
    for (k, v) in size.iter().enumerate() {
        specialize(&mut words, k as u32, *v);
    }
    if groups.is_empty() {
        groups.push([0, 0, 0]);
    }
    let mut env = Env {
        denorm: Denorm::Preserve,
        canonical_nan: false,
        mul_zero_positive: false,
        uniforms,
        arrays: HashMap::new(),
        local_id: [0; 3],
        group_id: [0; 3],
        local_index: 0,
        subgroup_size: subgroup,
        fold_identity: false,
    };
    for g in groups {
        env.group_id = g;
        let t = std::time::Instant::now();
        match run_workgroup(&words, &env, &uniform, mem, size, subgroup, steps) {
            Ok(done) => {
                let (lo, hi) = (done.steps.iter().min(), done.steps.iter().max());
                println!(
                    "group {g:?}: done in {:.1} s, steps {lo:?}..{hi:?}",
                    t.elapsed().as_secs_f64()
                );
                for (n, c) in &done.notes {
                    println!("  note {n:?} x{c}");
                }
                mem = done.mem;
            }
            Err(f) => {
                println!(
                    "group {g:?}: invocation {} failed: {}",
                    f.invocation, f.message
                );
                let mut at: HashMap<(u32, u32), Vec<usize>> = HashMap::new();
                for (i, w) in f.at.iter().enumerate() {
                    at.entry(*w).or_default().push(i);
                }
                for ((func, block), who) in at {
                    println!(
                        "  function %{func} block %{block}: invocations {}..{} ({}), steps {}",
                        who[0],
                        who[who.len() - 1],
                        who.len(),
                        f.steps[who[0]]
                    );
                }
                std::process::exit(1);
            }
        }
    }
    for (base, n, out) in dumps {
        let mut b = Vec::with_capacity(n as usize);
        for k in 0..n {
            b.push(mem.read(base + k, 1) as u8);
        }
        relift_cache::write_if_changed(std::path::Path::new(&out), &b)
            .unwrap_or_else(|e| panic!("{out}: {e}"));
    }
}

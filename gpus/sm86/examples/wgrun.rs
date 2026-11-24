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
//!   --grid X,Y,Z          every workgroup id below X,Y,Z
//!   --reference 1         run on the tree-walking reference interpreter
//!                         (relift_gpu_spvi::reference), one group at a time
//!   --threads N           run the workgroups as one dispatch on N threads
//!                         (they then share memory as on a device)
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
//!   --image S,B,E=FMT,W,H,D,LAYERS,LEVELS,FILE
//!                         an image at set S binding B element E: VkFormat
//!                         FMT, its texels from FILE (each level in turn,
//!                         in a level each layer or depth slice), D the
//!                         depth of a 3D image, else 1 (repeatable); images
//!                         from one FILE share their texels, as views of one
//!                         image at several bindings do
//!   --image-dump S,B,E=FILE
//!                         write that image's texels after the last
//!                         workgroup
//!   --buffer S,B=SOURCE   a storage buffer at set S binding B in the
//!                         memory all invocations share (repeatable)
//!   --buffer-dump S,B=FILE
//!                         write that buffer after the last workgroup
//!   --log-stores FILE     write one line per store outside the workgroup
//!                         memory: address, bytes, invocation, workgroup,
//!                         the SPIR-V function and block it was in
//! ```

use std::collections::HashMap;

use relift_gpu_nv::translate::{COMPUTE_BASE_TABLE_BINDING, DRIVER_CBUF_SET, compute_base_binding};
use relift_gpu_spvi::group::{Dispatch, run_dispatch, run_workgroup};
use relift_gpu_spvi::{Denorm, Env, Image, Memory, specialize};

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

/// A reference note as the compiled interpreter's.
fn note(n: relift_gpu_spvi::reference::group::Note) -> relift_gpu_spvi::group::Note {
    use relift_gpu_spvi::group::Note as N;
    use relift_gpu_spvi::reference::group::Note as R;
    match n {
        R::BarrierAfterReturn { block, returned } => N::BarrierAfterReturn { block, returned },
        R::BarrierBlocks { blocks } => N::BarrierBlocks { blocks },
        R::PartialShuffle { block, active } => N::PartialShuffle { block, active },
        R::InactiveLane { block } => N::InactiveLane { block },
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
    let mut threads = None;
    let mut reference = false;
    let mut mem = Memory::default();
    let mut uniform = Memory::default();
    let mut uniforms = HashMap::new();
    let mut dumps = Vec::new();
    let mut table = vec![0u8; 0x10000];
    let mut next_uniform = 0x7e00_0000_0000_0000u64;
    let mut images = HashMap::new();
    let mut image_dumps = Vec::new();
    let mut image_files: HashMap<String, u64> = HashMap::new();
    let mut next_image = 0x7d00_0000_0000_0000u64;
    let mut store_log: Option<String> = None;
    let mut log_lines = String::new();
    let mut buffers: HashMap<(u32, u32), (u64, u64)> = HashMap::new();
    let mut buffer_dumps: Vec<((u32, u32), String)> = Vec::new();
    let mut it = args[1..].iter();
    while let Some(a) = it.next() {
        let v = it.next().unwrap_or_else(|| panic!("{a} wants a value"));
        match a.as_str() {
            "--size" => size = triple(v),
            "--log-stores" => store_log = Some(v.clone()),
            "--image" => {
                let (key, rest) = v.split_once('=').expect("S,B,E=...");
                let k: Vec<u64> = key
                    .split(',')
                    .map(|x| x.parse().expect("a number"))
                    .collect();
                let f: Vec<&str> = rest.splitn(7, ',').collect();
                assert_eq!(f.len(), 7, "FMT,W,H,D,LAYERS,LEVELS,FILE");
                let n = |i: usize| f[i].parse::<u32>().expect("a number");
                let shared = image_files.get(f[6]).copied();
                let addr = shared.unwrap_or(next_image);
                let img = Image {
                    addr,
                    width: n(1),
                    height: n(2),
                    depth: n(3),
                    layers: n(4),
                    levels: n(5),
                    format: n(0),
                };
                assert!(
                    Image::texel_bytes(img.format).is_some(),
                    "format {}",
                    img.format
                );
                if shared.is_none() {
                    let texels = bytes(f[6]);
                    image_files.insert(f[6].to_string(), addr);
                    next_image += (texels.len() as u64 + 0xffff) & !0xffff;
                    mem.regions.push((addr, texels));
                }
                images.insert((k[0] as u32, k[1] as u32, k[2]), img);
            }
            "--image-dump" => {
                let (key, out) = v.split_once('=').expect("S,B,E=FILE");
                let k: Vec<u64> = key
                    .split(',')
                    .map(|x| x.parse().expect("a number"))
                    .collect();
                image_dumps.push(((k[0] as u32, k[1] as u32, k[2]), out.to_string()));
            }
            "--buffer" => {
                let (sb, src) = v.split_once('=').expect("S,B=SOURCE");
                let (s, b) = sb.split_once(',').expect("S,B");
                let key = (s.parse().expect("a set"), b.parse().expect("a binding"));
                let b8 = bytes(src);
                uniforms.insert(key, next_image);
                buffers.insert(key, (next_image, b8.len() as u64));
                next_image += (b8.len() as u64 + 0xffff) & !0xffff;
                mem.regions.push((buffers[&key].0, b8));
            }
            "--buffer-dump" => {
                let (sb, out) = v.split_once('=').expect("S,B=FILE");
                let (s, b) = sb.split_once(',').expect("S,B");
                let key: (u32, u32) = (s.parse().expect("a set"), b.parse().expect("a binding"));
                buffer_dumps.push((key, out.to_string()));
            }
            "--group" => groups.push(triple(v)),
            "--grid" => {
                let g = triple(v);
                for z in 0..g[2] {
                    for y in 0..g[1] {
                        for x in 0..g[0] {
                            groups.push([x, y, z]);
                        }
                    }
                }
            }
            "--reference" => reference = v == "1",
            "--threads" => threads = Some(v.parse::<usize>().expect("a number")),
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
        images: images.clone(),
        log_stores: store_log.is_some(),
    };
    if let Some(threads) = threads {
        let d = Dispatch {
            size,
            groups,
            subgroup,
            max_steps: steps,
            threads,
        };
        let t = std::time::Instant::now();
        match run_dispatch(&words, &env, &uniform, mem, &d) {
            Ok(done) => {
                let all = done.groups.iter().flat_map(|g| g.steps.iter());
                let total: u64 = all.clone().sum();
                let secs = t.elapsed().as_secs_f64();
                println!(
                    "{} groups on {threads} threads: done in {secs:.2} s, steps {:?}..{:?}, {:.0}M steps/s",
                    done.groups.len(),
                    all.clone().min(),
                    all.max(),
                    total as f64 / secs / 1e6
                );
                for g in &done.groups {
                    for (n, c) in &g.notes {
                        println!("  group {:?} note {n:?} x{c}", g.group);
                    }
                }
                mem = done.mem;
            }
            Err((k, f)) => {
                println!(
                    "group {:?}: invocation {} failed: {}",
                    d.groups[k], f.invocation, f.message
                );
                std::process::exit(1);
            }
        }
        groups = Vec::new();
    }
    for g in groups {
        env.group_id = g;
        let t = std::time::Instant::now();
        let r = if reference {
            relift_gpu_spvi::reference::group::run_workgroup(
                &words, &env, &uniform, mem, size, subgroup, steps,
            )
            .map(|d| relift_gpu_spvi::group::Done {
                mem: d.mem,
                notes: d.notes.into_iter().map(|(n, c)| (note(n), c)).collect(),
                steps: d.steps,
                stores: Vec::new(),
            })
            .map_err(|f| relift_gpu_spvi::group::Failure {
                invocation: f.invocation,
                message: f.message,
                at: f.at,
                steps: f.steps,
            })
        } else {
            run_workgroup(&words, &env, &uniform, mem, size, subgroup, steps)
        };
        match r {
            Ok(done) => {
                let (lo, hi) = (done.steps.iter().min(), done.steps.iter().max());
                println!(
                    "group {g:?}: done in {:.1} s, steps {lo:?}..{hi:?}",
                    t.elapsed().as_secs_f64()
                );
                for (n, c) in &done.notes {
                    println!("  note {n:?} x{c}");
                }
                for st in &done.stores {
                    log_lines.push_str(&format!(
                        "{:x} {} {} {},{},{} %{} %{}\n",
                        st.addr, st.bytes, st.invocation, g[0], g[1], g[2], st.func, st.block
                    ));
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
    if let Some(p) = &store_log {
        relift_cache::write_if_changed(std::path::Path::new(p), log_lines.as_bytes())
            .unwrap_or_else(|e| panic!("{p}: {e}"));
    }
    for (key, out) in image_dumps {
        let img = &images[&key];
        let (_, b) = mem
            .regions
            .iter()
            .find(|(b, _)| *b == img.addr)
            .expect("image region");
        relift_cache::write_if_changed(std::path::Path::new(&out), b)
            .unwrap_or_else(|e| panic!("{out}: {e}"));
    }
    for (key, out) in buffer_dumps {
        let (base, n) = buffers[&key];
        dumps.push((base, n, out));
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

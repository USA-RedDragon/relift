//! What the Z80 end-to-end tests share: a static discovery of the blocks a
//! program can reach from some roots, and the build of the emitted C with
//! the runtime's core and its Z80 layer into a program.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::path::{Path, PathBuf};

use relift_cpu_z80::{ENTRY, Options, lift_bytes};
use relift_emit::{Function, Insn, Output};
use relift_ir::{Block, Flow, Next};

/// A function found by [`discover`].
pub struct Found {
    pub entry: u64,
    pub blocks: BTreeMap<u64, Block>,
}

/// What [`discover`] found.
pub struct Discovered {
    pub functions: Vec<Found>,
    /// Call targets that fall inside a function of another entry.
    pub shared_entries: Vec<u64>,
    /// Instructions that were not lifted: address and message.
    pub failed: Vec<(u64, String)>,
}

fn bytes_at(mem: &[u8], pc: u64) -> [u8; 4] {
    let a = (pc & 0xFFFF) as usize;
    let mut w = [0u8; 4];
    for (k, b) in w.iter_mut().enumerate() {
        *b = mem[(a + k) & 0xFFFF];
    }
    w
}

struct Sets(Vec<usize>);

impl Sets {
    fn find(&mut self, x: usize) -> usize {
        let mut r = x;
        while self.0[r] != r {
            r = self.0[r];
        }
        let mut y = x;
        while self.0[y] != r {
            let n = self.0[y];
            self.0[y] = r;
            y = n;
        }
        r
    }

    fn join(&mut self, a: usize, b: usize) {
        let (a, b) = (self.find(a), self.find(b));
        self.0[a.max(b)] = a.min(b);
    }
}

/// Lifts every block reachable from `roots` in `mem` by recursive descent:
/// both ways of a conditional, the vector of an interrupt entry (a function
/// entry), the taken half of a conditional call or
/// return, the instruction after a call. A call target is a function entry;
/// blocks joined by jumps and fall-through share a function. Addresses for
/// which `external` holds are not lifted (the program supplies them).
pub fn discover(
    mem: &[u8],
    roots: &[u64],
    external: &dyn Fn(u64) -> bool,
    opts: &Options,
) -> Discovered {
    let mut blocks: BTreeMap<u64, Block> = BTreeMap::new();
    let mut failed = Vec::new();
    let mut jumps: Vec<(u64, u64)> = Vec::new();
    let mut entries: BTreeSet<u64> = roots.iter().copied().collect();
    let mut work: Vec<u64> = roots.to_vec();
    while let Some(key) = work.pop() {
        if blocks.contains_key(&key) || external(key) {
            continue;
        }
        let bytes = if key & !0xFFFF == ENTRY {
            [0u8; 4]
        } else {
            bytes_at(mem, key)
        };
        let l = match lift_bytes(&bytes, key, opts) {
            Ok(l) => l,
            Err(e) => {
                failed.push((key, e.to_string()));
                continue;
            }
        };
        let entry_block = key & !0xFFFF == ENTRY;
        let targets: Vec<u64> = match l.block.exit.next {
            Next::Const(n) => vec![n],
            Next::Select {
                taken, not_taken, ..
            } => vec![taken, not_taken],
            Next::Dynamic(_) => Vec::new(),
        };
        match l.block.exit.flow {
            Flow::Jump | Flow::Trap(_) if entry_block => {
                for t in targets {
                    if !external(t) {
                        entries.insert(t);
                    }
                    work.push(t);
                }
            }
            Flow::Jump | Flow::Trap(_) => {
                for t in targets {
                    if !external(t) {
                        jumps.push((key, t));
                    }
                    work.push(t);
                }
            }
            Flow::Call { ret } => {
                for t in targets {
                    if !external(t) {
                        entries.insert(t);
                    }
                    work.push(t);
                }
                if !external(ret) {
                    jumps.push((key, ret));
                }
                work.push(ret);
            }
            Flow::Return => {}
        }
        blocks.insert(key, l.block);
    }
    let keys: Vec<u64> = blocks.keys().copied().collect();
    let index: HashMap<u64, usize> = keys.iter().enumerate().map(|(i, k)| (*k, i)).collect();
    let mut sets = Sets((0..keys.len()).collect());
    for (a, b) in &jumps {
        if let (Some(i), Some(j)) = (index.get(a), index.get(b)) {
            sets.join(*i, *j);
        }
    }
    let mut groups: BTreeMap<usize, Vec<u64>> = BTreeMap::new();
    for (i, k) in keys.iter().enumerate() {
        groups.entry(sets.find(i)).or_default().push(*k);
    }
    let mut functions = Vec::new();
    let mut shared_entries = Vec::new();
    for (_, members) in groups {
        let roots_here: Vec<u64> = members
            .iter()
            .copied()
            .filter(|k| entries.contains(k))
            .collect();
        let Some(&entry) = roots_here.first() else {
            continue;
        };
        shared_entries.extend(roots_here.iter().skip(1));
        functions.push(Found {
            entry,
            blocks: members.iter().map(|k| (*k, blocks[k].clone())).collect(),
        });
    }
    functions.sort_by_key(|f| f.entry);
    Discovered {
        functions,
        shared_entries,
        failed,
    }
}

/// The emitter's view of `found`.
pub fn functions(found: &[Found]) -> Vec<Function<'_>> {
    found
        .iter()
        .map(|f| {
            let last = f
                .blocks
                .keys()
                .filter(|k| **k & !0xFFFF == 0)
                .max()
                .copied()
                .unwrap_or(f.entry);
            Function {
                entry: f.entry,
                name: format!("F_{:x}", f.entry),
                guest_name: None,
                span: last.saturating_sub(f.entry & 0xFFFF) + 4,
                insts: f
                    .blocks
                    .iter()
                    .map(|(k, b)| (*k, Insn::Lifted { block: b, raw: 0 }))
                    .collect(),
                tables: BTreeMap::new(),
                pads: Default::default(),
            }
        })
        .collect()
}

/// The C compiler of the test: clang, else `zig cc`.
fn compiler() -> Option<(PathBuf, Vec<String>)> {
    if let Some(c) = relift_build::find_tool("clang") {
        return Some((c, Vec::new()));
    }
    relift_build::find_tool("zig").map(|z| (z, vec!["cc".to_string()]))
}

fn run_cc(cc: &(PathBuf, Vec<String>), args: &[String]) {
    let out = relift_proc::command(&cc.0)
        .args(&cc.1)
        .args(args)
        .output()
        .expect("the C compiler runs");
    assert!(
        out.status.success(),
        "{} {}:\n{}",
        cc.0.display(),
        args.join(" "),
        String::from_utf8_lossy(&out.stderr)
    );
}

/// A scratch directory of the test, empty.
pub fn scratch(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(format!("{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    dir
}

/// Writes the emitted files and `machine` (the C of `machine.c`) into `dir`,
/// compiles them with the runtime's core and Z80 layer at optimization
/// `opt` and links `dir/prog`. `None` when no C compiler is on `PATH`.
pub fn build(dir: &Path, out: &Output, machine: &str, opt: &str) -> Option<PathBuf> {
    let Some(cc) = compiler() else {
        eprintln!("skipped: no clang or zig on PATH");
        return None;
    };
    let rt = Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime");
    let mut sources = Vec::new();
    for file in &out.files {
        let p = dir.join(&file.name);
        relift_cache::write_if_changed(&p, file.text.as_bytes()).expect("writes");
        if file.name.ends_with(".c") {
            sources.push(p);
        }
    }
    let m = dir.join("machine.c");
    relift_cache::write_if_changed(&m, machine.as_bytes()).expect("writes");
    sources.push(m);
    for entry in std::fs::read_dir(rt.join("core")).expect("lists the core") {
        let p = entry.expect("entry").path();
        if matches!(p.extension().and_then(|e| e.to_str()), Some("c" | "S")) {
            sources.push(p);
        }
    }
    sources.push(rt.join("cpus/z80/z80.c"));
    let mut flags: Vec<String> = vec![
        "-std=gnu11".into(),
        opt.into(),
        "-w".into(),
        "-D_GNU_SOURCE".into(),
        "-DRL_BUILDING_RUNTIME".into(),
        format!("-I{}", rt.join("core/include").display()),
        format!("-I{}", rt.join("core").display()),
        format!("-I{}", rt.join("cpus/z80/include").display()),
        format!("-I{}", rt.join("cpus/z80").display()),
        format!("-I{}", dir.display()),
    ];
    if cfg!(target_arch = "x86_64") {
        flags.push("-mcx16".into());
    }
    if cc.0.ends_with("zig") {
        flags.push("-fno-sanitize=undefined".into());
    }
    let objs: Vec<PathBuf> = (0..sources.len())
        .map(|i| dir.join(format!("o{i}.o")))
        .collect();
    let next = std::sync::atomic::AtomicUsize::new(0);
    let threads = std::thread::available_parallelism().map_or(4, |n| n.get().min(16));
    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    let Some(src) = sources.get(i) else { break };
                    let mut a = flags.clone();
                    a.extend([
                        "-c".into(),
                        src.display().to_string(),
                        "-o".into(),
                        objs[i].display().to_string(),
                    ]);
                    run_cc(&cc, &a);
                }
            });
        }
    });
    let exe = dir.join("prog");
    let mut a: Vec<String> = objs.iter().map(|o| o.display().to_string()).collect();
    a.extend([
        "-lm".into(),
        "-lpthread".into(),
        "-o".into(),
        exe.display().to_string(),
    ]);
    run_cc(&cc, &a);
    Some(exe)
}

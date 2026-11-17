//! Translates every program of a `relift-shaders scan` directory and counts
//! the reasons the rest fail. Task and mesh programs that `meshes.json`
//! pairs translate with their pipeline.
//!
//! `coverage <scan dir> [<spv out dir> | -] [threads] [failures list]`

mod meshes;

use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::translate_with;
use std::collections::BTreeMap;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let dir = args.get(1).cloned().unwrap_or_default();
    let out = match args.get(2).map(String::as_str) {
        None | Some("" | "-") => None,
        Some(o) => {
            let p = std::path::PathBuf::from(o);
            if !p.is_absolute() || !p.is_dir() {
                panic!(
                    "{o}: the output directory must be an existing absolute path, or - for none"
                );
            }
            Some(o.to_string())
        }
    };
    let threads: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(4);
    let mut entries: Vec<_> = std::fs::read_dir(&dir)
        .unwrap_or_else(|e| panic!("{dir}: {e}"))
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "bin"))
        .collect();
    entries.sort();
    let pipelines = meshes::read(std::path::Path::new(&dir));
    let next = AtomicUsize::new(0);
    let ok = AtomicUsize::new(0);
    let list: Mutex<String> = Mutex::new(String::new());
    let why: Mutex<BTreeMap<String, u64>> = Mutex::new(BTreeMap::new());
    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| {
                loop {
                    let k = next.fetch_add(1, Ordering::Relaxed);
                    let Some(p) = entries.get(k) else { break };
                    let Ok(b) = std::fs::read(p) else { continue };
                    let sha = p
                        .file_stem()
                        .map(|s| s.to_string_lossy())
                        .unwrap_or_default();
                    let o = meshes::options(&pipelines, &sha);
                    match translate_with(&Program::parse(&b), &o) {
                        Ok(sh) => {
                            ok.fetch_add(1, Ordering::Relaxed);
                            if let (Some(o), Some(stem)) = (&out, p.file_stem()) {
                                let bytes: Vec<u8> =
                                    sh.spirv.iter().flat_map(|w| w.to_le_bytes()).collect();
                                let path = std::path::Path::new(o)
                                    .join(format!("{}.spv", stem.to_string_lossy()));
                                relift_cache::write_if_changed(&path, &bytes)
                                    .unwrap_or_else(|e| panic!("{}: {e}", path.display()));
                            }
                        }
                        Err(e) => {
                            if let Ok(mut l) = list.lock() {
                                l.push_str(&format!("{}\t{}\n", p.display(), e.0));
                            }
                            let k = e.0.split(' ').take(2).collect::<Vec<_>>().join(" ");
                            if let Ok(mut w) = why.lock() {
                                *w.entry(k).or_default() += 1;
                            }
                        }
                    }
                }
            });
        }
    });
    if let (Some(f), Ok(l)) = (args.get(4), list.into_inner()) {
        relift_cache::write_if_changed(std::path::Path::new(f), l.as_bytes())
            .unwrap_or_else(|e| panic!("{f}: {e}"));
    }
    let why = why.into_inner().unwrap_or_default();
    let mut v: Vec<_> = why.into_iter().collect();
    v.sort_by_key(|x| std::cmp::Reverse(x.1));
    for (k, n) in v.iter().take(40) {
        println!("{n:8} {k}");
    }
    println!(
        "translated {} of {} ({} task and mesh programs with their pipeline)",
        ok.load(Ordering::Relaxed),
        entries.len(),
        pipelines.len()
    );
}

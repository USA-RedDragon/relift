//! Shared fixtures and a runner for the tool CLI tests: the synthetic game
//! and its romfs image are written once per test under the target's temp
//! dir, and each tool runs as a real subprocess.

#![allow(dead_code)]

use std::ffi::OsStr;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Output, Stdio};

use relift_switch_formats::synth::{self, Game};

/// A written fixture set.
pub struct Fixture {
    pub dir: PathBuf,
    pub game: Game,
    /// The base romfs as a romfs image.
    pub romfs: PathBuf,
}

/// Write the fixtures of test `name` into a fresh directory.
pub fn fixture(name: &str) -> Fixture {
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    let game = synth::game();
    let put = |file: &str, bytes: &[u8]| {
        let p = dir.join(file);
        relift_cache::write_if_changed(&p, bytes).expect("the fixture is writable");
        p
    };
    let files: Vec<(&str, &[u8])> = game
        .base_spec
        .romfs
        .iter()
        .map(|(p, b)| (p.as_str(), b.as_slice()))
        .collect();
    let romfs = put("romfs.img", &relift_switch_formats::romfs::build(&files));
    Fixture { dir, game, romfs }
}

/// Run a tool binary (`relift-romfs`, ...) with `args`, optional stdin, in
/// the fixture directory, with no inherited settings.
pub fn tool(bin: &str, dir: &Path, args: &[&OsStr], stdin: Option<&[u8]>) -> Output {
    let exe = exe(bin);
    let mut child = relift_proc::command(exe)
        .args(args)
        .current_dir(dir)
        .stdin(if stdin.is_some() {
            Stdio::piped()
        } else {
            Stdio::null()
        })
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("the tool starts");
    if let Some(bytes) = stdin {
        let mut pipe = child.stdin.take().expect("stdin is piped");
        let bytes = bytes.to_vec();
        let writer = std::thread::spawn(move || {
            let _ = pipe.write_all(&bytes);
        });
        let out = child.wait_with_output().expect("the tool finishes");
        writer.join().expect("the stdin writer finishes");
        out
    } else {
        child.wait_with_output().expect("the tool finishes")
    }
}

fn exe(bin: &str) -> PathBuf {
    match bin {
        "relift-romfs" => env!("CARGO_BIN_EXE_relift-romfs").into(),
        "relift-nso" => env!("CARGO_BIN_EXE_relift-nso").into(),
        other => panic!("unknown tool {other}"),
    }
}

/// Shorthand: run with string-like args and no stdin.
pub fn run(bin: &str, dir: &Path, args: &[&str]) -> Output {
    let os: Vec<&OsStr> = args.iter().map(OsStr::new).collect();
    tool(bin, dir, &os, None)
}

/// Shorthand: run with string-like args and stdin bytes.
pub fn run_stdin(bin: &str, dir: &Path, args: &[&str], stdin: &[u8]) -> Output {
    let os: Vec<&OsStr> = args.iter().map(OsStr::new).collect();
    tool(bin, dir, &os, Some(stdin))
}

pub fn ok(out: &Output) -> &Output {
    assert!(
        out.status.success(),
        "tool failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    out
}

/// Assert exit code 1 with exactly one stderr line containing `needle`.
pub fn fails_with(out: &Output, needle: &str) {
    let err = String::from_utf8_lossy(&out.stderr).into_owned();
    assert_eq!(out.status.code(), Some(1), "stderr: {err}");
    assert_eq!(err.trim_end().lines().count(), 1, "stderr: {err}");
    assert!(err.contains(needle), "want {needle:?} in {err:?}");
}

pub fn path(p: &Path) -> &str {
    p.to_str().expect("fixture paths are UTF-8")
}

pub fn json(out: &Output) -> serde_json::Value {
    serde_json::from_slice(&ok(out).stdout).expect("the tool printed JSON")
}

/// Sorted (path, bytes) of every file under `dir`, `/`-separated.
pub fn tree(dir: &Path) -> Vec<(String, Vec<u8>)> {
    fn walk(root: &Path, at: &Path, out: &mut Vec<(String, Vec<u8>)>) {
        for e in std::fs::read_dir(at).expect("dir reads") {
            let p = e.expect("entry").path();
            if p.is_dir() {
                walk(root, &p, out);
            } else {
                let rel = p.strip_prefix(root).expect("under root");
                out.push((
                    rel.to_str().expect("UTF-8").replace('\\', "/"),
                    std::fs::read(&p).expect("file reads"),
                ));
            }
        }
    }
    let mut out = Vec::new();
    walk(dir, dir, &mut out);
    out.sort();
    out
}

/// Write `bytes` as `dir/name` and return the path.
pub fn put(dir: &Path, name: &str, bytes: &[u8]) -> PathBuf {
    let p = dir.join(name);
    relift_cache::write_if_changed(&p, bytes).expect("the fixture is writable");
    p
}

/// Sorted (path, bytes) of a romfs spec.
pub fn sorted(files: &[(String, Vec<u8>)]) -> Vec<(String, Vec<u8>)> {
    let mut v = files.to_vec();
    v.sort();
    v
}

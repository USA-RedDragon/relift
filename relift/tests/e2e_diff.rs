//! End to end: `relift diff` runs the tiny program under gdb, stops at the
//! lifted function, runs the same function in the interpreter from the live
//! registers and memory, and compares. A build with the emitter's test fault
//! (the first add emitted as a subtract) must be reported with the first
//! differing register and page. Skipped with a note when clang or gdb is not
//! on `PATH`.

mod tiny;

use std::path::Path;

struct Run {
    code: Option<i32>,
    stdout: String,
    stderr: String,
}

fn tools() -> bool {
    for t in ["clang", "gdb"] {
        if relift_build::find_tool(t).is_none() {
            eprintln!("skipped: no {t} on PATH");
            return false;
        }
    }
    true
}

fn import(root: &Path, project: &Path) -> String {
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    cache
}

fn diff(project: &Path, cache: &str, args: &[&str], fault: Option<u64>) -> Run {
    let bin = std::path::PathBuf::from(env!("CARGO_BIN_EXE_relift"));
    let all: Vec<&str> = ["--cache-dir", cache, "-j", "4", "diff"]
        .into_iter()
        .chain(args.iter().copied())
        .collect();
    let out = tiny::seed::seeded(project, &all, &bin, || {
        let mut cmd = relift_proc::command(&bin);
        cmd.args(&all)
            .env_remove("RELIFT_JOBS")
            .env_remove("RELIFT_EMIT_TEST_FAULT")
            .current_dir(project);
        if let Some(f) = fault {
            cmd.env("RELIFT_EMIT_TEST_FAULT", format!("{f:x}"));
        }
        cmd.output().expect("the relift binary runs")
    });
    Run {
        code: out.status.code(),
        stdout: String::from_utf8_lossy(&out.stdout).into_owned(),
        stderr: String::from_utf8_lossy(&out.stderr).into_owned(),
    }
}

fn function_address(root: &Path) -> u64 {
    let entries = std::fs::read_dir(root.join("cache/projects"))
        .expect("the project cache exists")
        .map(|e| e.expect("entry").path().join("build/host/entries.txt"))
        .find(|p| p.is_file())
        .expect("the entry list");
    std::fs::read_to_string(entries)
        .expect("readable")
        .lines()
        .filter_map(|l| u64::from_str_radix(l.split(' ').nth(1)?.trim_start_matches("0x"), 16).ok())
        .find(|a| a & 0xfff == tiny::DIFF_FN)
        .expect("an entry at the function's offset")
}

const FN: &str = "main+0x30";

#[test]
fn a_correct_build_matches_every_call() {
    if !tools() {
        return;
    }
    let root = tiny::scratch("diff-match");
    let project = tiny::dump_diff(&root);
    let cache = import(&root, &project);
    let r = diff(&project, &cache, &["--count", "3", FN], None);
    assert_eq!(r.code, Some(0), "{}\n{}", r.stdout, r.stderr);
    assert!(
        r.stdout.contains("call 3: MATCH"),
        "{}\n{}",
        r.stdout,
        r.stderr
    );
    assert!(r.stdout.contains("SUMMARY"), "{}", r.stdout);
    assert!(r.stdout.contains("match=3"), "{}", r.stdout);
    assert!(!r.stdout.contains("MISMATCH"), "{}", r.stdout);
}

#[test]
fn a_miscompiled_add_is_reported_with_its_register_and_page() {
    if !tools() {
        return;
    }
    let root = tiny::scratch("diff-fault");
    let project = tiny::dump_diff(&root);
    let cache = import(&root, &project);
    let first = diff(&project, &cache, &[FN], None);
    assert_eq!(first.code, Some(0), "{}\n{}", first.stdout, first.stderr);
    let at = function_address(&root);
    let r = diff(&project, &cache, &[FN], Some(at));
    assert_eq!(r.code, Some(1), "{}\n{}", r.stdout, r.stderr);
    assert!(
        r.stdout.contains("call 1: MISMATCH"),
        "{}\n{}",
        r.stdout,
        r.stderr
    );
    assert!(
        r.stderr.contains("register x1: lifted 0x1, engine 0x11"),
        "{}",
        r.stderr
    );
    let data = at - tiny::DIFF_FN + 0x2000;
    assert!(
        r.stderr.contains(&format!(
            "page {data:#x}: 1 bytes differ, first at {data:#x}: lifted 01"
        )),
        "{}",
        r.stderr
    );
}

#[test]
fn a_function_that_reaches_a_system_call_is_stopped_and_named() {
    if !tools() {
        return;
    }
    let root = tiny::scratch("diff-svc");
    let project = tiny::dump_diff(&root);
    let cache = import(&root, &project);
    let r = diff(&project, &cache, &["main+0x0"], None);
    assert_eq!(r.code, Some(2), "{}\n{}", r.stdout, r.stderr);
    assert!(r.stdout.contains("STOPPED"), "{}\n{}", r.stdout, r.stderr);
    assert!(
        r.stderr.contains("system call svc #0x27"),
        "{}\n{}",
        r.stdout,
        r.stderr
    );
}

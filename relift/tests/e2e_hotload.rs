//! End to end: missed code. The tiny program calls a function through a
//! computed pointer that discovery cannot see. Without the cold copy
//! (`emit.cold` off), a shipped build (`relift build`) stops at the miss
//! with exit status 70; `relift run` hot-loads the function, runs to the
//! right output and records the address in `game.toml`; the next `relift
//! build` lifts it and runs with no miss. With the cold copy the shipped
//! build runs the function from it, and `relift run` logs the first entry.
//! Skipped with a note when neither clang nor zig is on `PATH`.

mod tiny;

use std::path::Path;

fn game(root: &Path, dev: bool) -> std::process::Output {
    let exe = std::fs::read_dir(root.join("cache").join("projects"))
        .unwrap()
        .map(|e| {
            let host = e.unwrap().path().join("build").join("host");
            if dev { host.join("dev") } else { host }
                .join(format!("game{}", std::env::consts::EXE_SUFFIX))
        })
        .find(|p| p.is_file())
        .expect("a built game");
    relift_proc::command(&exe)
        .env_remove("RELIFT_DEV_SOCKET")
        .current_dir(root.join("tiny"))
        .output()
        .expect("the game runs")
}

#[test]
fn a_missed_call_is_hot_loaded_and_lifted_by_the_next_build() {
    if relift_build::find_tool("clang").is_none() && relift_build::find_tool("zig").is_none() {
        eprintln!("skipped: no clang or zig on PATH");
        return;
    }
    let root = tiny::scratch("e2e-hotload");
    let project = tiny::dump_hidden(&root);
    let cache = root.join("cache").display().to_string();
    let r = |args: &[&str]| {
        let mut a = vec!["--cache-dir", &cache, "-j", "4", "--emit.cold=false"];
        a.extend_from_slice(args);
        tiny::relift(&project, &a)
    };
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let want = "hello from relift\n".repeat(3);

    let out = r(&["build"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let shipped = game(&root, false);
    let err = String::from_utf8_lossy(&shipped.stderr);
    assert_eq!(shipped.status.code(), Some(70), "{err}");
    assert!(
        err.contains("missed code: no function at guest 0x8000040"),
        "{err}"
    );
    assert!(shipped.stdout.is_empty());

    let out = r(&["run"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert_eq!(String::from_utf8_lossy(&out.stdout), want, "{stderr}");
    assert_eq!(
        stderr.matches("hot-loaded 0x8000040 (main+0x40)").count(),
        1,
        "{stderr}"
    );
    let toml = std::fs::read_to_string(project.join("game.toml")).unwrap();
    assert!(toml.contains("\"main+0x40\""), "{toml}");

    let dev = game(&root, true);
    let err = String::from_utf8_lossy(&dev.stderr);
    assert_eq!(dev.status.code(), Some(70), "{err}");
    assert!(err.contains("RELIFT_DEV_SOCKET is not set"), "{err}");

    let out = r(&["build"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(
        stderr.contains("built 2 functions, 0 instructions not lifted"),
        "{stderr}"
    );
    let shipped = game(&root, false);
    assert!(
        shipped.status.success(),
        "{}",
        String::from_utf8_lossy(&shipped.stderr)
    );
    assert_eq!(String::from_utf8_lossy(&shipped.stdout), want);

    let out = r(&["run"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert_eq!(String::from_utf8_lossy(&out.stdout), want);
    assert!(!stderr.contains("hot-loaded"), "{stderr}");
}

#[test]
fn a_missed_call_runs_from_the_cold_copy() {
    if relift_build::find_tool("clang").is_none() && relift_build::find_tool("zig").is_none() {
        eprintln!("skipped: no clang or zig on PATH");
        return;
    }
    let root = tiny::scratch("e2e-cold");
    let project = tiny::dump_hidden(&root);
    let cache = root.join("cache").display().to_string();
    let r = |args: &[&str]| {
        let mut a = vec!["--cache-dir", &cache, "-j", "4"];
        a.extend_from_slice(args);
        tiny::relift(&project, &a)
    };
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let want = "hello from relift\n".repeat(3);
    let out = r(&["build"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(
        stderr.contains("1 superset functions from 1 root"),
        "{stderr}"
    );
    let shipped = game(&root, false);
    let err = String::from_utf8_lossy(&shipped.stderr);
    assert!(shipped.status.success(), "{err}");
    assert_eq!(String::from_utf8_lossy(&shipped.stdout), want, "{err}");
    assert!(!err.contains("cold code"), "{err}");

    let out = r(&["run"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert_eq!(String::from_utf8_lossy(&out.stdout), want, "{stderr}");
    assert_eq!(
        stderr
            .matches("first entry to cold code at 0x8000040 (main+0x40)")
            .count(),
        1,
        "{stderr}"
    );
    assert!(!stderr.contains("hot-loaded"), "{stderr}");
}

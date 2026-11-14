//! End to end: a tiny hand-assembled A64 program, packed as an NSO with an
//! NPDM, goes through `relift import`, `relift build` (analysis, the C
//! emitter, clang and the runtime) and runs: it calls a function, prints a
//! debug string from .rodata through `svcOutputDebugString`, loops on a
//! counter, and
//! exits through `svcExitProcess`. Skipped with a note when clang is not on
//! `PATH`.

mod tiny;

#[test]
fn a_tiny_program_builds_and_runs() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-tiny");
    let project = tiny::dump(&root);
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = tiny::relift(&project, &["--cache-dir", &cache, "-j", "4", "run"]);
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    let want = "hello from relift\n".repeat(3);
    assert_eq!(stdout, want, "{stderr}");
    assert!(
        stderr.contains("main lift     2 functions, 0 unlifted")
            && stderr.contains("built 2 functions, 0 instructions not lifted"),
        "{stderr}"
    );
}

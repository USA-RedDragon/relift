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
    let game = std::fs::read_dir(root.join("cache").join("projects"))
        .unwrap()
        .map(|e| {
            e.unwrap()
                .path()
                .join("build/host/dev/rt")
                .join(if cfg!(target_os = "macos") {
                    "librelift_rt.dylib"
                } else {
                    "librelift_rt.so"
                })
        })
        .find(|p| p.is_file())
        .expect("the runtime library of the dev build");
    let bytes = std::fs::read(&game).unwrap();
    assert!(
        bytes.windows(17).any(|w| w == b"rl_vk_switch_hook"),
        "the runtime library dropped the Vulkan hook's object"
    );
    assert!(!stderr.contains("vulkan-forwarding turned on"), "{stderr}");
}

/// The same program with every function split into one C function per
/// instruction and one file per piece: the loop's back edge, the call and
/// its return cross pieces through the context, and the output is the same.
#[test]
fn a_tiny_program_split_into_pieces_runs_the_same() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-tiny-split");
    let project = tiny::dump(&root);
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = tiny::relift_env(
        &project,
        &["--cache-dir", &cache, "-j", "4", "run"],
        &[
            ("RELIFT_EMIT_PIECE_KIB", "0"),
            ("RELIFT_EMIT_SHARD_KIB", "0"),
        ],
    );
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert_eq!(stdout, "hello from relift\n".repeat(3), "{stderr}");
    let c = root.join("cache/projects/tiny/build/host/c/0_main");
    let shards = std::fs::read_dir(&c)
        .expect("the C dir exists")
        .filter(|e| {
            e.as_ref()
                .is_ok_and(|e| e.file_name().to_string_lossy().starts_with("shard"))
        })
        .count();
    let text = std::fs::read_to_string(c.join("shard0000.c")).expect("shard0000.c");
    assert!(text.contains("_part1(c, mem);"), "{text}");
    assert!(shards >= 3, "{shards} shards");
}

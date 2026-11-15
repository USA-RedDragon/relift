//! A cache copied or hard-linked to another path, and a project moved,
//! build nothing again: every lifted C file and object is reused and the
//! program is the same bytes. Skipped with a note when clang or Ninja is not
//! on `PATH`.

use std::path::{Path, PathBuf};

mod tiny;

fn tools() -> bool {
    let found =
        relift_build::find_tool("clang").is_some() && relift_build::find_tool("ninja").is_some();
    if !found {
        eprintln!("skipped: no clang or ninja on PATH");
    }
    found
}

fn link_tree(from: &Path, to: &Path) {
    std::fs::create_dir_all(to).expect("the copy dir is creatable");
    for e in std::fs::read_dir(from).expect("the dir reads").flatten() {
        let (a, b) = (e.path(), to.join(e.file_name()));
        if a.is_dir() {
            link_tree(&a, &b);
        } else {
            std::fs::hard_link(&a, &b).expect("the file links");
        }
    }
}

fn copy_tree(from: &Path, to: &Path) {
    std::fs::create_dir_all(to).expect("the copy dir is creatable");
    for e in std::fs::read_dir(from).expect("the dir reads").flatten() {
        let (a, b) = (e.path(), to.join(e.file_name()));
        if a.is_dir() {
            copy_tree(&a, &b);
        } else {
            relift_cache::write_if_changed(&b, &std::fs::read(&a).expect("the file reads"))
                .expect("the file writes");
        }
    }
}

fn run(project: &Path, cache: &Path, args: &[&str]) -> String {
    let cache = cache.display().to_string();
    let mut all = vec!["--cache-dir", &cache, "-j", "4"];
    all.extend_from_slice(args);
    let out = tiny::relift(project, &all);
    let text = format!(
        "{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(out.status.success(), "{text}");
    text
}

fn game(cache: &Path, name: &str) -> Vec<u8> {
    std::fs::read(cache.join("projects").join(name).join("build/host/game"))
        .expect("the program was built")
}

fn up_to_date(text: &str) -> bool {
    text.contains("compile  up to date") || text.contains("compile  0 files")
}

fn why_dirty(cache: &Path) -> String {
    let dir = cache.join("projects/tiny/build/host");
    let out = relift_proc::command("ninja")
        .args(["-n", "-d", "explain"])
        .current_dir(dir)
        .output();
    out.map_or_else(
        |e| e.to_string(),
        |o| String::from_utf8_lossy(&o.stderr).into_owned(),
    )
}

#[test]
fn a_linked_copy_of_the_cache_and_a_moved_project_build_nothing() {
    if !tools() {
        return;
    }
    let root = tiny::scratch("relocate-switch");
    let project = tiny::dump(&root);
    let first = run(&project, &root.join("cache"), &["import", "../dump"]);
    drop(first);
    let first = run(&project, &root.join("cache"), &["build"]);
    assert!(!up_to_date(&first), "the first build compiles: {first}");
    let built = game(&root.join("cache"), "tiny");

    let there = root.join("elsewhere with spaces");
    link_tree(&root.join("cache"), &there.join("cache"));
    copy_tree(&project, &there.join("tiny"));
    std::fs::remove_dir_all(root.join("cache")).expect("the first cache goes");
    let explained = why_dirty(&there.join("cache"));
    let moved = run(&there.join("tiny"), &there.join("cache"), &["build"]);
    assert!(
        up_to_date(&moved),
        "a linked copy compiles nothing: {moved}{explained}"
    );
    assert_eq!(game(&there.join("cache"), "tiny"), built);

    let again = root.join("moved");
    std::fs::create_dir_all(&again).expect("the parent is creatable");
    std::fs::rename(there.join("tiny"), again.join("tiny")).expect("the project moves");
    let explained = why_dirty(&there.join("cache"));
    let after = run(&again.join("tiny"), &there.join("cache"), &["build"]);
    assert!(
        up_to_date(&after),
        "a moved project compiles nothing: {after}{explained}"
    );
    assert_eq!(game(&there.join("cache"), "tiny"), built);
}

#[test]
fn another_copy_of_the_runtime_sources_is_the_same_build() {
    if !tools() {
        return;
    }
    let root = tiny::scratch("relocate-runtime");
    let project = tiny::dump(&root);
    run(&project, &root.join("cache"), &["import", "../dump"]);
    run(&project, &root.join("cache"), &["build"]);
    let built = game(&root.join("cache"), "tiny");

    let rt: PathBuf = root.join("other relift/runtime");
    copy_tree(
        &Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime"),
        &rt,
    );
    let rt = rt.display().to_string();
    let again = run(
        &project,
        &root.join("cache"),
        &["--runtime-dir", &rt, "build"],
    );
    assert!(
        up_to_date(&again),
        "a copy of the runtime compiles nothing: {again}"
    );
    assert_eq!(game(&root.join("cache"), "tiny"), built);

    let header = Path::new(&rt).join("core/include/relift/abi_core.h");
    let mut text = std::fs::read(&header).expect("the header reads");
    text.extend_from_slice(b"\n#define RL_RELOCATE_TEST 1\n");
    relift_cache::write_if_changed(&header, &text).expect("the header writes");
    let changed = run(
        &project,
        &root.join("cache"),
        &["--runtime-dir", &rt, "build"],
    );
    assert!(
        !up_to_date(&changed),
        "a changed header rebuilds: {changed}"
    );
}

fn files(dir: &Path, out: &mut Vec<PathBuf>) {
    for e in std::fs::read_dir(dir).expect("the dir reads").flatten() {
        let p = e.path();
        if p.is_dir() {
            files(&p, out);
        } else {
            out.push(p);
        }
    }
}

fn snapshot(dir: &Path) -> Vec<(PathBuf, Vec<u8>)> {
    let mut all = Vec::new();
    files(dir, &mut all);
    all.sort();
    all.into_iter()
        .filter(|p| {
            !p.file_name()
                .is_some_and(|n| n == ".ninja_log" || n == ".ninja_deps")
        })
        .map(|p| {
            let bytes = std::fs::read(&p).expect("the file reads");
            (p.strip_prefix(dir).expect("under dir").to_path_buf(), bytes)
        })
        .collect()
}

#[test]
fn rebuilding_one_hard_linked_copy_leaves_the_other_untouched() {
    if !tools() {
        return;
    }
    let root = tiny::scratch("relocate-linked");
    let project = tiny::dump(&root);
    run(&project, &root.join("cache"), &["import", "../dump"]);
    run(&project, &root.join("cache"), &["build"]);

    link_tree(&root.join("cache"), &root.join("linked"));
    let before = snapshot(&root.join("linked"));

    let rt: PathBuf = root.join("other relift/runtime");
    copy_tree(
        &Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime"),
        &rt,
    );
    let header = rt.join("core/include/relift/abi_core.h");
    let mut text = std::fs::read(&header).expect("the header reads");
    text.extend_from_slice(b"\n#define RL_RELOCATE_TEST 1\n");
    relift_cache::write_if_changed(&header, &text).expect("the header writes");
    let rt = rt.display().to_string();
    let changed = run(
        &project,
        &root.join("cache"),
        &["--runtime-dir", &rt, "build"],
    );
    assert!(
        !up_to_date(&changed),
        "a changed header rebuilds: {changed}"
    );

    assert!(
        snapshot(&root.join("linked")) == before,
        "the rebuild changed a file of the linked copy"
    );
}

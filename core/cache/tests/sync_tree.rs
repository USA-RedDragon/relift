//! `sync_tree` copies what changed, drops what is gone and leaves the rest
//! with its mtime.

use std::path::{Path, PathBuf};

use relift_cache::{sync_tree, write_if_changed};

fn scratch(name: &str) -> PathBuf {
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    dir
}

fn c_or_dir(p: &Path) -> bool {
    p.extension().is_none_or(|e| e == "c" || e == "h")
}

#[test]
fn a_synced_tree_is_rewritten_only_where_it_differs() {
    let top = scratch("sync-tree");
    let (a, b) = (top.join("a"), top.join("b"));
    std::fs::create_dir_all(a.join("inc")).expect("the dir is creatable");
    write_if_changed(&a.join("x.c"), b"one").expect("writes");
    write_if_changed(&a.join("inc/y.h"), b"two").expect("writes");
    write_if_changed(&a.join("notes.md"), b"skipped").expect("writes");
    assert_eq!(sync_tree(&a, &b, &c_or_dir).expect("syncs"), 2);
    assert!(!b.join("notes.md").exists());
    let before = std::fs::metadata(b.join("x.c"))
        .and_then(|m| m.modified())
        .expect("mtime");
    assert_eq!(sync_tree(&a, &b, &c_or_dir).expect("syncs"), 0);
    let after = std::fs::metadata(b.join("x.c"))
        .and_then(|m| m.modified())
        .expect("mtime");
    assert_eq!(before, after);
    write_if_changed(&a.join("inc/y.h"), b"three").expect("writes");
    std::fs::remove_file(a.join("x.c")).expect("removes");
    assert_eq!(sync_tree(&a, &b, &c_or_dir).expect("syncs"), 1);
    assert!(!b.join("x.c").exists());
    assert_eq!(std::fs::read(b.join("inc/y.h")).expect("reads"), b"three");
}

//! A build directory hard-linked to another place builds nothing there, and
//! its program is the same bytes. Skipped with a note when clang or Ninja is
//! not on `PATH`.

use std::path::{Path, PathBuf};

use relift_build::{Compile, Event, Plan, build_with, find_tool};

fn scratch(name: &str) -> PathBuf {
    let d = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("the scratch dir is creatable");
    d
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

fn plan(root: &Path, clang: &Path) -> (Plan, PathBuf) {
    let dir = root.join("projects/p/build/host");
    let compiles = ["a", "b"]
        .iter()
        .map(|n| Compile {
            src: dir.join(format!("c/{n}.c")),
            obj: dir.join(format!("obj/{n}.o")),
            flags: vec![
                "-O1".into(),
                "-g".into(),
                format!("-I{}", root.join("runtime/include").display()),
            ],
            pool: None,
            stage: 0,
        })
        .collect();
    let plan = Plan {
        cc: vec![clang.display().to_string()],
        asm: None,
        compiles,
        output: dir.join("game"),
        link_flags: Vec::new(),
        pools: Vec::new(),
        root: Some(root.to_path_buf()),
    };
    (plan, dir)
}

fn steps(plan: &Plan, dir: &Path, ninja: &Path) -> usize {
    let mut n = 0;
    build_with(plan, dir, Some(ninja), 2, &mut |e| {
        if let Event::Step { .. } = e {
            n += 1;
        }
    })
    .expect("the build succeeds");
    n
}

#[test]
fn a_linked_copy_of_the_build_directory_builds_nothing() {
    let (Some(clang), Some(ninja)) = (find_tool("clang"), find_tool("ninja")) else {
        eprintln!("skipped: no clang or ninja on PATH");
        return;
    };
    let top = scratch("relocate");
    let (a, b) = (top.join("a cache"), top.join("b"));
    let (pa, da) = plan(&a, &clang);
    std::fs::create_dir_all(da.join("c")).expect("the source dir is creatable");
    std::fs::create_dir_all(a.join("runtime/include")).expect("the include dir is creatable");
    relift_cache::write_if_changed(&a.join("runtime/include/h.h"), b"#define V 7\n")
        .expect("the header writes");
    relift_cache::write_if_changed(
        &da.join("c/a.c"),
        b"#include <h.h>\nint main(void) { return V - 7; }\n",
    )
    .expect("the source writes");
    relift_cache::write_if_changed(
        &da.join("c/b.c"),
        b"#include <h.h>\nint other(void) { return __LINE__ + V; }\n",
    )
    .expect("the source writes");
    assert_eq!(steps(&pa, &da, &ninja), 3);
    link_tree(&a, &b);
    let (pb, db) = plan(&b, &clang);
    assert_eq!(steps(&pb, &db, &ninja), 0, "the moved build is up to date");
    assert_eq!(
        std::fs::read(da.join("game")).expect("the program reads"),
        std::fs::read(db.join("game")).expect("the program reads")
    );
    relift_cache::write_if_changed(&b.join("runtime/include/h.h"), b"#define V 8\n")
        .expect("the header writes");
    assert_eq!(steps(&pb, &db, &ninja), 3, "a changed header rebuilds");
}

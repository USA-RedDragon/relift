//! End to end: a Switch 2 build does not carry the build machine's folders.
//! The program built from `tiny/pac.c` (its NPDM grants SVC 0x80, so it
//! lifts as a Switch 2 program and the build makes its shader folder) names
//! no path of the build directory, and a copy moved away from the build, with
//! the build deleted, still runs. Where the runtime then finds the table
//! (beside the executable) is checked by the runtime's own tests.

mod tiny;

use std::path::Path;

const SWITCH2: &[u32] = &[0x01, 0x27, 0x80];

fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    !needle.is_empty() && haystack.windows(needle.len()).any(|w| w == needle)
}

fn copy_tree(from: &Path, to: &Path) {
    std::fs::create_dir_all(to).expect("folder is made");
    for e in std::fs::read_dir(from).expect("folder reads") {
        let e = e.expect("entry reads");
        let dest = to.join(e.file_name());
        if e.file_type().expect("type reads").is_dir() {
            copy_tree(&e.path(), &dest);
        } else {
            relift_cache::write_if_changed(&dest, &std::fs::read(e.path()).expect("file reads"))
                .expect("file copies");
        }
    }
}

#[test]
fn a_switch_2_program_runs_from_a_moved_copy_of_its_build() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch("e2e-shaders-moved");
    let project = tiny::dump_pac(&root, false, false, SWITCH2);
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = tiny::relift(&project, &["--cache-dir", &cache, "-j", "4", "build"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let build = root.join("cache/projects/tiny/build/host");
    assert!(
        build.join("shaders").is_dir(),
        "the build makes its shader folder"
    );
    assert!(!build.join("c/shaders.c").exists());
    let game = std::fs::read(build.join("game")).expect("game reads");
    let here = build.display().to_string();
    assert!(
        !contains(&game, here.as_bytes()),
        "the program names its build folder {here}"
    );
    let moved = root.join("moved");
    std::fs::create_dir_all(&moved).expect("folder is made");
    relift_cache::write_if_changed(&moved.join("game"), &game).expect("game copies");
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(moved.join("game"), std::fs::Permissions::from_mode(0o755))
            .expect("game is executable");
    }
    copy_tree(&build.join("shaders"), &moved.join("shaders"));
    std::fs::remove_dir_all(root.join("cache")).expect("build is removed");
    let run = relift_proc::command(moved.join("game"))
        .current_dir(root.join("tiny"))
        .env("RELIFT_PAC", "fast")
        .output()
        .expect("the game runs");
    let stderr = String::from_utf8_lossy(&run.stderr);
    assert!(run.status.success(), "{stderr}");
    assert!(
        String::from_utf8_lossy(&run.stdout).ends_with("done\n"),
        "{stderr}"
    );
}

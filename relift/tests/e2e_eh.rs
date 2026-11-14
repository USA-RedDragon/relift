//! End to end: the program of `tiny/eh.s` throws through two frames into a
//! catch, runs a cleanup pad that resumes the unwind, and rethrows from the
//! catch. Its unwinder ends in either of the two shapes of register install
//! the guest unwinders have: libgcc's `br` to the pad, and LLVM libunwind's
//! register block loaded by a routine that `ret`s to the pad. Both must
//! land at the pads with the registers the unwinder restored.

mod tiny;

const WANT: &str = "cleanup\ncaught\nthrown\nouter caught\nrethrown\n";

fn run(name: &str, llvm: bool) {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = tiny::scratch(name);
    let project = tiny::dump_eh(&root, llvm);
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
    let out = relift_proc::command(root.join("cache/projects/tiny/build/host/game"))
        .current_dir(&project)
        .output()
        .expect("the game runs");
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert_eq!(stdout, WANT, "{stderr}");
}

#[test]
fn a_throw_unwinds_to_pads_through_a_libgcc_style_install() {
    run("e2e-eh-gcc", false);
}

#[test]
fn a_throw_unwinds_to_pads_through_an_llvm_libunwind_style_install() {
    run("e2e-eh-llvm", true);
}

#[test]
fn the_program_binary_is_what_its_source_assembles_to() {
    match tiny::eh_matches_source() {
        Some(same) => assert!(same, "run `sh relift/tests/tiny/eh.sh`"),
        None => eprintln!("skipped: no llvm-mc or llvm-objcopy on PATH"),
    }
}

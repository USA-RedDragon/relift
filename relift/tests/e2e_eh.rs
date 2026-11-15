//! End to end: the program of `tiny/eh.s` throws through two frames into a
//! catch, runs a cleanup pad that resumes the unwind, and rethrows from the
//! catch. Its unwinder ends in either of the two shapes of register install
//! the guest unwinders have: libgcc's `br` to the pad, and LLVM libunwind's
//! register block loaded by a routine that `ret`s to the pad. Both must
//! land at the pads with the registers the unwinder restored, in the hot
//! code and with every function run from the cold copy (`emit.cold-first`).

mod tiny;

const WANT: &str = "cleanup\ncaught\nthrown\nouter caught\nrethrown\n";

fn run(name: &str, llvm: bool, cold_first: bool) {
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
    let mut args = vec!["--cache-dir", &cache, "-j", "4"];
    if cold_first {
        args.push("--emit.cold-first=true");
    }
    args.push("build");
    let out = tiny::relift(&project, &args);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    if cold_first {
        let c = root.join("cache/projects/tiny/build/host/c");
        let program = std::fs::read_to_string(c.join("program.c")).expect("program.c");
        assert!(
            program.contains("static_units[] = { &rl_cold_0_main, &rl_unit_0_main, };"),
            "{program}"
        );
        let cold: String = std::fs::read_dir(c.join("0_main/cold"))
            .expect("the cold copy")
            .map(|e| e.expect("entry").path())
            .filter(|p| {
                p.file_name()
                    .is_some_and(|n| n.to_string_lossy().starts_with("shard"))
            })
            .map(|p| std::fs::read_to_string(p).expect("a cold shard"))
            .collect();
        assert!(cold.contains("lp_dispatch:"), "{cold}");
        for line in cold
            .lines()
            .filter(|l| l.contains("rc = ") && l.contains("(c, mem)"))
        {
            assert!(
                line.contains("_cold") || line.contains("= rl_"),
                "a cold function calls hot code: {line}"
            );
        }
    }
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
    run("e2e-eh-gcc", false, false);
}

#[test]
fn a_throw_unwinds_to_pads_through_an_llvm_libunwind_style_install() {
    run("e2e-eh-llvm", true, false);
}

#[test]
fn a_throw_unwinds_to_pads_in_the_cold_copy_through_a_libgcc_style_install() {
    run("e2e-eh-gcc-cold", false, true);
}

#[test]
fn a_throw_unwinds_to_pads_in_the_cold_copy_through_an_llvm_libunwind_style_install() {
    run("e2e-eh-llvm-cold", true, true);
}

#[test]
fn the_program_binary_is_what_its_source_assembles_to() {
    match tiny::eh_matches_source() {
        Some(same) => assert!(same, "run `sh relift/tests/tiny/eh.sh`"),
        None => eprintln!("skipped: no llvm-mc or llvm-objcopy on PATH"),
    }
}

//! End to end: the program of `tiny/ret.s` returns where the CPU would, not
//! to the call site. A function that rewrites its saved x30 continues at
//! the new address, and a return to an address outside the guest's address
//! space (what a failed pointer authentication leaves in x30) stops with an
//! instruction abort report (docs/abi.md section 5.4).

mod tiny;

fn run(name: &str, wild: bool) -> Option<std::process::Output> {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return None;
    }
    let root = tiny::scratch(name);
    let project = tiny::dump_ret(&root, wild);
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
    Some(
        relift_proc::command(root.join("cache/projects/tiny/build/host/game"))
            .current_dir(&project)
            .output()
            .expect("the game runs"),
    )
}

#[test]
fn a_return_to_a_rewritten_address_continues_there() {
    let Some(out) = run("e2e-ret-moved", false) else {
        return;
    };
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert_eq!(
        String::from_utf8_lossy(&out.stdout),
        "ret moved\n",
        "{stderr}"
    );
}

#[test]
fn a_return_to_a_non_canonical_address_is_an_instruction_abort() {
    let Some(out) = run("e2e-ret-wild", true) else {
        return;
    };
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(!out.status.success(), "{stderr}");
    assert_eq!(String::from_utf8_lossy(&out.stdout), "", "{stderr}");
    assert!(
        stderr.contains("instruction abort: return to 0x20")
            && stderr.contains("from the call at guest pc 0x")
            && stderr.contains("outside the 39-bit address space"),
        "{stderr}"
    );
}

#[test]
fn the_ret_program_binary_is_what_its_source_assembles_to() {
    match tiny::ret_matches_source() {
        Some(same) => assert!(same, "run `sh relift/tests/tiny/ret.sh`"),
        None => eprintln!("skipped: no llvm-mc or llvm-objcopy on PATH"),
    }
}

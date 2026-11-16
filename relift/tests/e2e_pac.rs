//! End to end: the program of `tiny/pac.c`, built with return-address
//! signing under the B key, runs through signed frames (PACIBSP, AUTIBSP,
//! RETAB), signs and authenticates with PACIB1716 and AUTIB1716, strips
//! with XPACLRI and checks PACGA, with both ciphers. Its NPDM grants SVC
//! 0x80, so it lifts for the Switch 2's Cortex-A78C; with a Switch NPDM
//! the project setting `game.console` picks that CPU instead, and without
//! it RETAB is undefined on the Cortex-A57.

mod tiny;

use std::path::{Path, PathBuf};
use std::process::Output;

const WANT: &str = "signed frames return\npacib1716 signs\nautib1716 authenticates\n\
autib1716 with another modifier fails\nxpaclri strips\npacga repeats\n\
pacga keeps the low half zero\npacga depends on its inputs\n";

const SWITCH2: &[u32] = &[0x01, 0x27, 0x80];
const SWITCH: &[u32] = &[0x01, 0x27];

fn clang() -> bool {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return false;
    }
    true
}

/// Imports and builds the program, with `console` as `game.console`;
/// returns the root and the project directory.
fn build(name: &str, leaf: bool, corrupt: bool, svcs: &[u32], console: Option<&str>) -> PathBuf {
    let root = tiny::scratch(name);
    let project = tiny::dump_pac(&root, leaf, corrupt, svcs);
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    if let Some(c) = console {
        let path = project.join("game.toml");
        let text = std::fs::read_to_string(&path).expect("game.toml reads");
        let text = text.replacen("[game]\n", &format!("[game]\nconsole = \"{c}\"\n"), 1);
        relift_cache::write_if_changed(&path, text.as_bytes()).expect("game.toml writes");
    }
    let out = tiny::relift(&project, &["--cache-dir", &cache, "-j", "4", "build"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    root
}

fn run(root: &Path, pac: &str) -> Output {
    relift_proc::command(root.join("cache/projects/tiny/build/host/game"))
        .current_dir(root.join("tiny"))
        .env("RELIFT_PAC", pac)
        .output()
        .expect("the game runs")
}

fn passes(name: &str, leaf: bool, svcs: &[u32], console: Option<&str>) {
    if !clang() {
        return;
    }
    let root = build(name, leaf, false, svcs, console);
    for pac in ["fast", "qarma5"] {
        let out = run(&root, pac);
        let stdout = String::from_utf8_lossy(&out.stdout);
        let stderr = String::from_utf8_lossy(&out.stderr);
        assert!(out.status.success(), "{pac}: {stderr}");
        assert_eq!(stdout, format!("{WANT}done\n"), "{pac}: {stderr}");
    }
}

#[test]
fn signed_frames_run_on_the_switch_2_cpu() {
    passes("e2e-pac", false, SWITCH2, None);
}

#[test]
fn signed_leaf_frames_run_on_the_switch_2_cpu() {
    passes("e2e-pac-leaf", true, SWITCH2, None);
}

#[test]
fn the_project_setting_picks_the_switch_2_cpu() {
    passes("e2e-pac-setting", false, SWITCH, Some("switch2"));
}

#[test]
fn retab_is_undefined_on_the_switch_cpu() {
    if !clang() {
        return;
    }
    let root = build("e2e-pac-a57", false, false, SWITCH, None);
    let out = run(&root, "fast");
    assert!(!out.status.success());
    assert_eq!(String::from_utf8_lossy(&out.stdout), "");
}

/// A failed authenticated return: AUTIBSP or RETAB leaves a non-canonical
/// return address (PAuth2 without FPAC), so the fetch from it faults and
/// the process crashes before it prints "returned".
#[test]
fn a_corrupted_signed_return_address_crashes() {
    if !clang() {
        return;
    }
    let root = build("e2e-pac-corrupt", false, true, SWITCH2, None);
    for pac in ["fast", "qarma5"] {
        let out = run(&root, pac);
        let stdout = String::from_utf8_lossy(&out.stdout);
        assert!(!out.status.success(), "{pac}: {stdout}");
        assert_eq!(stdout, format!("{WANT}corrupting\n"), "{pac}");
    }
}

#[test]
fn the_program_binaries_are_what_their_source_builds_to() {
    match tiny::pac_matches_source() {
        Some(same) => assert!(same, "run `sh relift/tests/tiny/pac.sh`"),
        None => eprintln!("skipped: no clang, ld.lld or llvm-objcopy on PATH"),
    }
}

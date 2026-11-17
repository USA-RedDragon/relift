//! The Nintendo DS CPU timing against the consoles: the timing test ROM of
//! `check/nds-timing` (built at test time with clang and ld.lld; every
//! timing kernel of both CPUs, measured as on the consoles) goes through
//! `relift import`, `relift build` and a headless run, which prints each
//! case's cycles per loop iteration through the no$gba debug port, and
//! `check/nds-timing/compare.py` checks every case against the three
//! consoles' results in `hw.csv` (and the open ones against `open.csv`).
//! Skipped with a note when clang, ld.lld, llvm-objcopy or python3 is not
//! on `PATH`.

mod tiny;

use std::path::Path;

fn tools() -> bool {
    ["clang", "ld.lld", "llvm-objcopy", "python3"]
        .iter()
        .all(|t| relift_build::find_tool(t).is_some())
}

fn ok(out: &std::process::Output, what: &str) {
    assert!(
        out.status.success(),
        "{what}\n{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
}

#[test]
fn every_timing_case_runs_as_on_the_consoles() {
    if !tools() {
        eprintln!("skipped: clang, ld.lld, llvm-objcopy or python3 is not on PATH");
        return;
    }
    let check = Path::new(env!("CARGO_MANIFEST_DIR")).join("../check/nds-timing");
    let root = tiny::scratch("e2e-nds-timing");
    let rom = root.join("rom/timing.nds");
    std::fs::create_dir_all(root.join("rom")).expect("the ROM dir is creatable");
    let out = relift_proc::command("python3")
        .arg("-I")
        .arg(check.join("build.py"))
        .arg(&rom)
        .output()
        .expect("python3 runs");
    ok(&out, "build.py");
    let project = root.join("game");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(
        &project,
        &["--cache-dir", &cache, "import", &rom.display().to_string()],
    );
    ok(&out, "relift import");
    let out = tiny::relift(&project, &["--cache-dir", &cache, "-j", "4", "build"]);
    ok(&out, "relift build");
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(stderr.contains("0 instructions not lifted"), "{stderr}");
    let host = root.join("cache/projects/game/build/host");
    let out = relift_proc::command(host.join("game"))
        .args(["--headless", "--max-ticks", "20000000000"])
        .output()
        .expect("the game runs");
    ok(&out, "the test ROM");
    let csv: String = String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter(|l| ["#", "cpu,", "9,", "7,"].iter().any(|p| l.starts_with(p)))
        .map(|l| format!("{l}\n"))
        .collect();
    assert!(csv.contains("# end"), "the run ends: {csv}");
    let run = root.join("run.csv");
    relift_cache::write_if_changed(&run, csv.as_bytes()).expect("the run is writable");
    let out = relift_proc::command("python3")
        .arg("-I")
        .arg(check.join("compare.py"))
        .arg(&run)
        .output()
        .expect("python3 runs");
    ok(&out, "compare.py");
}

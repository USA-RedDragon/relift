//! The Nintendo DS CPU timing against the consoles: the timing test ROM of
//! `check/nds-timing` goes through `relift import`, `relift build` and a
//! headless run, which prints each case's cycles per loop iteration, and
//! `check/nds-timing/compare.py` checks every case against the three
//! consoles' results in `hw.csv` (and the open ones against `open.csv`).
//!
//! The ROM is the devkitPro build that ran on the consoles: the file
//! `RELIFT_NDS_TIMING_ROM` names (built elsewhere, such as in the devkitPro
//! image), or `sdk/` built at test time with `DEVKITARM` set. Without either
//! it is the clang
//! build (`build.py`, with clang, ld.lld and llvm-objcopy), which runs every
//! case but the ones whose code is copied to the instruction TCM, and
//! `compare.py --clang` leaves those out. Skipped with a note when neither
//! can be built, or python3 is not on `PATH`. A second test runs the clang
//! calls cartridge (`build.py --calls`: the BIOS calls, the slot-1
//! transfers and the BIOS waits' wake latency) and `compare.py --calls`.

mod tiny;

use std::path::{Path, PathBuf};

fn ok(out: &std::process::Output, what: &str) {
    assert!(
        out.status.success(),
        "{what}\n{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
}

fn copy_dir(from: &Path, to: &Path) {
    std::fs::create_dir_all(to).expect("the copy dir is creatable");
    for e in std::fs::read_dir(from).expect("the dir is readable") {
        let e = e.expect("the entry is readable");
        let dst = to.join(e.file_name());
        if e.file_type().expect("the entry has a type").is_dir() {
            copy_dir(&e.path(), &dst);
        } else {
            let bytes = std::fs::read(e.path()).expect("the file is readable");
            relift_cache::write_if_changed(&dst, &bytes).expect("the file is writable");
        }
    }
}

/// The devkitPro build of the ROM, from a copy of `check` under `root`, or
/// `None` without devkitPro.
fn sdk_rom(check: &Path, root: &Path) -> Option<PathBuf> {
    let arm = PathBuf::from(std::env::var_os("DEVKITARM")?);
    let pro = std::env::var_os("DEVKITPRO")
        .map(PathBuf::from)
        .or_else(|| arm.parent().map(Path::to_path_buf))?;
    if !arm.join("bin").is_dir() || relift_build::find_tool("make").is_none() {
        return None;
    }
    let src = root.join("src");
    copy_dir(check, &src);
    let path = std::env::join_paths([pro.join("tools/bin"), arm.join("bin")].into_iter().chain(
        std::env::split_paths(&std::env::var_os("PATH").unwrap_or_default()),
    ))
    .expect("PATH joins");
    let out = relift_proc::command("make")
        .args(["-C", "sdk", "relift-hwtest-ntr.nds"])
        .current_dir(&src)
        .env("DEVKITPRO", &pro)
        .env("DEVKITARM", &arm)
        .env("PATH", path)
        .output()
        .expect("make runs");
    ok(&out, "the devkitPro build");
    Some(src.join("sdk/relift-hwtest-ntr.nds"))
}

/// The clang build of the ROM (the calls cartridge when `calls`), or `None`
/// without its tools.
fn clang_rom(check: &Path, root: &Path, calls: bool) -> Option<PathBuf> {
    if !["clang", "ld.lld", "llvm-objcopy"]
        .iter()
        .all(|t| relift_build::find_tool(t).is_some())
    {
        return None;
    }
    let rom = root.join("rom/timing.nds");
    std::fs::create_dir_all(root.join("rom")).expect("the ROM dir is creatable");
    let mut build = relift_proc::command("python3");
    build.arg("-I").arg(check.join("build.py")).arg(&rom);
    if calls {
        build.arg("--calls");
    }
    let out = build.output().expect("python3 runs");
    ok(&out, "build.py");
    Some(rom)
}

/// Lifts and runs rom under root and checks its cases with `compare.py`
/// (`flag`: its build, as compare.py names it).
fn run_and_compare(check: &Path, root: &Path, rom: &Path, flag: Option<&str>) {
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
        .args([
            "--headless",
            "--max-ticks",
            "40000000000",
            "--until-debug",
            "# end",
        ])
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
    let mut compare = relift_proc::command("python3");
    compare.arg("-I").arg(check.join("compare.py")).arg(&run);
    if let Some(f) = flag {
        compare.arg(f);
    }
    let out = compare.output().expect("python3 runs");
    ok(&out, "compare.py");
}

#[test]
fn every_timing_case_runs_as_on_the_consoles() {
    if relift_build::find_tool("python3").is_none() {
        eprintln!("skipped: python3 is not on PATH");
        return;
    }
    let check = Path::new(env!("CARGO_MANIFEST_DIR")).join("../check/nds-timing");
    let root = tiny::scratch("e2e-nds-timing");
    let prebuilt = std::env::var_os("RELIFT_NDS_TIMING_ROM").map(PathBuf::from);
    let (rom, flag) = match prebuilt.or_else(|| sdk_rom(&check, &root)) {
        Some(r) => (r, None),
        None => match clang_rom(&check, &root, false) {
            Some(r) => (r, Some("--clang")),
            None => {
                eprintln!(
                    "skipped: neither devkitPro (DEVKITARM) nor clang, ld.lld and llvm-objcopy"
                );
                return;
            }
        },
    };
    run_and_compare(&check, &root, &rom, flag);
}

#[test]
fn bios_calls_cartridge_and_wakes_take_the_consoles_time() {
    if relift_build::find_tool("python3").is_none() {
        eprintln!("skipped: python3 is not on PATH");
        return;
    }
    let check = Path::new(env!("CARGO_MANIFEST_DIR")).join("../check/nds-timing");
    let root = tiny::scratch("e2e-nds-timing-calls");
    let Some(rom) = clang_rom(&check, &root, true) else {
        eprintln!("skipped: clang, ld.lld or llvm-objcopy is not on PATH");
        return;
    };
    run_and_compare(&check, &root, &rom, Some("--calls"));
}

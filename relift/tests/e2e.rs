//! End to end: a tiny hand-assembled A64 program, packed as an NSO with an
//! NPDM, goes through `relift import`, `relift build` (analysis, the C
//! emitter, clang and the runtime) and runs: it calls a function, prints a
//! debug string from .rodata through `svcOutputDebugString`, loops on a
//! counter, and
//! exits through `svcExitProcess`. Skipped with a note when clang is not on
//! `PATH`.

use std::path::{Path, PathBuf};

use relift_switch_formats::nso::{NsoSpec, build};

const MSG: &[u8] = b"hello from relift";

fn adr(rd: u32, off: i32) -> u32 {
    let imm = off as u32;
    0x1000_0000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7ffff) << 5) | rd
}

fn bl(off: i32) -> u32 {
    0x9400_0000 | ((off as u32 >> 2) & 0x3ff_ffff)
}

fn movz(rd: u32, imm: u32) -> u32 {
    0xd280_0000 | (imm << 5) | rd
}

fn svc(imm: u32) -> u32 {
    0xd400_0001 | (imm << 5)
}

fn subs_imm(rd: u32, rn: u32, imm: u32) -> u32 {
    0xf100_0000 | (imm << 10) | (rn << 5) | rd
}

fn bne(off: i32) -> u32 {
    0x5400_0001 | (((off as u32 >> 2) & 0x7ffff) << 5)
}

const RET: u32 = 0xd65f_03c0;

fn program() -> Vec<u8> {
    let code = [
        movz(19, 3),
        adr(0, 0x1000 - 0x04),
        bl(0x30 - 0x08),
        svc(0x27),
        subs_imm(19, 19, 1),
        bne(-0x10),
        svc(0x07),
        0x1400_0000,
    ];
    let mut text: Vec<u8> = code.iter().flat_map(|w| w.to_le_bytes()).collect();
    text.resize(0x30, 0);
    for w in [movz(1, MSG.len() as u32), RET] {
        text.extend_from_slice(&w.to_le_bytes());
    }
    text
}

fn scratch(name: &str) -> PathBuf {
    let d = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("the scratch dir is creatable");
    d
}

fn relift(dir: &Path, args: &[&str]) -> std::process::Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift"))
        .args(args)
        .env_remove("RELIFT_JOBS")
        .current_dir(dir)
        .output()
        .expect("the relift binary runs")
}

#[test]
fn a_tiny_program_builds_and_runs() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let root = scratch("e2e-tiny");
    let exefs = root.join("dump/exefs");
    std::fs::create_dir_all(&exefs).expect("the dump dir is creatable");
    let nso = build(&NsoSpec {
        text: program(),
        rodata: MSG.to_vec(),
        data: vec![0; 0x10],
        bss: 0x10,
        compress: true,
        ..NsoSpec::default()
    });
    relift_cache::write_if_changed(&exefs.join("main"), &nso).expect("main writes");
    let npdm = relift_switch_formats::npdm::build("tiny", 0x0100_0000_0000_1234, 0x10000, &[]);
    relift_cache::write_if_changed(&exefs.join("main.npdm"), &npdm).expect("npdm writes");
    let project = root.join("tiny");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let cache = root.join("cache").display().to_string();
    let out = relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = relift(&project, &["--cache-dir", &cache, "-j", "4", "run"]);
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    let want = "hello from relift\n".repeat(3);
    assert_eq!(stdout, want, "{stderr}");
    assert!(
        stderr.contains("2 functions, 0 instructions do not lift"),
        "{stderr}"
    );
}

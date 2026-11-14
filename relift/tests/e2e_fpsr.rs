//! FPSR flags turn on by themselves: a program whose `MRS FPSR` result is
//! used gets the host flags (here DZC from `1.0 / 0.0` decides whether it
//! prints), and a program without one is built flag-free. Skipped with a
//! note when clang is not on `PATH`.

use std::path::{Path, PathBuf};

use relift_switch_formats::nso::{NsoSpec, build};

const MSG: &[u8] = b"divide by zero seen";

fn adr(rd: u32, off: i32) -> u32 {
    let imm = off as u32;
    0x1000_0000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7ffff) << 5) | rd
}

fn movz(rd: u32, imm: u32) -> u32 {
    0xd280_0000 | (imm << 5) | rd
}

fn svc(imm: u32) -> u32 {
    0xd400_0001 | (imm << 5)
}

/// `tbz wt, #bit, off`.
fn tbz(t: u32, bit: u32, off: i32) -> u32 {
    0x3600_0000 | (bit << 19) | (((off as u32 >> 2) & 0x3fff) << 5) | t
}

const FMOV_S1_ONE: u32 = 0x1e2e_1001;
const FMOV_S2_WZR: u32 = 0x1e27_03e2;
const FDIV_S0_S1_S2: u32 = 0x1e22_1820;
const MRS_X2_FPSR: u32 = 0xd53b_4422;
const NOP: u32 = 0xd503_201f;

/// Divides by zero, then prints when FPSR.DZC is set (`read`), or reads
/// nothing and prints when a zero register is (it is), and exits.
fn program(read: bool) -> Vec<u8> {
    let code = [
        FMOV_S1_ONE,
        FMOV_S2_WZR,
        FDIV_S0_S1_S2,
        if read { MRS_X2_FPSR } else { movz(2, 2) },
        tbz(2, 1, 0x20 - 0x10),
        adr(0, 0x1000 - 0x14),
        movz(1, MSG.len() as u32),
        svc(0x27),
        svc(0x07),
        0x1400_0000,
        NOP,
    ];
    code.iter().flat_map(|w| w.to_le_bytes()).collect()
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
        .env_remove("RELIFT_FP_EXACT")
        .current_dir(dir)
        .output()
        .expect("the relift binary runs")
}

/// Builds and runs the program; gives stdout, stderr and program.c.
fn build_and_run(name: &str, read: bool) -> (String, String, String) {
    let root = scratch(name);
    let exefs = root.join("dump/exefs");
    std::fs::create_dir_all(&exefs).expect("the dump dir is creatable");
    let nso = build(&NsoSpec {
        text: program(read),
        rodata: MSG.to_vec(),
        data: vec![0; 0x10],
        bss: 0x10,
        compress: true,
        ..NsoSpec::default()
    });
    relift_cache::write_if_changed(&exefs.join("main"), &nso).expect("main writes");
    let npdm = relift_switch_formats::npdm::build("fpsr", 0x0100_0000_0000_1235, 0x10000, &[]);
    relift_cache::write_if_changed(&exefs.join("main.npdm"), &npdm).expect("npdm writes");
    let project = root.join("fpsr");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    let cache = root.join("cache").display().to_string();
    let out = relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = relift(&project, &["--cache-dir", &cache, "-j", "4", "run"]);
    let stdout = String::from_utf8_lossy(&out.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&out.stderr).into_owned();
    assert!(out.status.success(), "{stderr}");
    let prog = std::fs::read_to_string(root.join("cache/projects/fpsr/build/host/c/program.c"))
        .expect("program.c is readable");
    (stdout, stderr, prog)
}

#[test]
fn a_program_that_reads_fpsr_gets_host_flags_without_a_setting() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let (stdout, stderr, prog) = build_and_run("e2e-fpsr-read", true);
    assert!(
        stderr.contains("FPSR flags on: 1 usable FPSR read,"),
        "{stderr}"
    );
    assert!(prog.contains("rl_a64_set_fp_flags(1)"));
    assert_eq!(stdout, "divide by zero seen\n", "{stderr}");
}

#[test]
fn a_program_that_never_reads_fpsr_stays_flag_free() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let (stdout, stderr, prog) = build_and_run("e2e-fpsr-none", false);
    assert!(!stderr.contains("FPSR flags on"), "{stderr}");
    assert!(!prog.contains("rl_a64_set_fp_flags"));
    assert_eq!(stdout, "divide by zero seen\n", "{stderr}");
}

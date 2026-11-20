//! FPSR flags turn on by themselves: a program whose `MRS FPSR` result is
//! used gets the host flags (here DZC from `1.0 / 0.0` decides whether it
//! prints), and a program without one is built flag-free. Within a program
//! only the functions whose FP can reach a read produce flags. Skipped with
//! a note when clang is not on `PATH`.

use std::path::{Path, PathBuf};

use relift_switch_formats::nso::{NsoSpec, build};

#[path = "tiny/seed.rs"]
mod seed;

const MSG: &[u8] = b"divide by zero seen";
const MSG2: &[u8] = b"fp ok";

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

fn relift(dir: &Path, args: &[&str], env: &[(&str, &str)]) -> std::process::Output {
    let bin = Path::new(env!("CARGO_BIN_EXE_relift"));
    seed::seeded(dir, args, bin, || {
        relift_proc::command(bin)
            .args(args)
            .env_remove("RELIFT_JOBS")
            .env_remove("RELIFT_FP_EXACT")
            .env("RELIFT_LIBVPX_DIR", "off")
            .env("RELIFT_DAV1D_DIR", "off")
            .envs(env.iter().copied())
            .current_dir(dir)
            .output()
            .expect("the relift binary runs")
    })
}

fn bl(from: u32, to: u32) -> u32 {
    0x9400_0000 | ((to.wrapping_sub(from) >> 2) & 0x3ff_ffff)
}

const RET: u32 = 0xd65f_03c0;

/// Calls B, then A, and exits. B adds 1.5 to itself and prints when it is
/// 3.0, with no FPSR read anywhere near; A clears FPSR, divides by zero and
/// prints when FPSR.DZC is set.
fn two_functions() -> Vec<u8> {
    let code = [
        // 0x00: the entry.
        bl(0x00, 0x10),
        bl(0x04, 0x38),
        svc(0x07),
        0x1400_0000,
        // 0x10: B.
        0x1e2f_1001, // fmov s1, #1.5
        0x1e21_2820, // fadd s0, s1, s1
        0x1e21_1003, // fmov s3, #3.0
        0x1e23_2000, // fcmp s0, s3
        0x5400_00a1, // b.ne 0x34
        adr(0, 0x1020 - 0x24),
        movz(1, MSG2.len() as u32),
        svc(0x27),
        NOP,
        RET,
        // 0x38: A.
        0xd51b_443f, // msr fpsr, xzr
        FMOV_S1_ONE,
        FMOV_S2_WZR,
        FDIV_S0_S1_S2,
        MRS_X2_FPSR,
        tbz(2, 1, 0x5c - 0x4c),
        adr(0, 0x1000 - 0x50),
        movz(1, MSG.len() as u32),
        svc(0x27),
        RET,
    ];
    code.iter().flat_map(|w| w.to_le_bytes()).collect()
}

/// Clears FPSR, calls B through a register, reads FPSR and prints when
/// DZC is set, and exits. B divides by zero and returns.
fn through_a_pointer() -> Vec<u8> {
    let code = [
        // 0x00: the entry.
        0xd51b_443f, // msr fpsr, xzr
        adr(9, 0x30 - 0x04),
        0xd63f_0120, // blr x9
        MRS_X2_FPSR,
        tbz(2, 1, 0x20 - 0x10),
        adr(0, 0x1000 - 0x14),
        movz(1, MSG.len() as u32),
        svc(0x27),
        svc(0x07),
        0x1400_0000,
        NOP,
        NOP,
        // 0x30: B.
        FMOV_S1_ONE,
        FMOV_S2_WZR,
        FDIV_S0_S1_S2,
        RET,
    ];
    code.iter().flat_map(|w| w.to_le_bytes()).collect()
}

/// Builds and runs the program; gives stdout, stderr and program.c.
fn build_and_run(name: &str, read: bool) -> (String, String, String) {
    let text = program(read);
    build_and_run_text(name, text, MSG.to_vec())
}

/// Builds and runs `text` with `rodata`; gives stdout, stderr and program.c.
fn build_and_run_text(name: &str, text: Vec<u8>, rodata: Vec<u8>) -> (String, String, String) {
    build_and_run_env(name, text, rodata, &[])
}

/// As [`build_and_run_text`], with more environment for `relift run`.
fn build_and_run_env(
    name: &str,
    text: Vec<u8>,
    rodata: Vec<u8>,
    env: &[(&str, &str)],
) -> (String, String, String) {
    let root = scratch(name);
    let exefs = root.join("dump/exefs");
    std::fs::create_dir_all(&exefs).expect("the dump dir is creatable");
    let nso = build(&NsoSpec {
        text,
        rodata,
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
    let out = relift(&project, &["--cache-dir", &cache, "import", "../dump"], &[]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = relift(&project, &["--cache-dir", &cache, "-j", "4", "run"], env);
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
        stderr.contains("1 live reads, every region is bounded"),
        "{stderr}"
    );
    assert!(prog.contains("rl_arm64_set_fp_flags(1)"));
    assert_eq!(stdout, "divide by zero seen\n", "{stderr}");
}

#[test]
fn a_program_that_never_reads_fpsr_stays_flag_free() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let (stdout, stderr, prog) = build_and_run("e2e-fpsr-none", false);
    assert!(!stderr.contains("FPSR flags"), "{stderr}");
    assert!(!prog.contains("rl_arm64_set_fp_flags"));
    assert_eq!(stdout, "divide by zero seen\n", "{stderr}");
}

#[test]
fn only_the_function_whose_fp_reaches_a_read_produces_flags() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let mut rodata = MSG.to_vec();
    rodata.resize(0x20, 0);
    rodata.extend_from_slice(MSG2);
    let (stdout, stderr, prog) = build_and_run_text("e2e-fpsr-two", two_functions(), rodata);
    assert!(prog.contains("rl_arm64_set_fp_flags(1)"));
    assert!(
        stderr.contains("FPSR flags: 1 of 3 functions, 1 live reads"),
        "{stderr}"
    );
    assert_eq!(stdout, "fp ok\ndivide by zero seen\n", "{stderr}");
    let root =
        Path::new(env!("CARGO_TARGET_TMPDIR")).join("e2e-fpsr-two/cache/projects/fpsr/build/host");
    let parts = std::fs::read_to_string(root.join("c/0_main/relift_parts.h"))
        .expect("the parts header is readable");
    // A (0x38) is in part 1 of the shard; B (0x10) and the entry stay in
    // the flag-free part 0.
    let defs: Vec<&str> = parts
        .lines()
        .filter(|l| l.starts_with("#define RL_PART_"))
        .collect();
    assert_eq!(defs.len(), 1, "{parts}");
    assert!(defs[0].contains("fn_00000038"), "{parts}");
    assert!(parts.contains("#define RL_PART1_shard0000 1"), "{parts}");
    assert!(root.join("dev/obj/lifted/0_main/shard0000.p1.o").exists());
    assert!(root.join("dev/obj/lifted/0_main/shard0000.o").exists());
}

#[test]
fn a_flagged_function_split_into_pieces_is_all_in_part_1() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let mut rodata = MSG.to_vec();
    rodata.resize(0x20, 0);
    rodata.extend_from_slice(MSG2);
    let (stdout, stderr, _) = build_and_run_env(
        "e2e-fpsr-split",
        two_functions(),
        rodata,
        &[
            ("RELIFT_EMIT_PIECE_KIB", "0"),
            ("RELIFT_EMIT_SHARD_KIB", "0"),
        ],
    );
    assert_eq!(stdout, "fp ok\ndivide by zero seen\n", "{stderr}");
    let root = Path::new(env!("CARGO_TARGET_TMPDIR"))
        .join("e2e-fpsr-split/cache/projects/fpsr/build/host");
    let c = root.join("c/0_main");
    let parts =
        std::fs::read_to_string(c.join("relift_parts.h")).expect("the parts header is readable");
    // One name, defined again (identically) for each shard holding it.
    let defs: std::collections::BTreeSet<&str> = parts
        .lines()
        .filter(|l| l.starts_with("#define RL_PART_"))
        .collect();
    assert_eq!(defs.len(), 1, "{parts}");
    assert!(defs.iter().all(|d| d.contains("fn_00000038")), "{parts}");
    // Every shard that defines a piece of A (its stub or a part) compiles
    // a part 1.
    let mut holding = 0;
    for e in std::fs::read_dir(&c).expect("the C dir is readable") {
        let path = e.expect("an entry").path();
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        if !name.starts_with("shard") {
            continue;
        }
        let text = std::fs::read_to_string(&path).expect("a shard is readable");
        if !text.contains("fn_00000038E") {
            continue;
        }
        let stem = name.trim_end_matches(".c");
        if text.contains("RL_TEXT RL_LIFTED(_ZN5guest4main11fn_00000038E") {
            holding += 1;
            assert!(
                parts.contains(&format!("#define RL_PART1_{stem} 1")),
                "{stem}: {parts}"
            );
            assert!(
                root.join(format!("dev/obj/lifted/0_main/{stem}.p1.o"))
                    .exists(),
                "{stem}"
            );
        }
    }
    assert!(holding >= 2, "{holding} shards hold pieces of A");
}

#[test]
fn flags_raised_through_a_function_pointer_reach_the_read() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let (stdout, stderr, prog) =
        build_and_run_text("e2e-fpsr-pointer", through_a_pointer(), MSG.to_vec());
    assert!(prog.contains("rl_arm64_set_fp_flags(1)"));
    assert!(
        stderr.contains("a region is unbounded: every function produces flags"),
        "{stderr}"
    );
    assert_eq!(stdout, "divide by zero seen\n", "{stderr}");
}

//! The hand-assembled tiny program the end-to-end tests build.

#![allow(dead_code, reason = "each test binary uses part of the fixture")]

use std::path::{Path, PathBuf};

use relift_switch_formats::nso::{NsoSpec, build};

pub const MSG: &[u8] = b"hello from relift";

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

fn mul(rd: u32, rn: u32, rm: u32) -> u32 {
    0x9b00_7c00 | (rm << 16) | (rn << 5) | rd
}

fn add(rd: u32, rn: u32, rm: u32) -> u32 {
    0x8b00_0000 | (rm << 16) | (rn << 5) | rd
}

fn blr(rn: u32) -> u32 {
    0xd63f_0000 | (rn << 5)
}

/// The guest offset of the function only [`hidden_call`] reaches.
pub const HIDDEN: u64 = 0x40;

/// The tiny program with a call that discovery cannot follow: the loop
/// computes the address of the function that sets the message length with
/// a multiply, which constant propagation does not evaluate, and calls it
/// with `blr`. Nothing else names that function, so a build misses it.
fn hidden_call() -> Vec<u8> {
    let code = [
        movz(19, 3),
        adr(0, 0x1000 - 0x04),
        movz(8, (HIDDEN / 4) as u32),
        movz(9, 4),
        mul(8, 8, 9),
        adr(10, -0x14),
        add(8, 10, 8),
        blr(8),
        svc(0x27),
        subs_imm(19, 19, 1),
        bne(-0x24),
        svc(0x07),
        0x1400_0000,
    ];
    let mut text: Vec<u8> = code.iter().flat_map(|w| w.to_le_bytes()).collect();
    text.resize(HIDDEN as usize, 0);
    for w in [movz(1, MSG.len() as u32), RET] {
        text.extend_from_slice(&w.to_le_bytes());
    }
    text
}

/// `ldr x8, [x0]`.
const LDR_X8_X0: u32 = 0xf940_0008;

/// `blr x8`.
const BLR_X8: u32 = 0xd63f_0100;

fn add_imm(rd: u32, rn: u32, imm: u32) -> u32 {
    0x9100_0000 | (imm << 10) | (rn << 5) | rd
}

fn str_x(rt: u32, rn: u32) -> u32 {
    0xf900_0000 | (rn << 5) | rt
}

/// The guest offset of the function [`diff_program`] calls.
pub const DIFF_FN: u64 = 0x30;

/// The tiny program with a function, called through `blr` so the C
/// compiler does not inline it, that adds, then stores the sum
/// into `.data`, which `relift diff` compares: x1 becomes the message length
/// through two adds.
fn diff_program() -> Vec<u8> {
    let code = [
        movz(19, 3),
        adr(0, 0x1000 - 0x04),
        adr(8, DIFF_FN as i32 - 0x08),
        blr(8),
        svc(0x27),
        subs_imm(19, 19, 1),
        bne(-0x14),
        svc(0x07),
        0x1400_0000,
    ];
    let mut text: Vec<u8> = code.iter().flat_map(|w| w.to_le_bytes()).collect();
    text.resize(DIFF_FN as usize, 0);
    let body = [
        movz(1, MSG.len() as u32 / 2),
        add(1, 1, 1),
        add_imm(1, 1, 1),
        adr(2, 0x2000 - 0x3c),
        str_x(1, 2),
        RET,
    ];
    for w in body {
        text.extend_from_slice(&w.to_le_bytes());
    }
    text
}

fn program(undefined: bool, pointer: bool) -> Vec<u8> {
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
    let tail = [
        Some(movz(1, MSG.len() as u32)),
        undefined.then_some(0),
        pointer.then_some(LDR_X8_X0),
        pointer.then_some(BLR_X8),
        Some(RET),
    ];
    for w in tail.into_iter().flatten() {
        text.extend_from_slice(&w.to_le_bytes());
    }
    text
}

/// The scratch directory `name`. `RELIFT_TEST_TMP` replaces the build's
/// target directory, for a test built on one host and run on another.
pub fn scratch(name: &str) -> PathBuf {
    let base = std::env::var_os("RELIFT_TEST_TMP")
        .map_or_else(|| PathBuf::from(env!("CARGO_TARGET_TMPDIR")), PathBuf::from);
    let d = base.join(name);
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("the scratch dir is creatable");
    d
}

/// Runs relift in `dir`. `RELIFT_TEST_BIN` replaces the binary cargo built,
/// for a test built on one host and run on another.
pub fn relift(dir: &Path, args: &[&str]) -> std::process::Output {
    let bin = std::env::var_os("RELIFT_TEST_BIN").map_or_else(
        || PathBuf::from(env!("CARGO_BIN_EXE_relift")),
        PathBuf::from,
    );
    relift_proc::command(bin)
        .args(args)
        .env_remove("RELIFT_JOBS")
        .current_dir(dir)
        .output()
        .expect("the relift binary runs")
}

/// Writes the decrypted dump of the tiny program under `root/dump` and
/// returns the project directory `root/tiny`.
pub fn dump(root: &Path) -> PathBuf {
    dump_with(root, false)
}

/// As [`dump`]; with `undefined`, the called function holds one word that
/// does not lift.
pub fn dump_with(root: &Path, undefined: bool) -> PathBuf {
    dump_text(root, program(undefined, false))
}

/// As [`dump`], with the call to a hidden function of [`hidden_call`].
pub fn dump_hidden(root: &Path) -> PathBuf {
    dump_text(root, hidden_call())
}

/// As [`dump`]; the called function also calls through a pointer it
/// loads from memory (`ldr x8, [x0]; blr x8`), a transfer analysis cannot
/// resolve. It is for analysis only: the program does not run.
pub fn dump_with_pointer(root: &Path) -> PathBuf {
    dump_text(root, program(false, true))
}

/// As [`dump`], with the function of [`diff_program`].
pub fn dump_diff(root: &Path) -> PathBuf {
    dump_text(root, diff_program())
}

fn dump_text(root: &Path, text: Vec<u8>) -> PathBuf {
    dump_nso(
        root,
        &NsoSpec {
            text,
            rodata: MSG.to_vec(),
            data: vec![0; 0x10],
            bss: 0x10,
            compress: true,
            ..NsoSpec::default()
        },
    )
}

const EH: &[u8] = include_bytes!("eh.bin");

/// As [`dump`], with the program of `eh.s`: it throws through two frames
/// into a catch, runs a cleanup pad that resumes, and rethrows. `llvm`
/// picks the register installer its unwinder ends in: the LLVM libunwind
/// shape (`ret` to the pc it loaded into x30) or the libgcc one (`br`).
pub fn dump_eh(root: &Path, llvm: bool) -> PathBuf {
    let mut data = vec![0u8; 0x1000];
    data[0x10] = u8::from(llvm);
    dump_nso(
        root,
        &NsoSpec {
            text: EH[..0x1000].to_vec(),
            rodata: EH[0x1000..].to_vec(),
            data,
            bss: 0x1000,
            compress: true,
            ..NsoSpec::default()
        },
    )
}

/// The bytes of `eh.bin` and whether `eh.s` still assembles to them.
pub fn eh_matches_source() -> Option<bool> {
    let tool = relift_build::find_tool("llvm-mc")?;
    let here = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/tiny");
    let out = scratch("eh-source").join("eh.o");
    let ok = relift_proc::command(tool)
        .args(["-triple=aarch64", "-filetype=obj", "-o"])
        .arg(&out)
        .arg(here.join("eh.s"))
        .status()
        .ok()?
        .success();
    let objcopy = relift_build::find_tool("llvm-objcopy")?;
    let bin = out.with_extension("bin");
    let copied = relift_proc::command(objcopy)
        .args(["-O", "binary", "--only-section=.text"])
        .arg(&out)
        .arg(&bin)
        .status()
        .ok()?
        .success();
    Some(ok && copied && std::fs::read(bin).ok()? == EH)
}

fn dump_nso(root: &Path, spec: &NsoSpec) -> PathBuf {
    let exefs = root.join("dump/exefs");
    std::fs::create_dir_all(&exefs).expect("the dump dir is creatable");
    let nso = build(spec);
    relift_cache::write_if_changed(&exefs.join("main"), &nso).expect("main writes");
    let npdm = relift_switch_formats::npdm::build("tiny", 0x0100_0000_0000_1234, 0x10000, &[]);
    relift_cache::write_if_changed(&exefs.join("main.npdm"), &npdm).expect("npdm writes");
    let project = root.join("tiny");
    std::fs::create_dir_all(&project).expect("the project dir is creatable");
    project
}

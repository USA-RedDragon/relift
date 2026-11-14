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

fn program(undefined: bool) -> Vec<u8> {
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
        Some(RET),
    ];
    for w in tail.into_iter().flatten() {
        text.extend_from_slice(&w.to_le_bytes());
    }
    text
}

pub fn scratch(name: &str) -> PathBuf {
    let d = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("the scratch dir is creatable");
    d
}

pub fn relift(dir: &Path, args: &[&str]) -> std::process::Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift"))
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
    dump_text(root, program(undefined))
}

/// As [`dump`], with the call to a hidden function of [`hidden_call`].
pub fn dump_hidden(root: &Path) -> PathBuf {
    dump_text(root, hidden_call())
}

fn dump_text(root: &Path, text: Vec<u8>) -> PathBuf {
    let exefs = root.join("dump/exefs");
    std::fs::create_dir_all(&exefs).expect("the dump dir is creatable");
    let nso = build(&NsoSpec {
        text,
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
    project
}

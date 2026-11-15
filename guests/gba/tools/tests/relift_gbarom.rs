//! `relift-gbarom` as a subprocess over synthetic ROMs.

use std::path::{Path, PathBuf};
use std::process::{Output, Stdio};

use relift_gba_formats::checksum::{hex, logo_sha256};
use relift_gba_formats::synth::{self, Spec};

fn dir(name: &str) -> PathBuf {
    let d = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("the scratch dir is creatable");
    d
}

fn put(dir: &Path, name: &str, bytes: &[u8]) -> PathBuf {
    let p = dir.join(name);
    relift_cache::write_if_changed(&p, bytes).expect("the fixture is writable");
    p
}

fn gbarom(dir: &Path, args: &[&str]) -> Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift-gbarom"))
        .args(args)
        .current_dir(dir)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .expect("the tool runs")
}

fn path(p: &Path) -> &str {
    p.to_str().expect("utf-8 path")
}

fn json(out: &Output) -> serde_json::Value {
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    serde_json::from_slice(&out.stdout).expect("JSON output")
}

fn sample() -> Vec<u8> {
    let mut rom = synth::rom(&Spec {
        title: "HELLO".to_owned(),
        game_code: "AHEL".to_owned(),
        maker: "01".to_owned(),
        version: 2,
        ..Spec::default()
    });
    synth::plant(&mut rom, 0x200, "FLASH1M_V103");
    rom
}

#[test]
fn json_reports_the_header_and_the_save_string() {
    let d = dir("gbarom-json");
    let rom = sample();
    let p = put(&d, "a.gba", &rom);
    let hash = hex(&logo_sha256(&rom));
    let v = json(&gbarom(&d, &[path(&p), "--json", "--logo-sha256", &hash]));
    assert_eq!(v["title"], "HELLO");
    assert_eq!(v["game_code"], "AHEL");
    assert_eq!(v["version"], 2);
    assert_eq!(v["entry_target"], 0x0800_00C0);
    assert_eq!(v["complement"]["ok"], true);
    assert_eq!(v["logo_ok"], true);
    assert_eq!(v["size_ok"], true);
    assert_eq!(v["save"]["type"], "flash128");
    assert_eq!(v["save"]["strings"][0]["string"], "FLASH1M_V103");
    assert_eq!(v["save"]["strings"][0]["offset"], 0x200);
}

#[test]
fn text_output_and_check_flag() {
    let d = dir("gbarom-text");
    let rom = sample();
    let p = put(&d, "a.gba", &rom);
    let hash = hex(&logo_sha256(&rom));
    let out = gbarom(&d, &[path(&p), "--check", "--logo-sha256", &hash]);
    assert!(out.status.success());
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    assert!(text.contains("title: HELLO"), "{text}");
    assert!(text.contains("save: FLASH1M_V103 at $200"), "{text}");
    assert!(!text.contains("MISMATCH"), "{text}");
    let wrong = gbarom(&d, &[path(&p)]);
    assert!(wrong.status.success());
    assert!(String::from_utf8_lossy(&wrong.stdout).contains("MISMATCH"));
    let checked = gbarom(&d, &[path(&p), "--check"]);
    assert_eq!(checked.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&checked.stderr).contains("checks failed"));
}

#[test]
fn a_bad_complement_fails_the_check() {
    let d = dir("gbarom-complement");
    let mut rom = sample();
    rom[0xBC] ^= 1;
    let hash = hex(&logo_sha256(&rom));
    let p = put(&d, "a.gba", &rom);
    let out = gbarom(&d, &[path(&p), "--check", "--logo-sha256", &hash]);
    assert_eq!(out.status.code(), Some(1));
    let text = gbarom(&d, &[path(&p)]);
    assert!(String::from_utf8_lossy(&text.stdout).contains("MISMATCH"));
}

#[test]
fn a_rom_without_a_save_string_says_so() {
    let d = dir("gbarom-nosave");
    let p = put(&d, "a.gba", &synth::rom(&Spec::default()));
    let out = gbarom(&d, &[path(&p)]);
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    assert!(text.contains("save: no library string found"), "{text}");
    let v = json(&gbarom(&d, &[path(&p), "--json"]));
    assert!(v["save"]["type"].is_null());
}

#[test]
fn multiboot_is_refused_with_one_line() {
    let d = dir("gbarom-multiboot");
    let p = put(&d, "mb.gba", &synth::multiboot(0x4000));
    let out = gbarom(&d, &[path(&p)]);
    assert_eq!(out.status.code(), Some(1));
    let err = String::from_utf8_lossy(&out.stderr).into_owned();
    assert_eq!(err.trim_end().lines().count(), 1, "{err}");
    assert!(err.contains("multiboot"), "{err}");
}

#[test]
fn bad_inputs_fail_with_one_line() {
    let d = dir("gbarom-bad");
    let short = put(&d, "short.gba", &[0; 64]);
    let out = gbarom(&d, &[path(&short)]);
    assert_eq!(out.status.code(), Some(1));
    let err = String::from_utf8_lossy(&out.stderr).into_owned();
    assert_eq!(err.trim_end().lines().count(), 1, "{err}");
    assert!(err.contains("shorter than the header"), "{err}");
    assert_eq!(gbarom(&d, &["nope.gba"]).status.code(), Some(1));
    let p = put(&d, "a.gba", &sample());
    let hex_bad = gbarom(&d, &[path(&p), "--logo-sha256", "zz"]);
    assert_eq!(hex_bad.status.code(), Some(1));
}

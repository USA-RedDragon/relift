//! `relift-gbrom` as a subprocess over synthetic ROMs.

use std::path::{Path, PathBuf};
use std::process::{Output, Stdio};

use relift_gb_formats::synth::{self, Spec};

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

fn gbrom(dir: &Path, args: &[&str]) -> Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift-gbrom"))
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

#[test]
fn json_reports_the_header() {
    let d = dir("gbrom-json");
    let rom = synth::rom(&Spec {
        title: "HELLO".to_owned(),
        cgb: 0x80,
        sgb: 3,
        cart_type: 0x10,
        rom_size: 0x02,
        ram_size: 0x03,
    });
    let p = put(&d, "a.gb", &rom);
    let v = json(&gbrom(&d, &[path(&p), "--json"]));
    assert_eq!(v["title"], "HELLO");
    assert_eq!(v["cgb"], "cgb-compatible");
    assert_eq!(v["sgb"], true);
    assert_eq!(v["mapper"], "mbc3");
    assert_eq!(v["features"]["rtc"], true);
    assert_eq!(v["rom_banks"], 8);
    assert_eq!(v["ram_bytes"], 0x8000);
    assert_eq!(v["entry_jump"], 0x150);
    assert_eq!(v["header_checksum"]["ok"], true);
    assert_eq!(v["global_checksum"]["ok"], true);
    assert_eq!(v["logo_ok"], true);
}

#[test]
fn text_output_and_check_flag() {
    let d = dir("gbrom-text");
    let mut rom = synth::rom(&Spec::default());
    let good = put(&d, "good.gb", &rom);
    let out = gbrom(&d, &[path(&good), "--check"]);
    assert!(out.status.success());
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    assert!(text.contains("title: SYNTH"), "{text}");
    assert!(text.contains("cartridge type: $00 none"), "{text}");
    assert!(
        text.contains("global checksum:") && !text.contains("MISMATCH"),
        "{text}"
    );
    rom[0x300] ^= 0xFF;
    let bad = put(&d, "bad.gb", &rom);
    let plain = gbrom(&d, &[path(&bad)]);
    assert!(plain.status.success());
    assert!(String::from_utf8_lossy(&plain.stdout).contains("MISMATCH"));
    let checked = gbrom(&d, &[path(&bad), "--check"]);
    assert_eq!(checked.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&checked.stderr).contains("checks failed"));
}

#[test]
fn multicart_is_reported() {
    let d = dir("gbrom-multi");
    let p = put(&d, "m.gb", &synth::mbc1_multicart());
    assert_eq!(json(&gbrom(&d, &[path(&p), "--json"]))["mapper"], "mbc1m");
}

#[test]
fn bad_inputs_fail_with_one_line() {
    let d = dir("gbrom-bad");
    let short = put(&d, "short.gb", &[0; 64]);
    let out = gbrom(&d, &[path(&short)]);
    assert_eq!(out.status.code(), Some(1));
    let err = String::from_utf8_lossy(&out.stderr).into_owned();
    assert_eq!(err.trim_end().lines().count(), 1, "{err}");
    assert!(err.contains("shorter than the header"), "{err}");
    let missing = gbrom(&d, &["nope.gb"]);
    assert_eq!(missing.status.code(), Some(1));
}

fn banked_rom() -> Vec<u8> {
    let mut rom = synth::rom(&Spec {
        cart_type: 0x19,
        rom_size: 0x02,
        ..Spec::default()
    });
    rom[..0x100].fill(0xFF);
    rom[0x150..].fill(0xFF);
    rom[0x150..0x159].copy_from_slice(&[0x3E, 0x02, 0xEA, 0x00, 0x20, 0xCD, 0x00, 0x40, 0xC9]);
    rom[0x8000] = 0xC9;
    synth::fix_checksums(&mut rom);
    rom
}

#[test]
fn functions_lists_functions_per_bank_as_json() {
    let d = dir("gbrom-functions");
    let p = put(&d, "a.gb", &banked_rom());
    let v = json(&gbrom(&d, &["functions", path(&p), "--json"]));
    assert_eq!(v["mapper"], "mbc5");
    assert_eq!(v["function_count"], 2);
    let banks = v["banks"].as_array().expect("banks");
    assert_eq!(banks[2]["functions"], 1);
    assert_eq!(banks[2]["bytes"], 1);
    assert_eq!(v["functions"][1]["entry"]["text"], "02:4000");
    assert!(v["unresolved"].as_array().expect("list").is_empty());
}

#[test]
fn functions_prints_text_and_summary_hides_the_list() {
    let d = dir("gbrom-functions-text");
    let p = put(&d, "a.gb", &banked_rom());
    let full = gbrom(&d, &["functions", path(&p)]);
    let text = String::from_utf8_lossy(&full.stdout).into_owned();
    assert!(text.contains("02:4000"), "{text}");
    let short = gbrom(&d, &["functions", path(&p), "--summary"]);
    let text = String::from_utf8_lossy(&short.stdout).into_owned();
    assert!(text.contains("bank 02: 1 functions"), "{text}");
    assert!(!text.contains("02:4000 "), "{text}");
}

#[test]
fn functions_strict_fails_on_an_unresolved_transfer() {
    let d = dir("gbrom-functions-strict");
    let mut rom = banked_rom();
    rom[0x150..0x15A]
        .copy_from_slice(&[0xFA, 0x00, 0xC0, 0x6F, 0x26, 0x40, 0xE9, 0x00, 0x00, 0x00]);
    synth::fix_checksums(&mut rom);
    let p = put(&d, "a.gb", &rom);
    assert!(gbrom(&d, &["functions", path(&p)]).status.success());
    let out = gbrom(&d, &["functions", path(&p), "--strict"]);
    assert_eq!(out.status.code(), Some(1));
    let v = json(&gbrom(&d, &["functions", path(&p), "--json"]));
    assert_eq!(v["unresolved_by_kind"]["indirect-jump"], 1);
}

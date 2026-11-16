//! `relift-smsrom` as a subprocess over synthetic ROMs.

use std::path::{Path, PathBuf};
use std::process::{Output, Stdio};

use relift_sms_formats::synth::{self, Spec};

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

fn smsrom(dir: &Path, args: &[&str]) -> Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift-smsrom"))
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

#[test]
fn json_reports_everything() {
    let d = dir("smsrom-json");
    let rom = synth::rom(&Spec {
        len: 8 * 0x4000,
        size_code: 0xF,
        stores: vec![0xFFFF],
        ..Spec::default()
    });
    let f = put(&d, "game.GG", &rom);
    let out = smsrom(&d, &["info", path(&f), "--json"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let v: serde_json::Value = serde_json::from_slice(&out.stdout).expect("JSON output");
    assert_eq!(v["banks"], 8);
    assert_eq!(v["system"], "sms");
    assert_eq!(v["system_source"], "header");
    assert_eq!(v["header"]["offset"], 0x7FF0);
    assert_eq!(v["header"]["region"], "SMS Export");
    assert_eq!(v["header"]["checksum"]["ok"], true);
    assert_eq!(v["mapper"]["kind"], "sega");
    assert_eq!(v["mapper"]["sega_stores"], 1);
}

#[test]
fn text_reports_the_main_lines() {
    let d = dir("smsrom-text");
    let rom = synth::rom(&Spec {
        len: 3 * 0x4000,
        copier_header: true,
        header_at: None,
        ..Spec::default()
    });
    let f = put(&d, "game.sg", &rom);
    let out = smsrom(&d, &["info", path(&f)]);
    assert!(out.status.success());
    let t = String::from_utf8_lossy(&out.stdout);
    assert!(t.contains("system: sg1000 (from extension)"), "{t}");
    assert!(t.contains("copier header skipped"), "{t}");
    assert!(t.contains("not a power of two"), "{t}");
    assert!(t.contains("header: none"), "{t}");
    assert!(t.contains("mapper: none"), "{t}");
}

#[test]
fn an_empty_file_is_one_line_on_stderr() {
    let d = dir("smsrom-empty");
    let f = put(&d, "e.sms", &[]);
    let out = smsrom(&d, &["info", path(&f)]);
    assert_eq!(out.status.code(), Some(1));
    let e = String::from_utf8_lossy(&out.stderr);
    assert!(e.contains("malformed"), "{e}");
}

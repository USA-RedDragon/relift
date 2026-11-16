//! `relift-ndsrom` as a subprocess over synthetic ROMs.

use std::path::{Path, PathBuf};
use std::process::{Output, Stdio};

use relift_nds_formats::checksum::{hex, sha256};
use relift_nds_formats::synth::{self, OverlaySpec, Spec, StaticSpec};

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

fn ndsrom(dir: &Path, args: &[&str]) -> Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift-ndsrom"))
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

fn text(out: &Output) -> String {
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8(out.stdout.clone()).expect("UTF-8 output")
}

fn sample() -> Vec<u8> {
    synth::rom(&Spec {
        title: "HELLO".to_owned(),
        game_code: "AHEL".to_owned(),
        arm9: StaticSpec {
            compress: true,
            ..StaticSpec::default()
        },
        overlays9: vec![OverlaySpec {
            id: 4,
            ram: 0x0210_0000,
            data: synth::pattern(0x400, 3),
            bss: 0x10,
            compress: true,
            authenticated: false,
            static_init: (0x0210_0000, 0x0210_0008),
        }],
        files: vec![
            ("a.txt".to_owned(), synth::pattern(10, 1)),
            ("d/b.bin".to_owned(), synth::pattern(20, 2)),
        ],
        ..Spec::default()
    })
}

#[test]
fn header_prints_text_and_json() {
    let d = dir("header");
    let rom = put(&d, "a.nds", &sample());
    let t = text(&ndsrom(&d, &["header", path(&rom)]));
    assert!(t.contains("title: HELLO"), "{t}");
    assert!(t.contains("header crc: "), "{t}");
    assert!(t.contains("logo: sha256"), "{t}");
    let v = json(&ndsrom(&d, &["header", path(&rom), "--json"]));
    assert_eq!(v["game_code"], "AHEL");
    assert_eq!(v["header_crc"]["ok"], true);
    assert_eq!(v["logo_crc"]["ok"], true);
    assert_eq!(v["logo_ok"], false);
    assert_eq!(v["arm9"]["ram"], "02000000");
}

#[test]
fn header_check_fails_on_a_wrong_logo_and_passes_with_its_hash() {
    let d = dir("check");
    let bytes = sample();
    let rom = put(&d, "a.nds", &bytes);
    let out = ndsrom(&d, &["header", path(&rom), "--check"]);
    assert!(!out.status.success());
    assert!(String::from_utf8_lossy(&out.stderr).contains("checks failed"));
    let h = hex(&sha256(&bytes[0xC0..0xC0 + 156]));
    let out = ndsrom(&d, &["header", path(&rom), "--check", "--logo-sha256", &h]);
    assert!(out.status.success());
    let out = ndsrom(&d, &["header", path(&rom), "--logo-sha256", "zz"]);
    assert!(!out.status.success());
}

#[test]
fn units_lists_hashes_of_decompressed_units() {
    let d = dir("units");
    let spec_bytes = sample();
    let rom = put(&d, "a.nds", &spec_bytes);
    let v = json(&ndsrom(&d, &["units", path(&rom), "--json"]));
    let list = v.as_array().expect("an array");
    assert_eq!(list.len(), 2 + 2 + 1 + 1);
    let s = &list[0];
    assert_eq!(s["name"], "arm9_static.bin");
    assert_eq!(s["compressed"], true);
    let want = synth::static_image(&StaticSpec::default()).0;
    assert_eq!(s["sha256"], hex(&sha256(&want)));
    let ov = list.iter().find(|u| u["kind"] == "overlay").unwrap();
    assert_eq!(ov["name"], "overlays9_4.bin");
    assert_eq!(ov["sha256"], hex(&sha256(&synth::pattern(0x400, 3))));
    let t = text(&ndsrom(&d, &["units", path(&rom)]));
    assert!(t.contains("arm9_auto_01ff8000.bin"), "{t}");
    assert!(t.contains("compressed"), "{t}");
}

#[test]
fn extract_writes_the_units_and_units_json() {
    let d = dir("extract");
    let rom = put(&d, "a.nds", &sample());
    let out = d.join("out");
    let t = text(&ndsrom(&d, &["extract", path(&rom), path(&out)]));
    assert!(t.contains("extracted 6 units"), "{t}");
    for name in [
        "arm9_raw.bin",
        "arm9_static.bin",
        "arm9_auto_01ff8000.bin",
        "arm9_auto_027e0000.bin",
        "arm7_raw.bin",
        "arm7_static.bin",
        "arm7_auto_03800000.bin",
        "overlays9_4.bin",
        "units.json",
    ] {
        assert!(out.join(name).exists(), "{name}");
    }
    let image = std::fs::read(out.join("arm9_static.bin")).unwrap();
    assert_eq!(image, synth::static_image(&StaticSpec::default()).0);
    assert_eq!(
        std::fs::read(out.join("overlays9_4.bin")).unwrap(),
        synth::pattern(0x400, 3)
    );
    let u: serde_json::Value =
        serde_json::from_slice(&std::fs::read(out.join("units.json")).unwrap()).unwrap();
    assert_eq!(u["arm9"]["compressed"], true);
    assert_eq!(u["arm9"]["autoload"].as_array().unwrap().len(), 2);
    assert_eq!(u["arm7"]["compressed"], false);
    assert_eq!(u["overlays9"][0]["id"], 4);
    assert_eq!(u["overlays9"][0]["compressed"], true);
    assert_eq!(u["overlays7"].as_array().unwrap().len(), 0);
}

#[test]
fn files_lists_paths_with_sizes() {
    let d = dir("files");
    let rom = put(&d, "a.nds", &sample());
    let t = text(&ndsrom(&d, &["files", path(&rom)]));
    assert!(t.lines().any(|l| l.trim() == "10 a.txt"), "{t}");
    assert!(t.lines().any(|l| l.trim() == "20 d/b.bin"), "{t}");
    let v = json(&ndsrom(&d, &["files", path(&rom), "--json"]));
    assert_eq!(v.as_array().unwrap().len(), 2);
}

#[test]
fn bad_input_is_a_one_line_error() {
    let d = dir("bad");
    let short = put(&d, "short.nds", &[0u8; 100]);
    let out = ndsrom(&d, &["header", path(&short)]);
    assert!(!out.status.success());
    let err = String::from_utf8_lossy(&out.stderr);
    assert_eq!(err.lines().count(), 1, "{err}");
    assert!(err.contains("malformed"));
    assert!(
        !ndsrom(&d, &["units", path(&d.join("missing.nds"))])
            .status
            .success()
    );
}

#[test]
fn functions_reports_every_unit_and_writes_one_file_each() {
    let d = dir("functions");
    let rom = put(&d, "a.nds", &sample());
    let out = d.join("out");
    let v = json(&ndsrom(
        &d,
        &[
            "functions",
            path(&rom),
            "--json",
            "--out",
            path(&out),
            "--tag",
            "t",
            "--jobs",
            "2",
        ],
    ));
    let units: Vec<&str> = v["units"]
        .as_array()
        .expect("an array")
        .iter()
        .map(|u| u["unit"].as_str().expect("a name"))
        .collect();
    assert_eq!(units, ["static", "arm7", "ov4"]);
    assert_eq!(
        v["units"][2]["sha256"],
        hex(&sha256(&synth::pattern(0x400, 3)))
    );
    for u in units {
        let f = out.join(format!("t-{u}.out.json"));
        let bytes = std::fs::read(&f).expect("a report per unit");
        let r: serde_json::Value = serde_json::from_slice(&bytes).expect("JSON report");
        assert!(r["functions"].is_array() && r["data"].is_array(), "{u}");
        assert_eq!(r["unit"], u);
    }
    let t = text(&ndsrom(&d, &["functions", path(&rom)]));
    assert!(
        t.lines().any(|l| l.starts_with("ov4 02100000-02100400")),
        "{t}"
    );
}

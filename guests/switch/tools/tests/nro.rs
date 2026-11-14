//! `relift-nro` as a user runs it.

use std::path::PathBuf;
use std::process::Output;

use relift_switch_formats::nro::{self, NroSpec};
use relift_switch_formats::romfs;

fn scratch(name: &str) -> PathBuf {
    let d = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("relift-nro-{name}"));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("scratch");
    d
}

fn run(args: &[&str]) -> Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift-nro"))
        .args(args)
        .output()
        .expect("relift-nro runs")
}

fn fixture(dir: &std::path::Path) -> PathBuf {
    let bytes = nro::build(&NroSpec {
        text: vec![0xAA; 0x100],
        rodata: b"ro".to_vec(),
        data: vec![0xDD; 0x20],
        bss: 0x1000,
        build_id: [0x11; 32],
        symbols: vec![("main_sym".into(), 0x1000, 4)],
        icon: Some(vec![0xFF, 0xD8, 9]),
        nacp: Some(vec![3u8; 0x4000]),
        romfs: Some(romfs::build(&[
            ("x/y.txt", b"deep"),
            ("top.bin", &[5u8; 9]),
        ])),
    });
    let p = dir.join("app.nro");
    relift_cache::write_if_changed(&p, &bytes).expect("write");
    p
}

#[test]
fn show_prints_json() {
    let d = scratch("show");
    let p = fixture(&d);
    let out = run(&["show", "--json", p.to_str().expect("utf8")]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let v: serde_json::Value = serde_json::from_slice(&out.stdout).expect("json");
    assert_eq!(v["build_id"], "11".repeat(32));
    assert_eq!(v["bss_size"], 0x1000);
    assert_eq!(v["symbols"][0]["name"], "main_sym");
    assert_eq!(v["assets"]["romfs"]["present"], true);
    assert_eq!(v["mod0"]["offset"], 0x80);
}

#[test]
fn extract_writes_segments_assets_and_romfs() {
    let d = scratch("extract");
    let p = fixture(&d);
    let o = d.join("out");
    let out = run(&[
        "extract",
        p.to_str().expect("utf8"),
        "-o",
        o.to_str().expect("utf8"),
    ]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    for f in [
        "text.bin",
        "rodata.bin",
        "data.bin",
        "image.bin",
        "icon.jpg",
        "control.nacp",
    ] {
        assert!(o.join(f).is_file(), "{f}");
    }
    assert_eq!(
        std::fs::read(o.join("icon.jpg")).expect("icon"),
        [0xFF, 0xD8, 9]
    );
    assert_eq!(
        std::fs::read(o.join("romfs/x/y.txt")).expect("romfs"),
        b"deep"
    );
    assert_eq!(
        std::fs::read(o.join("romfs/top.bin")).expect("romfs"),
        [5u8; 9]
    );
    // Unrelated input fails with the tool's name.
    let junk = d.join("junk.nro");
    relift_cache::write_if_changed(&junk, &[0u8; 0x200]).expect("write");
    let out = run(&["show", "--json", junk.to_str().expect("utf8")]);
    assert!(!out.status.success());
    assert!(String::from_utf8_lossy(&out.stderr).starts_with("relift-nro:"));
}

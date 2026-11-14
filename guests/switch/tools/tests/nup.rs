//! The `relift-nup` binary as a user runs it, over a synthetic container.

use std::path::{Path, PathBuf};
use std::process::Output;

use relift_switch_formats::nup::{BuildPackage, PackageKind, build};

fn scratch(name: &str) -> PathBuf {
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("scratch dir");
    dir
}

fn nup_file(dir: &Path) -> PathBuf {
    let pkg = |kind, version, main: &'static [u8], title_id| BuildPackage {
        title_id,
        version,
        kind,
        standalone: false,
        files: vec![("exefs/main", main), ("romfs/a/b.txt", main)],
        empty_dirs: vec![],
        meta: vec![],
    };
    let bytes = build(&[
        pkg(PackageKind::Base, 0, b"base", 0x0100_0000_0000_3000),
        pkg(
            PackageKind::Update,
            0x10000,
            b"update one",
            0x0100_0000_0000_3000,
        ),
        pkg(PackageKind::Addon, 0, b"dlc", 0x0100_0000_0000_3001),
    ]);
    let path = dir.join("game.nup");
    relift_cache::write_if_changed(&path, &bytes).expect("fixture writes");
    path
}

fn tool(args: &[&str]) -> Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift-nup"))
        .args(args)
        .current_dir(env!("CARGO_TARGET_TMPDIR"))
        .output()
        .expect("relift-nup runs")
}

fn s(p: &Path) -> &str {
    p.to_str().expect("utf-8 path")
}

#[test]
fn ls_json_lists_packages_and_files() {
    let dir = scratch("nup-cli-ls");
    let f = nup_file(&dir);
    let out = tool(&["ls", "--json", s(&f)]);
    assert!(out.status.success());
    let v: serde_json::Value = serde_json::from_slice(&out.stdout).expect("json");
    let pk = v["packages"].as_array().expect("packages");
    assert_eq!(pk.len(), 3);
    assert_eq!(pk[1]["kind"], "update");
    assert_eq!(pk[1]["version"], 0x10000);
    assert_eq!(pk[2]["title_id"], "0100000000003001");
    assert_eq!(pk[0]["files"][0]["path"], "exefs/main");
    let text = tool(&["ls", s(&f)]);
    assert!(text.status.success());
    assert!(String::from_utf8_lossy(&text.stdout).contains("romfs/a/b.txt"));
}

#[test]
fn extract_title_merges_the_newest_update() {
    let dir = scratch("nup-cli-title");
    let f = nup_file(&dir);
    let o = dir.join("out");
    let out = tool(&["extract", s(&f), "-o", s(&o)]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert_eq!(std::fs::read(o.join("exefs/main")).unwrap(), b"update one");
    assert_eq!(
        std::fs::read(o.join("romfs/a/b.txt")).unwrap(),
        b"update one"
    );
    let o2 = dir.join("base");
    let out = tool(&["extract", s(&f), "-o", s(&o2), "--update", "base"]);
    assert!(out.status.success());
    assert_eq!(std::fs::read(o2.join("exefs/main")).unwrap(), b"base");
}

#[test]
fn extract_one_package() {
    let dir = scratch("nup-cli-pkg");
    let f = nup_file(&dir);
    for (spec, want, rel) in [
        ("base", &b"base"[..], "romfs/a/b.txt"),
        ("update:65536", b"update one", "exefs/main"),
        ("update:0x10000", b"update one", "exefs/main"),
        ("dlc:0100000000003001:0", b"dlc", "romfs/a/b.txt"),
    ] {
        let o = dir.join(spec.replace(':', "_"));
        let out = tool(&["extract", s(&f), "-o", s(&o), "--package", spec]);
        assert!(
            out.status.success(),
            "{spec}: {}",
            String::from_utf8_lossy(&out.stderr)
        );
        assert_eq!(std::fs::read(o.join(rel)).unwrap(), want, "{spec}");
    }
}

#[test]
fn errors_are_one_line_and_exit_1() {
    let dir = scratch("nup-cli-err");
    let f = nup_file(&dir);
    let o = dir.join("out");
    let out = tool(&["extract", s(&f), "-o", s(&o), "--package", "update:7"]);
    assert_eq!(out.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&out.stderr).starts_with("relift-nup: no package update:7"));
    let mut bytes = std::fs::read(&f).unwrap();
    bytes[0x90] ^= 1;
    let bad = dir.join("bad.nup");
    relift_cache::write_if_changed(&bad, &bytes).unwrap();
    let out = tool(&["ls", s(&bad)]);
    assert_eq!(out.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&out.stderr).contains("hash mismatch"));
}

#[test]
fn stdin_input_works() {
    use std::io::Write;
    use std::process::Stdio;
    let dir = scratch("nup-cli-stdin");
    let f = nup_file(&dir);
    let mut child = relift_proc::command(env!("CARGO_BIN_EXE_relift-nup"))
        .args(["ls", "--json", "-"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .expect("spawns");
    let bytes = std::fs::read(&f).unwrap();
    child.stdin.take().unwrap().write_all(&bytes).unwrap();
    let out = child.wait_with_output().unwrap();
    assert!(out.status.success());
}

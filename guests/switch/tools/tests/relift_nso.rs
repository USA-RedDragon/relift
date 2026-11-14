//! `relift-nso` against the synthetic NSOs of an exefs.

mod common;

use common::*;

fn main_nso(name: &str) -> (Fixture, std::path::PathBuf) {
    let f = fixture(name);
    let nso = put(&f.dir, "main.nso", &f.game.base_spec.exefs[0].1);
    (f, nso)
}

#[test]
fn show_json_reports_module_build_id_segments_and_symbols() {
    let (f, nso) = main_nso("nso-show");
    let v = json(&run("relift-nso", &f.dir, &["show", path(&nso), "--json"]));
    assert_eq!(v["module_path"], "D:\\work\\game\\out\\game-main.nss");
    assert_eq!(v["module_name"], "game-main");
    assert_eq!(v["build_id"], "a1".repeat(32));
    assert_eq!(v["segments"].as_array().expect("segments").len(), 3);
    assert_eq!(v["bss_size"], 0x40);
    assert!(v["mod0"].is_object());
    assert_eq!(v["symbol_count"], 2);
    let text = run("relift-nso", &f.dir, &["show", path(&nso)]);
    assert!(String::from_utf8_lossy(&ok(&text).stdout).contains("module name: game-main"));
}

#[test]
fn symbols_lists_defined_and_undefined() {
    let (f, nso) = main_nso("nso-symbols");
    let v = json(&run(
        "relift-nso",
        &f.dir,
        &["symbols", path(&nso), "--json"],
    ));
    let syms = v.as_array().expect("array");
    let find = |n: &str| syms.iter().find(|s| s["name"] == n).expect("symbol");
    let main = find("nnMain");
    assert_eq!(main["value"], 0x100);
    assert_eq!(main["size"], 8);
    assert_eq!(main["type"], "func");
    assert_eq!(main["binding"], "global");
    assert_eq!(main["defined"], true);
    assert_eq!(find("nnImported")["defined"], false);
    let text = run("relift-nso", &f.dir, &["symbols", path(&nso)]);
    assert!(
        String::from_utf8_lossy(&ok(&text).stdout)
            .contains("nnMain\t0x100\t8\tfunc\tglobal\tdefined")
    );
}

#[test]
fn decompress_writes_the_hactool_layout_and_flat_adds_bss() {
    let (f, nso) = main_nso("nso-decompress");
    let out = f.dir.join("main.raw.nso");
    ok(&run(
        "relift-nso",
        &f.dir,
        &["decompress", path(&nso), "-o", path(&out)],
    ));
    let plain = std::fs::read(&out).expect("output reads");
    assert_eq!(&plain[..4], b"NSO0");
    assert!(plain.len() > f.game.base_spec.exefs[0].1.len() / 2);

    let back = run("relift-nso", &f.dir, &["show", path(&out), "--json"]);
    assert_eq!(json(&back)["build_id"], "a1".repeat(32));

    let flat = f.dir.join("main.flat");
    ok(&run(
        "relift-nso",
        &f.dir,
        &["decompress", path(&nso), "-o", path(&flat), "--flat"],
    ));
    let flat = std::fs::read(&flat).expect("flat reads");
    let n = relift_switch_formats::nso::Nso::parse(&f.game.base_spec.exefs[0].1).expect("parses");
    assert_eq!(flat, n.flat_image());
    assert_ne!(flat, plain);
}

#[test]
fn decompress_to_stdout_pipes_into_show() {
    let (f, nso) = main_nso("nso-pipe");
    let raw = run("relift-nso", &f.dir, &["decompress", path(&nso), "-o", "-"]);
    let out = run_stdin(
        "relift-nso",
        &f.dir,
        &["show", "-", "--json"],
        &ok(&raw).stdout,
    );
    assert_eq!(json(&out)["module_name"], "game-main");
}

#[test]
fn bad_inputs_exit_one_with_one_line() {
    let (f, _) = main_nso("nso-bad");
    fails_with(
        &run("relift-nso", &f.dir, &["show", "missing.nso"]),
        "missing.nso",
    );
    let out = run_stdin("relift-nso", &f.dir, &["show", "-"], &[0u8; 0x200]);
    assert_eq!(out.status.code(), Some(1));
    assert_eq!(String::from_utf8_lossy(&out.stderr).lines().count(), 1);
}

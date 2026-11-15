//! `relift-ir` on hex words and on a synthetic NSO.

mod common;

use common::*;

#[test]
fn hex_words_print_one_block_each() {
    let f = fixture("ir-hex");
    let out = run(
        "relift-ir",
        &f.dir,
        &["d2800540", "0x8b020020", "--pc", "0x4000"],
    );
    let text = String::from_utf8_lossy(&ok(&out).stdout).into_owned();
    assert!(
        text.contains("; 0x4000 d2800540 MOVZ_64_movewide"),
        "{text}"
    );
    assert!(text.contains("block 0x4000 len 4 regs arm64"), "{text}");
    assert!(text.contains("block 0x4004 len 4 regs arm64"), "{text}");
    assert!(text.contains("set x0, "), "{text}");
    assert_eq!(text.matches("exit jump 0x").count(), 2, "{text}");
}

#[test]
fn dce_and_json_report_ir_and_errors() {
    let f = fixture("ir-json");
    let v = json(&run(
        "relift-ir",
        &f.dir,
        &["8b020020", "00000000", "--dce", "--json"],
    ));
    let a = v.as_array().expect("array");
    assert_eq!(a.len(), 2);
    assert_eq!(a[0]["encoding"], "ADD_64_addsub_shift");
    assert!(a[0]["ir"].as_str().expect("ir").contains("add "));
    assert!(a[0]["error"].is_null());
    assert!(a[1]["ir"].is_null());
    assert!(a[1]["error"].is_string());
}

#[test]
fn nso_symbols_lift_over_their_size() {
    let f = fixture("ir-nso");
    let nso = put(&f.dir, "main.nso", &f.game.base_spec.exefs[0].1);
    let v = json(&run(
        "relift-ir",
        &f.dir,
        &["--nso", path(&nso), "--sym", "nnMain", "--json"],
    ));
    let a = v.as_array().expect("array");
    assert_eq!(a.len(), 2);
    assert_eq!(a[0]["addr"], "0x100");
    assert_eq!(a[1]["addr"], "0x104");
    let by_addr = json(&run(
        "relift-ir",
        &f.dir,
        &[
            "--nso",
            path(&nso),
            "--addr",
            "0x100",
            "--count",
            "2",
            "--json",
        ],
    ));
    assert_eq!(by_addr, v);
}

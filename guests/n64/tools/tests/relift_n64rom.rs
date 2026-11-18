//! `relift-n64rom` as a subprocess over synthetic ROMs.

use std::path::{Path, PathBuf};
use std::process::{Output, Stdio};

use relift_n64_formats::compress::Format;
use relift_n64_formats::synth::{self, Spec, encode};
use relift_n64_formats::{ByteOrder, Cic};

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

fn n64rom(dir: &Path, args: &[&str]) -> Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift-n64rom"))
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
    synth::rom(&Spec {
        name: "TEST IMAGE".to_owned(),
        crc_as: Some(Cic::C6103),
        ..Spec::default()
    })
}

#[test]
fn info_json_reports_order_header_and_forced_checksum() {
    let d = dir("n64rom-info");
    let be = sample();
    let v64 = ByteOrder::V64.encode(&be).unwrap();
    let f = put(&d, "a.v64", &v64);
    let v = json(&n64rom(&d, &["info", path(&f), "--json", "--cic", "6103"]));
    assert_eq!(v["byte_order"], "v64");
    assert_eq!(v["name"], "TEST IMAGE");
    assert_eq!(v["entry"], "80000400");
    assert_eq!(v["entry_adjusted"], "7FF00400");
    assert_eq!(v["cic"], "6103/7103");
    assert_eq!(v["crc"]["status"], "ok");
    let v = json(&n64rom(&d, &["info", path(&f), "--json", "--cic", "7101"]));
    assert_eq!(v["crc"]["status"], "mismatch");
}

#[test]
fn info_text_and_check_exit_codes() {
    let d = dir("n64rom-text");
    let f = put(&d, "a.z64", &sample());
    let out = n64rom(&d, &["info", path(&f)]);
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    assert!(text.contains("byte order: z64"), "{text}");
    assert!(text.contains("boot variant: unknown"), "{text}");
    assert!(text.contains("not checked"), "{text}");
    assert!(!n64rom(&d, &["info", path(&f), "--check"]).status.success());
    assert!(
        n64rom(&d, &["info", path(&f), "--check", "--cic", "6103"])
            .status
            .success()
    );
    let out = n64rom(&d, &["info", path(&f), "--cic", "9999"]);
    assert!(!out.status.success());
    assert!(String::from_utf8_lossy(&out.stderr).contains("unknown variant"));
}

#[test]
fn bad_input_is_a_one_line_error() {
    let d = dir("n64rom-bad");
    let f = put(&d, "bad.bin", &[1; 0x40]);
    let out = n64rom(&d, &["info", path(&f)]);
    assert!(!out.status.success());
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(err.contains("unknown byte order"), "{err}");
    assert_eq!(err.lines().count(), 1);
}

#[test]
fn normalize_writes_big_endian_from_each_order() {
    let d = dir("n64rom-normalize");
    let be = sample();
    for (order, name) in [
        (ByteOrder::V64, "v.v64"),
        (ByteOrder::N64, "n.n64"),
        (ByteOrder::Z64, "z.z64"),
    ] {
        let f = put(&d, name, &order.encode(&be).unwrap());
        let out = d.join("out").join(format!("{name}.z64"));
        let r = n64rom(&d, &["normalize", path(&f), path(&out)]);
        assert!(r.status.success(), "{}", String::from_utf8_lossy(&r.stderr));
        assert_eq!(std::fs::read(&out).unwrap(), be);
    }
}

#[test]
fn streams_lists_offsets_and_sizes() {
    let d = dir("n64rom-streams");
    let mut be = sample();
    let a = synth::filler(4, 300);
    let s = encode(Format::Yaz0, &a);
    be[0x2000..0x2000 + s.len()].copy_from_slice(&s);
    let f = put(&d, "s.n64", &ByteOrder::N64.encode(&be).unwrap());
    let v = json(&n64rom(&d, &["streams", path(&f), "--json"]));
    let list = v["streams"].as_array().unwrap();
    assert_eq!(list.len(), 1);
    assert_eq!(list[0]["offset"], 0x2000);
    assert_eq!(list[0]["format"], "Yaz0");
    assert_eq!(list[0]["decoded_size"], 300);
    let out = n64rom(&d, &["streams", path(&f)]);
    assert_eq!(String::from_utf8_lossy(&out.stdout), "00002000 Yaz0 300\n");
}

/// `jal 0x80000500`.
const JAL_500: u32 = 0x0C00_0140;
/// `jr ra`.
const JR_RA: u32 = 0x03E0_0008;
/// `addiu v0, zero, 7`.
const LI_V0_7: u32 = 0x2402_0007;

#[test]
fn functions_lists_the_code_reached_from_the_entry() {
    let d = dir("n64rom-functions");
    let mut be = sample();
    be[0x1000..].fill(0);
    let words: [u32; 6] = [JAL_500, 0, JR_RA, 0, 0, 0];
    for (k, w) in words.iter().enumerate() {
        be[0x1000 + 4 * k..0x1004 + 4 * k].copy_from_slice(&w.to_be_bytes());
    }
    for (k, w) in [LI_V0_7, JR_RA, 0].iter().enumerate() {
        be[0x1100 + 4 * k..0x1104 + 4 * k].copy_from_slice(&w.to_be_bytes());
    }
    let f = put(&d, "f.v64", &ByteOrder::V64.encode(&be).unwrap());
    let v = json(&n64rom(
        &d,
        &["functions", path(&f), "--json", "--cic", "6102"],
    ));
    assert_eq!(v["entry"], "80000400");
    assert_eq!(v["boot"]["variant"], "6102/7101");
    let fs = v["functions"].as_array().unwrap();
    assert!(
        fs.iter()
            .any(|f| f["addr"] == "80000400" && f["origin"] == "entry")
    );
    assert!(
        fs.iter()
            .any(|f| f["addr"] == "80000500" && f["origin"] == "call")
    );
    let units = v["units"].as_array().unwrap();
    assert!(
        units
            .iter()
            .any(|u| u["link"] == "80000400" && u["len"] == 16)
    );
    let out = n64rom(&d, &["functions", path(&f), "--cic", "6102"]);
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    assert!(text.contains("80000500 rom 00001100 call"), "{text}");
    let out = n64rom(&d, &["functions", path(&f), "--cic", "1234"]);
    assert!(!out.status.success());
}

#[test]
fn ir_prints_vr4300_and_rsp_blocks() {
    let d = dir("n64rom-ir");
    let out = n64rom(&d, &["ir", "--dce", "0x8c880004"]);
    assert!(out.status.success());
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(text.contains("regs vr4300"), "{text}");
    assert!(text.contains("io.load.be.any"), "{text}");
    let out = n64rom(&d, &["ir", "--rsp", "--dce", "0x4a031050"]);
    assert!(out.status.success());
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(text.contains("regs rsp"), "{text}");
    assert!(text.contains("lane.add.16"), "{text}");
}

#[test]
fn ir_rejects_a_word_that_is_not_hex() {
    let d = dir("n64rom-ir-bad");
    let out = n64rom(&d, &["ir", "zz"]);
    assert!(!out.status.success());
    assert_eq!(String::from_utf8_lossy(&out.stderr).lines().count(), 1);
}

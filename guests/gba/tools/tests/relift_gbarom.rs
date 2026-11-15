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
fn multiboot_entry_is_shown() {
    let d = dir("gbarom-multiboot");
    let p = put(&d, "mb.gba", &synth::multiboot(0x4000));
    let out = gbarom(&d, &[path(&p)]);
    assert!(out.status.success());
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    assert!(
        text.contains("multiboot entry: EA00000E (b $02000100)"),
        "{text}"
    );
    let v = json(&gbarom(&d, &[path(&p), "--json"]));
    assert_eq!(v["multiboot_target"], 0x0200_0100);
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

fn code_rom(indirect: bool) -> Vec<u8> {
    let mut rom = synth::rom(&Spec::default());
    let words: [u32; 3] = if indirect {
        [0xE591_0000, 0xE12F_FF10, 0]
    } else {
        [0xE59F_0000, 0xE12F_FF10, 0x0800_0101]
    };
    for (i, w) in words.iter().enumerate() {
        rom[0xC0 + 4 * i..0xC4 + 4 * i].copy_from_slice(&w.to_le_bytes());
    }
    rom[0x100..0x102].copy_from_slice(&0x4770u16.to_le_bytes());
    synth::fix_complement(&mut rom);
    rom
}

#[test]
fn functions_lists_arm_and_thumb_code_as_json() {
    let d = dir("gbarom-functions");
    let p = put(&d, "code.gba", &code_rom(false));
    let v = json(&gbarom(
        &d,
        &["functions", path(&p), "--json", "--instructions"],
    ));
    assert_eq!(v["function_count"], 1);
    let f = &v["functions"][0];
    assert_eq!(f["entry"]["text"], "A:08000000");
    assert_eq!(f["origin"], "entry");
    let insts: Vec<String> = f["instructions"]
        .as_array()
        .expect("instructions")
        .iter()
        .map(|i| {
            format!(
                "{:X}{}",
                i[0].as_u64().unwrap_or(0),
                i[1].as_str().unwrap_or("")
            )
        })
        .collect();
    assert_eq!(insts, ["8000000A", "80000C0A", "80000C4A", "8000100T"]);
    assert_eq!(v["data"][0], serde_json::json!([0x0800_00C8u32, 4]));
    assert_eq!(v["regions"][0]["region"], "rom");
}

#[test]
fn functions_text_and_strict() {
    let d = dir("gbarom-functions-strict");
    let good = put(&d, "good.gba", &code_rom(false));
    let out = gbarom(&d, &["functions", path(&good), "--strict"]);
    assert!(out.status.success());
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(text.contains("1 functions"), "{text}");
    assert!(
        text.contains("rom: 3 ARM and 1 Thumb instructions"),
        "{text}"
    );
    let bad = put(&d, "bad.gba", &code_rom(true));
    assert!(gbarom(&d, &["functions", path(&bad)]).status.success());
    let out = gbarom(&d, &["functions", path(&bad), "--strict"]);
    assert!(!out.status.success());
    assert!(String::from_utf8_lossy(&out.stderr).contains("1 unresolved transfers"));
    let v = json(&gbarom(
        &d,
        &["functions", path(&bad), "--json", "--summary"],
    ));
    assert_eq!(v["unresolved_by_kind"]["indirect-jump"], 1);
    assert_eq!(v["functions"].as_array().map(Vec::len), Some(0));
}

#[test]
fn functions_runs_a_multiboot_image_from_work_ram() {
    let d = dir("gbarom-functions-multiboot");
    let mut mb = synth::multiboot(0x400);
    mb[0x100..0x104].copy_from_slice(&0xE12F_FF1Eu32.to_le_bytes());
    let p = put(&d, "mb.gba", &mb);
    let v = json(&gbarom(
        &d,
        &[
            "functions",
            path(&p),
            "--multiboot",
            "--json",
            "--instructions",
        ],
    ));
    assert_eq!(v["copies"][0]["kind"], "load");
    let f = &v["functions"][0];
    assert_eq!(f["entry"]["text"], "A:020000C0@0");
    assert_eq!(
        f["instructions"],
        serde_json::json!([[0x0200_00C0u32, "A", 0, 4], [0x0200_0100u32, "A", 0, 4]])
    );
    let cart = put(&d, "cart.gba", &synth::rom(&Spec::default()));
    let out = gbarom(&d, &["functions", path(&cart), "--multiboot"]);
    assert_eq!(out.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&out.stderr).contains("no multiboot entry"));
}

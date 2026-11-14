//! Code mods on disk: the Atmosphere and Ryujinx layouts a user mod is read
//! from, and the bundled mods `game.toml` declares.

use std::path::{Path, PathBuf};

use relift::mods::{bundled, user};
use relift_switch_formats::nso::{NsoSpec, build};

fn scratch(name: &str) -> PathBuf {
    let d = Path::new(env!("CARGO_TARGET_TMPDIR"))
        .join("mods-layout")
        .join(name);
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("scratch");
    d
}

fn put(path: &Path, bytes: &[u8]) {
    std::fs::create_dir_all(path.parent().expect("parent")).expect("dir");
    relift_cache::write_if_changed(path, bytes).expect("write");
}

fn nso(fill: u8, id: u8) -> Vec<u8> {
    build(&NsoSpec {
        text: vec![fill; 0x40],
        rodata: vec![0; 16],
        data: vec![0; 16],
        build_id: [id; 32],
        ..NsoSpec::default()
    })
}

fn ips32(at: u32, bytes: &[u8]) -> Vec<u8> {
    let mut p = b"IPS32".to_vec();
    p.extend_from_slice(&(at + 0x100).to_be_bytes());
    p.extend_from_slice(&(bytes.len() as u16).to_be_bytes());
    p.extend_from_slice(bytes);
    p.extend_from_slice(b"EEOF");
    p
}

#[test]
fn atmosphere_and_ryujinx_layouts_are_read() {
    let d = scratch("layouts");
    let id = "ab".repeat(32);
    let atmo = d.join("atmo");
    put(
        &atmo.join(format!("exefs_patches/fast/{id}.ips")),
        &ips32(4, &[1]),
    );
    put(
        &atmo.join("contents/0100000000001000/exefs/main"),
        &nso(7, 0xcd),
    );
    put(&atmo.join("contents/0100000000001000/romfs/a.txt"), b"x");
    put(
        &atmo.join("contents/0100000000002000/exefs/main"),
        &nso(7, 0xcd),
    );
    let m = user::read(&atmo, "0100000000001000").expect("reads");
    assert_eq!(m.patches.len(), 1);
    assert_eq!(m.replacements.len(), 1);
    assert_eq!(m.hash.len(), 64);
    let ryu = d.join("ryu");
    put(
        &ryu.join("exefs/x.pchtxt"),
        format!("@nsobid-{id}\n@flag offset_shift 0x100\n00000008 0102\n").as_bytes(),
    );
    let m = user::read(&ryu, "").expect("reads");
    assert_eq!(m.patches[0].kind, "pchtxt");
    let empty = d.join("empty");
    put(&empty.join("romfs/a"), b"x");
    assert!(user::read(&empty, "").is_err());
    let ups = d.join("hack.ups");
    put(&ups, b"UPS1");
    let e = user::read(&ups, "").expect_err("a ROM patch").to_string();
    assert!(e.contains("ROM patch"), "{e}");
}

#[test]
fn game_toml_declares_bundled_mods_in_order() {
    let d = scratch("bundled-toml");
    put(
        &d.join("p").join(format!("{}.ips", "ab".repeat(20))),
        &ips32(0, &[7]),
    );
    let toml: toml::Table = "[[mods.bundled]]\nname = \"a\"\npatch = \"p\"\ndefault = true\n[[mods.bundled]]\nname = \"b\"\npatch = \"p\"\nafter = [\"a\"]\nexclusive = [\"a\"]\n"
        .parse()
        .expect("toml");
    let l = bundled::read(&toml, &d, "").expect("reads");
    assert_eq!(l.len(), 2);
    assert!(l[0].default && !l[1].default);
    assert_eq!(l[1].key(), "mod.b");
    assert_eq!(bundled::exclusive_pairs(&l), [(0, 1)]);
    let bad: toml::Table = "[[mods.bundled]]\nname = \"a\"\npatch = \"p\"\nafter = [\"b\"]\n"
        .parse()
        .expect("toml");
    assert!(bundled::read(&bad, &d, "").is_err());
    let ov: toml::Table = "[[mods.bundled]]\nname = \"a\"\noverride = \"x\"\n"
        .parse()
        .expect("toml");
    assert!(bundled::read(&ov, &d, "").is_err());
}

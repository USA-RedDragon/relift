//! `import::import` on synthetic decrypted dumps: the extracted tree (bare,
//! base only, base plus updates, DLC) and the NUP, the module table, module
//! identity from .rodata, write-if-changed on a second run, and the error
//! paths: a corrupted NUP, a missing exefs, unknown versions and encrypted
//! inputs.

use std::path::{Path, PathBuf};

use relift_switch_formats::Error;
use relift_switch_formats::import::{Event, Options, Report, import, import_with};
use relift_switch_formats::nso::Nso;
use relift_switch_formats::nup::UpdateSelect;
use relift_switch_formats::synth::{self, PROGRAM_ID};

fn scratch(name: &str) -> PathBuf {
    let d = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("scratch dir");
    d
}

fn put(dir: &Path, name: &str, bytes: &[u8]) -> PathBuf {
    let p = dir.join(name);
    relift_cache::write_if_changed(&p, bytes).expect("fixture writes");
    p
}

fn tree(name: &str) -> (PathBuf, synth::Game) {
    let d = scratch(name);
    let g = synth::game();
    g.write_tree(&d.join("dump")).expect("the tree writes");
    (d, g)
}

fn select(update: UpdateSelect) -> Options {
    Options { update }
}

fn read(p: PathBuf) -> Vec<u8> {
    std::fs::read(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))
}

/// The imported romfs equals `files`, file for file.
fn assert_romfs(out: &Path, r: &Report, files: &[(String, Vec<u8>)]) {
    for (path, bytes) in files {
        assert_eq!(&read(out.join("romfs").join(path)), bytes, "{path}");
    }
    assert_eq!(r.romfs_files, files.len());
}

fn main_build_id(out: &Path, r: &Report) -> [u8; 32] {
    let m = r.modules.iter().find(|m| m.slot == "main").expect("main");
    Nso::parse(&read(out.join(&m.path)))
        .expect("uncompressed NSO parses")
        .build_id
}

#[test]
fn tree_takes_the_newest_update_by_default() {
    let (d, g) = tree("import-tree-newest");
    let out = d.join("out");
    let r = import(&d.join("dump"), &out, &Options::default()).expect("import");
    assert_eq!(r.program_id, PROGRAM_ID);
    assert_eq!(r.version, 0x20000);
    let names: Vec<_> = r
        .modules
        .iter()
        .map(|m| (m.slot.as_str(), m.name.as_str()))
        .collect();
    assert_eq!(names, [("main", "game-main"), ("sdk", "nnSdk")]);
    assert_eq!(r.modules[0].build_id, "c1".repeat(32));
    assert_eq!(main_build_id(&out, &r), [0xC1; 32]);
    let table = std::fs::read_to_string(out.join("modules.toml")).expect("table");
    assert!(table.contains("name = \"game-main\"") && table.contains(&r.modules[0].sha256));
    assert_eq!(
        r.npdm.as_ref().expect("npdm").main_thread_stack_size,
        0x10_0000
    );
    assert_romfs(&out, &r, &g.updates[1].1.romfs);
    assert_eq!(r.dlc, ["dlc/0100123456781001/0"]);
    assert_eq!(
        read(out.join("dlc/0100123456781001/0/romfs/dlc.txt")),
        b"add-on"
    );
    let again = import(&d.join("dump"), &out, &Options::default()).expect("second import");
    assert_eq!(again.written, 0, "an unchanged re-import rewrites nothing");
}

#[test]
fn tree_update_flag_picks_an_older_update_or_the_base() {
    let (d, g) = tree("import-tree-pick");
    let out = d.join("older");
    let r = import(
        &d.join("dump"),
        &out,
        &select(UpdateSelect::Version(0x10000)),
    )
    .expect("older update");
    assert_eq!(r.version, 0x10000);
    assert_eq!(main_build_id(&out, &r), [0xB1; 32]);
    assert_romfs(&out, &r, &g.updates[0].1.romfs);
    assert!(!out.join("romfs/data/new/added.bin").exists());

    let out = d.join("base");
    let r = import(&d.join("dump"), &out, &select(UpdateSelect::None)).expect("base");
    assert_eq!(r.version, 0);
    assert_eq!(main_build_id(&out, &r), [0xA1; 32]);
    assert_romfs(&out, &r, &g.base_spec.romfs);

    let err = import(
        &d.join("dump"),
        &d.join("o"),
        &select(UpdateSelect::Version(7)),
    )
    .expect_err("no such update");
    assert!(
        err.to_string()
            .contains("no update 7 (updates: 65536, 131072)"),
        "{err}"
    );
}

#[test]
fn tree_base_only_and_bare_content_dir() {
    let d = scratch("import-tree-base");
    let g = synth::game();
    let base_only = synth::Game {
        updates: vec![],
        dlc: vec![],
        ..g.clone()
    };
    base_only.write_tree(&d.join("dump")).expect("tree");
    let out = d.join("out");
    let r = import(&d.join("dump"), &out, &Options::default()).expect("base only");
    assert_eq!((r.version, r.program_id), (0, PROGRAM_ID));
    assert_romfs(&out, &r, &g.base_spec.romfs);
    assert!(r.dlc.is_empty());

    let bare = d.join("dump/base");
    let out = d.join("bare");
    let r = import(&bare, &out, &Options::default()).expect("bare content dir");
    assert_eq!(r.version, 0);
    assert_eq!(main_build_id(&out, &r), [0xA1; 32]);
    assert_romfs(&out, &r, &g.base_spec.romfs);
}

#[test]
fn an_update_without_exefs_keeps_the_base_exefs() {
    let (d, g) = tree("import-tree-inherit");
    std::fs::remove_dir_all(d.join("dump/updates/131072/exefs")).expect("rm exefs");
    let out = d.join("out");
    let r = import(&d.join("dump"), &out, &Options::default()).expect("import");
    assert_eq!(r.version, 0x20000);
    assert_eq!(main_build_id(&out, &r), [0xA1; 32]);
    assert_romfs(&out, &r, &g.updates[1].1.romfs);
}

#[test]
fn missing_exefs_is_reported() {
    let (d, _) = tree("import-tree-no-exefs");
    for dir in [
        "dump/base/exefs",
        "dump/updates/65536/exefs",
        "dump/updates/131072/exefs",
    ] {
        std::fs::remove_dir_all(d.join(dir)).expect("rm exefs");
    }
    let err = import(&d.join("dump"), &d.join("o"), &Options::default()).expect_err("fails");
    assert!(
        matches!(err, Error::Format(_)) && err.to_string().contains("no exefs/"),
        "{err}"
    );
    let empty = d.join("empty");
    std::fs::create_dir_all(&empty).expect("dir");
    let err = import(&empty, &d.join("o"), &Options::default()).expect_err("fails");
    assert!(err.to_string().contains("not a dump directory"), "{err}");
}

#[test]
fn a_non_decimal_update_dir_is_reported() {
    let (d, _) = tree("import-tree-bad-version");
    std::fs::rename(d.join("dump/updates/65536"), d.join("dump/updates/v1.0.1")).expect("rename");
    let err = import(&d.join("dump"), &d.join("o"), &Options::default()).expect_err("fails");
    assert!(
        err.to_string().contains("not a decimal update version"),
        "{err}"
    );
}

#[test]
fn nup_imports_newest_older_base_and_dlc() {
    let d = scratch("import-nup");
    let g = synth::game();
    let file = put(&d, "game.nup", &g.nup());
    let out = d.join("out");
    let r = import(&file, &out, &Options::default()).expect("import");
    assert_eq!((r.version, r.program_id), (0x20000, PROGRAM_ID));
    assert_eq!(r.modules[0].name, "game-main");
    assert_eq!(main_build_id(&out, &r), [0xC1; 32]);
    assert_romfs(&out, &r, &g.updates[1].1.romfs);
    assert_eq!(r.dlc, ["dlc/0100123456781001/0"]);
    assert_eq!(
        read(out.join("dlc/0100123456781001/0/romfs/dlc.txt")),
        b"add-on"
    );

    let out = d.join("older");
    let r = import(&file, &out, &select(UpdateSelect::Version(0x10000))).expect("older");
    assert_eq!(main_build_id(&out, &r), [0xB1; 32]);
    assert_romfs(&out, &r, &g.updates[0].1.romfs);

    let out = d.join("base");
    let r = import(&file, &out, &select(UpdateSelect::None)).expect("base");
    assert_eq!(r.version, 0);
    assert_romfs(&out, &r, &g.base_spec.romfs);

    assert!(import(&file, &d.join("o"), &select(UpdateSelect::Version(0x30000))).is_err());
}

#[test]
fn a_corrupted_nup_file_is_a_hash_error() {
    let d = scratch("import-nup-corrupt");
    let g = synth::game();
    let mut bytes = g.nup();
    let at = bytes
        .windows(9)
        .position(|w| w == b"version 2")
        .expect("the raw readme is in the blob");
    bytes[at + 8] ^= 1;
    let file = put(&d, "bad.nup", &bytes);
    let out = d.join("out");
    let err = import(&file, &out, &Options::default()).expect_err("fails");
    assert!(matches!(err, Error::Hash(_)), "{err}");
    assert!(
        !out.join("romfs/readme.txt").exists(),
        "a file that failed its hash is not left behind"
    );
}

#[test]
fn encrypted_inputs_are_refused() {
    let d = scratch("import-encrypted");
    let mut nsp = vec![0u8; 0x400];
    nsp[..4].copy_from_slice(b"PFS0");
    let mut xci = vec![0u8; 0x400];
    xci[0x100..0x104].copy_from_slice(b"HEAD");
    let mut nca3 = vec![0u8; 0x400];
    nca3[0x200..0x204].copy_from_slice(b"NCA3");
    let mut nca0 = vec![0u8; 0x400];
    nca0[0x200..0x204].copy_from_slice(b"NCA0");
    let noise: Vec<u8> = (0..0x400u32).map(|i| (i * 37 % 251) as u8).collect();
    for (name, bytes) in [
        ("game.nsp", &nsp),
        ("renamed.bin", &nsp),
        ("card.xci", &xci),
        ("card.dat", &xci),
        ("game.nsz", &noise),
        ("card.xcz", &noise),
        ("program.nca", &noise),
        ("program.ncz", &noise),
        ("PROGRAM.NCA", &noise),
        ("renamed.dat", &nca3),
        ("old.bin", &nca0),
    ] {
        let file = put(&d, name, bytes);
        let err = import(&file, &d.join("o"), &Options::default()).expect_err(name);
        assert!(matches!(err, Error::Encrypted(_)), "{name}: {err}");
        let msg = err.to_string();
        assert!(
            msg.contains(name)
                && msg.contains("decrypted dumps only")
                && msg.contains("separate tool"),
            "{msg}"
        );
    }
    assert!(
        !d.join("o").exists(),
        "nothing is written for a refused input"
    );
    let junk = put(&d, "junk.bin", &noise);
    let err = import(&junk, &d.join("o"), &Options::default()).expect_err("unknown");
    assert!(
        matches!(err, Error::Format(_)) && err.to_string().contains("not a .nup file"),
        "{err}"
    );
}

/// An NCA whose header is still encrypted shows no magic, whatever its name:
/// it is not read as anything, nothing is written, and the message tells the
/// user that relift takes decrypted dumps only.
#[test]
fn an_encrypted_header_is_refused_without_a_key() {
    let d = scratch("import-encrypted-header");
    let noise: Vec<u8> = (0..0x4000u32).map(|i| (i * 37 % 251) as u8).collect();
    for name in ["blob.bin", "0123456789abcdef.cnmt", "noext"] {
        let file = put(&d, name, &noise);
        let err = import(&file, &d.join("o"), &Options::default()).expect_err(name);
        let msg = err.to_string();
        assert!(
            msg.contains("decrypted dumps only") && msg.contains("separate tool"),
            "{name}: {msg}"
        );
    }
    assert!(!d.join("o").exists(), "nothing is written");
}

/// Each stage's announced totals equal the bytes and files it reported.
fn check_events(file: &Path, out: &Path) -> Vec<String> {
    type Totals = (Option<u64>, Option<u64>, u64, u64);
    let mut stages: Vec<(String, Totals)> = Vec::new();
    let r = import_with(file, out, &Options::default(), &mut |e| match e {
        Event::Begin {
            stage,
            files,
            bytes,
        } => stages.push((stage.to_string(), (files, bytes, 0, 0))),
        Event::Chunk(n) => stages.last_mut().expect("a stage is open").1.3 += n,
        Event::File => stages.last_mut().expect("a stage is open").1.2 += 1,
        Event::End => {}
    })
    .expect("import");
    assert!(!r.modules.is_empty());
    for (name, (files, bytes, got_files, got_bytes)) in &stages {
        assert_eq!(*files, Some(*got_files), "{name} files");
        assert_eq!(*bytes, Some(*got_bytes), "{name} bytes");
    }
    stages.into_iter().map(|(n, _)| n).collect()
}

#[test]
fn progress_events_match_the_totals_for_a_tree_and_a_nup() {
    let (d, g) = tree("import-events-tree");
    let names = check_events(&d.join("dump"), &d.join("out"));
    assert_eq!(names, ["exefs", "romfs", "dlc/0100123456781001/0"]);
    let file = put(&d, "game.nup", &g.nup());
    let names = check_events(&file, &d.join("out-nup"));
    assert_eq!(names, ["exefs", "romfs", "dlc/0100123456781001/0"]);
}

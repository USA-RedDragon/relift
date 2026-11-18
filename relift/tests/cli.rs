//! The binary as a user runs it: help and version succeed, every stub
//! subcommand says it is not implemented and fails, build and run without a
//! project say what is missing, global settings flags
//! work on either side of a subcommand, and `import` extracts synthetic
//! decrypted dumps and refuses encrypted ones.

use std::process::Output;

fn relift(args: &[&str]) -> Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift"))
        .args(args)
        .env_remove("RELIFT_JOBS")
        .current_dir(env!("CARGO_TARGET_TMPDIR"))
        .output()
        .expect("the relift binary runs")
}

fn stderr(out: &Output) -> String {
    String::from_utf8_lossy(&out.stderr).into_owned()
}

#[test]
fn help_and_version_succeed() {
    for args in [&["--help"][..], &["--version"], &["build", "--help"]] {
        let out = relift(args);
        assert!(
            out.status.success(),
            "relift {args:?} failed -- {}",
            stderr(&out)
        );
    }
}

#[test]
fn every_stub_subcommand_fails_as_not_implemented() {
    for (args, name) in [
        (&["check", "0x7100"][..], "check"),
        (&["where", "0x7100"], "where"),
    ] {
        let out = relift(args);
        assert!(!out.status.success(), "relift {args:?} succeeded");
        let err = stderr(&out);
        assert!(
            err.starts_with(&format!("relift {name}: not implemented yet; ")),
            "relift {args:?}: {err}"
        );
    }
    let out = relift(&["where", "0x7100"]);
    assert!(
        stderr(&out).contains("`relift coverage`"),
        "{}",
        stderr(&out)
    );
    let out = relift(&["check", "0x7100"]);
    assert!(
        stderr(&out).contains("`relift diff <function>`"),
        "{}",
        stderr(&out)
    );
}

#[test]
fn report_on_a_binary_file_says_it_is_not_a_stop_report() {
    let dir = std::path::PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("cli-report-binary");
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    let file = dir.join("junk.bin");
    relift_cache::write_if_changed(&file, &[0xff, 0xfe, 0x00, 0x80, 0x81])
        .expect("the file writes");
    let out = relift(&["report", &file.display().to_string()]);
    assert!(!out.status.success(), "relift report succeeded");
    let err = stderr(&out);
    assert!(err.contains("not a stop report"), "{err}");
    assert!(!err.contains("UTF-8"), "{err}");
}

#[test]
fn a_toolchain_setting_that_names_no_tool_is_refused_before_the_build() {
    for (args, name) in [
        (
            &["--toolchain.clang", "/nonexistent/clang", "build"][..],
            "build",
        ),
        (&["--toolchain.ninja", "no-such-ninja-9", "run"], "run"),
    ] {
        let out = relift(args);
        assert!(!out.status.success(), "relift {args:?} succeeded");
        let err = stderr(&out);
        assert!(
            err.starts_with(&format!("relift {name}: toolchain.")),
            "relift {args:?}: {err}"
        );
        assert!(err.contains("clear it to search PATH"), "{err}");
    }
    let err = stderr(&relift(&[
        "--toolchain.clang",
        "/nonexistent/clang",
        "build",
    ]));
    assert!(err.contains("/nonexistent/clang is not a file"), "{err}");
    let err = stderr(&relift(&["--toolchain.ninja", "no-such-ninja-9", "run"]));
    assert!(err.contains("no-such-ninja-9 is not on PATH"), "{err}");
}

#[test]
fn build_and_run_without_a_project_say_what_is_missing() {
    for (args, name) in [
        (&["build"][..], "build"),
        (&["run", "--trace-os", "--strict"], "run"),
    ] {
        let out = relift(args);
        assert!(!out.status.success(), "relift {args:?} succeeded");
        let err = stderr(&out);
        assert!(
            err.starts_with(&format!("relift {name}: game.toml")),
            "relift {args:?} -- {err}"
        );
        assert!(
            err.contains("run `relift import <dump>` here first"),
            "{err}"
        );
        assert_eq!(err.lines().count(), 1, "{err}");
    }
    let err = stderr(&relift(&["build", "--target", "a", "--target", "b"]));
    assert!(
        err.contains("target a: only one target builds at a time"),
        "{err}"
    );
}

#[test]
fn settings_flags_work_before_and_after_a_subcommand() {
    for args in [
        &["-j", "2", "build"][..],
        &["build", "--jobs", "2", "--target", "x"],
    ] {
        let err = stderr(&relift(args));
        assert!(
            err.starts_with("relift build: "),
            "relift {args:?} did not reach the subcommand -- {err}"
        );
    }
}

#[test]
fn relift_toml_in_the_working_dir_is_read() {
    let dir = std::path::PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("relift-toml");
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    relift_cache::write_if_changed(&dir.join("relift.toml"), b"jobs = \"from-file\"\n")
        .expect("the config file is writable");
    let out = relift_proc::command(env!("CARGO_BIN_EXE_relift"))
        .args(["check", "x"])
        .current_dir(&dir)
        .output()
        .expect("the relift binary runs");
    assert!(
        !out.status.success() && stderr(&out).contains("jobs"),
        "{}",
        stderr(&out)
    );
}

#[test]
fn the_example_config_loads() {
    let example =
        std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../relift.example.toml");
    let dir = std::path::PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("relift-example");
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    relift_cache::write_if_changed(
        &dir.join("relift.toml"),
        &std::fs::read(&example).expect("relift.example.toml reads"),
    )
    .expect("the config file is writable");
    let out = relift_proc::command(env!("CARGO_BIN_EXE_relift"))
        .args(["check", "x"])
        .current_dir(&dir)
        .output()
        .expect("the relift binary runs");
    assert!(
        stderr(&out).starts_with("relift check: not implemented yet; "),
        "{}",
        stderr(&out)
    );
}

fn import_in(dir: &std::path::Path, args: &[&str]) -> Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift"))
        .args(args)
        .env_remove("RELIFT_CACHE_DIR")
        .current_dir(dir)
        .output()
        .expect("the relift binary runs")
}

/// The bytes of `path` in the romfs imported into folder `dir`.
fn romfs_file(dir: &std::path::Path, path: &str) -> Vec<u8> {
    use relift_switch_formats::{romfs::Romfs, romfs_image};
    let found = romfs_image::find(dir).expect("an imported romfs");
    let r = Romfs::open(romfs_image::open(found.path()).expect("it opens")).expect("it parses");
    let f = r
        .files()
        .iter()
        .find(|f| f.path == path)
        .expect("the file is in the romfs");
    let mut out = Vec::new();
    r.copy_file(f, &mut out).expect("it reads");
    out
}

fn scratch(name: &str) -> std::path::PathBuf {
    let root = std::path::PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(root.join("game")).expect("the project dir is creatable");
    root
}

#[test]
fn import_reads_an_extracted_tree_and_picks_the_update() {
    let root = scratch("cli-import-tree");
    let project = root.join("game");
    let g = relift_switch_formats::synth::game();
    g.write_tree(&root.join("dump")).expect("the tree writes");
    let dump = root.join("dump").display().to_string();
    let cache = root.join("cache").display().to_string();

    let out = import_in(&project, &["--cache-dir", &cache, "import", &dump]);
    assert!(out.status.success(), "{}", stderr(&out));
    let import = root.join("cache/projects/game/import");
    assert_eq!(romfs_file(&import, "readme.txt"), b"version 2");
    assert!(import.join("romfs.img").is_file() && !import.join("romfs").exists());
    assert_eq!(
        romfs_file(&import.join("dlc/0100123456781001/0"), "dlc.txt"),
        b"add-on"
    );
    let table = std::fs::read_to_string(import.join("modules.toml")).expect("module table");
    assert!(table.contains("name = \"nnSdk\""), "{table}");
    let game = std::fs::read_to_string(project.join("game.toml")).expect("game.toml stub");
    assert!(game.contains("title_id = \"0100123456780000\""), "{game}");
    assert!(game.contains("version = 131072"), "{game}");

    let older = root.join("cache/older");
    let older_s = older.display().to_string();
    let out = import_in(
        &project,
        &[
            "--cache-dir",
            &older_s,
            "import",
            &dump,
            "--update",
            "65536",
        ],
    );
    assert!(out.status.success(), "{}", stderr(&out));
    assert_eq!(
        romfs_file(&older.join("projects/game/import"), "readme.txt"),
        b"version 1"
    );

    let out = import_in(
        &project,
        &["--cache-dir", &cache, "import", &dump, "--update", "3"],
    );
    assert!(!out.status.success());
    let err = stderr(&out);
    assert!(
        err.starts_with("relift import: ") && err.contains("no update 3"),
        "{err}"
    );
}

#[test]
fn import_reads_a_nup_and_picks_the_base() {
    let root = scratch("cli-import-nup");
    let project = root.join("game");
    let g = relift_switch_formats::synth::game();
    let file = root.join("game.nup");
    relift_cache::write_if_changed(&file, &g.nup()).expect("the fixture is writable");
    let file = file.display().to_string();
    let cache = root.join("cache").display().to_string();
    let out = import_in(
        &project,
        &["--cache-dir", &cache, "import", "--update", "base", &file],
    );
    assert!(out.status.success(), "{}", stderr(&out));
    let import = root.join("cache/projects/game/import");
    assert_eq!(romfs_file(&import, "readme.txt"), b"version 0");
    assert!(import.join("romfs.map").is_file() && !import.join("romfs").exists());
    let game = std::fs::read_to_string(project.join("game.toml")).expect("game.toml stub");
    assert!(game.contains("version = 0"), "{game}");
}

#[test]
fn import_refuses_encrypted_dumps_pointing_to_a_separate_tool() {
    let root = scratch("cli-import-encrypted");
    let project = root.join("game");
    let mut nsp = vec![0u8; 0x400];
    nsp[..4].copy_from_slice(b"PFS0");
    let mut plain_header = vec![0u8; 0x400];
    plain_header[0x200..0x204].copy_from_slice(b"NCA3");
    let noise: Vec<u8> = (0..0x400u32).map(|i| (i * 37 % 251) as u8).collect();
    let cache = root.join("cache").display().to_string();
    for (name, bytes) in [
        ("game.nsp", &nsp),
        ("game.nsz", &noise),
        ("card.xci", &noise),
        ("card.xcz", &noise),
        ("program.nca", &noise),
        ("program.ncz", &noise),
        ("renamed.dat", &nsp),
        ("renamed.bin", &plain_header),
    ] {
        let file = root.join(name);
        relift_cache::write_if_changed(&file, bytes).expect("the fixture is writable");
        let file = file.display().to_string();
        let out = import_in(&project, &["--cache-dir", &cache, "import", &file]);
        assert!(!out.status.success());
        let err = stderr(&out);
        assert_eq!(err.trim_end().lines().count(), 1, "{err}");
        assert!(
            err.starts_with(&format!(
                "relift import: {file}: relift reads decrypted dumps only"
            )) && err.contains("convert an encrypted dump with a separate tool first"),
            "{err}"
        );
    }
    let keys_file = ["prod", ".keys"].concat();
    let out = import_in(&project, &["--keys", &keys_file, "import", "x"]);
    assert!(!out.status.success(), "the keys setting is gone");
}

#[test]
fn a_relative_cache_dir_is_taken_from_the_working_directory() {
    let root = scratch("cli-relative-cache");
    let project = root.join("game");
    let rom = root.join("synth.gba");
    let image = relift_gba_formats::synth::rom(&relift_gba_formats::synth::Spec::default());
    relift_cache::write_if_changed(&rom, &image).expect("the ROM is writable");
    let rom = rom.display().to_string();
    let out = import_in(&project, &["--cache-dir", "rel-cache", "import", &rom]);
    assert!(out.status.success(), "{}", stderr(&out));
    let game = std::fs::read_to_string(project.join("game.toml")).expect("game.toml");
    let import = project.join("rel-cache/projects/game/import");
    assert!(import.join("rom.gba").is_file(), "{game}");
    let want = format!("import_dir = {:?}", import.display().to_string());
    assert!(game.lines().any(|l| l == want), "{game}");
}

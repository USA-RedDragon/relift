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
        assert_eq!(
            stderr(&out).trim(),
            format!("relift {name}: not implemented"),
            "relift {args:?}"
        );
    }
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
    }
    let err = stderr(&relift(&["build", "--target", "a", "--target", "b"]));
    assert!(
        err.contains("target a: only the host target builds so far"),
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
    assert_eq!(stderr(&out).trim(), "relift check: not implemented");
}

fn import_in(dir: &std::path::Path, args: &[&str]) -> Output {
    relift_proc::command(env!("CARGO_BIN_EXE_relift"))
        .args(args)
        .env_remove("RELIFT_CACHE_DIR")
        .current_dir(dir)
        .output()
        .expect("the relift binary runs")
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
    assert_eq!(
        std::fs::read(import.join("romfs/readme.txt")).expect("the romfs is extracted"),
        b"version 2"
    );
    assert_eq!(
        std::fs::read(import.join("dlc/0100123456781001/0/romfs/dlc.txt")).expect("the DLC"),
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
        std::fs::read(older.join("projects/game/import/romfs/readme.txt")).expect("romfs"),
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
    assert_eq!(
        std::fs::read(import.join("romfs/readme.txt")).expect("the romfs is extracted"),
        b"version 0"
    );
    let game = std::fs::read_to_string(project.join("game.toml")).expect("game.toml stub");
    assert!(game.contains("version = 0"), "{game}");
}

#[test]
fn import_refuses_encrypted_dumps_pointing_to_a_separate_tool() {
    let root = scratch("cli-import-encrypted");
    let project = root.join("game");
    let mut nsp = vec![0u8; 0x400];
    nsp[..4].copy_from_slice(b"PFS0");
    let mut nca = vec![0u8; 0x400];
    nca[0x200..0x204].copy_from_slice(b"NCA3");
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
        ("renamed.bin", &nca),
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
    let out = import_in(&project, &["--keys", "prod.keys", "import", "x"]);
    assert!(!out.status.success(), "the keys setting is gone");
}

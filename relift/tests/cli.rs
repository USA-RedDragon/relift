//! The binary as a user runs it: help and version succeed, every stub
//! subcommand says it is not implemented and fails, and global settings
//! flags work on either side of a subcommand.

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
        (&["import", "dump.nsp"][..], "import"),
        (&["build"], "build"),
        (&["run", "--target", "linux-x64"], "run"),
        (&["check"], "check"),
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
fn settings_flags_work_before_and_after_a_subcommand() {
    for args in [
        &["-j", "2", "build"][..],
        &["build", "--jobs", "2", "--target", "x"],
    ] {
        let err = stderr(&relift(args));
        assert!(
            err.contains("not implemented"),
            "relift {args:?} did not reach the subcommand -- {err}"
        );
    }
}

#[test]
fn an_unpinned_cross_image_is_refused() {
    let out = relift(&["--cross-image", "example.invalid/image:latest", "check"]);
    assert!(!out.status.success());
    assert!(
        stderr(&out).contains("not pinned by digest"),
        "{}",
        stderr(&out)
    );
}

#[test]
fn relift_toml_in_the_working_dir_is_read() {
    let dir = std::path::PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("relift-toml");
    std::fs::create_dir_all(&dir).expect("the scratch dir is creatable");
    relift_cache::write_if_changed(
        &dir.join("relift.toml"),
        b"cross-image = \"example.invalid/from-file:latest\"\n",
    )
    .expect("the config file is writable");
    let out = relift_proc::command(env!("CARGO_BIN_EXE_relift"))
        .arg("check")
        .current_dir(&dir)
        .output()
        .expect("the relift binary runs");
    assert!(
        stderr(&out).contains("from-file") && stderr(&out).contains("not pinned by digest"),
        "{}",
        stderr(&out)
    );
}

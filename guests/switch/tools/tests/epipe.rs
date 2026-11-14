//! A reader that closes stdout early (`relift-nso symbols x | head`) is a
//! normal end: the tools exit 0 and print nothing on stderr.

mod common;

use std::path::Path;
use std::process::Stdio;

use common::*;

/// Run `exe` with `args` and stdout a pipe whose read end is closed before
/// the tool writes; return its exit code and stderr.
fn closed_stdout(exe: &str, dir: &Path, args: &[&str]) -> (Option<i32>, String) {
    let mut child = relift_proc::command(exe)
        .args(args)
        .current_dir(dir)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("the tool starts");
    drop(child.stdout.take());
    let out = child.wait_with_output().expect("the tool finishes");
    (
        out.status.code(),
        String::from_utf8_lossy(&out.stderr).into_owned(),
    )
}

#[test]
fn closed_stdout_exits_zero_quietly() {
    let f = fixture("epipe");
    let nso = put(&f.dir, "main.nso", &f.game.base_spec.exefs[0].1);
    let (img, nso) = (path(&f.romfs), path(&nso));
    let cases: [(&str, &[&str]); 6] = [
        (env!("CARGO_BIN_EXE_relift-romfs"), &["ls", img]),
        (env!("CARGO_BIN_EXE_relift-romfs"), &["ls", img, "--json"]),
        (env!("CARGO_BIN_EXE_relift-nso"), &["show", nso]),
        (env!("CARGO_BIN_EXE_relift-nso"), &["symbols", nso]),
        (
            env!("CARGO_BIN_EXE_relift-nso"),
            &["symbols", nso, "--json"],
        ),
        (
            env!("CARGO_BIN_EXE_relift-nso"),
            &["decompress", nso, "-o", "-"],
        ),
    ];
    for (exe, args) in cases {
        let (code, err) = closed_stdout(exe, &f.dir, args);
        assert_eq!(code, Some(0), "{exe} {args:?}: {err}");
        assert_eq!(err, "", "{exe} {args:?}");
    }
}

#[test]
fn other_errors_still_fail() {
    let f = fixture("epipe-missing");
    let (code, err) = closed_stdout(
        env!("CARGO_BIN_EXE_relift-romfs"),
        &f.dir,
        &["ls", "missing.img"],
    );
    assert_eq!(code, Some(1), "{err}");
    assert!(err.starts_with("relift-romfs:"), "{err}");
}

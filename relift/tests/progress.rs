//! Progress output of `relift import` and `relift build` as a user sees it:
//! plain lines when the output is not a terminal (snapshots with the times
//! and paths masked), `-q` and `-v`, and color: `--color`, `NO_COLOR` and
//! `TERM=dumb` on a real pseudo-terminal where `script` exists.

mod tiny;

use std::path::Path;
use std::process::Output;

fn run(dir: &Path, env: &[(&str, &str)], args: &[&str]) -> Output {
    let mut cmd = relift_proc::command(env!("CARGO_BIN_EXE_relift"));
    cmd.args(args)
        .env_remove("RELIFT_JOBS")
        .env_remove("RELIFT_COLOR")
        .env_remove("NO_COLOR")
        .env_remove("CI")
        .env_remove("TERM")
        .env_remove("COLUMNS")
        .env("RELIFT_PACKAGE_APPIMAGE", "never")
        .current_dir(dir);
    for (k, v) in env {
        cmd.env(k, v);
    }
    cmd.output().expect("the relift binary runs")
}

fn stderr(out: &Output) -> String {
    String::from_utf8_lossy(&out.stderr).into_owned()
}

/// Masks what changes between runs: times, the number of files compiled,
/// directories, and the in-progress compile lines (their number depends on
/// timing).
fn mask(text: &str, root: &Path) -> String {
    let root = root.display().to_string();
    let mut out = Vec::new();
    for line in text.lines() {
        if line.contains("%)") {
            continue;
        }
        let line = line.replace(&root, "<root>");
        let mut words: Vec<String> = Vec::new();
        for w in line.split(' ') {
            let w = w.to_string();
            if w == "s" && words.last().is_some_and(|p| p.parse::<f64>().is_ok()) {
                words.pop();
                words.push("<time>".into());
            } else if w.contains('m')
                && w.ends_with('s')
                && w.chars().next().is_some_and(|c| c.is_ascii_digit())
            {
                words.push("<time>".into());
            } else {
                words.push(w);
            }
        }
        let mut line = words.join(" ");
        if line.trim_start().starts_with("compile") {
            line = line
                .split(' ')
                .map(|w| if w.parse::<u32>().is_ok() { "<n>" } else { w })
                .collect::<Vec<_>>()
                .join(" ");
        }
        out.push(line);
    }
    out.join("\n") + "\n"
}

fn sgr(text: &str) -> bool {
    text.split("\x1b[").skip(1).any(|rest| {
        rest.split_once('m')
            .is_some_and(|(p, _)| p.chars().all(|c| c.is_ascii_digit() || c == ';'))
    })
}

fn has_clang_and_ninja() -> bool {
    relift_build::find_tool("clang").is_some() && relift_build::find_tool("ninja").is_some()
}

#[test]
fn import_prints_one_plain_line_per_stage_when_not_a_terminal() {
    let root = tiny::scratch("progress-import");
    let project = tiny::dump(&root);
    let cache = root.join("cache").display().to_string();
    let out = run(&project, &[], &["--cache-dir", &cache, "import", "../dump"]);
    assert!(out.status.success(), "{}", stderr(&out));
    let err = stderr(&out);
    assert!(!err.contains('\x1b') && !err.contains('\r'), "{err:?}");
    let want = "relift import: ../dump\n\
        \x20 exefs extract  2 files, 524 bytes  <time>\n\
        imported title 0100000000001234 v0, 1 module, 0 romfs files, 0 DLC, 4 files written into <root>/cache/projects/tiny/import; wrote game.toml, <time>\n";
    let got = mask(&err, &root);
    assert!(got.starts_with("relift import: ../dump\n  exefs"), "{got}");
    assert_eq!(got, want);
}

#[test]
fn build_prints_plain_lines_that_match_the_snapshot() {
    if !has_clang_and_ninja() {
        eprintln!("skipped: no clang or ninja on PATH");
        return;
    }
    let root = tiny::scratch("progress-build");
    let project = tiny::dump(&root);
    let cache = root.join("cache").display().to_string();
    assert!(
        run(&project, &[], &["--cache-dir", &cache, "import", "../dump"])
            .status
            .success()
    );
    let no_sdl3 = root.join("no-sdl3").display().to_string();
    let no_moltenvk = root.join("no-moltenvk").display().to_string();
    let no_opus = root.join("no-opus").display().to_string();
    let env = [
        ("RELIFT_SDL3_DIR", no_sdl3.as_str()),
        ("RELIFT_MOLTENVK_DIR", no_moltenvk.as_str()),
        ("RELIFT_OPUS_DIR", no_opus.as_str()),
    ];
    let out = run(&project, &env, &["--cache-dir", &cache, "-j", "4", "build"]);
    assert!(out.status.success(), "{}", stderr(&out));
    let err = stderr(&out);
    assert!(!err.contains('\x1b') && !err.contains('\r'), "{err:?}");
    let want = format!(
        "relift build: tiny (1 module, {}-{})\n\
        \x20 main discover 2 functions  <time>\n\
        \x20 main lift     2 functions, 0 unlifted  <time>\n\
        \x20 main emit     1 shard, 0 overrides, 0 of 2 reused  <time>\n\
        \x20 main cold     1 cold shard, 0 superset functions  <time>\n\
        main: 1024 words: 2 hot entries, 8 cold entries, 1014 traps (1014 zero, 0 padding, 0 undecodable)\n\
        window: SDL3 not found, building headless and silent (set RELIFT_SDL3_DIR); \
        opus: libopus not found, Opus streams decode to silence (set RELIFT_OPUS_DIR); \
        vp9: libvpx not found, VP9 video stays undecoded (run runtime/tools/libvpx_src.py); \
        av1: dav1d not found, AV1 video stays undecoded (run runtime/tools/dav1d_src.py)\n\
        \x20      compile  <n> files, clang -O2, -j4  <time>\n\
        built 2 functions, 0 instructions not lifted, <time> in all\n\
        \x20 output: <root>/cache/projects/tiny/build/host/game\n",
        std::env::consts::OS,
        std::env::consts::ARCH
    );
    assert_eq!(mask(&err, &root), want);

    let again = run(&project, &env, &["--cache-dir", &cache, "-j", "4", "build"]);
    assert!(
        mask(&stderr(&again), &root).contains("compile  up to date, clang -O2, -j4  <time>\n"),
        "{}",
        stderr(&again)
    );
}

#[test]
fn quiet_prints_only_the_summary_and_verbose_adds_shards_and_unlifted_lists() {
    if !has_clang_and_ninja() {
        eprintln!("skipped: no clang or ninja on PATH");
        return;
    }
    let root = tiny::scratch("progress-verbosity");
    let project = tiny::dump_with(&root, true);
    let cache = root.join("cache").display().to_string();
    assert!(
        run(&project, &[], &["--cache-dir", &cache, "import", "../dump"])
            .status
            .success()
    );

    let out = run(&project, &[], &["--cache-dir", &cache, "build", "-q"]);
    assert!(out.status.success(), "{}", stderr(&out));
    let quiet = mask(&stderr(&out), &root);
    let lines: Vec<&str> = quiet.lines().collect();
    assert_eq!(lines.len(), 3, "{quiet}");
    assert!(
        lines[0].starts_with("built 2 functions, 1 instructions not lifted, <time>"),
        "{quiet}"
    );
    assert!(
        lines[1].starts_with("  not lifted, with addresses: <root>/"),
        "{quiet}"
    );
    assert!(lines[2].starts_with("  output: <root>/"), "{quiet}");

    let list = root.join("cache/projects/tiny/build/host/unlifted.txt");
    let list = std::fs::read_to_string(list).expect("the unlifted list is written");
    assert!(
        list.starts_with("main\t0x") && list.contains("\t00000000\t"),
        "{list}"
    );

    let out = run(&project, &[], &["--cache-dir", &cache, "-v", "build"]);
    let verbose = stderr(&out);
    assert!(out.status.success(), "{verbose}");
    assert!(
        verbose.contains("  main  0x") && verbose.contains("  00000000  "),
        "{verbose}"
    );
    let quiet_run = run(&project, &[], &["--cache-dir", &cache, "build"]);
    assert!(
        !stderr(&quiet_run).contains("  main  0x"),
        "the list is for -v"
    );
}

#[test]
fn verbose_names_each_compiled_file() {
    if !has_clang_and_ninja() {
        eprintln!("skipped: no clang or ninja on PATH");
        return;
    }
    let root = tiny::scratch("progress-verbose-files");
    let project = tiny::dump(&root);
    let cache = root.join("cache").display().to_string();
    assert!(
        run(&project, &[], &["--cache-dir", &cache, "import", "../dump"])
            .status
            .success()
    );
    let out = run(
        &project,
        &[],
        &["--cache-dir", &cache, "build", "--verbose"],
    );
    let err = stderr(&out);
    assert!(
        err.contains("  cc ") && err.contains("shard0000.c"),
        "{err}"
    );
    assert!(err.contains("  link "), "{err}");
}

#[test]
fn a_failed_build_still_prints_the_error_in_one_place() {
    let root = tiny::scratch("progress-error");
    let project = tiny::dump(&root);
    let cache = root.join("cache").display().to_string();
    let out = run(&project, &[], &["--cache-dir", &cache, "build"]);
    assert!(!out.status.success());
    let err = stderr(&out);
    assert!(err.starts_with("relift build: game.toml"), "{err}");
}

#[test]
fn color_always_paints_and_never_does_not_in_plain_output() {
    let root = tiny::scratch("progress-color-plain");
    let project = tiny::dump(&root);
    let cache = root.join("cache").display().to_string();
    let args = |color: &'static str| {
        [
            "--cache-dir",
            cache.as_str(),
            "--color",
            color,
            "import",
            "../dump",
        ]
        .map(String::from)
    };
    for (color, env, painted) in [
        ("always", &[][..], true),
        ("always", &[("NO_COLOR", "1")][..], true),
        ("never", &[][..], false),
        ("never", &[("TERM", "xterm")][..], false),
        ("auto", &[("NO_COLOR", "1")][..], false),
        ("auto", &[("TERM", "dumb")][..], false),
        ("auto", &[][..], false),
    ] {
        let a = args(color);
        let a: Vec<&str> = a.iter().map(String::as_str).collect();
        let out = run(&project, env, &a);
        assert!(out.status.success(), "{}", stderr(&out));
        let err = stderr(&out);
        assert_eq!(sgr(&err), painted, "--color {color} {env:?}: {err:?}");
        if !painted {
            assert!(!err.contains('\x1b'), "--color {color} {env:?}: {err:?}");
        }
    }
    let bad = run(&project, &[], &["--color", "sometimes", "import", "x"]);
    assert!(
        !bad.status.success() && stderr(&bad).contains("auto, always or never"),
        "{}",
        stderr(&bad)
    );
}

fn quote(s: &str) -> String {
    format!("'{}'", s.replace('\'', "'\\''"))
}

/// Runs relift on a pseudo-terminal through `script`; returns what the
/// terminal showed, or `None` when `script` is missing.
fn on_a_terminal(dir: &Path, env: &[(&str, &str)], args: &[&str]) -> Option<String> {
    let script = relift_build::find_tool("script")?;
    let mut line = quote(env!("CARGO_BIN_EXE_relift"));
    for a in args {
        line.push(' ');
        line.push_str(&quote(a));
    }
    let mut cmd = relift_proc::command(script);
    if cfg!(any(target_os = "macos", target_os = "freebsd")) {
        cmd.args(["-q", "/dev/null", "sh", "-c", &line]);
    } else {
        cmd.args(["-qec", &line, "/dev/null"]);
    }
    cmd.env_remove("NO_COLOR")
        .env_remove("CI")
        .env_remove("RELIFT_COLOR")
        .env("TERM", "xterm-256color")
        .env("COLUMNS", "100")
        .current_dir(dir);
    for (k, v) in env {
        cmd.env(k, v);
    }
    let out = cmd.output().ok()?;
    Some(String::from_utf8_lossy(&out.stdout).into_owned())
}

#[test]
fn a_terminal_gets_color_and_live_lines_unless_told_otherwise() {
    let root = tiny::scratch("progress-pty");
    let project = tiny::dump(&root);
    let cache = root.join("cache").display().to_string();
    let base = ["--cache-dir", cache.as_str()];
    let go = |env: &[(&str, &str)], extra: &[&str]| {
        let mut args: Vec<&str> = base.to_vec();
        args.extend_from_slice(extra);
        args.extend_from_slice(&["import", "../dump"]);
        on_a_terminal(&project, env, &args)
    };
    let Some(plain) = go(&[], &[]) else {
        eprintln!("skipped: no script(1) on PATH");
        return;
    };
    assert!(sgr(&plain), "a terminal gets color: {plain:?}");
    assert!(
        plain.contains("\x1b[1A\r\x1b[J") || plain.contains("exefs"),
        "{plain:?}"
    );

    for (what, env, extra) in [
        ("NO_COLOR=1", &[("NO_COLOR", "1")][..], &[][..]),
        ("--color never", &[][..], &["--color", "never"][..]),
        (
            "--color=never with NO_COLOR unset",
            &[("TERM", "xterm")][..],
            &["--color=never"][..],
        ),
    ] {
        let shown = go(env, extra).expect("script exists");
        assert!(!sgr(&shown), "{what} must not color: {shown:?}");
        assert!(shown.contains("exefs"), "{what}: {shown:?}");
    }

    let dumb = go(&[("TERM", "dumb")], &[]).expect("script exists");
    assert!(
        !dumb.contains("\x1b["),
        "TERM=dumb gets no escape codes at all: {dumb:?}"
    );
    assert!(
        dumb.contains("exefs") && dumb.contains("imported title"),
        "{dumb:?}"
    );

    let forced = go(&[("TERM", "dumb")], &["--color", "always"]).expect("script exists");
    assert!(
        sgr(&forced),
        "--color always overrides TERM=dumb: {forced:?}"
    );
}

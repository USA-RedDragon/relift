//! End to end: missed code. The tiny program calls a function through a
//! computed pointer that discovery cannot see. Without the cold copy
//! (`emit.cold` off), a shipped build (`relift build`) stops at the miss
//! with exit status 70; `relift run` hot-loads the function, runs to the
//! right output and records the address in `game.toml`; the next `relift
//! build` lifts it and runs with no miss. With the cold copy the shipped
//! build runs the function from it, and `relift run` logs the first entry.
//! Skipped with a note when neither clang nor zig is on `PATH`.

mod tiny;

use std::path::Path;

/// The one stop report in `dir`.
fn the_report(dir: &Path) -> String {
    let files: Vec<_> = std::fs::read_dir(dir)
        .map(|d| d.map(|e| e.unwrap().path()).collect())
        .unwrap_or_default();
    assert_eq!(files.len(), 1, "{files:?}");
    std::fs::read_to_string(&files[0]).unwrap()
}

fn game(root: &Path, dev: bool) -> std::process::Output {
    let exe = std::fs::read_dir(root.join("cache").join("projects"))
        .unwrap()
        .map(|e| {
            let host = e.unwrap().path().join("build").join("host");
            if dev { host.join("dev") } else { host }
                .join(format!("game{}", std::env::consts::EXE_SUFFIX))
        })
        .find(|p| p.is_file())
        .expect("a built game");
    let reports = root.join(if dev { "reports-dev" } else { "reports" });
    if reports.exists() {
        std::fs::remove_dir_all(&reports).unwrap();
    }
    relift_proc::command(&exe)
        .env_remove("RELIFT_DEV_SOCKET")
        .env("RELIFT_REPORT_DIR", &reports)
        .current_dir(root.join("tiny"))
        .output()
        .expect("the game runs")
}

/// `toml` without its `[hints]` table.
fn hints_free(toml: &str) -> String {
    toml.split("\n[hints]").next().unwrap_or(toml).to_string() + "\n"
}

#[test]
fn a_missed_call_is_hot_loaded_and_lifted_by_the_next_build() {
    if relift_build::find_tool("clang").is_none() && relift_build::find_tool("zig").is_none() {
        eprintln!("skipped: no clang or zig on PATH");
        return;
    }
    let root = tiny::scratch("e2e-hotload");
    let project = tiny::dump_hidden(&root);
    let cache = root.join("cache").display().to_string();
    let r = |args: &[&str]| {
        let mut a = vec!["--cache-dir", &cache, "-j", "4", "--emit.cold=false"];
        a.extend_from_slice(args);
        tiny::relift(&project, &a)
    };
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let want = "hello from relift\n".repeat(3);

    let out = r(&["build"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let shipped = game(&root, false);
    let err = String::from_utf8_lossy(&shipped.stderr);
    assert_eq!(shipped.status.code(), Some(70), "{err}");
    assert!(
        err.contains(
            "[relift error stop] missed code: no function at guest 0x8000040; report written to "
        ),
        "{err}"
    );
    assert!(shipped.stdout.is_empty());
    let report = the_report(&root.join("reports"));
    for want in [
        "\nkind: missed code\n",
        "\nguest: switch\n",
        "\ntransfer: call\ntarget: 0x8000040 main+0x40\n",
        "\ntarget code: not lifted (outside every lifted function)\n",
        "\ntarget where: main text segment\n",
        "\nsite: 0x800001c main+0x1c\nsite function: 0x8000000 main+0x0 ",
        "\ncaller function: 0x8000000 main+0x0 ",
        "\n  x8  0000000008000040  x9  0000000000000004",
        "\nguest backtrace: lifted frames, innermost first; tail calls leave no frame\n  #0 0x8000000 main+0x0 ",
    ] {
        assert!(report.contains(want), "{want:?} in\n{report}");
    }
    assert!(err.contains(&report), "{err}");
    let reports = root.join("reports");
    let file = std::fs::read_dir(&reports)
        .unwrap()
        .next()
        .unwrap()
        .unwrap()
        .path();
    let file = file.display().to_string();
    let out = r(&["report", &file]);
    let text = String::from_utf8_lossy(&out.stdout);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(
        text.starts_with("missed code: no function at guest 0x8000040\n"),
        "{text}"
    );
    assert!(text.contains("lifted C at the site (c/"), "{text}");
    if relift_build::find_tool("llvm-mc").is_some() {
        assert!(text.contains("=> 0x800001c: blr\tx8\n"), "{text}");
    }
    assert!(text.contains("rc = rl_indirect(c, r8)(c, mem);"), "{text}");
    assert!(
        text.contains("0x8000040 is not among the build's function entries"),
        "{text}"
    );
    assert!(
        text.contains("added main+0x40 to the hints in game.toml; the next relift build lifts it"),
        "{text}"
    );
    let toml = std::fs::read_to_string(project.join("game.toml")).unwrap();
    assert!(toml.contains("\"main+0x40\""), "{toml}");
    relift_cache::write_if_changed(&project.join("game.toml"), hints_free(&toml).as_bytes())
        .unwrap();

    let out = r(&["run"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert_eq!(String::from_utf8_lossy(&out.stdout), want, "{stderr}");
    assert_eq!(
        stderr.matches("hot-loaded 0x8000040 (main+0x40)").count(),
        1,
        "{stderr}"
    );
    let toml = std::fs::read_to_string(project.join("game.toml")).unwrap();
    assert!(toml.contains("\"main+0x40\""), "{toml}");

    let dev = game(&root, true);
    let err = String::from_utf8_lossy(&dev.stderr);
    assert_eq!(dev.status.code(), Some(70), "{err}");
    assert!(err.contains("RELIFT_DEV_SOCKET is not set"), "{err}");

    let out = r(&["build"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(
        stderr.contains("built 2 functions, 0 instructions not lifted"),
        "{stderr}"
    );
    let shipped = game(&root, false);
    assert!(
        shipped.status.success(),
        "{}",
        String::from_utf8_lossy(&shipped.stderr)
    );
    assert_eq!(String::from_utf8_lossy(&shipped.stdout), want);

    let out = r(&["run"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert_eq!(String::from_utf8_lossy(&out.stdout), want);
    assert!(!stderr.contains("hot-loaded"), "{stderr}");
}

#[test]
fn a_missed_call_runs_from_the_cold_copy() {
    if relift_build::find_tool("clang").is_none() && relift_build::find_tool("zig").is_none() {
        eprintln!("skipped: no clang or zig on PATH");
        return;
    }
    let root = tiny::scratch("e2e-cold");
    let project = tiny::dump_hidden(&root);
    let cache = root.join("cache").display().to_string();
    let r = |args: &[&str]| {
        let mut a = vec!["--cache-dir", &cache, "-j", "4"];
        a.extend_from_slice(args);
        tiny::relift(&project, &a)
    };
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let want = "hello from relift\n".repeat(3);
    let out = r(&["build"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert!(
        stderr.contains("1 superset functions from 1 root"),
        "{stderr}"
    );
    let shipped = game(&root, false);
    let err = String::from_utf8_lossy(&shipped.stderr);
    assert!(shipped.status.success(), "{err}");
    assert_eq!(String::from_utf8_lossy(&shipped.stdout), want, "{err}");
    assert!(!err.contains("cold code"), "{err}");

    let out = r(&["run"]);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{stderr}");
    assert_eq!(String::from_utf8_lossy(&out.stdout), want, "{stderr}");
    assert_eq!(
        stderr
            .matches("first entry to cold code at 0x8000040 (main+0x40)")
            .count(),
        1,
        "{stderr}"
    );
    assert!(!stderr.contains("hot-loaded"), "{stderr}");
}

#[test]
fn a_shipped_build_reports_the_site_of_a_computed_jump_that_missed() {
    if relift_build::find_tool("clang").is_none() && relift_build::find_tool("zig").is_none() {
        eprintln!("skipped: no clang or zig on PATH");
        return;
    }
    let root = tiny::scratch("e2e-miss-jump");
    let project = tiny::dump_hidden_jump(&root);
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let out = tiny::relift(
        &project,
        &[
            "--cache-dir",
            &cache,
            "-j",
            "4",
            "--emit.cold=false",
            "build",
        ],
    );
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let shipped = game(&root, false);
    let err = String::from_utf8_lossy(&shipped.stderr);
    assert_eq!(shipped.status.code(), Some(70), "{err}");
    let report = the_report(&root.join("reports"));
    let site = 0x800_0000 + tiny::HIDDEN_SITE;
    for want in [
        "\ntransfer: jump\ntarget: 0x8000040 main+0x40\n".to_string(),
        format!(
            "\nsite: {site:#x} main+{:#x}\nsite function: 0x8000000 main+0x0 ",
            tiny::HIDDEN_SITE
        ),
        format!("\nsite words {:#x}: ", site - 0x10),
    ] {
        assert!(report.contains(&want), "{want:?} in\n{report}");
    }
    assert!(!report.contains("\ncaller function:"), "{report}");
}

//! A game's settings end to end: a misspelled key in an override fails the
//! build, and a program built with settings declared still runs. Skipped
//! with a note when clang is not on `PATH`.

mod tiny;

const SETTINGS: &str =
    "\n[settings.fast_text]\ntype = \"bool\"\nlabel = \"Fast text\"\ndefault = false\n";

fn imported(name: &str) -> (std::path::PathBuf, std::path::PathBuf, String) {
    let root = tiny::scratch(name);
    let project = tiny::dump(&root);
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let toml = project.join("relift.toml");
    let mut text = std::fs::read_to_string(&toml).unwrap();
    text.push_str(SETTINGS);
    relift_cache::write_if_changed(&toml, text.as_bytes()).unwrap();
    (root, project, cache)
}

#[test]
fn a_misspelled_setting_key_fails_the_build() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let (_root, project, cache) = imported("settings-typo");
    std::fs::create_dir_all(project.join("overrides")).unwrap();
    relift_cache::write_if_changed(
        &project.join("overrides/a.c"),
        "#include <relift/override.h>\nint f(void) { return relift_setting_bool(\"fast_txt\"); }\n"
            .as_bytes(),
    )
    .unwrap();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "-j", "4", "build"]);
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(!out.status.success(), "{err}");
    assert!(
        err.contains("overrides/a.c:2") && err.contains("did you mean \"fast_text\""),
        "{err}"
    );
}

#[test]
fn a_program_with_declared_settings_builds_and_runs() {
    if relift_build::find_tool("clang").is_none() {
        eprintln!("skipped: no clang on PATH");
        return;
    }
    let (_root, project, cache) = imported("settings-run");
    std::fs::create_dir_all(project.join("overrides")).unwrap();
    relift_cache::write_if_changed(
        &project.join("overrides/a.c"),
        "#include <relift/override.h>\nint f(void) { return relift_setting_bool(\"fast_text\"); }\n".as_bytes(),
    )
    .unwrap();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "-j", "4", "run"]);
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "{err}");
    assert_eq!(
        String::from_utf8_lossy(&out.stdout),
        "hello from relift\n".repeat(3),
        "{err}"
    );
}

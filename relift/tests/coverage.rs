//! `relift coverage` on the tiny program: the text and JSON reports, and
//! the hints of `relift.toml`.

mod tiny;

use std::path::{Path, PathBuf};

use relift::hints::Hint;

fn imported(name: &str, pointer: bool) -> (PathBuf, String) {
    let root = tiny::scratch(name);
    let project = if pointer {
        tiny::dump_with_pointer(&root)
    } else {
        tiny::dump(&root)
    };
    let cache = root.join("cache").display().to_string();
    let out = tiny::relift(&project, &["--cache-dir", &cache, "import", "../dump"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    (project, cache)
}

fn coverage(project: &Path, cache: &str, args: &[&str]) -> String {
    let mut all = vec!["--cache-dir", cache, "coverage"];
    all.extend_from_slice(args);
    let out = tiny::relift(project, &all);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8(out.stdout).expect("the report is UTF-8")
}

fn report(project: &Path, cache: &str) -> serde_json::Value {
    serde_json::from_str(&coverage(project, cache, &["--json"])).expect("the report is JSON")
}

#[test]
fn the_tiny_program_has_nothing_unresolved() {
    let (project, cache) = imported("coverage-tiny", false);
    let j = report(&project, &cache);
    let m = &j["modules"][0];
    assert_eq!(j["modules"].as_array().map(Vec::len), Some(1));
    assert_eq!(m["name"], "main");
    assert_eq!(m["functions"], 2);
    assert_eq!(m["unlifted"], 0);
    assert_eq!(m["through_slots"], 0);
    assert_eq!(m["unresolved"], serde_json::json!([]));
    assert_eq!(m["hints"], serde_json::json!([]));
    let text = coverage(&project, &cache, &[]);
    let lines: Vec<&str> = text.lines().collect();
    assert_eq!(lines.len(), 2, "{text}");
    assert!(lines[0].starts_with("module  base"), "{text}");
    assert!(lines[1].starts_with("main  "), "{text}");
}

#[test]
fn a_call_through_a_loaded_pointer_is_a_pointer_site() {
    let (project, cache) = imported("coverage-pointer", true);
    let j = report(&project, &cache);
    let m = &j["modules"][0];
    let base = m["base"].as_u64().expect("the base is a number");
    assert_eq!(
        m["unresolved"],
        serde_json::json!([{
            "addr": base + 0x38,
            "offset": 0x38,
            "function": "fn_30",
            "kind": "call",
            "reason": "pointer",
            "source": "loaded",
            "symbol": null,
        }])
    );
    let text = coverage(&project, &cache, &["--module", "main"]);
    let site = format!(
        "  {:#x}  main+0x38  fn_30  call  pointer  loaded",
        base + 0x38
    );
    assert!(text.lines().any(|l| l == site), "{text}");
    assert!(text.contains("main: 1 transfer unresolved"), "{text}");
}

#[test]
fn hints_are_listed_with_whether_they_are_lifted() {
    let (project, cache) = imported("coverage-hints", false);
    let hints = [0x30, 0x400].map(|offset| Hint {
        module: "main".into(),
        offset,
    });
    relift::hints::add(&project.join("relift.toml"), &hints).expect("the hints are written");
    let j = report(&project, &cache);
    let m = &j["modules"][0];
    let base = m["base"].as_u64().expect("the base is a number");
    assert_eq!(
        m["hints"],
        serde_json::json!([
            {"offset": 0x30, "addr": base + 0x30, "lifted": true},
            {"offset": 0x400, "addr": base + 0x400, "lifted": false},
        ])
    );
    let text = coverage(&project, &cache, &[]);
    assert!(text.contains("main: 2 hints"), "{text}");
    assert!(
        text.lines()
            .any(|l| l == format!("  {:#x}  main+0x400  not lifted", base + 0x400)),
        "{text}"
    );
}

#[test]
fn a_module_filter_naming_no_module_fails() {
    let (project, cache) = imported("coverage-filter", false);
    let out = tiny::relift(
        &project,
        &["--cache-dir", &cache, "coverage", "--module", "nope"],
    );
    assert!(!out.status.success());
    let err = String::from_utf8_lossy(&out.stderr);
    assert!(err.contains("no module \"nope\""), "{err}");
}

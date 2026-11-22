//! `build_with` reports real step counts from Ninja and from the direct
//! compiler path, and keeps the end of a failing tool's output. Skipped with
//! a note when clang (or Ninja) is not on `PATH`.

use std::path::{Path, PathBuf};

use relift_build::{Compile, Event, Plan, build_with, find_tool};

fn scratch(name: &str) -> PathBuf {
    let d = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("the scratch dir is creatable");
    d
}

fn plan(dir: &Path, clang: &Path, bodies: &[&str]) -> Plan {
    let mut compiles = Vec::new();
    for (i, body) in bodies.iter().enumerate() {
        let src = dir.join(format!("f{i}.c"));
        relift_cache::write_if_changed(&src, body.as_bytes()).expect("the source writes");
        compiles.push(Compile {
            src,
            obj: dir.join("obj").join(format!("f{i}.o")),
            flags: vec!["-O0".into()],
            pool: None,
            stage: 0,
        });
    }
    Plan {
        cc: vec![clang.display().to_string()],
        asm: None,
        compiles,
        output: dir.join("program"),
        link_flags: Vec::new(),
        pools: Vec::new(),
        root: None,
    }
}

const MAIN: &str = "int main(void) { return 0; }\n";
const FN: &str = "int helper(void) { return 1; }\n";

fn steps(plan: &Plan, dir: &Path, ninja: Option<&Path>) -> (Vec<(usize, usize)>, usize) {
    let mut seen = Vec::new();
    let mut ticks = 0;
    build_with(plan, dir, ninja, 2, &mut |e| match e {
        Event::Step { done, total, .. } => seen.push((done, total)),
        Event::Tick => ticks += 1,
    })
    .expect("the build succeeds");
    (seen, ticks)
}

#[test]
fn ninja_counts_are_ninjas_own() {
    let (Some(clang), Some(ninja)) = (find_tool("clang"), find_tool("ninja")) else {
        eprintln!("skipped: no clang or ninja on PATH");
        return;
    };
    let dir = scratch("steps-ninja");
    let p = plan(
        &dir,
        &clang,
        &[MAIN, FN, FN.replace("helper", "other").as_str()],
    );
    let (seen, _) = steps(&p, &dir, Some(&ninja));
    assert_eq!(seen.last(), Some(&(4, 4)), "{seen:?}");
    assert_eq!(seen.len(), 4);
    assert!(seen.iter().all(|(_, t)| *t == 4));
    let (again, _) = steps(&p, &dir, Some(&ninja));
    assert!(
        again.is_empty(),
        "an up to date build has no steps: {again:?}"
    );
}

#[test]
fn the_direct_path_counts_compiled_objects_and_the_link() {
    let Some(clang) = find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let dir = scratch("steps-direct");
    let p = plan(&dir, &clang, &[MAIN, FN]);
    let (seen, _) = steps(&p, &dir, None);
    assert_eq!(seen, [(1, 3), (2, 3), (3, 3)]);
    let (again, _) = steps(&p, &dir, None);
    assert!(again.is_empty(), "{again:?}");
}

#[test]
fn a_failing_compile_keeps_the_compiler_output() {
    let Some(clang) = find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    for use_ninja in [false, true] {
        let ninja = if use_ninja { find_tool("ninja") } else { None };
        if use_ninja && ninja.is_none() {
            continue;
        }
        let dir = scratch(if use_ninja {
            "fail-ninja"
        } else {
            "fail-direct"
        });
        let p = plan(&dir, &clang, &[MAIN, "int broken(void) { return nope; }\n"]);
        let err = build_with(&p, &dir, ninja.as_deref(), 1, &mut |_| {})
            .expect_err("the build fails")
            .to_string();
        assert!(err.contains("nope"), "{err}");
    }
}

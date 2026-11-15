use std::path::{Path, PathBuf};

use relift::game_settings::{gb_settings, runtime_settings};

const HARNESS: &str = r#"#include <stdio.h>
#include "core.h"
#include <relift/settings.h>
void DECLARE(void);
const rl_guest_desc *rl_core_guest(void) { return NULL; }
int main(void)
{
    static char b[65536];
    DECLARE();
    rl_settings_schema_json(b, sizeof b);
    puts(b);
    return 0;
}
"#;

fn scratch(name: &str) -> PathBuf {
    let d = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../target/tmp")
        .join(name);
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("the scratch dir is creatable");
    d
}

fn declared(
    name: &str,
    declare: &str,
    includes: &[&str],
    sources: &[&str],
) -> Option<Vec<serde_json::Value>> {
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return None;
    };
    let rt = Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime");
    let dir = scratch(name);
    let src = dir.join("harness.c");
    relift_cache::write_if_changed(&src, HARNESS.replace("DECLARE", declare).as_bytes()).unwrap();
    let exe = dir.join(format!("harness{}", std::env::consts::EXE_SUFFIX));
    let mut cmd = relift_proc::command(&cc);
    cmd.args(["-std=c11", "-D_GNU_SOURCE", "-D_DARWIN_C_SOURCE"]);
    if cfg!(target_arch = "x86_64") {
        cmd.arg("-mcx16");
    }
    for inc in ["core/include", "core"].iter().chain(includes) {
        cmd.arg(format!("-I{}", rt.join(inc).display()));
    }
    cmd.arg(&src);
    for f in [
        "core/settings.c",
        "core/log.c",
        "core/ports.c",
        "core/input.c",
        "core/input_script.c",
    ]
    .iter()
    .chain(sources)
    {
        cmd.arg(rt.join(f));
    }
    cmd.arg("-o").arg(&exe).arg("-lpthread");
    let out = cmd.output().expect("clang runs");
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let run = relift_proc::command(&exe)
        .output()
        .expect("the harness runs");
    assert!(run.status.success());
    let mut c: Vec<serde_json::Value> = serde_json::from_slice(&run.stdout).unwrap();
    for s in &mut c {
        s.as_object_mut().unwrap().remove("env");
    }
    Some(c)
}

fn as_json(settings: &[relift_info::Setting]) -> Vec<serde_json::Value> {
    settings
        .iter()
        .map(|s| serde_json::to_value(s).unwrap())
        .collect()
}

#[test]
fn the_runtime_schema_in_c_equals_the_list_in_rust() {
    let Some(c) = declared(
        "settings-schema",
        "rl_switch_declare_settings",
        &["cpus/a64/include", "cpus/a64", "guests/switch"],
        &["guests/switch/settings.c"],
    ) else {
        return;
    };
    assert_eq!(c, as_json(&runtime_settings()));
}

#[test]
fn the_game_boy_schema_in_c_equals_the_lists_in_rust() {
    let Some(c) = declared(
        "settings-schema-gb",
        "rl_gb_declare_settings",
        &["guests/gb/exact"],
        &["guests/gb/exact/settings.c"],
    ) else {
        return;
    };
    let mut rust = runtime_settings();
    rust.retain(|s| s.key == "video" || s.key == "language");
    rust.extend(gb_settings());
    assert_eq!(c, as_json(&rust));
}

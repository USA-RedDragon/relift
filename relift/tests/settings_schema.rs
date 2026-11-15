use std::path::{Path, PathBuf};

use relift::game_settings::runtime_settings;

const HARNESS: &str = r#"#include <stdio.h>
#include "core.h"
#include <relift/settings.h>
void rl_switch_declare_settings(void);
const rl_guest_desc *rl_core_guest(void) { return NULL; }
int main(void)
{
    static char b[65536];
    rl_switch_declare_settings();
    rl_settings_schema_json(b, sizeof b);
    puts(b);
    return 0;
}
"#;

fn scratch() -> PathBuf {
    let d = Path::new(env!("CARGO_MANIFEST_DIR")).join("../target/tmp/settings-schema");
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("the scratch dir is creatable");
    d
}

#[test]
fn the_runtime_schema_in_c_equals_the_list_in_rust() {
    let Some(cc) = relift_build::find_tool("clang") else {
        eprintln!("skipped: no clang on PATH");
        return;
    };
    let rt = Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime");
    let dir = scratch();
    let src = dir.join("harness.c");
    relift_cache::write_if_changed(&src, HARNESS.as_bytes()).unwrap();
    let exe = dir.join(format!("harness{}", std::env::consts::EXE_SUFFIX));
    let mut cmd = relift_proc::command(&cc);
    cmd.args(["-std=c11", "-D_GNU_SOURCE", "-D_DARWIN_C_SOURCE"]);
    if cfg!(target_arch = "x86_64") {
        cmd.arg("-mcx16");
    }
    for inc in [
        "core/include",
        "core",
        "cpus/a64/include",
        "cpus/a64",
        "guests/switch",
    ] {
        cmd.arg(format!("-I{}", rt.join(inc).display()));
    }
    cmd.arg(&src);
    for f in [
        "core/settings.c",
        "core/log.c",
        "core/ports.c",
        "core/input.c",
        "core/input_script.c",
        "guests/switch/settings.c",
    ] {
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
    let rust: Vec<serde_json::Value> = runtime_settings()
        .iter()
        .map(|s| serde_json::to_value(s).unwrap())
        .collect();
    assert_eq!(c, rust);
}

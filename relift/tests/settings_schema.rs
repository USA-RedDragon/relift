use std::path::{Path, PathBuf};

use relift::game_settings::{gb_settings, nds_settings, runtime_settings, touch_pad_settings};

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
        "core/touchpad.c",
        "core/touchpad_draw.c",
        "core/touchpad_font.c",
    ]
    .iter()
    .chain(sources)
    {
        cmd.arg(rt.join(f));
    }
    cmd.arg("-o").arg(&exe).arg("-lpthread").arg("-lm");
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
    Some(c.into_iter().map(numbers_as_floats).collect())
}

fn as_json(settings: &[relift_info::Setting]) -> Vec<serde_json::Value> {
    settings
        .iter()
        .map(|s| numbers_as_floats(serde_json::to_value(s).unwrap()))
        .collect()
}

/// `v` with every number as a float, so a float setting's whole-number bounds, which C prints as `1` and serde as
/// `1.0`, compare equal.
fn numbers_as_floats(v: serde_json::Value) -> serde_json::Value {
    match v {
        serde_json::Value::Number(n) => n
            .as_f64()
            .and_then(serde_json::Number::from_f64)
            .map_or(serde_json::Value::Number(n), serde_json::Value::Number),
        serde_json::Value::Array(a) => a.into_iter().map(numbers_as_floats).collect(),
        serde_json::Value::Object(o) => o
            .into_iter()
            .map(|(k, x)| (k, numbers_as_floats(x)))
            .collect(),
        other => other,
    }
}

#[test]
fn the_runtime_schema_in_c_equals_the_list_in_rust() {
    let Some(mut c) = declared(
        "settings-schema",
        "rl_switch_declare_settings",
        &["cpus/arm64/include", "cpus/arm64", "guests/switch"],
        &[
            "guests/switch/settings.c",
            "guests/switch/touchpad.c",
            "cpus/arm64/pac.c",
        ],
    ) else {
        return;
    };
    let layout = c.len();
    c.retain(|s| {
        !s["key"]
            .as_str()
            .unwrap()
            .starts_with("input.touch_pad.layout.switch.")
    });
    assert!(c.len() < layout);
    let mut rust = runtime_settings();
    rust.extend(touch_pad_settings());
    assert_eq!(c, as_json(&rust));
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
    rust.retain(|s| {
        matches!(
            s.key.as_str(),
            "video"
                | "language"
                | "video.present_mode"
                | "video.present_trace"
                | "video.fullscreen"
                | "video.gpu_driver"
                | "gpu.math"
                | "gpu.clock_floor"
                | "video.render_resolution"
                | "gpu.profile"
                | "picture.aa"
                | "picture.aa_quality"
                | "picture.filter"
                | "picture.upscaler"
                | "picture.upscale_scale"
                | "picture.taa_sharpness"
                | "renderdoc.capture"
                | "renderdoc.capture_dir"
                | "present.capture"
                | "debug.gdb"
                | "debug.gdb_wait"
                | "text_entry.text"
                | "text_entry.default"
                | "text_entry.panel"
        )
    });
    rust.extend(gb_settings());
    assert_eq!(c, as_json(&rust));
}

#[test]
fn the_ds_screen_settings_in_c_equal_the_list_in_rust() {
    let Some(mut c) = declared(
        "settings-schema-nds",
        "rl_nds_declare_settings",
        &["cpus/arm32/include", "cpus/arm32", "guests/nds"],
        &["guests/nds/settings.c"],
    ) else {
        return;
    };
    c.retain(|s| s["key"].as_str().unwrap().starts_with("video.nds_"));
    assert_eq!(c, as_json(&nds_settings()));
}

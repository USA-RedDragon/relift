//! Every host preset is what its device's capability capture implies. The
//! captures under `tests/caps` are Vulkan Profiles JSON (`vulkaninfo
//! --json`, or a vulkan.gpuinfo.org report's profile) or an Android vkjson
//! (`adb shell cmd gpu vkjson`), trimmed by `tests/caps/trim.py` to the
//! members `HostProfile::from_device` reads; each names its source.

use relift_gpu_sm86::translate::{DeviceCaps, HostProfile};
use serde_json::Value;

const FLAGS: &[(&str, u64)] = &[
    ("VK_SHADER_STAGE_VERTEX_BIT", 0x1),
    ("VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT", 0x2),
    ("VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT", 0x4),
    ("VK_SHADER_STAGE_GEOMETRY_BIT", 0x8),
    ("VK_SHADER_STAGE_FRAGMENT_BIT", 0x10),
    ("VK_SHADER_STAGE_COMPUTE_BIT", 0x20),
    ("VK_SHADER_STAGE_TASK_BIT_EXT", 0x40),
    ("VK_SHADER_STAGE_MESH_BIT_EXT", 0x80),
    ("VK_SUBGROUP_FEATURE_BASIC_BIT", 0x1),
    ("VK_SUBGROUP_FEATURE_VOTE_BIT", 0x2),
    ("VK_SUBGROUP_FEATURE_ARITHMETIC_BIT", 0x4),
    ("VK_SUBGROUP_FEATURE_BALLOT_BIT", 0x8),
    ("VK_SUBGROUP_FEATURE_SHUFFLE_BIT", 0x10),
    ("VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT", 0x20),
    ("VK_SUBGROUP_FEATURE_CLUSTERED_BIT", 0x40),
    ("VK_SUBGROUP_FEATURE_QUAD_BIT", 0x80),
];

const DRIVERS: &[(&str, u64)] = &[
    ("VK_DRIVER_ID_AMD_PROPRIETARY", 1),
    ("VK_DRIVER_ID_MESA_RADV", 3),
    ("VK_DRIVER_ID_NVIDIA_PROPRIETARY", 4),
    ("VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA", 6),
    ("VK_DRIVER_ID_QUALCOMM_PROPRIETARY", 8),
    ("VK_DRIVER_ID_ARM_PROPRIETARY", 9),
    ("VK_DRIVER_ID_MESA_LLVMPIPE", 13),
    ("VK_DRIVER_ID_MOLTENVK", 14),
    ("VK_DRIVER_ID_MESA_TURNIP", 18),
];

/// The value of one member: booleans as 0 or 1, flag and driver names as
/// their numbers.
fn number(v: &Value) -> Option<u64> {
    match v {
        Value::Bool(b) => Some(u64::from(*b)),
        Value::Number(n) => n.as_u64().or_else(|| n.as_f64().map(|f| f as u64)),
        Value::String(s) => DRIVERS.iter().find(|d| d.0 == s).map(|d| d.1),
        Value::Array(a) => Some(
            a.iter()
                .filter_map(Value::as_str)
                .map(|f| FLAGS.iter().find(|x| x.0 == f).map_or(0, |x| x.1))
                .fold(0, |a, b| a | b),
        ),
        _ => None,
    }
}

/// Every member anywhere under `v`. The same member may appear in more than
/// one structure (a core one and its extension's); the captures agree on it.
fn members(v: &Value, caps: &mut DeviceCaps) {
    if let Value::Object(o) = v {
        for (k, x) in o {
            if x.is_object() {
                members(x, caps);
            } else if let Some(a) = x.as_array().filter(|a| a.iter().all(Value::is_number)) {
                for (i, e) in a.iter().enumerate() {
                    let n = number(e).expect("a number");
                    let old = caps.values.insert(format!("{k}[{i}]"), n);
                    assert!(old.is_none_or(|o| o == n), "{k}[{i}]: {old:?} and {n}");
                }
            } else if let Some(n) = number(x) {
                let old = caps.values.insert(k.clone(), n);
                assert!(old.is_none_or(|o| o == n), "{k}: {old:?} and {n}");
            }
        }
    }
}

fn load(name: &str) -> DeviceCaps {
    let path = format!("{}/tests/caps/{name}.json", env!("CARGO_MANIFEST_DIR"));
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    let v: Value = serde_json::from_str(&text).unwrap_or_else(|e| panic!("{path}: {e}"));
    let mut caps = DeviceCaps::default();
    if let Some(dev) = v["capabilities"]["device"].as_object() {
        if let Some(e) = dev["extensions"].as_object() {
            caps.extensions.extend(e.keys().cloned());
        }
        members(&dev["features"], &mut caps);
        members(&dev["properties"], &mut caps);
    } else {
        let dev = &v["devices"][0];
        for e in dev["extensions"].as_array().into_iter().flatten() {
            if let Some(n) = e["extensionName"].as_str() {
                caps.extensions.insert(n.into());
            }
        }
        members(dev, &mut caps);
    }
    caps
}

#[test]
fn presets_are_their_captures() {
    for p in HostProfile::presets() {
        let caps = load(&p.name);
        assert_eq!(HostProfile::from_device(&p.name, &caps), p, "{}", p.name);
    }
}

/// The Adreno 740 driver lacks maximal reconvergence, `shaderInt64`, the
/// shader clock and float atomic add, which its preset once claimed.
#[test]
fn adreno_reports_what_it_lacks() {
    let p = HostProfile::adreno();
    assert!(!p.maximal_reconvergence);
    assert!(!p.int64 && !p.int64_atomics_buffer && !p.int64_atomics_image);
    assert!(!p.float32_atomic_add);
    assert_eq!(p.clock, relift_gpu_sm86::translate::Clock::None);
}

/// Thor's Adreno 740 can require 64-wide full subgroups in compute: the
/// mesh fallback packs two guest workgroups into each. Devices with a
/// fixed size below 32 keep one.
#[test]
fn mesh_slots_fill_a_required_wide_subgroup() {
    let p = HostProfile::adreno();
    assert_eq!(p.compute_subgroup, Some(64));
    assert_eq!(p.mesh_slots, 2);
    for p in [HostProfile::lavapipe(), HostProfile::mali()] {
        assert_eq!((p.compute_subgroup, p.mesh_slots), (None, 1), "{}", p.name);
    }
    assert_eq!(HostProfile::nvidia().compute_subgroup, Some(32));
}

/// A device at Vulkan's minimum limits: no capture has fewer than 1024
/// invocations per workgroup, so this one is built from the minimum values
/// the specification requires, with a wide subgroup the device could
/// require. The limits come through, and a required subgroup wider than
/// the workgroup allows takes no slots.
#[test]
fn workgroup_limits_come_from_the_device() {
    let mut caps = load("adreno");
    caps.values
        .insert("maxComputeWorkGroupInvocations".into(), 128);
    for (k, n) in [128, 128, 64].into_iter().enumerate() {
        caps.values
            .insert(format!("maxComputeWorkGroupSize[{k}]"), n);
    }
    let p = HostProfile::from_device("minimum", &caps);
    assert_eq!(p.max_workgroup_invocations, 128);
    assert_eq!(p.max_workgroup_size, [128, 128, 64]);
    assert_eq!(p.mesh_slots, 2);
    caps.values.insert("minSubgroupSize".into(), 128);
    caps.values
        .insert("maxComputeWorkGroupInvocations".into(), 64);
    let p = HostProfile::from_device("minimum", &caps);
    assert_eq!(p.compute_subgroup, Some(128));
    assert_eq!(p.mesh_slots, 1);
}

//! Translating a game's programs at build time, for the device the build is
//! for.
//!
//! relift translates shaders when it builds a game, on the player's device
//! or for a device whose capabilities were captured: the profile is
//! [`HostProfile::from_device`] of that device's [`DeviceCaps`], which come
//! as JSON ([`DeviceCaps::from_json`]). [`translate_set`] translates every
//! program the runtime recorded and gives the [`Table`] the runtime looks
//! programs up in.
//!
//! # Capabilities JSON
//!
//! The same shapes as the captures in `tests/caps`: a Vulkan Profiles JSON
//! (`vulkaninfo --json`, a vulkan.gpuinfo.org profile), an Android vkjson
//! (`adb shell cmd gpu vkjson`), or the runtime's own dump,
//!
//! ```json
//! {"capabilities": {"device": {
//!     "extensions": {"VK_KHR_shader_maximal_reconvergence": 1},
//!     "features": {"VkPhysicalDeviceVulkan12Features": {"shaderInt64": true}},
//!     "properties": {"VkPhysicalDeviceProperties": {"deviceID": 8708}}}}}
//! ```
//!
//! where structures may nest in any way and each member goes by its Vulkan
//! name. Booleans are true or false (or 0 and 1), numbers are numbers,
//! flag sets are arrays of their bit names (`VK_SHADER_STAGE_COMPUTE_BIT`)
//! or numbers, `driverID` is its `VK_DRIVER_ID_` name or number, and an
//! array of numbers (`maxComputeWorkGroupSize`) gives one value per element
//! (`maxComputeWorkGroupSize[0]`). A member that appears in several
//! structures must have the same value in each.
//!
//! # Table
//!
//! [`Table::to_bytes`] lays the translations out for the runtime, all
//! little-endian:
//!
//! * the magic `RLSPV001` (8 bytes) and the entry count (u32);
//! * per entry, sorted by key: the key (32 bytes, the SHA-256 the runtime
//!   names the program by), the offset of its SPIR-V from the start of the
//!   table in bytes and its length in 32-bit words (u32 each);
//! * the SPIR-V words of every entry, each module aligned to 4 bytes.

use super::{HostProfile, Interface, Options, Shader, Stage, translate_with};
use crate::program::Program;
use serde_json::Value;
use std::collections::BTreeMap;

use super::DeviceCaps;

/// A program the runtime recorded: its key and the bytes
/// [`Program::parse`] reads (the SPH, if any, then the code), with the
/// options its pipeline gives (the `host` field is replaced).
pub struct Input {
    pub key: [u8; 32],
    pub code: Vec<u8>,
    pub options: Options,
}

/// The translated programs, by key, and the ones that did not translate
/// with the reason.
#[derive(Default)]
pub struct Table {
    pub shaders: BTreeMap<[u8; 32], (Stage, Vec<u32>, Interface)>,
    pub failed: BTreeMap<[u8; 32], String>,
}

/// Translates every program of `inputs` for `host`.
pub fn translate_set(host: &HostProfile, inputs: &[Input]) -> Table {
    let mut t = Table::default();
    for i in inputs {
        let o = Options {
            host: host.clone(),
            ..i.options.clone()
        };
        match translate_with(&Program::parse(&i.code), &o) {
            Ok(Shader {
                stage,
                spirv,
                interface,
            }) => {
                t.shaders.insert(i.key, (stage, spirv, interface));
            }
            Err(e) => {
                t.failed.insert(i.key, e.0);
            }
        }
    }
    t
}

impl Table {
    /// The table's bytes (see the module documentation).
    pub fn to_bytes(&self) -> Vec<u8> {
        let n = self.shaders.len();
        let mut out = Vec::new();
        out.extend(b"RLSPV001");
        out.extend((n as u32).to_le_bytes());
        let mut at = 12 + 40 * n;
        for (key, (_, words, _)) in &self.shaders {
            out.extend(key);
            out.extend((at as u32).to_le_bytes());
            out.extend((words.len() as u32).to_le_bytes());
            at += 4 * words.len();
        }
        for (_, words, _) in self.shaders.values() {
            out.extend(words.iter().flat_map(|w| w.to_le_bytes()));
        }
        out
    }
}

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
    ("VK_DRIVER_ID_AMD_OPEN_SOURCE", 2),
    ("VK_DRIVER_ID_MESA_RADV", 3),
    ("VK_DRIVER_ID_NVIDIA_PROPRIETARY", 4),
    ("VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS", 5),
    ("VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA", 6),
    ("VK_DRIVER_ID_IMAGINATION_PROPRIETARY", 7),
    ("VK_DRIVER_ID_QUALCOMM_PROPRIETARY", 8),
    ("VK_DRIVER_ID_ARM_PROPRIETARY", 9),
    ("VK_DRIVER_ID_GOOGLE_SWIFTSHADER", 10),
    ("VK_DRIVER_ID_GGP_PROPRIETARY", 11),
    ("VK_DRIVER_ID_BROADCOM_PROPRIETARY", 12),
    ("VK_DRIVER_ID_MESA_LLVMPIPE", 13),
    ("VK_DRIVER_ID_MOLTENVK", 14),
    ("VK_DRIVER_ID_COREAVI_PROPRIETARY", 15),
    ("VK_DRIVER_ID_JUICE_PROPRIETARY", 16),
    ("VK_DRIVER_ID_VERISILICON_PROPRIETARY", 17),
    ("VK_DRIVER_ID_MESA_TURNIP", 18),
    ("VK_DRIVER_ID_MESA_V3DV", 19),
    ("VK_DRIVER_ID_MESA_PANVK", 20),
    ("VK_DRIVER_ID_SAMSUNG_PROPRIETARY", 21),
    ("VK_DRIVER_ID_MESA_VENUS", 22),
    ("VK_DRIVER_ID_MESA_DOZEN", 23),
    ("VK_DRIVER_ID_MESA_NVK", 24),
    ("VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA", 25),
    ("VK_DRIVER_ID_MESA_HONEYKRISP", 26),
];

/// The value of one member: booleans as 0 or 1, flag and driver names as
/// their numbers (flags `from_device` does not read count as 0).
fn number(v: &Value) -> Option<u64> {
    match v {
        Value::Bool(b) => Some(u64::from(*b)),
        Value::Number(n) => n.as_u64().or_else(|| n.as_f64().map(|f| f as u64)),
        Value::String(s) => DRIVERS.iter().find(|d| d.0 == s).map(|d| d.1),
        Value::Array(a) if a.iter().all(Value::is_string) => Some(
            a.iter()
                .filter_map(Value::as_str)
                .filter_map(|f| FLAGS.iter().find(|x| x.0 == f).map(|x| x.1))
                .fold(0, |a, b| a | b),
        ),
        _ => None,
    }
}

fn insert(caps: &mut DeviceCaps, k: String, n: u64) -> Result<(), String> {
    match caps.values.insert(k.clone(), n) {
        Some(o) if o != n => Err(format!("{k}: {o} and {n}")),
        _ => Ok(()),
    }
}

/// Every member anywhere under `v`.
fn members(v: &Value, caps: &mut DeviceCaps) -> Result<(), String> {
    let Value::Object(o) = v else { return Ok(()) };
    for (k, x) in o {
        if x.is_object() {
            members(x, caps)?;
        } else if let Some(a) = x
            .as_array()
            .filter(|a| !a.is_empty() && a.iter().all(Value::is_number))
        {
            for (i, e) in a.iter().enumerate() {
                if let Some(n) = number(e) {
                    insert(caps, format!("{k}[{i}]"), n)?;
                }
            }
        } else if let Some(n) = number(x) {
            insert(caps, k.clone(), n)?;
        }
    }
    Ok(())
}

impl DeviceCaps {
    /// The capabilities in a JSON capture (see the module documentation).
    pub fn from_json(text: &str) -> Result<DeviceCaps, String> {
        let v: Value = serde_json::from_str(text).map_err(|e| e.to_string())?;
        let mut caps = DeviceCaps::default();
        if let Some(dev) = v["capabilities"]["device"].as_object() {
            match dev.get("extensions") {
                Some(Value::Object(e)) => caps.extensions.extend(e.keys().cloned()),
                Some(Value::Array(e)) => caps
                    .extensions
                    .extend(e.iter().filter_map(Value::as_str).map(String::from)),
                _ => {}
            }
            for part in ["features", "properties"] {
                if let Some(p) = dev.get(part) {
                    members(p, &mut caps)?;
                }
            }
        } else if let Some(dev) = v["devices"].get(0) {
            for e in dev["extensions"].as_array().into_iter().flatten() {
                if let Some(n) = e["extensionName"].as_str() {
                    caps.extensions.insert(n.into());
                }
            }
            let rest: serde_json::Map<String, Value> = dev
                .as_object()
                .into_iter()
                .flatten()
                .filter(|(k, _)| !matches!(k.as_str(), "extensions" | "formats" | "layers"))
                .map(|(k, v)| (k.clone(), v.clone()))
                .collect();
            members(&Value::Object(rest), &mut caps)?;
        } else {
            return Err("neither a Vulkan profile nor a vkjson".into());
        }
        Ok(caps)
    }
}

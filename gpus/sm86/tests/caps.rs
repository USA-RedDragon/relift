//! Every host preset is what its device's capability capture implies. The
//! captures under `tests/caps` are Vulkan Profiles JSON (`vulkaninfo
//! --json`, or a vulkan.gpuinfo.org report's profile) or an Android vkjson
//! (`adb shell cmd gpu vkjson`), trimmed by `tests/caps/trim.py` to the
//! members `HostProfile::from_device` reads; each names its source. They
//! load through `DeviceCaps::from_json`, as a build's captures do.

use relift_gpu_sm86::translate::{DeviceCaps, HostProfile};

fn load(name: &str) -> DeviceCaps {
    let path = format!("{}/tests/caps/{name}.json", env!("CARGO_MANIFEST_DIR"));
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    DeviceCaps::from_json(&text).unwrap_or_else(|e| panic!("{path}: {e}"))
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

/// Native halves and dropped NaN selects are probe results, not promises:
/// the NVIDIA capture's bits and probes turn them on, and a capture whose
/// probe failed or never ran (no `ReliftDeviceProbes` member) keeps the
/// exact integer and NaN-select paths even with every feature bit on.
#[test]
fn half_and_nan_paths_need_the_device_probes() {
    let caps = load("nvidia");
    let p = HostProfile::from_device("nvidia", &caps);
    assert!(p.native_f16 && p.canonical_nan);
    for (probe, f16, nan) in [
        ("exactFloat16", false, true),
        ("canonicalNan32", true, false),
    ] {
        let mut c = caps.clone();
        c.values.insert(probe.into(), 0);
        let p = HostProfile::from_device("nvidia", &c);
        assert_eq!((p.native_f16, p.canonical_nan), (f16, nan), "{probe} false");
        let mut c = caps.clone();
        c.values.remove(probe);
        let p = HostProfile::from_device("nvidia", &c);
        assert_eq!(
            (p.native_f16, p.canonical_nan),
            (f16, nan),
            "{probe} missing"
        );
    }
    let mut c = caps.clone();
    c.values.insert("shaderDenormPreserveFloat16".into(), 0);
    assert!(!HostProfile::from_device("nvidia", &c).native_f16);
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

/// A table holds each translation at its offset, sorted by key, and lists
/// the programs that did not translate.
#[test]
fn a_program_set_translates_into_a_table() {
    use relift_gpu_sm86::translate::Options;
    use relift_gpu_sm86::translate::build::{Input, translate_set};
    let exit = 0x14du128 | (4 << 9) | (7 << 12) | (7 << 87) | (7 << 110) | (7 << 113);
    let bra = 0x147u128 | (4 << 9) | (7 << 12) | (0xffff_ffff_fffc << 34) | (7 << 87);
    let good: Vec<u8> = [exit, bra].iter().flat_map(|w| w.to_le_bytes()).collect();
    let bad: Vec<u8> = [0x1ffu128, exit, bra]
        .iter()
        .flat_map(|w| w.to_le_bytes())
        .collect();
    let input = |k: u8, code: &[u8]| Input {
        key: [k; 32],
        code: code.to_vec(),
        options: Options::default(),
    };
    let host = HostProfile::from_device("adreno", &load("adreno"));
    let t = translate_set(&host, &[input(2, &good), input(1, &good), input(3, &bad)]);
    assert_eq!(t.shaders.len(), 2);
    assert!(t.failed.contains_key(&[3; 32]));
    let b = t.to_bytes();
    assert_eq!(&b[..8], b"RLSPV001");
    assert_eq!(u32::from_le_bytes(b[8..12].try_into().unwrap()), 2);
    assert_eq!(b[12], 1, "sorted by key");
    let at = u32::from_le_bytes(b[44..48].try_into().unwrap()) as usize;
    let len = u32::from_le_bytes(b[48..52].try_into().unwrap()) as usize;
    let words = &t.shaders[&[1; 32]].1;
    assert_eq!(len, words.len());
    assert_eq!(b[at..at + 4], words[0].to_le_bytes());
    assert_eq!(b.len(), 12 + 2 * 40 + 8 * len);
    let z = t.to_compressed_bytes(19).expect("compresses");
    assert_eq!(&z[..8], b"RLSPV002");
    assert_eq!(z[12], 1, "sorted by key");
    let at = u64::from_le_bytes(z[44..52].try_into().unwrap()) as usize;
    let size = u32::from_le_bytes(z[52..56].try_into().unwrap()) as usize;
    let n = u32::from_le_bytes(z[56..60].try_into().unwrap()) as usize;
    assert_eq!(n, words.len());
    let raw = zstd::bulk::decompress(&z[at..at + size], 4 * n).expect("one frame");
    assert_eq!(
        raw,
        words
            .iter()
            .flat_map(|w| w.to_le_bytes())
            .collect::<Vec<u8>>()
    );
    assert!(z.len() < b.len());
}

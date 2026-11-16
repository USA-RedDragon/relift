//! Known answers from an RTX 3090: every recorded program is translated for
//! several hosts, validated, and run by a SPIR-V interpreter under each
//! denormal behavior the host may have; every output must match the device
//! bit for bit. See tests/kat/file.rs for the data and kat/README.md for how
//! it was recorded.

mod common;
#[path = "kat/file.rs"]
mod file;
#[path = "kat/spvi.rs"]
mod spvi;

use common::translate_with;
use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::{Denorm as HostDenorm, HostProfile, Options};
use spvi::{Denorm, Env, Memory, Module, Run};
use std::collections::{HashMap, HashSet};

/// Variants the translator rejects, as it should: it cannot do them exactly
/// yet. Their vectors stay for when it learns them.
const REJECTED: &[&str] = &[
    "F2FP.BF16.PACK_AB",
    "F2FP.RELU.BF16.PACK_AB",
    "F2FP.BF16.PACK_AB.RZ",
    "F2FP.RELU.BF16.PACK_AB.RZ",
    "F2FP.SATFINITE.BF16.PACK_AB.RZ",
    "HADD2 bit 85",
    "HMUL2 bit 85",
    "HMUL2.FMZ",
    "HFMA2.FMZ",
    "HFMA2.BF16_V2",
];

/// How a known mismatch may differ from the device.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Known {
    /// Only where both are NaN: the device writes 0x7fffffff for every NaN
    /// result, the translation keeps a host NaN.
    NanBits,
}

/// Variants that translate but still differ from the device in a way a fix
/// in flight addresses. Every other difference fails, and so does an entry
/// that no longer differs, so each goes away with its fix.
const KNOWN: &[(&str, &[&str], Known)] = &[(
    "half",
    &[
        "HADD2.F32 -RZ, b",
        "HADD2.F32 -RZ, b.H0_H0",
        "HADD2.F32 -RZ, b.H1_H1",
        "HADD2.F32 -RZ, -|b|.H1_H1",
        "HADD2.F32.FTZ -RZ, b.H1_H1",
    ],
    Known::NanBits,
)];

fn known(family: &str, label: &str) -> Option<Known> {
    KNOWN
        .iter()
        .find(|k| k.0 == family && k.1.contains(&label))
        .map(|k| k.2)
}

fn is_nan(x: u32) -> bool {
    x & 0x7fff_ffff > 0x7f80_0000
}

const CBUF: u64 = 0x1_0000_0000;
const IN: u64 = 0x2_0000_0000;
const OUT: u64 = 0x3_0000_0000;

/// The hosts the programs run for: the ones with buffer device addresses
/// (the interpreter has no segment table). NVIDIA uses the MUFU model too:
/// its native path relies on the driver, which an interpreter cannot check.
fn hosts() -> Vec<HostProfile> {
    ["nvidia", "radv", "lavapipe", "adreno", "moltenvk"]
        .iter()
        .map(|n| HostProfile::preset(n).unwrap())
        .collect()
}

/// The denormal behaviors a host may show for a module.
fn behaviors(m: &Module, host: &HostProfile) -> Vec<Denorm> {
    if m.modes.contains(&4459) {
        return vec![Denorm::Preserve];
    }
    if m.modes.contains(&4460) {
        return vec![Denorm::FlushBefore, Denorm::FlushAfter];
    }
    match host.denorm {
        HostDenorm::Flush => vec![Denorm::FlushBefore],
        HostDenorm::Preserve => vec![Denorm::Preserve],
        HostDenorm::Unknown => vec![Denorm::Preserve, Denorm::FlushBefore, Denorm::FlushAfter],
    }
}

fn spirv_bytes(w: &[u32]) -> Vec<u8> {
    w.iter().flat_map(|x| x.to_le_bytes()).collect()
}

/// Runs one program on one row: the wrapper reads the row from `c[0][0x160]`
/// and writes its outputs to `c[0][0x168]`, both for thread 0.
fn exec(m: &Module, denorm: Denorm, row: &[u32; 4], outs: usize) -> Vec<u32> {
    let mut cb = vec![0u8; 0x10000];
    for k in 0..3 {
        cb[4 * k..4 * k + 4].copy_from_slice(&1u32.to_le_bytes());
    }
    cb[0x160..0x168].copy_from_slice(&IN.to_le_bytes());
    cb[0x168..0x170].copy_from_slice(&OUT.to_le_bytes());
    let input: Vec<u8> = row.iter().flat_map(|x| x.to_le_bytes()).collect();
    let mem = Memory {
        regions: vec![(CBUF, cb), (IN, input), (OUT, vec![0; 64])],
    };
    let env = Env {
        denorm,
        uniforms: HashMap::from([((1, 0), CBUF)]),
        local_id: [0; 3],
        group_id: [0; 3],
    };
    let mut r = Run::new(m, &env, mem);
    r.main();
    (0..outs)
        .map(|k| r.mem.read(OUT + 4 * k as u64, 4) as u32)
        .collect()
}

fn check_family(name: &str) {
    let f = file::load(name);
    let mut failures = Vec::new();
    for v in &f.variants {
        let p = Program::parse(&v.code);
        let mut seen = HashSet::new();
        let mut rejected = None;
        let known = known(name, &v.label);
        let mut any_known = false;
        for host in hosts() {
            let o = Options {
                host: host.clone(),
                exact_mufu: true,
                ..Options::default()
            };
            let s = match translate_with(&p, &o) {
                Ok(s) => s,
                Err(e) => {
                    rejected = Some(e.to_string());
                    continue;
                }
            };
            let m = Module::parse(&s.spirv);
            for d in behaviors(&m, &host) {
                if !seen.insert((spirv_bytes(&s.spirv), d)) {
                    continue;
                }
                let mut bad = 0;
                for (i, row) in f.rows.iter().enumerate() {
                    let got = exec(&m, d, row, v.outs);
                    let want = &v.expect[i * v.outs..(i + 1) * v.outs];
                    if got == want {
                        continue;
                    }
                    let nan_only = got
                        .iter()
                        .zip(want)
                        .all(|(g, w)| g == w || is_nan(*g) && is_nan(*w));
                    if known == Some(Known::NanBits) && nan_only {
                        any_known = true;
                        continue;
                    }
                    bad += 1;
                    if bad <= 3 {
                        failures.push(format!(
                            "{name}: {} on {} ({d:?}): in {:08x?} device {:08x?} translated {:08x?}",
                            v.label,
                            host.name,
                            &row[..f.nin],
                            want,
                            got
                        ));
                    }
                }
                if bad > 3 {
                    failures.push(format!(
                        "{name}: {} on {} ({d:?}): {bad} of {} rows differ",
                        v.label,
                        host.name,
                        f.rows.len()
                    ));
                }
            }
        }
        if known.is_some() && !any_known && rejected.is_none() {
            failures.push(format!(
                "{name}: {} now matches: drop it from KNOWN",
                v.label
            ));
        }
        match (rejected, REJECTED.contains(&v.label.as_str())) {
            (Some(e), false) => failures.push(format!("{name}: {} rejected: {e}", v.label)),
            (None, true) => failures.push(format!(
                "{name}: {} now translates: drop it from REJECTED",
                v.label
            )),
            _ => {}
        }
    }
    assert!(
        failures.is_empty(),
        "{} failures:\n{}",
        failures.len(),
        failures.join("\n")
    );
}

macro_rules! families {
    ($($n:ident),*) => {
        $(#[test] fn $n() { check_family(stringify!($n)); })*

        #[test]
        fn every_file_has_a_test() {
            let listed: Vec<&str> = vec![$(stringify!($n)),*];
            for n in file::names() {
                assert!(listed.contains(&n.as_str()), "tests/kat/{n}.kat has no test");
            }
        }
    };
}

families!(
    shf, fadd, fmul, ffma, fcmp, f2i, f2i_f16, i2f, f2f, frnd, f2fp, half, int, mufu_f16, mufu_64h,
    mufu_rcp, mufu_rsq, mufu_sqrt, mufu_ex2, mufu_lg2, mufu_sin, mufu_cos
);

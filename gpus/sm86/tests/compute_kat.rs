//! The compute programs recorded from the NVIDIA driver (`kat/compute`,
//! `tests/kat/compute.txt`) translate and validate for every host preset,
//! and each reports what is inexact on the hosts where the recorded
//! answers allow it. Running them against the answers needs a Vulkan
//! device: `kat/compute/kat.py --replay`.

mod common;

use common::translate_with;
use relift_gpu_nv::translate::{HostProfile, Options};

/// The programs of the record, by case name.
fn programs() -> Vec<(String, Vec<u8>)> {
    let text = include_str!("kat/compute.txt");
    let mut out = Vec::new();
    let mut name = None;
    for line in text.lines() {
        let (kind, rest) = line.split_once(' ').expect("a kind and a value");
        match kind {
            "case" => name = Some(rest.to_string()),
            "program" => {
                let bytes = (0..rest.len())
                    .step_by(2)
                    .map(|k| u8::from_str_radix(&rest[k..k + 2], 16).expect("hex"))
                    .collect();
                out.push((name.take().expect("a case before its program"), bytes));
            }
            _ => {}
        }
    }
    out
}

#[test]
fn recorded_programs_translate_for_every_host() {
    let progs = programs();
    assert!(progs.len() >= 5, "{} programs", progs.len());
    for (name, bytes) in &progs {
        let p = relift_gpu_sm86::program::parse(bytes);
        for host in HostProfile::presets() {
            let o = Options {
                host: host.clone(),
                ..Options::default()
            };
            let s =
                translate_with(&p, &o).unwrap_or_else(|e| panic!("{name} on {}: {e}", host.name));
            let inexact = &s.interface.host.inexact;
            let allowed = |l: &String| {
                l.starts_with("MEMBAR.SC")
                    || (!host.maximal_reconvergence
                        && (l.contains("no maximal reconvergence") || l.starts_with("WARPSYNC")))
            };
            assert!(
                inexact.iter().all(allowed),
                "{name} on {}: {inexact:?}",
                host.name
            );
        }
    }
}

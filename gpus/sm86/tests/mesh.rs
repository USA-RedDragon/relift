//! Task and mesh programs: the SASS the NVIDIA driver built for the
//! known-answer cases of `kat/mesh` (tests/kat/mesh.txt), whose native and
//! compute translations kat/mesh/kat.py drew on an RTX 3090 identical to
//! the driver's own image, word for word. Here each program is translated
//! for every host preset and both back ends and validated; the back end a
//! host takes follows its mesh shader limits.

mod common;

use common::{translate_with, validate};
use relift_gpu_sm86::program::{Program, Stage};
use relift_gpu_sm86::translate::mesh_fallback::{indexed, mesh_record_words, scan, vertex};
use relift_gpu_sm86::translate::{
    HostProfile, MeshLimits, MeshPath, MeshPipeline, MeshTopology, Options, mesh_components,
};

struct Case {
    name: String,
    pipe: MeshPipeline,
    programs: Vec<(String, Program)>,
}

fn hex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).expect("hex"))
        .collect()
}

fn cases() -> Vec<Case> {
    let text = include_str!("kat/mesh.txt");
    let mut out: Vec<Case> = Vec::new();
    for line in text.lines() {
        let (kind, rest) = line.split_once(' ').expect("kat line");
        match kind {
            "case" => out.push(Case {
                name: rest.into(),
                pipe: MeshPipeline::default(),
                programs: Vec::new(),
            }),
            "pipeline" => {
                let p = &mut out.last_mut().expect("case").pipe;
                for kv in rest.split(' ') {
                    let (k, v) = kv.split_once('=').expect("key=value");
                    let n = || v.parse::<u32>().expect("number");
                    match k {
                        "max_vertices" => p.max_vertices = n(),
                        "max_primitives" => p.max_primitives = n(),
                        "task_lanes" => p.task_lanes = n(),
                        "mesh_lanes" => p.mesh_lanes = n(),
                        "payload_bytes" => p.payload_bytes = n(),
                        "primitive_id_location" => p.primitive_id_location = Some(n()),
                        "fragment_per_primitive" => p.fragment_per_primitive = u64::from(n()),
                        "topology" => {
                            p.topology = match v {
                                "point" => MeshTopology::Points,
                                "line" => MeshTopology::Lines,
                                _ => MeshTopology::Triangles,
                            }
                        }
                        "primitive_sph" => {
                            let b = hex(v);
                            let mut w = [0u32; 32];
                            for (k, x) in w.iter_mut().enumerate() {
                                *x = u32::from_le_bytes(b[4 * k..4 * k + 4].try_into().unwrap());
                            }
                            p.primitive_sph = Some(w);
                        }
                        _ => panic!("pipeline key {k}"),
                    }
                }
            }
            "program" => {
                let (k, h) = rest.split_once(' ').expect("program kind");
                let c = out.last_mut().expect("case");
                c.programs.push((k.into(), Program::parse(&hex(h))));
            }
            "answer" => {}
            _ => panic!("kat line {kind}"),
        }
    }
    out
}

fn options(host: HostProfile, pipe: &MeshPipeline, compute: bool) -> Options {
    let mut o = Options {
        host,
        mesh_compute: compute,
        ..Options::default()
    };
    o.pipeline.mesh = pipe.clone();
    o
}

#[test]
fn the_cases_have_their_stages() {
    let cases = cases();
    assert_eq!(cases.len(), 7);
    for c in &cases {
        for (k, p) in &c.programs {
            let want = match k.as_str() {
                "task" => Stage::Task,
                "mesh" => Stage::Mesh,
                _ => Stage::Fragment,
            };
            assert_eq!(p.stage(), want, "{} {k}", c.name);
        }
        let task = c.programs.iter().any(|(k, _)| k == "task");
        let mesh = &c.programs.iter().find(|(k, _)| k == "mesh").unwrap().1;
        assert_eq!(mesh.after_task(), task, "{}", c.name);
    }
}

#[test]
fn every_case_translates_on_every_host_and_back_end() {
    for c in cases() {
        for host in HostProfile::presets() {
            for compute in [false, true] {
                let native = !compute && host.mesh_shader.is_some();
                for (k, p) in &c.programs {
                    let o = options(host.clone(), &c.pipe, compute);
                    let s = translate_with(p, &o)
                        .unwrap_or_else(|e| panic!("{} {k} on {}: {e}", c.name, host.name));
                    if k == "frag" {
                        continue;
                    }
                    let want = if native {
                        MeshPath::Native
                    } else {
                        MeshPath::Compute
                    };
                    assert_eq!(
                        s.interface.host.mesh, want,
                        "{} {k} on {}",
                        c.name, host.name
                    );
                    assert!(
                        s.interface.host.inexact.is_empty(),
                        "{} {k} on {}: {:?}",
                        c.name,
                        host.name,
                        s.interface.host.inexact
                    );
                }
            }
        }
    }
}

#[test]
fn the_compute_back_end_helpers_validate() {
    for c in cases() {
        let mesh = &c.programs.iter().find(|(k, _)| k == "mesh").unwrap().1;
        let sph = mesh.sph.as_ref().unwrap().words;
        validate(&scan());
        validate(&vertex(&c.pipe, &sph).unwrap_or_else(|e| panic!("{}: {e}", c.name)));
    }
}

/// Only pipelines with per-primitive outputs, which every vertex of a
/// primitive must carry, draw the compute back end without indices.
#[test]
fn pipelines_without_per_primitive_outputs_draw_indexed() {
    let mut listed: Vec<String> = cases()
        .into_iter()
        .filter(|c| !indexed(&c.pipe))
        .map(|c| c.name)
        .collect();
    listed.sort();
    assert_eq!(listed, ["ext-cull", "nv-prim"]);
}

/// A mesh record holds the declared maximum of vertices and primitives
/// per output row, not the stage buffer's blocks of 32, and an indexed
/// draw's record holds no count or indices (they go to the index region).
#[test]
fn mesh_records_are_sized_from_the_declared_maximums() {
    let want = [
        ("ext-cull", 1 + 40 * 3 + 48 * 10 + 40 * 3),
        ("nv-lines", 50 * 8),
        ("nv-prim", 1 + 20 * 3 + 40 * 8 + 20 * 5),
        ("nv-small", 3 * 8),
    ];
    for (name, words) in want {
        let c = cases().into_iter().find(|c| c.name == name).unwrap();
        let mesh = &c.programs.iter().find(|(k, _)| k == "mesh").unwrap().1;
        let n = mesh_components(&mesh.sph.as_ref().unwrap().words)
            .unwrap()
            .len() as u32;
        assert_eq!(mesh_record_words(&c.pipe, n), words, "{name}");
    }
}

#[test]
fn a_mesh_host_takes_the_compute_back_end_when_forced_or_short_of_limits() {
    let c = cases().into_iter().find(|c| c.name == "nv-multi").unwrap();
    let mesh = &c.programs.iter().find(|(k, _)| k == "mesh").unwrap().1;
    let path = |host: HostProfile, compute: bool| {
        let o = options(host, &c.pipe, compute);
        translate_with(mesh, &o).unwrap().interface.host.mesh
    };
    assert_eq!(path(HostProfile::nvidia(), false), MeshPath::Native);
    assert_eq!(path(HostProfile::nvidia(), true), MeshPath::Compute);
    assert_eq!(path(HostProfile::moltenvk(), false), MeshPath::Compute);
    let small = HostProfile {
        mesh_shader: Some(MeshLimits {
            max_mesh_output_vertices: 32,
            ..MeshLimits::NVIDIA
        }),
        ..HostProfile::nvidia()
    };
    assert_eq!(path(small, false), MeshPath::Compute);
}

#[test]
fn the_pipeline_comes_from_the_glslc_headers() {
    let mut mesh = vec![0u8; 0xe0];
    mesh[0x56] = 7;
    mesh[0x58..0x5c].copy_from_slice(&0x0080_0020u32.to_le_bytes());
    mesh[0x5c..0x60].copy_from_slice(&0x0301_0000u32.to_le_bytes());
    mesh[0x60..0x64].copy_from_slice(&0x0020_0060u32.to_le_bytes());
    mesh[0x64..0x68].copy_from_slice(&0x10u32.to_le_bytes());
    mesh[0x68..0x6c].copy_from_slice(&0x80u32.to_le_bytes());
    let mut task = vec![0u8; 0xe0];
    task[0x56] = 6;
    task[0x58..0x5c].copy_from_slice(&0x0c80_0010u32.to_le_bytes());
    let mut code = vec![0u8; 0x90];
    code[0x10] = 0x81;
    let p = MeshPipeline::from_glslc(Some(&task), &mesh, &code).unwrap();
    assert_eq!((p.max_vertices, p.max_primitives), (0x60, 0x20));
    assert_eq!(p.topology, MeshTopology::Triangles);
    assert_eq!((p.mesh_lanes, p.task_lanes, p.payload_bytes), (32, 16, 128));
    assert_eq!(p.primitive_sph.unwrap()[0], 0x81);
    task[0x56] = 7;
    assert!(MeshPipeline::from_glslc(Some(&task), &mesh, &code).is_none());
}

/// The instructions of a SPIR-V module: opcode and words.
fn instructions(words: &[u32]) -> Vec<(u16, &[u32])> {
    let mut out = Vec::new();
    let mut i = 5;
    while i < words.len() {
        let n = (words[i] >> 16) as usize;
        out.push((words[i] as u16, &words[i..i + n]));
        i += n;
    }
    out
}

/// The workgroup variables the entry point of a compute back end program
/// stores zeros into before it calls the guest, and those it copies word
/// for word into the storage buffer after.
fn zeroed_and_copied(words: &[u32]) -> (Vec<u32>, Vec<u32>) {
    let ins = instructions(words);
    let entry = ins
        .iter()
        .find(|(o, _)| *o == 15)
        .map(|(_, w)| w[2])
        .unwrap();
    let zeros: Vec<u32> = ins
        .iter()
        .filter(|(o, w)| *o == 43 && w[3] == 0)
        .map(|(_, w)| w[2])
        .collect();
    let class: std::collections::HashMap<u32, u32> = ins
        .iter()
        .filter(|(o, _)| *o == 59)
        .map(|(_, w)| (w[2], w[3]))
        .collect();
    let start = ins
        .iter()
        .position(|(o, w)| *o == 54 && w[2] == entry)
        .unwrap();
    let body = &ins[start..];
    let end = body.iter().position(|(o, _)| *o == 56).unwrap();
    let body = &body[..end];
    let call = body.iter().position(|(o, _)| *o == 57).unwrap();
    let mut chain = std::collections::HashMap::new();
    let mut loaded = std::collections::HashMap::new();
    let (mut zeroed, mut copied) = (Vec::new(), Vec::new());
    for (k, (o, w)) in body.iter().enumerate() {
        match *o {
            65 => {
                chain.insert(w[2], w[3]);
            }
            61 => {
                if let Some(&v) = chain.get(&w[3]) {
                    loaded.insert(w[2], v);
                }
            }
            62 => {
                let Some(&to) = chain.get(&w[1]) else {
                    continue;
                };
                if k < call && class.get(&to) == Some(&4) && zeros.contains(&w[2]) {
                    zeroed.push(to);
                }
                if k > call
                    && class.get(&to) == Some(&12)
                    && let Some(&from) = loaded.get(&w[2])
                    && class.get(&from) == Some(&4)
                {
                    copied.push(from);
                }
            }
            _ => {}
        }
    }
    (zeroed, copied)
}

/// Workgroup memory starts undefined, so the compute back end zeroes the
/// stage buffers a task or mesh record copies: a program that leaves
/// outputs unwritten stores the same record on every host.
#[test]
fn the_compute_back_end_zeroes_the_stage_buffers_its_records_copy() {
    for c in cases() {
        for (k, p) in &c.programs {
            if k == "frag" {
                continue;
            }
            for host in HostProfile::presets() {
                let s = translate_with(p, &options(host.clone(), &c.pipe, true)).unwrap();
                let (zeroed, copied) = zeroed_and_copied(&s.spirv);
                assert!(!copied.is_empty(), "{} {k} on {}", c.name, host.name);
                for v in copied {
                    assert!(
                        zeroed.contains(&v),
                        "{} {k} on {}: %{v} is copied but not zeroed",
                        c.name,
                        host.name
                    );
                }
            }
        }
    }
}

/// What each store to `CullPrimitiveEXT` writes: `Some(constant)` for an
/// `OpConstantTrue` or `OpConstantFalse`, `None` for a computed bool.
fn cull_stores(words: &[u32]) -> Vec<Option<bool>> {
    let ins = instructions(words);
    let consts: std::collections::HashMap<u32, bool> = ins
        .iter()
        .filter(|(o, _)| *o == 41 || *o == 42)
        .map(|(o, w)| (w[2], *o == 41))
        .collect();
    let var = ins
        .iter()
        .find(|(o, w)| *o == 71 && w[2] == 11 && w[3] == 5299)
        .map(|(_, w)| w[1])
        .expect("a CullPrimitiveEXT variable");
    let ptrs: Vec<u32> = ins
        .iter()
        .filter(|(o, w)| *o == 65 && w[3] == var)
        .map(|(_, w)| w[2])
        .collect();
    ins.iter()
        .filter(|(o, w)| *o == 62 && ptrs.contains(&w[1]))
        .map(|(_, w)| consts.get(&w[2]).copied())
        .collect()
}

/// lavapipe's LLVM back end aborts on a computed `CullPrimitiveEXT`, so its
/// profile stores constants under a branch; the other mesh hosts store the
/// bool directly.
#[test]
fn lavapipe_writes_the_cull_flag_with_constant_stores() {
    let c = cases().into_iter().find(|c| c.name == "ext-cull").unwrap();
    let mesh = &c.programs.iter().find(|(k, _)| k == "mesh").unwrap().1;
    let stores = |host: HostProfile| {
        let s = translate_with(mesh, &options(host, &c.pipe, false)).unwrap();
        assert_eq!(s.interface.host.mesh, MeshPath::Native);
        cull_stores(&s.spirv)
    };
    let lvp = stores(HostProfile::lavapipe());
    assert_eq!(lvp, [Some(true), Some(false)]);
    let nv = stores(HostProfile::nvidia());
    assert_eq!(nv, [None]);
}

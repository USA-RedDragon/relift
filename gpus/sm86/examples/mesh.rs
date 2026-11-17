//! Translates a task or mesh program, and writes the compute back end's
//! helper passes for a mesh program.
//!
//! `mesh <program> <out.spv> [host] [--compute] [pipeline...]`
//! `mesh helpers <mesh program> <dir> [pipeline...]` writes
//! `scan-tasks.spv` and `vertex.spv`.
//!
//! Pipeline flags: `--verts N`, `--prims N`, `--topo point|line|tri`,
//! `--task-lanes N`, `--mesh-lanes N`, `--payload BYTES`, `--prim-sph FILE`,
//! `--prim-id-loc N`, `--frag-per-prim MASK`.

use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::mesh_fallback::{mesh_record_words, scan, vertex};
use relift_gpu_sm86::translate::{
    HostProfile, MeshPipeline, MeshTopology, Options, mesh_components, translate_with,
};

fn write(path: &str, words: &[u32]) {
    let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    relift_cache::write_if_changed(std::path::Path::new(path), &bytes)
        .unwrap_or_else(|e| panic!("{path}: {e}"));
}

fn main() {
    let mut p = MeshPipeline::default();
    let mut compute = false;
    let mut rest = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut num = || -> u32 {
            let v = args.next().expect("flag value");
            v.parse().unwrap_or_else(|_| panic!("{a} {v}"))
        };
        match a.as_str() {
            "--verts" => p.max_vertices = num(),
            "--prims" => p.max_primitives = num(),
            "--task-lanes" => p.task_lanes = num(),
            "--mesh-lanes" => p.mesh_lanes = num(),
            "--payload" => p.payload_bytes = num(),
            "--prim-sph" => {
                let f = args.next().expect("primitive SPH file");
                let b = std::fs::read(&f).unwrap_or_else(|e| panic!("{f}: {e}"));
                let mut w = [0u32; 32];
                for (k, x) in w.iter_mut().enumerate() {
                    *x = u32::from_le_bytes(b[4 * k..4 * k + 4].try_into().expect("128 bytes"));
                }
                p.primitive_sph = Some(w);
            }
            "--prim-id-loc" => p.primitive_id_location = Some(num()),
            "--frag-per-prim" => p.fragment_per_primitive = u64::from(num()),
            "--compute" => compute = true,
            "--topo" => {
                p.topology = match args.next().as_deref() {
                    Some("point") => MeshTopology::Points,
                    Some("line") => MeshTopology::Lines,
                    _ => MeshTopology::Triangles,
                }
            }
            _ => rest.push(a),
        }
    }
    let helpers = rest[0] == "helpers";
    let src = &rest[usize::from(helpers)];
    let b = std::fs::read(src).unwrap_or_else(|e| panic!("{src}: {e}"));
    let prog = Program::parse(&b);
    if helpers {
        let sph = prog.sph.as_ref().expect("a mesh program has an SPH").words;
        let n = mesh_components(&sph).expect("mesh outputs").len() as u32;
        let dir = &rest[2];
        write(&format!("{dir}/scan-tasks.spv"), &scan());
        write(
            &format!("{dir}/vertex.spv"),
            &vertex(&p, &sph).expect("vertex shader"),
        );
        println!(
            "ok {n} components {} record words",
            mesh_record_words(&p, n)
        );
        return;
    }
    let mut o = Options::default();
    if let Some(n) = rest.get(2) {
        o.host = HostProfile::preset(n).unwrap_or_else(|| panic!("no host preset {n}"));
    }
    o.pipeline.mesh = p;
    o.mesh_compute = compute;
    match translate_with(&prog, &o) {
        Ok(s) => {
            write(&rest[1], &s.spirv);
            println!(
                "ok {:?} {:?} {:?} inexact {:?}",
                s.stage, s.interface.host.mesh, s.interface.host.warp, s.interface.host.inexact
            );
        }
        Err(e) => println!("unsupported {e}"),
    }
}

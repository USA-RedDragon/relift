//! The coverage example translates task and mesh programs with the
//! pipeline `meshes.json` pairs them in, as relift does, instead of
//! counting them as failures.

#[path = "../examples/coverage/meshes.rs"]
mod meshes;

use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::{MeshPath, translate_with};

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// The task and mesh programs of the nv-small known answer case.
fn programs() -> (Vec<u8>, Vec<u8>) {
    let text = include_str!("kat/mesh.txt");
    let mut case = false;
    let (mut task, mut mesh) = (Vec::new(), Vec::new());
    for line in text.lines() {
        if let Some(name) = line.strip_prefix("case ") {
            case = name == "nv-small";
        } else if let Some(rest) = line.strip_prefix("program ").filter(|_| case) {
            let (k, h) = rest.split_once(' ').unwrap();
            let b: Vec<u8> = (0..h.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(&h[i..i + 2], 16).unwrap())
                .collect();
            match k {
                "task" => task = b,
                "mesh" => mesh = b,
                _ => {}
            }
        }
    }
    (task, mesh)
}

#[test]
fn paired_task_and_mesh_programs_translate_with_their_pipeline() {
    let dir = std::path::Path::new(env!("CARGO_TARGET_TMPDIR")).join("coverage-meshes");
    let mut mesh = vec![0u8; 0xe0];
    mesh[0x56] = 7;
    mesh[0x58..0x5c].copy_from_slice(&0x0030_0008u32.to_le_bytes());
    mesh[0x5c..0x60].copy_from_slice(&0x0300_0000u32.to_le_bytes());
    mesh[0x60..0x64].copy_from_slice(&0x0001_0003u32.to_le_bytes());
    let mut task = vec![0u8; 0xe0];
    task[0x56] = 6;
    task[0x58..0x5c].copy_from_slice(&0x0000_0010u32.to_le_bytes());
    let json = format!(
        r#"[{{"task": "t", "mesh": "m", "fragment": "f", "task_glslc": "{}", "mesh_glslc": "{}"}}]"#,
        hex(&task),
        hex(&mesh)
    );
    relift_cache::write_if_changed(&dir.join("meshes.json"), json.as_bytes()).unwrap();
    let pipelines = meshes::read(&dir);
    assert_eq!(pipelines.len(), 2);
    let (t, m) = programs();
    for (sha, b) in [("t", &t), ("m", &m)] {
        let p = Program::parse(b);
        let o = meshes::options(&pipelines, sha);
        assert_eq!(
            (
                o.pipeline.mesh.task_lanes,
                o.pipeline.mesh.mesh_lanes,
                o.pipeline.mesh.payload_bytes
            ),
            (16, 8, 48)
        );
        assert_eq!(
            (o.pipeline.mesh.max_vertices, o.pipeline.mesh.max_primitives),
            (3, 1)
        );
        let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{sha}: {}", e.0));
        assert_eq!(s.interface.host.mesh, MeshPath::Native);
        let alone = translate_with(&p, &meshes::options(&pipelines, "other")).unwrap();
        assert_ne!(
            alone.spirv, s.spirv,
            "{sha} translated the same without its pipeline"
        );
    }
}

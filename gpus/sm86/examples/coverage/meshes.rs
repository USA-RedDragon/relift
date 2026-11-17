//! The task and mesh pipelines of a `relift-shaders scan` directory
//! (`meshes.json`), so that task and mesh programs translate with their
//! pipeline as relift translates them.

use relift_gpu_sm86::translate::{MeshPipeline, Options};
use std::collections::HashMap;
use std::path::Path;

const GLSLC_PRIMITIVE_SPH: usize = 0x64;

fn unhex(s: &str) -> Option<Vec<u8>> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok())
        .collect()
}

/// The pipeline of every task and mesh program that `meshes.json` in `dir`
/// pairs, by SHA-256 (hex); the first pipeline a program is in wins. Empty
/// without a `meshes.json`.
pub fn read(dir: &Path) -> HashMap<String, MeshPipeline> {
    let mut out = HashMap::new();
    let path = dir.join("meshes.json");
    let Ok(b) = std::fs::read(&path) else {
        return out;
    };
    let v: serde_json::Value =
        serde_json::from_slice(&b).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    for e in v.as_array().into_iter().flatten() {
        let bytes = |k: &str| e.get(k).and_then(|x| x.as_str()).and_then(unhex);
        let Some(mesh) = bytes("mesh_glslc") else {
            continue;
        };
        // from_glslc finds the primitive SPH in the mesh code at the offset
        // the header names; meshes.json holds only the SPH itself.
        let mut code = Vec::new();
        if let (Some(sph), Some(off)) = (
            bytes("primitive_sph"),
            mesh.get(GLSLC_PRIMITIVE_SPH..GLSLC_PRIMITIVE_SPH + 4),
        ) {
            let off = u32::from_le_bytes(off.try_into().expect("four bytes")) as usize;
            code.resize(off, 0);
            code.extend_from_slice(&sph);
        }
        let Some(p) = MeshPipeline::from_glslc(bytes("task_glslc").as_deref(), &mesh, &code) else {
            continue;
        };
        for k in ["task", "mesh"] {
            if let Some(sha) = e.get(k).and_then(|x| x.as_str()) {
                out.entry(sha.to_string()).or_insert_with(|| p.clone());
            }
        }
    }
    out
}

/// The translation options for program `sha`: with its pipeline when it is
/// a task or mesh program.
pub fn options(pipelines: &HashMap<String, MeshPipeline>, sha: &str) -> Options {
    let mut o = Options::default();
    if let Some(p) = pipelines.get(sha) {
        o.pipeline.mesh = p.clone();
    }
    o
}

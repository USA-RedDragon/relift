//! The task and mesh pipelines of a game's programs.
//!
//! A task or mesh program's pipeline state (workgroup size, payload size,
//! output limits and topology, the primitive SPH) is not in its code but in
//! the GLSLC header NVN takes beside it, which the scan of the game's files
//! keeps in `meshes.json` (see `relift_switch_shaders`). [`Pipelines`] pairs
//! each program of the file with the pipeline it runs in, for the host the
//! table is built for, and gives the translator's options of every program
//! the pipeline reaches: the task and mesh programs, and the fragment
//! program, which the back end decides how it reads the primitive's
//! outputs.
//!
//! A compute back end draw needs two modules the programs do not hold, the
//! scan of the task counts and the vertex program that draws the records
//! ([`helpers`]); they go in the table under keys made from the mesh
//! program's own.

use std::collections::HashMap;
use std::path::Path;

use relift_gpu_nv::translate::mesh_fallback::{indexed, mesh_record_words, scan, vertex};
use relift_gpu_nv::translate::{
    HostProfile, MeshPath, MeshPipeline, MeshTopology, Options, mesh_components,
};

use super::hex_key;

/// The scan's file of pipelines, in the shader folder.
pub(super) const FILE: &str = "meshes.json";

/// Where a program is in a task and mesh pipeline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Role {
    Task,
    Mesh,
    Fragment,
}

/// What the runtime needs to draw a pipeline's programs, besides the
/// programs.
#[derive(Clone, Debug)]
pub(super) struct Shape {
    /// The pipeline, with what the back end and the fragment program agree
    /// on (`MeshPipeline::for_host`).
    pub pipeline: MeshPipeline,
    /// The mesh program's SPH.
    pub sph: [u32; 32],
}

/// A program of a pipeline.
#[derive(Clone, Debug)]
pub(super) struct Member {
    pub role: Role,
    pub shape: Shape,
}

/// Every program of the pipelines in a folder, by key.
#[derive(Default)]
pub(super) struct Pipelines {
    members: HashMap<[u8; 32], Member>,
}

fn unhex(s: &str) -> Option<Vec<u8>> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok())
        .collect()
}

/// The offset in a GLSLC header of the primitive SPH's offset in the code.
const GLSLC_PRIMITIVE_SPH: usize = 0x64;

impl Pipelines {
    /// The pipelines `dir`/meshes.json names, for `host`. A pipeline whose
    /// mesh program the folder lacks, or whose state the translation does
    /// not take, is left out (its programs then translate with the
    /// defaults, or fail as they do).
    ///
    /// # Errors
    ///
    /// The file cannot be read as JSON.
    pub(super) fn read(
        dir: &Path,
        host: &HostProfile,
    ) -> Result<Pipelines, crate::import::BoxError> {
        let mut out = Pipelines::default();
        let path = dir.join(FILE);
        let Ok(bytes) = std::fs::read(&path) else {
            return Ok(out);
        };
        let v: serde_json::Value =
            serde_json::from_slice(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
        for e in v.as_array().into_iter().flatten() {
            let hexes = |k: &str| e.get(k).and_then(|x| x.as_str()).and_then(unhex);
            let key = |k: &str| {
                e.get(k)
                    .and_then(|x| x.as_str())
                    .and_then(|s| hex_key(&format!("{s}.bin")))
            };
            let (Some(mesh_glslc), Some(mesh_key)) = (hexes("mesh_glslc"), key("mesh")) else {
                continue;
            };
            let Ok(code) = std::fs::read(
                dir.join("programs")
                    .join(format!("{}.bin", super::hex(&mesh_key))),
            ) else {
                continue;
            };
            let prog = relift_gpu_sm86::program::parse(&code);
            let Some(sph) = prog.sph.as_ref().map(|s| s.words) else {
                continue;
            };
            let mut primitive_code = Vec::new();
            if let (Some(prim), Some(off)) = (
                hexes("primitive_sph"),
                mesh_glslc.get(GLSLC_PRIMITIVE_SPH..GLSLC_PRIMITIVE_SPH + 4),
            ) {
                let off = u32::from_le_bytes(off.try_into().expect("four bytes")) as usize;
                primitive_code.resize(off, 0);
                primitive_code.extend_from_slice(&prim);
            }
            let Some(p) = MeshPipeline::from_glslc(
                hexes("task_glslc").as_deref(),
                &mesh_glslc,
                &primitive_code,
            ) else {
                continue;
            };
            let Ok((_, pipeline)) = p.for_host(host, &sph) else {
                continue;
            };
            let shape = Shape { pipeline, sph };
            for (name, role) in [
                ("task", Role::Task),
                ("mesh", Role::Mesh),
                ("fragment", Role::Fragment),
            ] {
                if let Some(k) = key(name) {
                    out.members.entry(k).or_insert_with(|| Member {
                        role,
                        shape: shape.clone(),
                    });
                }
            }
        }
        Ok(out)
    }

    /// The pipeline member `key` is, if any.
    pub(super) fn member(&self, key: &[u8; 32]) -> Option<&Member> {
        self.members.get(key)
    }

    /// The translation options of program `key`: the defaults, with its
    /// pipeline when it is in one.
    pub(super) fn options(&self, key: &[u8; 32]) -> Options {
        let mut o = Options::default();
        if let Some(m) = self.members.get(key) {
            o.pipeline.mesh = m.shape.pipeline.clone();
        }
        o
    }
}

/// The interface record of a task or mesh program: the stage (6 or 7),
/// whether the native back end draws it (else the compute one), whether
/// the compute back end's draw is indexed, the guest workgroups per
/// compute workgroup, and the topology (1 points, 2 lines, 3 triangles).
pub(super) const RECORD_MESH: u32 = 4;
/// The interface record of the limits of a pipeline: the vertices and
/// primitives a mesh workgroup outputs, and the stage buffer's words.
pub(super) const RECORD_MESH_LIMITS: u32 = 5;
/// The interface record of the compute back end's buffers: the words of a
/// mesh record, the words of a mesh's indices, and the workgroup sizes of
/// the task (low half) and mesh (high half) programs.
pub(super) const RECORD_MESH_RECORD: u32 = 6;

/// The records of a task or mesh program `m` translated for back end `path`
/// with `mesh_slots`.
pub(super) fn records(m: &Member, path: MeshPath, mesh_slots: u32) -> Vec<[u32; 4]> {
    let p = &m.shape.pipeline;
    let stage = match m.role {
        Role::Task => 6,
        Role::Mesh => 7,
        Role::Fragment => return Vec::new(),
    };
    let components = mesh_components(&m.shape.sph).map_or(0, |c| c.len() as u32);
    let topology = match p.topology {
        MeshTopology::Points => 1,
        MeshTopology::Lines => 2,
        MeshTopology::Triangles => 3,
    };
    let native = path == MeshPath::Native;
    vec![
        [
            RECORD_MESH,
            stage | u32::from(native) << 8 | u32::from(indexed(p)) << 9,
            mesh_slots,
            topology,
        ],
        [
            RECORD_MESH_LIMITS,
            p.max_vertices,
            p.max_primitives,
            p.payload_words(),
        ],
        [
            RECORD_MESH_RECORD,
            mesh_record_words(p, components),
            p.index_words(),
            p.task_lanes | p.mesh_lanes << 16,
        ],
    ]
}

/// The key of a helper module of mesh program `key`.
fn helper_key(kind: &[u8], key: &[u8; 32]) -> [u8; 32] {
    let mut b = kind.to_vec();
    b.extend_from_slice(key);
    relift_switch_formats::sha256(&b)
}

/// What the key of the scan of the task counts of mesh program `key` is
/// the SHA-256 of, ahead of `key`.
const SCAN: &[u8] = b"relift mesh scan";
/// What the key of the vertex program that draws the records of mesh
/// program `key` is the SHA-256 of, ahead of `key`.
const VERTEX: &[u8] = b"relift mesh vertex";

/// A helper module: its key and SPIR-V words.
pub(super) type Helper = ([u8; 32], Vec<u32>);

/// The helper modules of the compute back end's draw of mesh program
/// `key`, translated for `host` with `mesh_slots`: the scan and the
/// vertex program, by key.
pub(super) fn helpers(
    key: &[u8; 32],
    m: &Member,
    host: &HostProfile,
    mesh_slots: u32,
) -> Result<Vec<Helper>, String> {
    let words = vertex(&m.shape.pipeline, &m.shape.sph).map_err(|e| e.0)?;
    Ok(vec![
        (helper_key(SCAN, key), scan(host, mesh_slots)),
        (helper_key(VERTEX, key), words),
    ])
}

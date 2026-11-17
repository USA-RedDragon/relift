//! Task and mesh programs.
//!
//! NVIDIA types a task program as a vertex program and a mesh program as a
//! tessellation evaluation program, or as a vertex program when no task
//! program runs before it; all have SPH word 4 `0xff000000` (see
//! `Sph::isbe_program`), and a mesh program is the one that writes mesh
//! outputs (`Program::stage`). A workgroup is one warp: lane `n` is local
//! invocation `n`, and a workgroup of fewer than 32 invocations
//! ([`MeshPipeline::task_lanes`], [`MeshPipeline::mesh_lanes`]) runs only
//! its first lanes. The stages talk through the stage buffer (ISBE), which
//! `ISBERD` reads and `ISBEWR` writes, as measured on an RTX 3090 with
//! programs the NVIDIA driver compiled from GLSL (`NV_mesh_shader` and
//! `EXT_mesh_shader`). `SR_LANEID` is the local invocation index.
//!
//! - A task program writes its output with `ISBEWR.O.ATTR.32 [a]` (byte
//!   address `a`): word 1 (`[0x4]`) is the number of mesh workgroups it
//!   launches, the payload follows the header (at `0x10` or `0x20`
//!   depending on the compiler; `EXT_mesh_shader` also puts the x, y and z
//!   counts at `[0x8]`, `[0xc]` and `[0x10]`, of which word 1 is the
//!   product). A task that writes no count launches none. `ISBERD.O.ATTR`
//!   reads its own output back, and `ISBERD.ATTR.SKEW.32 [RZ]` gives the
//!   task's linear workgroup index.
//! - A mesh program reads the output of the task that launched it with
//!   `ISBERD.ATTR [a]`, except that word 0 reads the mesh's index among the
//!   workgroups that task launched (its own workgroup index when no task
//!   program runs).
//! - A mesh program writes per-vertex outputs with
//!   `ISBEWR.O.ATTR.SKEW.32 [a]`: the vertices come in blocks of 32, each
//!   block holding one 32-word row per output component, so component `k`
//!   of vertex `v` is at `0x80 (n (v / 32) + k) + 4 (v % 32)` for `n`
//!   components. The components are the ones the SPH output map names
//!   (point size, position, generic locations, clip distances), in address
//!   order. Per-primitive outputs follow the per-vertex blocks in the same
//!   form; past them the program may keep scratch bytes of its own
//!   (`ISBEWR.O.ATTR [a]` with 8, 16 or 32-bit size).
//! - `ISBEWR.O.32 [0x3]` (map access) sets the primitive count and
//!   `ISBEWR.O [4 + i]` byte `i` of the flat primitive index array (three
//!   indices per triangle, two per line, one per point). The vertex count
//!   is not written: the hardware takes the vertices the indices name.
//!
//! The maximum vertex and primitive counts, the output topology, the
//! workgroup and payload sizes and the per-primitive outputs are not in the
//! SPH; NVN takes them from the GLSLC program header beside the code, and
//! they come from [`MeshPipeline`] (see [`MeshPipeline::from_glslc`]).
//!
//! The translation keeps the stage buffer: the output of a task program,
//! and the attribute and map output of a mesh program, are workgroup
//! arrays the guest's `ISBEWR` and `ISBERD` address as the hardware does.
//! The program runs as a function; the entry point (a workgroup of the
//! guest's size) zeroes the counts, calls it (on invocation 0 alone when
//! it runs the whole warp in software, `WarpPath::Simt`), waits for the
//! workgroup and then hands the buffer on, through one of two back ends
//! ([`MeshPath`]):
//!
//! - Native (`SPV_EXT_mesh_shader`): a task workgroup launches
//!   `OpEmitMeshTasksEXT(count, 1, 1)` with the stage buffer as its
//!   payload; a mesh workgroup sets `maximum vertices` and the primitive
//!   count and copies the vertex components and indices to its outputs.
//! - Compute: task and mesh programs run as compute shaders over a storage
//!   buffer (see [`fallback`]): each task workgroup stores its stage
//!   buffer, a scan turns the counts into the mesh dispatch, each mesh
//!   workgroup stores its primitive count, indices and vertices in its
//!   record, a second scan gives the draw, and a vertex shader draws every
//!   primitive in order (task, then mesh, then primitive index), so
//!   rasterization order and blending match the native path.

use super::super::{Ctx, INPUT, OUTPUT, Res, bad};
use crate::program::Stage;
use crate::spirv::op;
use crate::{Inst, Word};

pub mod fallback;

const CAP_MESH_SHADING: u32 = 5283;
const CAP_CLIP_DISTANCE: u32 = 32;
const MODEL_TASK: u32 = 5364;
const MODEL_MESH: u32 = 5365;
const MODEL_COMPUTE: u32 = 5;
const MODE_LOCAL_SIZE: u32 = 17;
const MODE_OUTPUT_VERTICES: u32 = 26;
const MODE_OUTPUT_POINTS: u32 = 27;
const MODE_OUTPUT_LINES: u32 = 5269;
const MODE_OUTPUT_PRIMITIVES: u32 = 5270;
const MODE_OUTPUT_TRIANGLES: u32 = 5298;
const OP_EMIT_MESH_TASKS: u16 = 5294;
const OP_SET_MESH_OUTPUTS: u16 = 5295;
const OP_FUNCTION_CALL: u16 = 57;
const OP_ATOMIC_AND: u16 = 240;
const OP_ATOMIC_OR: u16 = 241;
const TASK_PAYLOAD: u32 = 5402;
const WORKGROUP: u32 = 4;
const PRIVATE: u32 = 6;
const B_POSITION: u32 = 0;
const B_POINT_SIZE: u32 = 1;
const B_CLIP_DISTANCE: u32 = 3;
const B_NUM_WORKGROUPS: u32 = 24;
const B_WORKGROUP_ID: u32 = 26;
const B_LOCAL_INVOCATION_INDEX: u32 = 29;
const B_PRIMITIVE_ID: u32 = 7;
const B_LAYER: u32 = 9;
const B_VIEWPORT_INDEX: u32 = 10;
const B_CULL_PRIMITIVE: u32 = 5299;
const DECO_PER_PRIMITIVE: u32 = 5271;
const CAP_MULTI_VIEWPORT: u32 = 57;
const B_POINT_INDICES: u32 = 5294;
const B_LINE_INDICES: u32 = 5295;
const B_TRIANGLE_INDICES: u32 = 5296;
const SCOPE_WORKGROUP: u32 = 2;
const SEM_ACQUIRE_RELEASE: u32 = 0x8;
const SEM_WORKGROUP_MEMORY: u32 = 0x100;

/// Byte offset in driver constant bank 0 where the runtime puts the first
/// task of a draw (`DrawMeshTasks(first, count)`): the hardware's workgroup
/// index of the root stage (task, or mesh without task) counts from it.
pub const MESH_FIRST_TASK_OFFSET: u32 = 0x1f4;

/// The primitives a mesh program outputs.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum MeshTopology {
    Points,
    Lines,
    #[default]
    Triangles,
}

impl MeshTopology {
    /// Indices per primitive.
    pub fn vertices(self) -> u32 {
        match self {
            MeshTopology::Points => 1,
            MeshTopology::Lines => 2,
            MeshTopology::Triangles => 3,
        }
    }
}

/// The task and mesh pipeline state the SPH does not carry. NVN keeps it
/// in the GLSLC program header of each stage (see
/// [`MeshPipeline::from_glslc`]).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MeshPipeline {
    /// `max_vertices` of the mesh program, at most 256.
    pub max_vertices: u32,
    /// `max_primitives` of the mesh program, at most 256.
    pub max_primitives: u32,
    pub topology: MeshTopology,
    /// Invocations of a task workgroup (`local_size_x`), at most 32.
    pub task_lanes: u32,
    /// Invocations of a mesh workgroup, at most 32.
    pub mesh_lanes: u32,
    /// Bytes of the stage buffer a task program writes and its mesh
    /// programs read (header and payload), a multiple of 4.
    pub payload_bytes: u32,
    /// The primitive SPH of a mesh program: its output map (words 13 on)
    /// names the per-primitive outputs, which follow the per-vertex ones in
    /// the stage buffer. None: no per-primitive outputs.
    pub primitive_sph: Option<[u32; 32]>,
    /// Compute back end: the location of a flat integer the vertex shader
    /// passes the per-primitive PrimitiveId in, and the fragment program
    /// reads `gl_PrimitiveID` from (the draw's own primitive index is not
    /// the mesh program's).
    pub primitive_id_location: Option<u32>,
    /// Native back end: the generic locations (bit `n` for location `n`)
    /// that are per-primitive, which the fragment program must declare
    /// `PerPrimitiveEXT`.
    pub fragment_per_primitive: u64,
}

impl Default for MeshPipeline {
    /// The largest pipeline: what any program fits in, though a host may
    /// not have the shared memory for its stage buffers.
    fn default() -> MeshPipeline {
        MeshPipeline {
            max_vertices: 256,
            max_primitives: 256,
            topology: MeshTopology::Triangles,
            task_lanes: 32,
            mesh_lanes: 32,
            payload_bytes: 16384,
            primitive_sph: None,
            primitive_id_location: None,
            fragment_per_primitive: 0,
        }
    }
}

/// GLSLC program header: the stage byte, task 6 and mesh 7.
const GLSLC_STAGE: usize = 0x56;
const GLSLC_TASK: u8 = 6;
const GLSLC_MESH: u8 = 7;
/// Workgroup size (low half) and stage buffer bytes (high half).
const GLSLC_WORKGROUP: usize = 0x58;
/// Byte 3: output topology (1 points, 2 lines, 3 triangles).
const GLSLC_TOPOLOGY: usize = 0x5c;
/// Maximum vertices (low half) and primitives (high half).
const GLSLC_LIMITS: usize = 0x60;
/// Offset and size of the primitive SPH in the stage's code.
const GLSLC_PRIMITIVE_SPH: usize = 0x64;

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?))
}

impl MeshPipeline {
    /// The pipeline of a mesh program from the GLSLC headers of its stages
    /// (`mesh`, and `task` when one runs before it) and the mesh stage's
    /// code (the primitive SPH lives there).
    pub fn from_glslc(task: Option<&[u8]>, mesh: &[u8], mesh_code: &[u8]) -> Option<MeshPipeline> {
        if *mesh.get(GLSLC_STAGE)? != GLSLC_MESH {
            return None;
        }
        let wg = u32_at(mesh, GLSLC_WORKGROUP)?;
        let topology = match u32_at(mesh, GLSLC_TOPOLOGY)? >> 24 {
            1 => MeshTopology::Points,
            2 => MeshTopology::Lines,
            3 => MeshTopology::Triangles,
            _ => return None,
        };
        let limits = u32_at(mesh, GLSLC_LIMITS)?;
        let mut p = MeshPipeline {
            max_vertices: limits & 0xffff,
            max_primitives: limits >> 16,
            topology,
            mesh_lanes: wg & 0xffff,
            payload_bytes: wg >> 16,
            ..MeshPipeline::default()
        };
        if let Some(t) = task {
            if *t.get(GLSLC_STAGE)? != GLSLC_TASK {
                return None;
            }
            p.task_lanes = u32_at(t, GLSLC_WORKGROUP)? & 0xffff;
        }
        let off = u32_at(mesh, GLSLC_PRIMITIVE_SPH)? as usize;
        let len = u32_at(mesh, GLSLC_PRIMITIVE_SPH + 4)? as usize;
        if len == 0x80 {
            let b = mesh_code.get(off..off + len)?;
            let mut w = [0u32; 32];
            for (k, x) in w.iter_mut().enumerate() {
                *x = u32_at(b, 4 * k)?;
            }
            p.primitive_sph = Some(w);
        }
        Some(p)
    }

    /// Words of the task stage buffer.
    pub fn payload_words(&self) -> u32 {
        (self.payload_bytes / 4).max(2)
    }

    /// Words of the flat index array.
    pub fn index_words(&self) -> u32 {
        self.max_primitives * self.topology.vertices()
    }

    /// Words of the per-vertex output blocks for `components` components
    /// per vertex.
    pub fn vertex_words(&self, components: u32) -> u32 {
        self.max_vertices.div_ceil(32) * 32 * components
    }

    /// Words of the per-primitive output blocks for `components`
    /// components per primitive.
    pub fn primitive_words(&self, components: u32) -> u32 {
        self.max_primitives.div_ceil(32) * 32 * components
    }

    /// The per-primitive outputs, in stage buffer order.
    pub fn primitive_components(&self) -> Res<Vec<PrimComponent>> {
        let Some(sph) = &self.primitive_sph else {
            return Ok(Vec::new());
        };
        let mut out = Vec::new();
        for b in 0..(19 - 13 + 1) * 32 {
            if (sph[13 + (b / 32) as usize] >> (b % 32)) & 1 == 0 {
                continue;
            }
            let a = 0x40 + 4 * b;
            out.push(match a {
                0x60 => PrimComponent::PrimitiveId,
                0x64 => PrimComponent::Layer,
                0x68 => PrimComponent::ViewportIndex,
                0x80..0x280 => PrimComponent::Generic((a - 0x80) / 16, (a % 16) / 4),
                0x3a0 => PrimComponent::ViewportMask,
                _ => return bad(format!("per-primitive output {a:#x}")),
            });
        }
        Ok(out)
    }

    fn check(&self) -> Res<()> {
        if !(1..=256).contains(&self.max_vertices) || !(1..=256).contains(&self.max_primitives) {
            return bad("mesh output counts outside 1 to 256");
        }
        if !(1..=32).contains(&self.task_lanes) || !(1..=32).contains(&self.mesh_lanes) {
            return bad("task or mesh workgroup outside 1 to 32 invocations");
        }
        if !self.payload_bytes.is_multiple_of(4) || self.payload_bytes > 16384 {
            return bad("task payload not a whole number of words up to 16 KiB");
        }
        Ok(())
    }
}

/// Where a per-primitive component of a mesh program goes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PrimComponent {
    PrimitiveId,
    Layer,
    ViewportIndex,
    /// Generic location and component.
    Generic(u32, u32),
    /// The viewports the primitive goes to; with one viewport, the
    /// primitive is culled when bit 0 is clear.
    ViewportMask,
}

/// How a task or mesh program runs on the host.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum MeshPath {
    /// Not a task or mesh program.
    #[default]
    None,
    /// `VK_EXT_mesh_shader` task and mesh shaders.
    Native,
    /// Compute shaders, scans and a vertex shader (see [`fallback`]).
    Compute,
}

/// Where a per-vertex component of a mesh program goes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Component {
    PointSize,
    /// Position component 0 to 3.
    Position(u32),
    /// Generic location and component.
    Generic(u32, u32),
    /// Clip distance `n`.
    Clip(u32),
}

/// The per-vertex components of a mesh program, in stage buffer order:
/// every word of the SPH output map (words 13 to 18, bit `b` for attribute
/// `0x40 + 4 b`), which must be a per-vertex output.
pub fn mesh_components(sph: &[u32; 32]) -> Res<Vec<Component>> {
    let mut out = Vec::new();
    for b in 0..192u32 {
        if (sph[13 + (b / 32) as usize] >> (b % 32)) & 1 == 0 {
            continue;
        }
        let a = 0x40 + 4 * b;
        out.push(match a {
            0x6c => Component::PointSize,
            0x70..0x80 => Component::Position((a - 0x70) / 4),
            0x80..0x280 => Component::Generic((a - 0x80) / 16, (a % 16) / 4),
            0x2c0..0x2e0 => Component::Clip((a - 0x2c0) / 4),
            0x300..0x340 => Component::Generic(32 + (a - 0x300) / 16, (a % 16) / 4),
            _ => return bad(format!("mesh output {a:#x}")),
        });
    }
    Ok(out)
}

/// The variables of the stage buffer and the back end's own state.
#[derive(Default)]
pub(crate) struct State {
    pub pipe: MeshPipeline,
    pub path: MeshPath,
    /// Task output (task) or input (mesh) stage buffer.
    payload: Option<u32>,
    /// Mesh attribute output.
    attr: Option<u32>,
    /// Mesh primitive count and indices.
    map: Option<u32>,
    /// The fallback's storage buffer.
    buf: Option<u32>,
    /// Fallback mesh: word offset of the launching task's record.
    task_rec: Option<u32>,
    /// Fallback mesh: the index stage buffer word 0 reads.
    mesh_index: Option<u32>,
    components: Vec<Component>,
    prims: Vec<PrimComponent>,
    /// A mesh program after a task program.
    with_task: bool,
}

fn addr(c: &mut Ctx, w: Word) -> u32 {
    let r = c.load_reg(w.get(24, 8) as u8);
    let k = c.m.c_u32(w.get(40, 16));
    let u = c.u32t();
    c.m.emit(op::I_ADD, u, &[r, k])
}

/// Bits of an access: 8, 16 or 32.
fn size(w: Word) -> Res<u32> {
    match w.get(74, 2) {
        0 => Ok(8),
        1 => Ok(16),
        2 => Ok(32),
        _ => bad("isbe size"),
    }
}

fn name(i: &Inst) -> String {
    let mut n = vec![i.mnemonic];
    n.extend(&i.mods);
    n.join(".")
}

impl Ctx {
    pub(crate) fn mesh_lanes(&self) -> u32 {
        match self.stage {
            Stage::Task => self.io.mesh.pipe.task_lanes,
            _ => self.io.mesh.pipe.mesh_lanes,
        }
    }

    fn native(&self) -> bool {
        self.io.mesh.path == MeshPath::Native
    }

    fn u32_array_var(&mut self, class: u32, len: u32) -> u32 {
        let u = self.u32t();
        let arr = self.m.t_array(u, len);
        let p = self.m.t_ptr(class, arr);
        let var = self.m.global(p, class);
        self.globals.push(var);
        var
    }

    fn payload_var(&mut self) -> u32 {
        if let Some(v) = self.io.mesh.payload {
            return v;
        }
        let n = self.io.mesh.pipe.payload_words();
        let class = if self.native() {
            TASK_PAYLOAD
        } else {
            WORKGROUP
        };
        let v = self.u32_array_var(class, n);
        self.io.mesh.payload = Some(v);
        v
    }

    /// Words of the mesh attribute stage buffer: the per-vertex and
    /// per-primitive blocks, then a block of 32 words per 32 primitives
    /// the program may keep its own bytes in.
    fn attr_words(&self) -> u32 {
        let p = &self.io.mesh.pipe;
        let v = p.vertex_words(self.io.mesh.components.len() as u32);
        v + p.primitive_words(self.io.mesh.prims.len() as u32 + 1)
    }

    fn attr_var(&mut self) -> u32 {
        if let Some(v) = self.io.mesh.attr {
            return v;
        }
        let n = self.attr_words().max(1);
        let v = self.u32_array_var(WORKGROUP, n);
        self.io.mesh.attr = Some(v);
        v
    }

    fn map_var(&mut self) -> u32 {
        if let Some(v) = self.io.mesh.map {
            return v;
        }
        let n = 1 + self.io.mesh.pipe.index_words();
        let v = self.u32_array_var(WORKGROUP, n);
        self.io.mesh.map = Some(v);
        v
    }

    /// The fallback's storage buffer: a runtime array of words.
    pub(crate) fn mesh_buffer(&mut self) -> u32 {
        if let Some(v) = self.io.mesh.buf {
            return v;
        }
        let v = fallback::buffer_var(&mut self.m);
        self.globals.push(v);
        self.io.mesh.buf = Some(v);
        v
    }

    fn private_u32(&mut self) -> u32 {
        let u = self.u32t();
        let p = self.m.t_ptr(PRIVATE, u);
        let var = self.m.global(p, PRIVATE);
        self.globals.push(var);
        var
    }

    fn elem_ptr(&mut self, var: u32, class: u32, idx: u32) -> u32 {
        let u = self.u32t();
        let p = self.m.t_ptr(class, u);
        self.m.emit(op::ACCESS_CHAIN, p, &[var, idx])
    }

    /// Word `idx` of a stage buffer array of `len` words, 0 past its end.
    fn read_word(&mut self, var: u32, class: u32, idx: u32, len: u32) -> u32 {
        let u = self.u32t();
        let b = self.boolt();
        let n = self.m.c_u32(len);
        let inb = self.m.emit(op::U_LESS_THAN, b, &[idx, n]);
        let zero = self.m.c_u32(0);
        let at = self.select(u, inb, idx, zero);
        let p = self.elem_ptr(var, class, at);
        let v = self.m.emit(op::LOAD, u, &[p]);
        self.select(u, inb, v, zero)
    }

    /// Stores word `idx` of an array of `len` words; past its end the
    /// store is dropped.
    fn write_word(&mut self, var: u32, class: u32, idx: u32, len: u32, v: u32) {
        let b = self.boolt();
        let n = self.m.c_u32(len);
        let inb = self.m.emit(op::U_LESS_THAN, b, &[idx, n]);
        self.if_then(inb, |c| {
            let p = c.elem_ptr(var, class, idx);
            c.m.emit_void(op::STORE, &[p, v]);
        });
    }

    pub(crate) fn if_then(&mut self, cond: u32, f: impl FnOnce(&mut Ctx)) {
        let then = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[cond, then, merge]);
        self.m.emit_void(op::LABEL, &[then]);
        f(self);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
    }

    /// `for (i = start; i < end; i += step) f(i)` with a function variable
    /// for `i`, which the caller provides.
    pub(crate) fn counted_loop(
        &mut self,
        var: u32,
        start: u32,
        end: u32,
        step: u32,
        f: impl FnOnce(&mut Ctx, u32),
    ) {
        let u = self.u32t();
        let b = self.boolt();
        self.m.emit_void(op::STORE, &[var, start]);
        let header = self.m.id();
        let cond = self.m.id();
        let body = self.m.id();
        let cont = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::BRANCH, &[header]);
        self.m.emit_void(op::LABEL, &[header]);
        self.m.emit_void(op::LOOP_MERGE, &[merge, cont, 0]);
        self.m.emit_void(op::BRANCH, &[cond]);
        self.m.emit_void(op::LABEL, &[cond]);
        let i = self.m.emit(op::LOAD, u, &[var]);
        let go = self.m.emit(op::U_LESS_THAN, b, &[i, end]);
        self.m.emit_void(op::BRANCH_CONDITIONAL, &[go, body, merge]);
        self.m.emit_void(op::LABEL, &[body]);
        f(self, i);
        self.m.emit_void(op::BRANCH, &[cont]);
        self.m.emit_void(op::LABEL, &[cont]);
        let i = self.m.emit(op::LOAD, u, &[var]);
        let n = self.m.emit(op::I_ADD, u, &[i, step]);
        self.m.emit_void(op::STORE, &[var, n]);
        self.m.emit_void(op::BRANCH, &[header]);
        self.m.emit_void(op::LABEL, &[merge]);
    }

    pub(crate) fn workgroup_barrier(&mut self, sem: u32) {
        let s = self.m.c_u32(SCOPE_WORKGROUP);
        let m = self.m.c_u32(SEM_ACQUIRE_RELEASE | sem);
        self.m.emit_void(op::CONTROL_BARRIER, &[s, s, m]);
    }

    fn uvec3_in(&mut self, b: u32, comp: u32) -> u32 {
        let u = self.u32t();
        let v3 = self.m.t_vec(u, 3);
        let var = self.builtin(b, INPUT, v3);
        let p = self.m.t_ptr(INPUT, u);
        let ci = self.m.c_u32(comp);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, ci]);
        self.m.emit(op::LOAD, u, &[ptr])
    }

    /// The linear index of this workgroup in the dispatch.
    fn linear_workgroup(&mut self) -> u32 {
        let u = self.u32t();
        let x = self.uvec3_in(B_WORKGROUP_ID, 0);
        let y = self.uvec3_in(B_WORKGROUP_ID, 1);
        let z = self.uvec3_in(B_WORKGROUP_ID, 2);
        let nx = self.uvec3_in(B_NUM_WORKGROUPS, 0);
        let ny = self.uvec3_in(B_NUM_WORKGROUPS, 1);
        let zy = self.m.emit(op::I_MUL, u, &[z, ny]);
        let zy = self.m.emit(op::I_ADD, u, &[zy, y]);
        let r = self.m.emit(op::I_MUL, u, &[zy, nx]);
        self.m.emit(op::I_ADD, u, &[r, x])
    }

    /// The hardware's index of a workgroup of the stage that starts the
    /// draw: the linear dispatch index plus the draw's first task.
    fn root_index(&mut self, linear: u32) -> Res<u32> {
        let first = self.load_cbuf(0, i64::from(MESH_FIRST_TASK_OFFSET))?;
        let u = self.u32t();
        Ok(self.m.emit(op::I_ADD, u, &[linear, first]))
    }

    fn local_index_in(&mut self) -> u32 {
        let u = self.u32t();
        let var = self.builtin(B_LOCAL_INVOCATION_INDEX, INPUT, u);
        self.m.emit(op::LOAD, u, &[var])
    }

    /// Stage buffer word 0 of a mesh program: its index among the
    /// workgroups its task launched.
    fn mesh_index(&mut self) -> Res<u32> {
        let u = self.u32t();
        if let Some(var) = self.io.mesh.mesh_index {
            return Ok(self.m.emit(op::LOAD, u, &[var]));
        }
        let lin = self.linear_workgroup();
        if self.io.mesh.with_task {
            Ok(lin)
        } else {
            self.root_index(lin)
        }
    }
}

/// ISBERD in a task or mesh program.
pub(crate) fn isberd(c: &mut Ctx, i: &Inst) -> Res<()> {
    let w = c.word;
    let d = Ctx::dst_reg(&i.ops[0])?;
    let (out, skew, class) = (w.bit(79), w.bit(78), w.get(76, 2));
    let bits = size(w)?;
    let v = match (c.stage, out, class, skew) {
        (Stage::Task, false, 3, true) | (Stage::Mesh, false, 3, true) if !c.io.mesh.with_task => {
            if w.get(24, 8) != 255 || w.get(40, 16) != 0 || bits != 32 {
                return bad(format!("{} of a root stage's input", name(i)));
            }
            let lin = c.linear_workgroup();
            c.root_index(lin)?
        }
        (Stage::Task, true, 3, false) if bits == 32 => {
            let a = addr(c, w);
            let wi = c.lk_shr(a, 2);
            let var = c.payload_var();
            let class = if c.native() { TASK_PAYLOAD } else { WORKGROUP };
            let n = c.io.mesh.pipe.payload_words();
            c.read_word(var, class, wi, n)
        }
        (Stage::Mesh, false, 3, false) => {
            let a = addr(c, w);
            let wi = c.lk_shr(a, 2);
            let word = mesh_input_word(c, wi)?;
            let index = c.mesh_index()?;
            let u = c.u32t();
            let b = c.boolt();
            let zero = c.m.c_u32(0);
            let is0 = c.m.emit(op::I_EQUAL, b, &[wi, zero]);
            let word = c.select(u, is0, index, word);
            sub_word(c, word, a, bits)
        }
        (Stage::Mesh, true, 3, _) => {
            let a = addr(c, w);
            let wi = c.lk_shr(a, 2);
            let var = c.attr_var();
            let n = c.attr_words();
            let word = c.read_word(var, WORKGROUP, wi, n);
            sub_word(c, word, a, bits)
        }
        _ => {
            return bad(format!(
                "stage buffer {} in a {:?} program",
                name(i),
                c.stage
            ));
        }
    };
    c.store_reg(d, v);
    Ok(())
}

/// The bits of a sub-word access at byte address `a` of `word`.
fn sub_word(c: &mut Ctx, word: u32, a: u32, bits: u32) -> u32 {
    if bits == 32 {
        return word;
    }
    let u = c.u32t();
    let three = c.m.c_u32(3);
    let byte = c.m.emit(op::BITWISE_AND, u, &[a, three]);
    let s = c.lk_shl(byte, 3);
    let n = c.m.c_u32(bits);
    c.m.emit(op::BIT_FIELD_U_EXTRACT, u, &[word, s, n])
}

/// Stores the low `bits` of `v` at byte address `a` of word `wi` of a
/// workgroup array of `len` words. Lanes may write other bytes of the same
/// word at once, so the store clears and sets its bits atomically.
fn write_sub_word(c: &mut Ctx, var: u32, wi: u32, len: u32, a: u32, bits: u32, v: u32) {
    let b = c.boolt();
    let n = c.m.c_u32(len);
    let inb = c.m.emit(op::U_LESS_THAN, b, &[wi, n]);
    c.if_then(inb, |c| {
        let u = c.u32t();
        let three = c.m.c_u32(3);
        let byte = c.m.emit(op::BITWISE_AND, u, &[a, three]);
        let s = c.lk_shl(byte, 3);
        let m = c.m.c_u32((1u32 << bits) - 1);
        let val = c.m.emit(op::BITWISE_AND, u, &[v, m]);
        let val = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[val, s]);
        let mask = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[m, s]);
        let keep = c.m.emit(op::NOT, u, &[mask]);
        let p = c.elem_ptr(var, WORKGROUP, wi);
        let scope = c.m.c_u32(SCOPE_WORKGROUP);
        let sem = c.m.c_u32(0);
        c.m.emit(OP_ATOMIC_AND, u, &[p, scope, sem, keep]);
        c.m.emit(OP_ATOMIC_OR, u, &[p, scope, sem, val]);
    });
}

/// Word `wi` of the stage buffer the mesh's task wrote (0 without one).
fn mesh_input_word(c: &mut Ctx, wi: u32) -> Res<u32> {
    if !c.io.mesh.with_task {
        return Ok(c.m.c_u32(0));
    }
    let n = c.io.mesh.pipe.payload_words();
    if c.native() {
        let var = c.payload_var();
        return Ok(c.read_word(var, TASK_PAYLOAD, wi, n));
    }
    let buf = c.mesh_buffer();
    let u = c.u32t();
    let rec_var = c.io.mesh.task_rec.expect("fallback mesh task record");
    let rec = c.m.emit(op::LOAD, u, &[rec_var]);
    let b = c.boolt();
    let nk = c.m.c_u32(n);
    let inb = c.m.emit(op::U_LESS_THAN, b, &[wi, nk]);
    let zero = c.m.c_u32(0);
    let at = c.select(u, inb, wi, zero);
    let idx = c.m.emit(op::I_ADD, u, &[rec, at]);
    let v = fallback::load(c, buf, idx);
    Ok(c.select(u, inb, v, zero))
}

/// ISBEWR in a task or mesh program.
pub(crate) fn isbewr(c: &mut Ctx, i: &Inst) -> Res<()> {
    let w = c.word;
    let (out, class) = (w.bit(79), w.get(76, 2));
    let bits = size(w)?;
    if !out {
        return bad(format!("stage buffer {}", name(i)));
    }
    let v = c.load_reg(w.get(32, 8) as u8);
    match (c.stage, class) {
        (Stage::Task, 3) if bits == 32 && !w.bit(78) => {
            let a = addr(c, w);
            let wi = c.lk_shr(a, 2);
            let var = c.payload_var();
            let class = if c.native() { TASK_PAYLOAD } else { WORKGROUP };
            let n = c.io.mesh.pipe.payload_words();
            c.write_word(var, class, wi, n, v);
        }
        (Stage::Mesh, 3) if bits == 32 => {
            let a = addr(c, w);
            let wi = c.lk_shr(a, 2);
            let var = c.attr_var();
            let n = c.attr_words();
            c.write_word(var, WORKGROUP, wi, n, v);
        }
        (Stage::Mesh, 3) if !w.bit(78) => {
            let a = addr(c, w);
            let wi = c.lk_shr(a, 2);
            let var = c.attr_var();
            let n = c.attr_words();
            write_sub_word(c, var, wi, n, a, bits, v);
        }
        (Stage::Mesh, 0) if bits == 32 => {
            if w.get(24, 8) != 255 || w.get(40, 16) != 3 {
                return bad(format!("{} other than the primitive count", name(i)));
            }
            let var = c.map_var();
            let zero = c.m.c_u32(0);
            let p = c.elem_ptr(var, WORKGROUP, zero);
            c.m.emit_void(op::STORE, &[p, v]);
        }
        (Stage::Mesh, 0) if bits == 8 => {
            if w.get(40, 16) < 4 {
                return bad(format!("{} below the index array", name(i)));
            }
            let a = addr(c, w);
            let u = c.u32t();
            let three = c.m.c_u32(3);
            let wi = c.m.emit(op::I_SUB, u, &[a, three]);
            let ff = c.m.c_u32(0xff);
            let byte = c.m.emit(op::BITWISE_AND, u, &[v, ff]);
            let var = c.map_var();
            let n = 1 + c.io.mesh.pipe.index_words();
            c.write_word(var, WORKGROUP, wi, n, byte);
        }
        _ => {
            return bad(format!(
                "stage buffer {} in a {:?} program",
                name(i),
                c.stage
            ));
        }
    }
    Ok(())
}

/// Checks the pipeline and lists the outputs before the body is emitted.
pub(crate) fn prescan(c: &mut Ctx, mesh_compute: bool) -> Res<()> {
    if !c.stage.is_mesh_pipeline() {
        return Ok(());
    }
    c.io.mesh.pipe.check()?;
    if c.stage == Stage::Mesh {
        c.io.mesh.components = mesh_components(&c.sph_words)?;
        c.io.mesh.prims = c.io.mesh.pipe.primitive_components()?;
        c.io.mesh.with_task = (c.sph_words[0] >> 10) & 0xf == 3;
    }
    let shared = if c.stage == Stage::Mesh {
        4 * (c.attr_words() + 1 + c.io.mesh.pipe.index_words())
    } else {
        0
    };
    let fits = c
        .host
        .mesh_shader
        .is_some_and(|l| l.fits(&c.io.mesh.pipe, shared));
    c.io.mesh.path = if mesh_compute || !fits {
        MeshPath::Compute
    } else {
        MeshPath::Native
    };
    c.iface.host.mesh = c.io.mesh.path;
    if c.io.mesh.path == MeshPath::Native {
        c.cap(CAP_MESH_SHADING);
        c.exts.insert("SPV_EXT_mesh_shader");
    }
    if c.io.mesh.path == MeshPath::Compute && c.stage == Stage::Mesh && c.io.mesh.with_task {
        c.io.mesh.task_rec = Some(c.private_u32());
        c.io.mesh.mesh_index = Some(c.private_u32());
    }
    Ok(())
}

/// The entry point of a task or mesh program: it calls `main` (the
/// guest's code) between the stage buffer set-up and the hand-over.
pub(crate) fn entry(c: &mut Ctx, main: u32) -> Res<u32> {
    let void = c.m.t_void();
    let fnt = c.m.t_fn(void);
    let u = c.u32t();
    let b = c.boolt();
    let entry = c.m.id();
    c.m.emit_void(op::FUNCTION, &[void, entry, 0, fnt]);
    let label = c.m.id();
    c.m.emit_void(op::LABEL, &[label]);
    let pu = c.m.t_ptr(super::super::FUNCTION, u);
    let vars: Vec<u32> = (0..3)
        .map(|_| c.m.emit(op::VARIABLE, pu, &[super::super::FUNCTION]))
        .collect();
    let lanes = c.mesh_lanes();
    let lane = c.local_index_in();
    let zero = c.m.c_u32(0);
    let one = c.m.c_u32(1);
    let lane0 = c.m.emit(op::I_EQUAL, b, &[lane, zero]);
    let compute = !c.native();
    if compute {
        fallback::enter(c, lane, &vars)?;
    }
    match c.stage {
        Stage::Task if compute => {
            // Workgroup memory starts undefined and the whole stage buffer
            // goes to the task record: zero it so every host stores the same.
            let var = c.payload_var();
            let n = c.m.c_u32(c.io.mesh.pipe.payload_words());
            let step = c.m.c_u32(lanes);
            c.counted_loop(vars[0], lane, n, step, |c, i| {
                let p = c.elem_ptr(var, WORKGROUP, i);
                c.m.emit_void(op::STORE, &[p, zero]);
            });
        }
        Stage::Task => {
            let var = c.payload_var();
            c.if_then(lane0, |c| {
                let p = c.elem_ptr(var, TASK_PAYLOAD, one);
                c.m.emit_void(op::STORE, &[p, zero]);
            });
        }
        _ => {
            let var = c.map_var();
            c.if_then(lane0, |c| {
                let p = c.elem_ptr(var, WORKGROUP, zero);
                c.m.emit_void(op::STORE, &[p, zero]);
            });
            if compute {
                // The mesh record takes the outputs a program leaves
                // unwritten too: zero them so every host stores the same.
                let attr = c.attr_var();
                fallback::record_outputs(c, vars[0], lane, lanes, |c, _, s| {
                    let p = c.elem_ptr(attr, WORKGROUP, s);
                    c.m.emit_void(op::STORE, &[p, zero]);
                });
            }
        }
    }
    c.workgroup_barrier(SEM_WORKGROUP_MEMORY);
    if c.iface.host.warp == crate::translate::WarpPath::Simt {
        c.if_then(lane0, |c| {
            c.m.emit(OP_FUNCTION_CALL, void, &[main]);
        });
    } else {
        c.m.emit(OP_FUNCTION_CALL, void, &[main]);
    }
    c.workgroup_barrier(SEM_WORKGROUP_MEMORY);
    match (c.stage, compute) {
        (Stage::Task, false) => {
            let var = c.payload_var();
            let p = c.elem_ptr(var, TASK_PAYLOAD, one);
            let n = c.m.emit(op::LOAD, u, &[p]);
            c.m.emit_void(OP_EMIT_MESH_TASKS, &[n, one, one, var]);
        }
        (Stage::Mesh, false) => {
            mesh_outputs(c, lane, lanes, vars[0])?;
            c.m.emit_void(op::RETURN, &[]);
        }
        (_, true) => {
            fallback::leave(c, lane, lanes, &vars)?;
            c.m.emit_void(op::RETURN, &[]);
        }
        _ => unreachable!(),
    }
    c.m.emit_void(op::FUNCTION_END, &[]);
    Ok(entry)
}

/// The primitive count the program set, at most the pipeline's maximum.
pub(crate) fn primitive_count(c: &mut Ctx) -> u32 {
    let u = c.u32t();
    let var = c.map_var();
    let zero = c.m.c_u32(0);
    let p = c.elem_ptr(var, WORKGROUP, zero);
    let n = c.m.emit(op::LOAD, u, &[p]);
    let max = c.m.c_u32(c.io.mesh.pipe.max_primitives);
    let glsl = c.glsl;
    c.m.emit(op::EXT_INST, u, &[glsl, crate::spirv::glsl::U_MIN, n, max])
}

/// Index `k` of primitive `p`, clamped to the vertices.
pub(crate) fn primitive_index(c: &mut Ctx, p: u32, k: u32) -> u32 {
    let u = c.u32t();
    let vpp = c.io.mesh.pipe.topology.vertices();
    let var = c.map_var();
    let base = c.lk_mul(p, vpp);
    let at = c.m.c_u32(1 + k);
    let at = c.m.emit(op::I_ADD, u, &[base, at]);
    let ptr = c.elem_ptr(var, WORKGROUP, at);
    let v = c.m.emit(op::LOAD, u, &[ptr]);
    let max = c.m.c_u32(c.io.mesh.pipe.max_vertices - 1);
    let glsl = c.glsl;
    c.m.emit(op::EXT_INST, u, &[glsl, crate::spirv::glsl::U_MIN, v, max])
}

/// Component `k` of vertex `v` from the attribute stage buffer.
pub(crate) fn vertex_word(c: &mut Ctx, v: u32, k: u32) -> u32 {
    let u = c.u32t();
    let n = c.io.mesh.components.len() as u32;
    let blk = c.lk_shr(v, 5);
    let blk = c.lk_mul(blk, 32 * n);
    let lo = c.m.c_u32(31);
    let lo = c.m.emit(op::BITWISE_AND, u, &[v, lo]);
    let at = c.m.emit(op::I_ADD, u, &[blk, lo]);
    let kk = c.m.c_u32(32 * k);
    let at = c.m.emit(op::I_ADD, u, &[at, kk]);
    let var = c.attr_var();
    let p = c.elem_ptr(var, WORKGROUP, at);
    c.m.emit(op::LOAD, u, &[p])
}

pub(crate) fn components(c: &Ctx) -> Vec<Component> {
    c.io.mesh.components.clone()
}

/// Per-primitive component `k` of primitive `p` from the attribute stage
/// buffer.
pub(crate) fn primitive_word(c: &mut Ctx, p: u32, k: u32) -> u32 {
    let u = c.u32t();
    let pipe = c.io.mesh.pipe.clone();
    let base = pipe.vertex_words(c.io.mesh.components.len() as u32);
    let n = c.io.mesh.prims.len() as u32;
    let blk = c.lk_shr(p, 5);
    let blk = c.lk_mul(blk, 32 * n);
    let lo = c.m.c_u32(31);
    let lo = c.m.emit(op::BITWISE_AND, u, &[p, lo]);
    let at = c.m.emit(op::I_ADD, u, &[blk, lo]);
    let kk = c.m.c_u32(base + 32 * k);
    let at = c.m.emit(op::I_ADD, u, &[at, kk]);
    let var = c.attr_var();
    let ptr = c.elem_ptr(var, WORKGROUP, at);
    c.m.emit(op::LOAD, u, &[ptr])
}

/// The native mesh shader's hand-over: output counts, then every vertex
/// and every primitive, spread over the lanes.
fn mesh_outputs(c: &mut Ctx, lane: u32, lanes: u32, i_var: u32) -> Res<()> {
    let pipe = c.io.mesh.pipe.clone();
    let n = primitive_count(c);
    let maxv = c.m.c_u32(pipe.max_vertices);
    c.m.emit_void(OP_SET_MESH_OUTPUTS, &[maxv, n]);
    let comps = c.io.mesh.components.clone();
    let mut targets = Vec::new();
    for (k, comp) in comps.iter().enumerate() {
        targets.push((k as u32, output_target(c, *comp, pipe.max_vertices)?));
    }
    let step = c.m.c_u32(lanes);
    let f = c.f32t();
    c.counted_loop(i_var, lane, maxv, step, |c, v| {
        for (k, (var, sub)) in &targets {
            let bits = vertex_word(c, v, *k);
            let val = c.m.emit(op::BITCAST, f, &[bits]);
            let p = c.m.t_ptr(OUTPUT, f);
            let ptr = match sub {
                Some(s) => {
                    let s = c.m.c_u32(*s);
                    c.m.emit(op::ACCESS_CHAIN, p, &[*var, v, s])
                }
                None => c.m.emit(op::ACCESS_CHAIN, p, &[*var, v]),
            };
            c.m.emit_void(op::STORE, &[ptr, val]);
        }
    });
    let vpp = pipe.topology.vertices();
    let u = c.u32t();
    let (bi, ty) = match pipe.topology {
        MeshTopology::Points => (B_POINT_INDICES, u),
        MeshTopology::Lines => (B_LINE_INDICES, c.m.t_vec(u, 2)),
        MeshTopology::Triangles => (B_TRIANGLE_INDICES, c.m.t_vec(u, 3)),
    };
    let arr = c.m.t_array(ty, pipe.max_primitives);
    let ind = c.builtin(bi, OUTPUT, arr);
    c.iface.builtins.insert(bi);
    let prims = c.io.mesh.prims.clone();
    c.counted_loop(i_var, lane, n, step, |c, p| {
        let idx: Vec<u32> = (0..vpp).map(|k| primitive_index(c, p, k)).collect();
        let v = if vpp == 1 {
            idx[0]
        } else {
            c.m.emit(op::COMPOSITE_CONSTRUCT, ty, &idx)
        };
        let pt = c.m.t_ptr(OUTPUT, ty);
        let ptr = c.m.emit(op::ACCESS_CHAIN, pt, &[ind, p]);
        c.m.emit_void(op::STORE, &[ptr, v]);
        for (k, comp) in prims.iter().enumerate() {
            let bits = primitive_word(c, p, k as u32);
            native_primitive_output(c, *comp, p, bits, pipe.max_primitives);
        }
    });
    Ok(())
}

/// Stores one per-primitive component of a native mesh shader.
fn native_primitive_output(c: &mut Ctx, comp: PrimComponent, p: u32, bits: u32, prims: u32) {
    let i = c.m.t_i32();
    let f = c.f32t();
    let b = c.boolt();
    let u = c.u32t();
    let (var, ty, val, sub) = match comp {
        PrimComponent::PrimitiveId | PrimComponent::Layer | PrimComponent::ViewportIndex => {
            let bi = match comp {
                PrimComponent::PrimitiveId => B_PRIMITIVE_ID,
                PrimComponent::Layer => B_LAYER,
                _ => B_VIEWPORT_INDEX,
            };
            let var = c.per_primitive_builtin(bi, i, prims);
            let v = c.m.emit(op::BITCAST, i, &[bits]);
            (var, i, v, None)
        }
        PrimComponent::ViewportMask => {
            let var = c.per_primitive_builtin(B_CULL_PRIMITIVE, b, prims);
            let one = c.m.c_u32(1);
            let zero = c.m.c_u32(0);
            let bit0 = c.m.emit(op::BITWISE_AND, u, &[bits, one]);
            let v = c.m.emit(op::I_EQUAL, b, &[bit0, zero]);
            if c.host.constant_cull_stores {
                let pt = c.m.t_ptr(OUTPUT, b);
                let ptr = c.m.emit(op::ACCESS_CHAIN, pt, &[var, p]);
                let not = c.m.emit(op::LOGICAL_NOT, b, &[v]);
                for (cond, k) in [(v, true), (not, false)] {
                    c.if_then(cond, |c| {
                        let k = c.m.c_bool(k);
                        c.m.emit_void(op::STORE, &[ptr, k]);
                    });
                }
                return;
            }
            (var, b, v, None)
        }
        PrimComponent::Generic(loc, k) => {
            let var = match c.outputs.get(&loc) {
                Some(&v) => v,
                None => {
                    let v4 = c.m.t_vec(f, 4);
                    let arr = c.m.t_array(v4, prims);
                    let pt = c.m.t_ptr(OUTPUT, arr);
                    let var = c.m.global(pt, OUTPUT);
                    c.m.deco(var, 30, &[loc]);
                    c.m.deco(var, DECO_PER_PRIMITIVE, &[]);
                    c.outputs.insert(loc, var);
                    c.iface.outputs.insert(loc);
                    var
                }
            };
            let v = c.m.emit(op::BITCAST, f, &[bits]);
            (var, f, v, Some(k))
        }
    };
    let pt = c.m.t_ptr(OUTPUT, ty);
    let ptr = match sub {
        Some(k) => {
            let k = c.m.c_u32(k);
            c.m.emit(op::ACCESS_CHAIN, pt, &[var, p, k])
        }
        None => c.m.emit(op::ACCESS_CHAIN, pt, &[var, p]),
    };
    c.m.emit_void(op::STORE, &[ptr, val]);
}

/// The per-vertex output array of a component of a native mesh shader,
/// and the index into each vertex's element (none for a scalar).
fn output_target(c: &mut Ctx, comp: Component, verts: u32) -> Res<(u32, Option<u32>)> {
    let f = c.f32t();
    let v4 = c.m.t_vec(f, 4);
    Ok(match comp {
        Component::PointSize => (c.mesh_builtin_out(B_POINT_SIZE, f, verts), None),
        Component::Position(k) => (c.mesh_builtin_out(B_POSITION, v4, verts), Some(k)),
        Component::Clip(n) => {
            let count = 32 - (c.sph_words[18] & 0xff).leading_zeros();
            c.cap(CAP_CLIP_DISTANCE);
            c.iface.clip_distances = count;
            let arr = c.m.t_array(f, count);
            (c.mesh_builtin_out(B_CLIP_DISTANCE, arr, verts), Some(n))
        }
        Component::Generic(loc, k) => {
            if let Some(&v) = c.outputs.get(&loc) {
                return Ok((v, Some(k)));
            }
            let arr = c.m.t_array(v4, verts);
            let p = c.m.t_ptr(OUTPUT, arr);
            let var = c.m.global(p, OUTPUT);
            c.m.deco(var, 30, &[loc]);
            c.outputs.insert(loc, var);
            c.iface.outputs.insert(loc);
            (var, Some(k))
        }
    })
}

impl Ctx {
    fn per_primitive_builtin(&mut self, b: u32, elem: u32, prims: u32) -> u32 {
        if let Some(&v) = self.builtins.get(&b) {
            return v;
        }
        if b == B_VIEWPORT_INDEX {
            self.cap(CAP_MULTI_VIEWPORT);
        }
        let arr = self.m.t_array(elem, prims);
        self.iface.builtins.insert(b);
        let var = self.builtin(b, OUTPUT, arr);
        self.m.deco(var, DECO_PER_PRIMITIVE, &[]);
        var
    }

    fn mesh_builtin_out(&mut self, b: u32, elem: u32, verts: u32) -> u32 {
        if let Some(&v) = self.builtins.get(&b) {
            return v;
        }
        let arr = self.m.t_array(elem, verts);
        self.iface.builtins.insert(b);
        self.builtin(b, OUTPUT, arr)
    }

    pub(crate) fn lk_shr(&mut self, a: u32, k: u32) -> u32 {
        let u = self.u32t();
        let k = self.m.c_u32(k);
        self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[a, k])
    }

    pub(crate) fn lk_shl(&mut self, a: u32, k: u32) -> u32 {
        let u = self.u32t();
        let k = self.m.c_u32(k);
        self.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[a, k])
    }

    pub(crate) fn lk_mul(&mut self, a: u32, k: u32) -> u32 {
        let u = self.u32t();
        let k = self.m.c_u32(k);
        self.m.emit(op::I_MUL, u, &[a, k])
    }
}

/// Execution model, modes and capabilities of a task or mesh program.
pub(crate) fn model(c: &mut Ctx) -> u32 {
    match (c.stage, c.io.mesh.path) {
        (_, MeshPath::Compute) => MODEL_COMPUTE,
        (Stage::Task, _) => MODEL_TASK,
        _ => MODEL_MESH,
    }
}

pub(crate) fn modes(c: &mut Ctx, entry: u32) {
    let lanes = c.mesh_lanes();
    crate::spirv::inst(
        &mut c.m.modes,
        op::EXECUTION_MODE,
        &[entry, MODE_LOCAL_SIZE, lanes, 1, 1],
    );
    if c.io.mesh.path == MeshPath::Native && c.stage == Stage::Mesh {
        let p = c.io.mesh.pipe.clone();
        let topo = match p.topology {
            MeshTopology::Points => MODE_OUTPUT_POINTS,
            MeshTopology::Lines => MODE_OUTPUT_LINES,
            MeshTopology::Triangles => MODE_OUTPUT_TRIANGLES,
        };
        for m in [
            vec![entry, MODE_OUTPUT_VERTICES, p.max_vertices],
            vec![entry, MODE_OUTPUT_PRIMITIVES, p.max_primitives],
            vec![entry, topo],
        ] {
            crate::spirv::inst(&mut c.m.modes, op::EXECUTION_MODE, &m);
        }
    }
}

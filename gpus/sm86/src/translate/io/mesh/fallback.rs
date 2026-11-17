//! The compute back end of task and mesh programs, for hosts without
//! `VK_EXT_mesh_shader`.
//!
//! Every pass shares one storage buffer of words, binding [`BINDING`] of
//! descriptor set [`GUEST_MEMORY_SET`]. It starts with a header of
//! [`HEADER_WORDS`] words (the `H_*` constants are word indices) that the
//! runtime fills before the draw, then the regions the header names. A
//! draw of `n` tasks (or, without a task program, `n` mesh workgroups)
//! runs:
//!
//! 1. the task program as a compute shader over `n` workgroups (any
//!    dispatch whose linear size is at least `n`): workgroup `t` stores its
//!    stage buffer (`MeshPipeline::payload_words` words, the mesh count in
//!    word 1) as task record `t`, and the mesh count as word `t` of the task
//!    scan region (`n + 1` words);
//! 2. [`scan`], one workgroup: the exclusive prefix of the mesh counts, in
//!    place in the task scan region, their sum in [`H_MESHES`] and
//!    the indirect dispatch of the mesh pass in [`H_DISPATCH`] (x at most
//!    65535, then y), of one workgroup per `Interface::mesh_slots` meshes;
//! 3. the mesh program as a compute shader, by that indirect dispatch:
//!    mesh workgroup `g` finds its task by binary search of the task scan
//!    and stores mesh record `g` ([`mesh_record_words`]). A workgroup past
//!    [`H_MESH_CAP`] records sets [`H_OVERFLOW`] and stores nothing: the
//!    runtime must grow the buffer and draw again. Workgroup 0 writes the
//!    indirect draw: every mesh gets `max_primitives` primitive slots;
//! 4. the draw, with the list topology of the mesh output and the vertex
//!    shader [`vertex`] builds. Slot `s` is primitive `s % max_primitives`
//!    of mesh `s / max_primitives`, so the primitives come in task, mesh
//!    and primitive order, and a slot past the mesh's primitive count is
//!    put outside the clip volume and draws nothing. Without per-primitive
//!    outputs the draw is [`indexed`] ([`H_DRAW_INDEXED`], indices in the
//!    index region the mesh pass writes); otherwise it is not
//!    ([`H_DRAW`]) and vertex `i` is corner `i % k` of slot `i / k`.
//!
//! A workgroup of several slots (`Interface::mesh_slots`) runs guest
//! workgroup `g` in slot `g % slots` of workgroup `g / slots`, and step 1
//! runs over `ceil(n / slots)` workgroups.
//!
//! Without a task program, steps 1 and 2 are skipped and the runtime sets
//! [`H_MESHES`] and the mesh dispatch itself.

use super::super::super::{Ctx, INPUT, OUTPUT, Res, bad};
use super::{Component, MeshPipeline, MeshTopology, PrimComponent, WORKGROUP};
use crate::program::Stage;
use crate::spirv::{Module, op};
use crate::translate::{GUEST_MEMORY_SET, HostProfile};

/// Binding of the fallback's storage buffer in [`GUEST_MEMORY_SET`].
pub const BINDING: u32 = 2;
/// Task workgroups of the draw (with a task program): set by the runtime.
pub const H_TASKS: u32 = 0;
/// Mesh workgroups: written by the task scan, or set by the runtime when
/// no task program runs.
pub const H_MESHES: u32 = 1;
/// Set to 1 by a mesh workgroup that found no record.
pub const H_OVERFLOW: u32 = 3;
/// Indirect dispatch of the mesh pass (x, y, z).
pub const H_DISPATCH: u32 = 4;
/// Indirect draw (vertex count, instance count, first vertex, first
/// instance).
pub const H_DRAW: u32 = 8;
/// Mesh records the buffer holds: set by the runtime.
pub const H_MESH_CAP: u32 = 12;
/// Word offset of task record 0 (records of `payload_words`): set by the
/// runtime.
pub const H_TASK_RECS: u32 = 13;
/// Word offset of the task scan (`H_TASKS + 1` words): set by the runtime.
pub const H_TASK_SCAN: u32 = 14;
/// Word offset of mesh record 0 (records of [`mesh_record_words`]): set by
/// the runtime.
pub const H_MESH_RECS: u32 = 15;
/// Word offset of the index region (`H_MESH_CAP` meshes of
/// `MeshPipeline::index_words` indices) of an [`indexed`] draw: set by the
/// runtime.
pub const H_INDEXES: u32 = 16;
/// Indexed indirect draw of an [`indexed`] pipeline (index count, instance
/// count, first index, vertex offset, first instance), with the storage
/// buffer bound as the 32-bit index buffer at offset 0.
pub const H_DRAW_INDEXED: u32 = 17;
/// Words of the header.
pub const HEADER_WORDS: u32 = 32;
/// The widest x of an indirect dispatch the passes write.
pub const DISPATCH_X: u32 = 65535;
/// Invocations of a scan workgroup on a host that allows them.
const SCAN_LANES: u32 = 512;

/// Invocations of the scan pass's one workgroup on `host`: [`SCAN_LANES`],
/// or the largest power of two below it that the host's
/// `maxComputeWorkGroupInvocations` and `maxComputeWorkGroupSize` x allow
/// (Vulkan promises 128).
pub fn scan_lanes(host: &HostProfile) -> u32 {
    let most = SCAN_LANES
        .min(host.max_workgroup_invocations)
        .min(host.max_workgroup_size[0])
        .max(1);
    1 << most.ilog2()
}

const STORAGE_BUFFER: u32 = 12;
const B_VERTEX_INDEX: u32 = 42;
const B_POSITION: u32 = 0;
const B_POINT_SIZE: u32 = 1;
const B_CLIP_DISTANCE: u32 = 3;
const B_LAYER: u32 = 9;
const B_VIEWPORT_INDEX: u32 = 10;
const CAP_MULTI_VIEWPORT: u32 = 57;
const CAP_VIEWPORT_INDEX_LAYER: u32 = 5254;
const SEM_WORKGROUP_MEMORY: u32 = 0x100;

/// Words of a mesh record for a mesh program with `components`
/// per-vertex components. A record is the primitive count and the indices
/// (only when the draw is not [`indexed`]: an indexed draw takes them from
/// the index region), then one row per per-vertex component of
/// `max_vertices` words and one row per per-primitive component of
/// `max_primitives` words, so neighboring vertices read neighboring words
/// and the stage buffer's padding to blocks of 32 is not stored.
pub fn mesh_record_words(p: &MeshPipeline, components: u32) -> u32 {
    let prims = p.primitive_components().map_or(0, |v| v.len() as u32);
    let rows = p.max_vertices * components + p.max_primitives * prims;
    if indexed(p) {
        rows
    } else {
        1 + p.index_words() + rows
    }
}

/// Runs `f(c, record word, stage buffer word)`, spread over the lanes, for
/// every output word of a mesh record ([`mesh_record_words`]): word `i`
/// of the per-vertex rows, then of the per-primitive rows, with the word
/// of the attribute stage buffer it comes from. `var` is a function
/// variable for the loop.
pub(crate) fn record_outputs(
    c: &mut Ctx,
    var: u32,
    lane: u32,
    lanes: u32,
    mut f: impl FnMut(&mut Ctx, u32, u32),
) {
    let p = c.io.mesh.pipe.clone();
    let nc = c.io.mesh.components.len() as u32;
    let np = c.io.mesh.prims.len() as u32;
    let regions = [
        (p.max_vertices, nc, 0, 0),
        (
            p.max_primitives,
            np,
            p.max_vertices * nc,
            p.vertex_words(nc),
        ),
    ];
    let step = c.m.c_u32(lanes);
    for (rows, n, rec0, stage0) in regions {
        if n == 0 {
            continue;
        }
        let end = c.m.c_u32(rows * n);
        c.counted_loop(var, lane, end, step, |c, i| {
            let u = c.u32t();
            let r = c.m.c_u32(rows);
            let e = c.m.emit(op::U_MOD, u, &[i, r]);
            let k = c.m.emit(op::U_DIV, u, &[i, r]);
            let blk = c.lk_shr(e, 5);
            let blk = c.lk_mul(blk, 32 * n);
            let lo = c.m.c_u32(31);
            let lo = c.m.emit(op::BITWISE_AND, u, &[e, lo]);
            let row = c.lk_shl(k, 5);
            let at = add(c, blk, lo);
            let at = add(c, at, row);
            let s0 = c.m.c_u32(stage0);
            let at = add(c, at, s0);
            let r0 = c.m.c_u32(rec0);
            let rec = add(c, i, r0);
            f(c, rec, at);
        });
    }
}

/// The draw is indexed: without per-primitive outputs, vertices that
/// primitives share run the vertex shader once. Vertex `v` of mesh `g` is
/// index `g * (max_vertices + 1) + v`; index `g * (max_vertices + 1) +
/// max_vertices` is a vertex outside the clip volume, for the slots past
/// the mesh's primitive count. The largest index, `H_MESH_CAP *
/// (max_vertices + 1) - 1`, must be within `maxDrawIndexedIndexValue`.
pub fn indexed(p: &MeshPipeline) -> bool {
    p.primitive_components().is_ok_and(|v| v.is_empty())
}

/// The storage buffer variable.
pub(crate) fn buffer_var(m: &mut Module) -> u32 {
    let u = m.t_u32();
    let ra = m.t_runtime_array(u);
    m.deco(ra, 6, &[4]);
    let st = m.t_struct_unique(&[ra]);
    m.deco(st, 2, &[]);
    m.member_deco(st, 0, 35, &[0]);
    let p = m.t_ptr(STORAGE_BUFFER, st);
    let var = m.global(p, STORAGE_BUFFER);
    m.deco(var, 34, &[GUEST_MEMORY_SET]);
    m.deco(var, 33, &[BINDING]);
    var
}

fn ptr(c: &mut Ctx, buf: u32, idx: u32) -> u32 {
    let u = c.u32t();
    let p = c.m.t_ptr(STORAGE_BUFFER, u);
    let zero = c.m.c_u32(0);
    c.m.emit(op::ACCESS_CHAIN, p, &[buf, zero, idx])
}

pub(crate) fn load(c: &mut Ctx, buf: u32, idx: u32) -> u32 {
    let u = c.u32t();
    let p = ptr(c, buf, idx);
    c.m.emit(op::LOAD, u, &[p])
}

fn store(c: &mut Ctx, buf: u32, idx: u32, v: u32) {
    let p = ptr(c, buf, idx);
    c.m.emit_void(op::STORE, &[p, v]);
}

fn header(c: &mut Ctx, buf: u32, h: u32) -> u32 {
    let k = c.m.c_u32(h);
    load(c, buf, k)
}

fn add(c: &mut Ctx, a: u32, b: u32) -> u32 {
    let u = c.u32t();
    c.m.emit(op::I_ADD, u, &[a, b])
}

fn sub(c: &mut Ctx, a: u32, b: u32) -> u32 {
    let u = c.u32t();
    c.m.emit(op::I_SUB, u, &[a, b])
}

/// Returns from the entry point when `cond` holds (for the whole
/// workgroup: `cond` must be uniform).
fn return_if(c: &mut Ctx, cond: u32) {
    let then = c.m.id();
    let merge = c.m.id();
    c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
    c.m.emit_void(op::BRANCH_CONDITIONAL, &[cond, then, merge]);
    c.m.emit_void(op::LABEL, &[then]);
    c.m.emit_void(op::RETURN, &[]);
    c.m.emit_void(op::LABEL, &[merge]);
}

/// The largest `t` in `0..n` with `buf[base + t] <= key`, or 0.
fn search(c: &mut Ctx, buf: u32, base: u32, n: u32, key: u32, vars: &[u32]) -> u32 {
    let (lo, hi) = (vars[0], vars[1]);
    let u = c.u32t();
    let b = c.boolt();
    let zero = c.m.c_u32(0);
    let one = c.m.c_u32(1);
    c.m.emit_void(op::STORE, &[lo, zero]);
    c.m.emit_void(op::STORE, &[hi, n]);
    let header = c.m.id();
    let cond = c.m.id();
    let body = c.m.id();
    let cont = c.m.id();
    let merge = c.m.id();
    c.m.emit_void(op::BRANCH, &[header]);
    c.m.emit_void(op::LABEL, &[header]);
    c.m.emit_void(op::LOOP_MERGE, &[merge, cont, 0]);
    c.m.emit_void(op::BRANCH, &[cond]);
    c.m.emit_void(op::LABEL, &[cond]);
    let l = c.m.emit(op::LOAD, u, &[lo]);
    let h = c.m.emit(op::LOAD, u, &[hi]);
    let d = sub(c, h, l);
    let go = c.m.emit(op::U_GREATER_THAN, b, &[d, one]);
    c.m.emit_void(op::BRANCH_CONDITIONAL, &[go, body, merge]);
    c.m.emit_void(op::LABEL, &[body]);
    let s = add(c, l, h);
    let mid = c.lk_shr(s, 1);
    let at = add(c, base, mid);
    let v = load(c, buf, at);
    let le = c.m.emit(op::U_LESS_THAN_EQUAL, b, &[v, key]);
    let nl = c.select(u, le, mid, l);
    let nh = c.select(u, le, h, mid);
    c.m.emit_void(op::STORE, &[lo, nl]);
    c.m.emit_void(op::STORE, &[hi, nh]);
    c.m.emit_void(op::BRANCH, &[cont]);
    c.m.emit_void(op::LABEL, &[cont]);
    c.m.emit_void(op::BRANCH, &[header]);
    c.m.emit_void(op::LABEL, &[merge]);
    c.m.emit(op::LOAD, u, &[lo])
}

/// Returns from the entry point when `cond` holds for the whole
/// workgroup, or, with `live` (a workgroup of several guest workgroups),
/// clears this invocation's `live`.
fn leave_if(c: &mut Ctx, cond: u32, live: Option<u32>) {
    match live {
        Some(v) => {
            let b = c.boolt();
            let on = c.m.emit(op::LOAD, b, &[v]);
            let not = c.m.emit(op::LOGICAL_NOT, b, &[cond]);
            let on = c.m.emit(op::LOGICAL_AND, b, &[on, not]);
            c.m.emit_void(op::STORE, &[v, on]);
        }
        None => return_if(c, cond),
    }
}

/// `cond`, and with `live` also this invocation's `live`.
fn and_live(c: &mut Ctx, cond: u32, live: Option<u32>) -> u32 {
    match live {
        Some(v) => {
            let b = c.boolt();
            let on = c.m.emit(op::LOAD, b, &[v]);
            c.m.emit(op::LOGICAL_AND, b, &[on, cond])
        }
        None => cond,
    }
}

/// The set-up of a task or mesh workgroup before the guest runs:
/// workgroups past the draw return (or, with `live`, clear it), a mesh
/// workgroup finds its task.
pub(crate) fn enter(c: &mut Ctx, lane: u32, vars: &[u32], live: Option<u32>) -> Res<()> {
    let buf = c.mesh_buffer();
    let lin = c.linear_workgroup();
    match c.stage {
        Stage::Task => {
            let n = header(c, buf, H_TASKS);
            let b = c.boolt();
            let past = c.m.emit(op::U_GREATER_THAN_EQUAL, b, &[lin, n]);
            leave_if(c, past, live);
        }
        Stage::Mesh => {
            let n = header(c, buf, H_MESHES);
            let b = c.boolt();
            let past = c.m.emit(op::U_GREATER_THAN_EQUAL, b, &[lin, n]);
            leave_if(c, past, live);
            let u = c.u32t();
            let first = c.m.emit(op::BITWISE_OR, u, &[lin, lane]);
            let zero = c.m.c_u32(0);
            let first = c.m.emit(op::I_EQUAL, b, &[first, zero]);
            let first = and_live(c, first, live);
            c.if_then(first, |c| {
                let cap = header(c, buf, H_MESH_CAP);
                let u = c.u32t();
                let glsl = c.glsl;
                let m =
                    c.m.emit(op::EXT_INST, u, &[glsl, crate::spirv::glsl::U_MIN, n, cap]);
                let p = c.io.mesh.pipe.clone();
                let verts = c.lk_mul(m, p.max_primitives * p.topology.vertices());
                let one = c.m.c_u32(1);
                let draw = if indexed(&p) {
                    let first = header(c, buf, H_INDEXES);
                    vec![
                        (H_DRAW_INDEXED, verts),
                        (H_DRAW_INDEXED + 1, one),
                        (H_DRAW_INDEXED + 2, first),
                        (H_DRAW_INDEXED + 3, zero),
                        (H_DRAW_INDEXED + 4, zero),
                    ]
                } else {
                    vec![
                        (H_DRAW, verts),
                        (H_DRAW + 1, one),
                        (H_DRAW + 2, zero),
                        (H_DRAW + 3, zero),
                    ]
                };
                for (h, v) in draw {
                    let k = c.m.c_u32(h);
                    store(c, buf, k, v);
                }
            });
            let cap = header(c, buf, H_MESH_CAP);
            let full = c.m.emit(op::U_GREATER_THAN_EQUAL, b, &[lin, cap]);
            let flag = and_live(c, full, live);
            c.if_then(flag, |c| {
                let one = c.m.c_u32(1);
                let k = c.m.c_u32(H_OVERFLOW);
                store(c, buf, k, one);
            });
            leave_if(c, full, live);
            if c.io.mesh.with_task {
                let find = |c: &mut Ctx| {
                    let tasks = header(c, buf, H_TASKS);
                    let scan = header(c, buf, H_TASK_SCAN);
                    let t = search(c, buf, scan, tasks, lin, vars);
                    let at = add(c, scan, t);
                    let first = load(c, buf, at);
                    let j = sub(c, lin, first);
                    let recs = header(c, buf, H_TASK_RECS);
                    let pw = c.io.mesh.pipe.payload_words();
                    let off = c.lk_mul(t, pw);
                    let rec = add(c, recs, off);
                    let (rv, jv) = (
                        c.io.mesh.task_rec.expect("task record variable"),
                        c.io.mesh.mesh_index.expect("mesh index variable"),
                    );
                    c.m.emit_void(op::STORE, &[rv, rec]);
                    c.m.emit_void(op::STORE, &[jv, j]);
                };
                match live {
                    Some(v) => {
                        let on = c.m.emit(op::LOAD, b, &[v]);
                        c.if_then(on, find);
                    }
                    None => find(c),
                }
            }
        }
        _ => return bad("compute back end of a stage other than task or mesh"),
    }
    Ok(())
}

/// The hand-over after the guest ran: a task stores its stage buffer, a
/// mesh its primitives and vertices.
pub(crate) fn leave(c: &mut Ctx, lane: u32, lanes: u32, vars: &[u32]) -> Res<()> {
    let buf = c.mesh_buffer();
    let lin = c.linear_workgroup();
    let step = c.m.c_u32(lanes);
    let i_var = vars[0];
    match c.stage {
        Stage::Task => {
            let pw = c.io.mesh.pipe.payload_words();
            let recs = header(c, buf, H_TASK_RECS);
            let off = c.lk_mul(lin, pw);
            let rec = add(c, recs, off);
            let var = c.payload_var();
            let n = c.m.c_u32(pw);
            c.counted_loop(i_var, lane, n, step, |c, i| {
                let p = c.elem_ptr(var, WORKGROUP, i);
                let u = c.u32t();
                let v = c.m.emit(op::LOAD, u, &[p]);
                let at = add(c, rec, i);
                store(c, buf, at, v);
            });
            let zero = c.m.c_u32(0);
            let b = c.boolt();
            let lane0 = c.m.emit(op::I_EQUAL, b, &[lane, zero]);
            c.if_then(lane0, |c| {
                let one = c.m.c_u32(1);
                let p = c.elem_ptr(var, WORKGROUP, one);
                let u = c.u32t();
                let n = c.m.emit(op::LOAD, u, &[p]);
                let scan = header(c, buf, H_TASK_SCAN);
                let at = add(c, scan, lin);
                store(c, buf, at, n);
            });
        }
        _ => {
            let p = c.io.mesh.pipe.clone();
            let nc = super::components(c).len() as u32;
            let mr = mesh_record_words(&p, nc);
            let recs = header(c, buf, H_MESH_RECS);
            let off = c.lk_mul(lin, mr);
            let rec = add(c, recs, off);
            let n = super::primitive_count(c);
            let zero = c.m.c_u32(0);
            let one = c.m.c_u32(1);
            let vpp = p.topology.vertices();
            let ni = c.lk_mul(n, vpp);
            let map = c.map_var();
            let maxv = c.m.c_u32(p.max_vertices - 1);
            let local = |c: &mut Ctx, i: u32| {
                let u = c.u32t();
                let at = add(c, i, one);
                let ptr = c.elem_ptr(map, WORKGROUP, at);
                let v = c.m.emit(op::LOAD, u, &[ptr]);
                let glsl = c.glsl;
                c.m.emit(op::EXT_INST, u, &[glsl, crate::spirv::glsl::U_MIN, v, maxv])
            };
            let vb = if indexed(&p) {
                let iw = p.index_words();
                let idx = header(c, buf, H_INDEXES);
                let off = c.lk_mul(lin, iw);
                let idx = add(c, idx, off);
                let first = c.lk_mul(lin, p.max_vertices + 1);
                let dead = c.m.c_u32(p.max_vertices);
                let dead = add(c, first, dead);
                let iwc = c.m.c_u32(iw);
                c.counted_loop(i_var, lane, iwc, step, |c, i| {
                    let u = c.u32t();
                    let b = c.boolt();
                    let live = c.m.emit(op::U_LESS_THAN, b, &[i, ni]);
                    let k = c.m.c_u32(iw - 1);
                    let j = c.select(u, live, i, k);
                    let v = local(c, j);
                    let v = add(c, first, v);
                    let v = c.select(u, live, v, dead);
                    let at = add(c, idx, i);
                    store(c, buf, at, v);
                });
                rec
            } else {
                let b = c.boolt();
                let lane0 = c.m.emit(op::I_EQUAL, b, &[lane, zero]);
                c.if_then(lane0, |c| store(c, buf, rec, n));
                let ib = add(c, rec, one);
                c.counted_loop(i_var, lane, ni, step, |c, i| {
                    let v = local(c, i);
                    let at = add(c, ib, i);
                    store(c, buf, at, v);
                });
                let iw = c.m.c_u32(p.index_words());
                add(c, ib, iw)
            };
            let attr = c.attr_var();
            record_outputs(c, i_var, lane, lanes, |c, r, s| {
                let u = c.u32t();
                let ptr = c.elem_ptr(attr, WORKGROUP, s);
                let w = c.m.emit(op::LOAD, u, &[ptr]);
                let at = add(c, vb, r);
                store(c, buf, at, w);
            });
        }
    }
    Ok(())
}

/// The scan pass (one workgroup of [`scan_lanes`] invocations) over the
/// mesh counts of the task records on `host`, for a mesh program of `slots`
/// guest workgroups per workgroup (`Interface::mesh_slots`).
pub fn scan(host: &HostProfile, slots: u32) -> Vec<u32> {
    let n_lanes = scan_lanes(host);
    let mut c = Ctx::bare(Stage::Compute);
    let u = c.u32t();
    let b = c.boolt();
    let buf = c.mesh_buffer();
    let (main, vars) = c.helper_begin(4);
    let lanes = c.m.c_u32(n_lanes);
    let lane = c.local_index_in();
    let zero = c.m.c_u32(0);
    let one = c.m.c_u32(1);
    let n = header(&mut c, buf, H_TASKS);
    let out = header(&mut c, buf, H_TASK_SCAN);
    let count_of = |c: &mut Ctx, i: u32| {
        let at = add(c, out, i);
        load(c, buf, at)
    };
    let m1 = c.m.c_u32(n_lanes - 1);
    let nn = add(&mut c, n, m1);
    let chunk = c.lk_shr(nn, n_lanes.trailing_zeros());
    let start = c.m.emit(op::I_MUL, u, &[lane, chunk]);
    let end = add(&mut c, start, chunk);
    let glsl = c.glsl;
    let start = c.m.emit(
        op::EXT_INST,
        u,
        &[glsl, crate::spirv::glsl::U_MIN, start, n],
    );
    let end =
        c.m.emit(op::EXT_INST, u, &[glsl, crate::spirv::glsl::U_MIN, end, n]);
    let (i_var, sum_var) = (vars[0], vars[1]);
    c.m.emit_void(op::STORE, &[sum_var, zero]);
    c.counted_loop(i_var, start, end, one, |c, i| {
        let v = count_of(c, i);
        let s = c.m.emit(op::LOAD, u, &[sum_var]);
        let s = add(c, s, v);
        c.m.emit_void(op::STORE, &[sum_var, s]);
    });
    let arr = c.m.t_array(u, n_lanes);
    let pa = c.m.t_ptr(WORKGROUP, arr);
    let sh = c.m.global(pa, WORKGROUP);
    c.globals.push(sh);
    let own = c.m.emit(op::LOAD, u, &[sum_var]);
    let p_own = c.elem_ptr(sh, WORKGROUP, lane);
    c.m.emit_void(op::STORE, &[p_own, own]);
    c.workgroup_barrier(SEM_WORKGROUP_MEMORY);
    for d in (0..n_lanes.trailing_zeros()).map(|k| 1u32 << k) {
        let dk = c.m.c_u32(d);
        let has = c.m.emit(op::U_GREATER_THAN_EQUAL, b, &[lane, dk]);
        let from = c.m.emit(op::I_SUB, u, &[lane, dk]);
        let from = c.select(u, has, from, lane);
        let pf = c.elem_ptr(sh, WORKGROUP, from);
        let other = c.m.emit(op::LOAD, u, &[pf]);
        let other = c.select(u, has, other, zero);
        let mine = c.m.emit(op::LOAD, u, &[p_own]);
        let s = add(&mut c, mine, other);
        c.workgroup_barrier(SEM_WORKGROUP_MEMORY);
        c.m.emit_void(op::STORE, &[p_own, s]);
        c.workgroup_barrier(SEM_WORKGROUP_MEMORY);
    }
    let incl = c.m.emit(op::LOAD, u, &[p_own]);
    let excl = sub(&mut c, incl, own);
    c.m.emit_void(op::STORE, &[sum_var, excl]);
    c.counted_loop(i_var, start, end, one, |c, i| {
        let v = count_of(c, i);
        let s = c.m.emit(op::LOAD, u, &[sum_var]);
        let at = add(c, out, i);
        store(c, buf, at, s);
        let s = add(c, s, v);
        c.m.emit_void(op::STORE, &[sum_var, s]);
    });
    let last = c.m.emit(op::I_EQUAL, b, &[lane, m1]);
    c.if_then(last, |c| {
        let at = add(c, out, n);
        store(c, buf, at, incl);
        let k = c.m.c_u32(H_MESHES);
        store(c, buf, k, incl);
        let wx = c.m.c_u32(DISPATCH_X);
        let glsl = c.glsl;
        let sm = c.m.c_u32(slots.max(1) - 1);
        let groups = add(c, incl, sm);
        let sk = c.m.c_u32(slots.max(1));
        let groups = c.m.emit(op::U_DIV, u, &[groups, sk]);
        let x = c.m.emit(
            op::EXT_INST,
            u,
            &[glsl, crate::spirv::glsl::U_MIN, groups, wx],
        );
        let wm = c.m.c_u32(DISPATCH_X - 1);
        let y = add(c, groups, wm);
        let y = c.m.emit(op::U_DIV, u, &[y, wx]);
        for (h, v) in [(H_DISPATCH, x), (H_DISPATCH + 1, y), (H_DISPATCH + 2, one)] {
            let k = c.m.c_u32(h);
            store(c, buf, k, v);
        }
    });
    let _ = lanes;
    c.m.emit_void(op::RETURN, &[]);
    c.m.emit_void(op::FUNCTION_END, &[]);
    c.helper_finish(main, 5, &[n_lanes, 1, 1])
}

/// The vertex shader of the draw for a mesh program with SPH `sph`: it
/// writes the outputs the mesh program's SPH names, from the mesh records.
pub fn vertex(p: &MeshPipeline, sph: &[u32; 32]) -> Res<Vec<u32>> {
    let comps = super::mesh_components(sph)?;
    let nc = comps.len() as u32;
    let mut c = Ctx::bare(Stage::Vertex);
    let u = c.u32t();
    let f = c.f32t();
    let buf = c.mesh_buffer();
    let (main, _) = c.helper_begin(0);
    let i32t = c.m.t_i32();
    let i = c.builtin(B_VERTEX_INDEX, INPUT, i32t);
    let iv = c.m.emit(op::LOAD, i32t, &[i]);
    let iv = c.m.emit(op::BITCAST, u, &[iv]);
    let mr = mesh_record_words(p, nc);
    let recs = header(&mut c, buf, H_MESH_RECS);
    let bt = c.boolt();
    let one = c.m.c_u32(1);
    let (g, live, prim) = if indexed(p) {
        let k = c.m.c_u32(p.max_vertices + 1);
        let g = c.m.emit(op::U_DIV, u, &[iv, k]);
        let v = c.m.emit(op::U_MOD, u, &[iv, k]);
        let maxv = c.m.c_u32(p.max_vertices);
        let live = c.m.emit(op::U_LESS_THAN, bt, &[v, maxv]);
        (g, live, None)
    } else {
        let vpp = p.topology.vertices();
        let k = c.m.c_u32(vpp);
        let q = c.m.emit(op::U_DIV, u, &[iv, k]);
        let corner = c.m.emit(op::U_MOD, u, &[iv, k]);
        let maxp = c.m.c_u32(p.max_primitives);
        let g = c.m.emit(op::U_DIV, u, &[q, maxp]);
        let prim = c.m.emit(op::U_MOD, u, &[q, maxp]);
        let off = c.lk_mul(g, mr);
        let rec = add(&mut c, recs, off);
        let count = load(&mut c, buf, rec);
        let live = c.m.emit(op::U_LESS_THAN, bt, &[prim, count]);
        (g, live, Some((prim, corner)))
    };
    let (body, dead, merge) = (c.m.id(), c.m.id(), c.m.id());
    c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
    c.m.emit_void(op::BRANCH_CONDITIONAL, &[live, body, dead]);
    c.m.emit_void(op::LABEL, &[body]);
    let off = c.lk_mul(g, mr);
    let rec = add(&mut c, recs, off);
    let (v, prim, vb) = match prim {
        Some((prim, corner)) => {
            let ib = add(&mut c, rec, one);
            let pi = c.lk_mul(prim, p.topology.vertices());
            let pi = add(&mut c, ib, pi);
            let pi = add(&mut c, pi, corner);
            let iw = c.m.c_u32(p.index_words());
            let vb = add(&mut c, ib, iw);
            (load(&mut c, buf, pi), prim, vb)
        }
        None => {
            let k = c.m.c_u32(p.max_vertices + 1);
            let v = c.m.emit(op::U_MOD, u, &[iv, k]);
            (v, v, rec)
        }
    };
    let v4 = c.m.t_vec(f, 4);
    let clip_count = 32 - (sph[18] & 0xff).leading_zeros();
    for (k, comp) in comps.iter().enumerate() {
        let row = c.m.c_u32(k as u32 * p.max_vertices);
        let at = add(&mut c, vb, row);
        let at = add(&mut c, at, v);
        let bits = load(&mut c, buf, at);
        let val = c.m.emit(op::BITCAST, f, &[bits]);
        let pf = c.m.t_ptr(OUTPUT, f);
        let target = match *comp {
            Component::PointSize => {
                let var = c.builtin(B_POINT_SIZE, OUTPUT, f);
                c.iface.builtins.insert(B_POINT_SIZE);
                var
            }
            Component::Position(s) => {
                let var = c.builtin(B_POSITION, OUTPUT, v4);
                c.iface.builtins.insert(B_POSITION);
                let s = c.m.c_u32(s);
                c.m.emit(op::ACCESS_CHAIN, pf, &[var, s])
            }
            Component::Clip(n) => {
                c.cap(super::CAP_CLIP_DISTANCE);
                let arr = c.m.t_array(f, clip_count);
                let var = c.builtin(B_CLIP_DISTANCE, OUTPUT, arr);
                c.iface.builtins.insert(B_CLIP_DISTANCE);
                c.iface.clip_distances = clip_count;
                let s = c.m.c_u32(n);
                c.m.emit(op::ACCESS_CHAIN, pf, &[var, s])
            }
            Component::Generic(loc, s) => {
                let var = match c.outputs.get(&loc) {
                    Some(&v) => v,
                    None => {
                        let p4 = c.m.t_ptr(OUTPUT, v4);
                        let var = c.m.global(p4, OUTPUT);
                        c.m.deco(var, 30, &[loc]);
                        c.outputs.insert(loc, var);
                        c.iface.outputs.insert(loc);
                        var
                    }
                };
                let s = c.m.c_u32(s);
                c.m.emit(op::ACCESS_CHAIN, pf, &[var, s])
            }
        };
        c.m.emit_void(op::STORE, &[target, val]);
    }
    let prims = p.primitive_components()?;
    let pw = c.m.c_u32(p.max_vertices * nc);
    let pb = add(&mut c, vb, pw);
    let i32t = c.m.t_i32();
    for (k, comp) in prims.iter().enumerate() {
        let row = c.m.c_u32(k as u32 * p.max_primitives);
        let at = add(&mut c, pb, row);
        let at = add(&mut c, at, prim);
        let bits = load(&mut c, buf, at);
        match *comp {
            PrimComponent::Generic(loc, s) => {
                let var = match c.outputs.get(&loc) {
                    Some(&v) => v,
                    None => {
                        let p4 = c.m.t_ptr(OUTPUT, v4);
                        let var = c.m.global(p4, OUTPUT);
                        c.m.deco(var, 30, &[loc]);
                        c.outputs.insert(loc, var);
                        c.iface.outputs.insert(loc);
                        var
                    }
                };
                let pf = c.m.t_ptr(OUTPUT, f);
                let s = c.m.c_u32(s);
                let ptr = c.m.emit(op::ACCESS_CHAIN, pf, &[var, s]);
                let val = c.m.emit(op::BITCAST, f, &[bits]);
                c.m.emit_void(op::STORE, &[ptr, val]);
            }
            PrimComponent::PrimitiveId => {
                let Some(loc) = p.primitive_id_location else {
                    return bad("per-primitive PrimitiveId without a location to pass it in");
                };
                let pi = c.m.t_ptr(OUTPUT, i32t);
                let var = c.m.global(pi, OUTPUT);
                c.m.deco(var, 30, &[loc]);
                c.m.deco(var, 14, &[]);
                c.outputs.insert(loc, var);
                c.iface.outputs.insert(loc);
                let val = c.m.emit(op::BITCAST, i32t, &[bits]);
                c.m.emit_void(op::STORE, &[var, val]);
            }
            PrimComponent::Layer | PrimComponent::ViewportIndex => {
                let b = if *comp == PrimComponent::Layer {
                    B_LAYER
                } else {
                    B_VIEWPORT_INDEX
                };
                c.cap(CAP_VIEWPORT_INDEX_LAYER);
                c.exts.insert("SPV_EXT_shader_viewport_index_layer");
                if b == B_VIEWPORT_INDEX {
                    c.cap(CAP_MULTI_VIEWPORT);
                }
                let var = c.builtin(b, OUTPUT, i32t);
                c.iface.builtins.insert(b);
                let val = c.m.emit(op::BITCAST, i32t, &[bits]);
                c.m.emit_void(op::STORE, &[var, val]);
            }
            PrimComponent::ViewportMask => {
                let one = c.m.c_u32(1);
                let zero = c.m.c_u32(0);
                let bit0 = c.m.emit(op::BITWISE_AND, u, &[bits, one]);
                let bt = c.boolt();
                let culled = c.m.emit(op::I_EQUAL, bt, &[bit0, zero]);
                c.if_then(culled, culled_position);
            }
        }
    }
    c.m.emit_void(op::BRANCH, &[merge]);
    c.m.emit_void(op::LABEL, &[dead]);
    culled_position(&mut c);
    c.m.emit_void(op::BRANCH, &[merge]);
    c.m.emit_void(op::LABEL, &[merge]);
    c.m.emit_void(op::RETURN, &[]);
    c.m.emit_void(op::FUNCTION_END, &[]);
    Ok(c.helper_finish(main, 0, &[]))
}

/// Puts the vertex outside the clip volume, so its primitive draws nothing.
fn culled_position(c: &mut Ctx) {
    let f = c.f32t();
    let v4 = c.m.t_vec(f, 4);
    let var = c.builtin(B_POSITION, OUTPUT, v4);
    c.iface.builtins.insert(B_POSITION);
    let two = c.m.c_f32(0x4000_0000);
    let onef = c.m.c_f32(0x3f80_0000);
    let out =
        c.m.emit(op::COMPOSITE_CONSTRUCT, v4, &[two, two, two, onef]);
    c.m.emit_void(op::STORE, &[var, out]);
}

/// The topology of the fallback's draw.
pub fn draw_topology(p: &MeshPipeline) -> MeshTopology {
    p.topology
}

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
//!    word 1) as task record `t`;
//! 2. [`scan`], one workgroup: the exclusive prefix of the mesh counts in
//!    the task scan region (`n + 1` words), their sum in [`H_MESHES`] and
//!    the indirect dispatch of the mesh pass in [`H_DISPATCH`] (x at most
//!    65535, then y);
//! 3. the mesh program as a compute shader, by that indirect dispatch:
//!    mesh workgroup `g` finds its task by binary search of the task scan
//!    and stores mesh record `g` ([`mesh_record_words`]). A workgroup past
//!    [`H_MESH_CAP`] records sets [`H_OVERFLOW`] and stores nothing: the
//!    runtime must grow the buffer and draw again. Workgroup 0 writes the
//!    non-indexed indirect draw in [`H_DRAW`]: every mesh gets
//!    `max_primitives` primitive slots;
//! 4. the draw, with the list topology of the mesh output and the vertex
//!    shader [`vertex`] builds: vertex `i` is corner `i % k` of slot
//!    `i / k`, primitive `slot % max_primitives` of mesh
//!    `slot / max_primitives`, so the primitives come in task, mesh and
//!    primitive order. A slot past the mesh's primitive count is put
//!    outside the clip volume and draws nothing.
//!
//! Without a task program, steps 1 and 2 are skipped and the runtime sets
//! [`H_MESHES`] and the mesh dispatch itself.

use super::super::super::{Ctx, INPUT, OUTPUT, Res, bad};
use super::{Component, MeshPipeline, MeshTopology, PrimComponent, WORKGROUP};
use crate::program::Stage;
use crate::spirv::{Module, op};
use crate::translate::GUEST_MEMORY_SET;

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
/// Words of the header.
pub const HEADER_WORDS: u32 = 32;
/// The widest x of an indirect dispatch the passes write.
pub const DISPATCH_X: u32 = 65535;
/// Invocations of a scan workgroup.
const SCAN_LANES: u32 = 128;

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
/// per-vertex components: the primitive count, the indices, then the
/// per-vertex and per-primitive outputs as the stage buffer holds them
/// (blocks of 32 vertices or primitives, component by component), so the
/// mesh pass copies them word for word and neighboring vertices read
/// neighboring words.
pub fn mesh_record_words(p: &MeshPipeline, components: u32) -> u32 {
    let prims = p.primitive_components().map_or(0, |v| v.len() as u32);
    1 + p.index_words() + p.vertex_words(components) + p.primitive_words(prims)
}

/// Word of component `k` of element `e` in blocks of 32 elements of `n`
/// components each, from `base`.
fn block_word(c: &mut Ctx, base: u32, e: u32, n: u32, k: u32) -> u32 {
    let u = c.u32t();
    let blk = c.lk_shr(e, 5);
    let blk = c.lk_mul(blk, 32 * n);
    let lo = c.m.c_u32(31);
    let lo = c.m.emit(op::BITWISE_AND, u, &[e, lo]);
    let at = add(c, blk, lo);
    let kk = c.m.c_u32(32 * k);
    let at = add(c, at, kk);
    add(c, base, at)
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

/// The set-up of a task or mesh workgroup before the guest runs:
/// workgroups past the draw return, a mesh workgroup finds its task.
pub(crate) fn enter(c: &mut Ctx, lane: u32, vars: &[u32]) -> Res<()> {
    let buf = c.mesh_buffer();
    let lin = c.linear_workgroup();
    match c.stage {
        Stage::Task => {
            let n = header(c, buf, H_TASKS);
            let b = c.boolt();
            let past = c.m.emit(op::U_GREATER_THAN_EQUAL, b, &[lin, n]);
            return_if(c, past);
        }
        Stage::Mesh => {
            let n = header(c, buf, H_MESHES);
            let b = c.boolt();
            let past = c.m.emit(op::U_GREATER_THAN_EQUAL, b, &[lin, n]);
            return_if(c, past);
            let u = c.u32t();
            let first = c.m.emit(op::BITWISE_OR, u, &[lin, lane]);
            let zero = c.m.c_u32(0);
            let first = c.m.emit(op::I_EQUAL, b, &[first, zero]);
            c.if_then(first, |c| {
                let cap = header(c, buf, H_MESH_CAP);
                let u = c.u32t();
                let glsl = c.glsl;
                let m =
                    c.m.emit(op::EXT_INST, u, &[glsl, crate::spirv::glsl::U_MIN, n, cap]);
                let p = c.io.mesh.pipe.clone();
                let verts = c.lk_mul(m, p.max_primitives * p.topology.vertices());
                let one = c.m.c_u32(1);
                for (h, v) in [
                    (H_DRAW, verts),
                    (H_DRAW + 1, one),
                    (H_DRAW + 2, zero),
                    (H_DRAW + 3, zero),
                ] {
                    let k = c.m.c_u32(h);
                    store(c, buf, k, v);
                }
            });
            let cap = header(c, buf, H_MESH_CAP);
            let full = c.m.emit(op::U_GREATER_THAN_EQUAL, b, &[lin, cap]);
            let then = c.m.id();
            let merge = c.m.id();
            c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
            c.m.emit_void(op::BRANCH_CONDITIONAL, &[full, then, merge]);
            c.m.emit_void(op::LABEL, &[then]);
            let one = c.m.c_u32(1);
            let k = c.m.c_u32(H_OVERFLOW);
            store(c, buf, k, one);
            c.m.emit_void(op::RETURN, &[]);
            c.m.emit_void(op::LABEL, &[merge]);
            if c.io.mesh.with_task {
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
        }
        _ => {
            let p = c.io.mesh.pipe.clone();
            let comps = super::components(c);
            let nc = comps.len() as u32;
            let mr = mesh_record_words(&p, nc);
            let recs = header(c, buf, H_MESH_RECS);
            let off = c.lk_mul(lin, mr);
            let rec = add(c, recs, off);
            let n = super::primitive_count(c);
            let zero = c.m.c_u32(0);
            let b = c.boolt();
            let lane0 = c.m.emit(op::I_EQUAL, b, &[lane, zero]);
            c.if_then(lane0, |c| store(c, buf, rec, n));
            let vpp = p.topology.vertices();
            let one = c.m.c_u32(1);
            let ib = add(c, rec, one);
            let ni = c.lk_mul(n, vpp);
            let map = c.map_var();
            let maxv = c.m.c_u32(p.max_vertices - 1);
            c.counted_loop(i_var, lane, ni, step, |c, i| {
                let u = c.u32t();
                let at = add(c, i, one);
                let ptr = c.elem_ptr(map, WORKGROUP, at);
                let v = c.m.emit(op::LOAD, u, &[ptr]);
                let glsl = c.glsl;
                let v =
                    c.m.emit(op::EXT_INST, u, &[glsl, crate::spirv::glsl::U_MIN, v, maxv]);
                let at = add(c, ib, i);
                store(c, buf, at, v);
            });
            let iw = c.m.c_u32(p.index_words());
            let vb = add(c, ib, iw);
            let np = super::prim_components(c).len() as u32;
            let words = c.m.c_u32(p.vertex_words(nc) + p.primitive_words(np));
            let attr = c.attr_var();
            c.counted_loop(i_var, lane, words, step, |c, i| {
                let u = c.u32t();
                let ptr = c.elem_ptr(attr, WORKGROUP, i);
                let w = c.m.emit(op::LOAD, u, &[ptr]);
                let at = add(c, vb, i);
                store(c, buf, at, w);
            });
        }
    }
    Ok(())
}

/// The scan pass (one workgroup of 128 invocations) over the mesh counts
/// of the task records.
pub fn scan(p: &MeshPipeline) -> Vec<u32> {
    let mut c = Ctx::bare(Stage::Compute);
    let u = c.u32t();
    let b = c.boolt();
    let buf = c.mesh_buffer();
    let (main, vars) = c.helper_begin(4);
    let lanes = c.m.c_u32(SCAN_LANES);
    let lane = c.local_index_in();
    let zero = c.m.c_u32(0);
    let one = c.m.c_u32(1);
    let n = header(&mut c, buf, H_TASKS);
    let recs = header(&mut c, buf, H_TASK_RECS);
    let stride = p.payload_words();
    let out = header(&mut c, buf, H_TASK_SCAN);
    let count_of = |c: &mut Ctx, i: u32| {
        let off = c.lk_mul(i, stride);
        let off = add(c, recs, off);
        let k = c.m.c_u32(1);
        let off = add(c, off, k);
        load(c, buf, off)
    };
    let m1 = c.m.c_u32(SCAN_LANES - 1);
    let nn = add(&mut c, n, m1);
    let chunk = c.lk_shr(nn, 7);
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
    let arr = c.m.t_array(u, SCAN_LANES);
    let pa = c.m.t_ptr(WORKGROUP, arr);
    let sh = c.m.global(pa, WORKGROUP);
    c.globals.push(sh);
    let own = c.m.emit(op::LOAD, u, &[sum_var]);
    let p_own = c.elem_ptr(sh, WORKGROUP, lane);
    c.m.emit_void(op::STORE, &[p_own, own]);
    c.workgroup_barrier(SEM_WORKGROUP_MEMORY);
    for d in [1u32, 2, 4, 8, 16, 32, 64] {
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
        let s = c.m.emit(op::LOAD, u, &[sum_var]);
        let at = add(c, out, i);
        store(c, buf, at, s);
        let v = count_of(c, i);
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
        let x = c.m.emit(
            op::EXT_INST,
            u,
            &[glsl, crate::spirv::glsl::U_MIN, incl, wx],
        );
        let wm = c.m.c_u32(DISPATCH_X - 1);
        let y = add(c, incl, wm);
        let y = c.m.emit(op::U_DIV, u, &[y, wx]);
        for (h, v) in [(H_DISPATCH, x), (H_DISPATCH + 1, y), (H_DISPATCH + 2, one)] {
            let k = c.m.c_u32(h);
            store(c, buf, k, v);
        }
    });
    let _ = lanes;
    c.m.emit_void(op::RETURN, &[]);
    c.m.emit_void(op::FUNCTION_END, &[]);
    c.helper_finish(main, 5, &[SCAN_LANES, 1, 1])
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
    let vpp = p.topology.vertices();
    let k = c.m.c_u32(vpp);
    let q = c.m.emit(op::U_DIV, u, &[iv, k]);
    let corner = c.m.emit(op::U_MOD, u, &[iv, k]);
    let maxp = c.m.c_u32(p.max_primitives);
    let g = c.m.emit(op::U_DIV, u, &[q, maxp]);
    let prim = c.m.emit(op::U_MOD, u, &[q, maxp]);
    let mr = mesh_record_words(p, nc);
    let recs = header(&mut c, buf, H_MESH_RECS);
    let off = c.lk_mul(g, mr);
    let rec = add(&mut c, recs, off);
    let count = load(&mut c, buf, rec);
    let bt = c.boolt();
    let live = c.m.emit(op::U_LESS_THAN, bt, &[prim, count]);
    let (body, dead, merge) = (c.m.id(), c.m.id(), c.m.id());
    c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
    c.m.emit_void(op::BRANCH_CONDITIONAL, &[live, body, dead]);
    c.m.emit_void(op::LABEL, &[body]);
    let one = c.m.c_u32(1);
    let ib = add(&mut c, rec, one);
    let pi = c.lk_mul(prim, vpp);
    let pi = add(&mut c, ib, pi);
    let pi = add(&mut c, pi, corner);
    let v = load(&mut c, buf, pi);
    let iw = c.m.c_u32(p.index_words());
    let vb = add(&mut c, ib, iw);
    let v4 = c.m.t_vec(f, 4);
    let clip_count = 32 - (sph[18] & 0xff).leading_zeros();
    for (k, comp) in comps.iter().enumerate() {
        let at = block_word(&mut c, vb, v, nc, k as u32);
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
    let np = prims.len() as u32;
    let pw = c.m.c_u32(p.vertex_words(nc));
    let vbase = add(&mut c, ib, iw);
    let pb = add(&mut c, vbase, pw);
    let i32t = c.m.t_i32();
    for (k, comp) in prims.iter().enumerate() {
        let at = block_word(&mut c, pb, prim, np, k as u32);
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

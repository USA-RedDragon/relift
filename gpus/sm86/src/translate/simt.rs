//! Warps in software: one host invocation runs a whole guest warp of 32
//! lanes ([`WarpPath::Simt`], compute only).
//!
//! Registers and predicates become arrays of 32 entries, uniform
//! registers stay single values. An execution mask (`exec`) says which
//! lanes are active. Straight-line runs of instructions are a loop over
//! the active lanes, each lane running the whole run before the next one;
//! a run ends before a memory access that could see another lane's write
//! in the same run, so that each memory instruction is ordered against the
//! others as it is when the warp executes it.
//!
//! Control flow follows the blocks and loops of [`super::cfg`], but every
//! branch is taken per lane: a block's lanes split into one mask per
//! successor, the code of each successor runs with its mask, and the lanes
//! meet again at the branch's immediate post-dominator, where SM86's
//! BSSY/BSYNC put the reconvergence. Lanes that leave a construct (go
//! round a loop, leave it, or reach its merge) add themselves to that
//! destination's mask and wait there; a loop goes round while any lane
//! went round it; lanes at EXIT are dropped. The host's own control flow
//! is uniform within the invocation, so the structure needs no
//! reconvergence guarantees from the host.
//!
//! Warp operations run in two passes. The first goes over the active
//! lanes and records each lane's operands (a ballot, a reduction, or one
//! word per lane) and whether its guard let it execute the instruction.
//! The second goes over the lanes that executed it and computes their
//! results from the records; a shuffle from a lane that did not execute it
//! reads 0, as on SM86. Instructions that write uniform registers run once
//! per warp, as the first active lane.
//!
//! The host workgroup has one invocation per guest warp: the
//! `WorkgroupSize` the shader declares is the guest size (specialization
//! constants 0, 1 and 2, as on every path) divided by 32, rounded up, and
//! the thread ids come from the invocation index and the lane. Local
//! memory and the clock counters are per lane. A task or mesh workgroup is
//! one warp: invocation 0 runs it (see `io::mesh`).

use super::cfg::{Block, EXIT, Graph, Sel, Term};
use super::{Code, Ctx, INPUT, Res, bad};
use crate::spirv::{glsl, op};
use crate::{Inst, Operand};
use std::collections::HashMap;

const BUILTIN_LOCAL_INVOCATION_INDEX: u32 = 29;

/// What the lane loop being emitted does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Pass {
    /// Runs the instructions for each active lane.
    Lane,
    /// Records the operands of a warp operation for each of the 32 lanes;
    /// writes nothing.
    Collect,
    /// Computes the warp operation's results from the records.
    Replay,
}

/// What a warp primitive records in the first pass.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
enum Rec {
    Ballot,
    Reduce,
    Words,
}

pub(crate) struct State {
    pub pass: Pass,
    /// The lane being run (an SSA value inside a lane loop).
    pub lane: u32,
    /// In [`Pass::Collect`]: whether the lane executes the instruction.
    act: u32,
    exec: u32,
    gact: u32,
    todo: u32,
    /// The guest warp's index in the workgroup.
    warp: u32,
    recs: HashMap<(Rec, usize), u32>,
    used: HashMap<Rec, usize>,
    inits: Vec<(u32, u32)>,
    /// Arrays of 32 words, one per lane, beyond the registers.
    pub words: Vec<u32>,
}

/// How an instruction runs in a warp.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Lane(Mem),
    /// Writes uniform registers: once, as the first active lane.
    Once,
    /// Reads other lanes: two passes; `uniform` if it writes uniform
    /// registers.
    Cross {
        uniform: bool,
    },
    /// A workgroup barrier: once, outside any lane loop.
    Barrier,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Mem {
    None,
    Read,
    Write,
}

pub(crate) fn cross(i: &Inst) -> bool {
    matches!(
        i.mnemonic,
        "SHFL" | "VOTE" | "VOTEU" | "REDUX" | "MATCH" | "LDSM"
    ) || super::mem::lanes::r2ur_votes(i)
}

pub(crate) fn writes_uniform(i: &Inst) -> bool {
    i.mnemonic.starts_with('U') || matches!(i.mnemonic, "S2UR" | "R2UR" | "VOTEU" | "REDUX")
}

fn mem(i: &Inst) -> Mem {
    match i.mnemonic {
        "LDS" | "LDG" | "LD" | "LDSM" | "TEX" | "TLD" | "TLD4" | "TXD" | "SULD" => Mem::Read,
        "STS" | "STG" | "ST" | "ATOM" | "ATOMG" | "ATOMS" | "RED" | "SUST" | "SUATOM" | "SURED"
        | "MEMBAR" => Mem::Write,
        _ => Mem::None,
    }
}

fn kind(i: &Inst) -> Kind {
    if cross(i) {
        Kind::Cross {
            uniform: writes_uniform(i) && !super::mem::lanes::r2ur_votes(i),
        }
    } else if i.mnemonic == "BAR" {
        Kind::Barrier
    } else if writes_uniform(i) {
        Kind::Once
    } else {
        Kind::Lane(mem(i))
    }
}

impl Ctx {
    fn st(&mut self) -> &mut State {
        self.simt.as_mut().expect("simt state")
    }

    fn sv(&self) -> &State {
        self.simt.as_ref().expect("simt state")
    }

    pub(crate) fn simt_pass(&self) -> Option<Pass> {
        self.simt.as_ref().map(|s| s.pass)
    }

    fn mask_var(&mut self) -> u32 {
        let v = self.m.id();
        self.locals.push(v);
        v
    }

    fn ld(&mut self, var: u32) -> u32 {
        let u = self.u32t();
        self.m.emit(op::LOAD, u, &[var])
    }

    fn sb(&mut self, o: u16, a: u32, b: u32) -> u32 {
        let u = self.u32t();
        self.m.emit(o, u, &[a, b])
    }

    fn scmp(&mut self, o: u16, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(o, t, &[a, b])
    }

    /// The lowest set bit's index, taken modulo 32: an index past the
    /// arrays must not exist even where the host evaluates it for an
    /// invocation that is not running that code.
    fn lsb(&mut self, v: u32) -> u32 {
        let u = self.u32t();
        let l = self.ext(u, glsl::FIND_I_LSB, &[v]);
        let k31 = self.m.c_u32(31);
        self.sb(op::BITWISE_AND, l, k31)
    }

    fn bit_of(&mut self, lane: u32) -> u32 {
        let one = self.m.c_u32(1);
        self.sb(op::SHIFT_LEFT_LOGICAL, one, lane)
    }

    fn or_into(&mut self, var: u32, v: u32) {
        let old = self.ld(var);
        let n = self.sb(op::BITWISE_OR, old, v);
        self.m.emit_void(op::STORE, &[var, n]);
    }

    /// Pointer to the current lane's entry of a per-lane array of `ty`.
    pub(crate) fn lane_ptr(&mut self, var: u32, ty: u32) -> u32 {
        let lane = self.sv().lane;
        let p = self.m.t_ptr(super::FUNCTION, ty);
        self.m.emit(op::ACCESS_CHAIN, p, &[var, lane])
    }

    /// The guest warp's lanes that exist: all 32 unless the workgroup ends
    /// inside the warp.
    fn existing(&mut self, warp: u32) -> u32 {
        let n = self.guest_invocations();
        let k5 = self.m.c_u32(5);
        let base = self.sb(op::SHIFT_LEFT_LOGICAL, warp, k5);
        let left = self.sb(op::I_SUB, n, base);
        let k32 = self.m.c_u32(32);
        let full = self.scmp(op::U_GREATER_THAN_EQUAL, left, k32);
        let one = self.m.c_u32(1);
        let k31 = self.m.c_u32(31);
        let sh = self.sb(op::BITWISE_AND, left, k31);
        let b = self.sb(op::SHIFT_LEFT_LOGICAL, one, sh);
        let part = self.sb(op::I_SUB, b, one);
        let all = self.m.c_u32(!0);
        let u = self.u32t();
        self.select(u, full, all, part)
    }

    /// The guest workgroup's invocation count, a specialization constant.
    pub(crate) fn guest_invocations(&mut self) -> u32 {
        if self.stage.is_mesh_pipeline() {
            let n = self.mesh_lanes();
            return self.m.c_u32(n);
        }
        let ws = self.workgroup_size();
        let u = self.u32t();
        let x = self.m.spec_op(u, op::COMPOSITE_EXTRACT, &[ws, 0]);
        let y = self.m.spec_op(u, op::COMPOSITE_EXTRACT, &[ws, 1]);
        let z = self.m.spec_op(u, op::COMPOSITE_EXTRACT, &[ws, 2]);
        let xy = self.m.spec_op(u, op::I_MUL, &[x, y]);
        self.m.spec_op(u, op::I_MUL, &[xy, z])
    }

    /// The host workgroup size: one invocation per guest warp.
    pub(crate) fn simt_workgroup_size(&mut self) -> u32 {
        let n = self.guest_invocations();
        let u = self.u32t();
        let k31 = self.m.c_u32(31);
        let k5 = self.m.c_u32(5);
        let r = self.m.spec_op(u, op::I_ADD, &[n, k31]);
        let w = self.m.spec_op(u, op::SHIFT_RIGHT_LOGICAL, &[r, k5]);
        let one = self.m.c_u32(1);
        let v3 = self.m.t_vec(u, 3);
        self.m.spec_composite(v3, &[w, one, one])
    }

    /// Component `comp` of the current lane's guest thread id.
    pub(crate) fn simt_tid(&mut self, comp: u32) -> u32 {
        let warp = self.sv().warp;
        let lane = self.sv().lane;
        let k5 = self.m.c_u32(5);
        let base = self.sb(op::SHIFT_LEFT_LOGICAL, warp, k5);
        let lin = self.sb(op::BITWISE_OR, base, lane);
        let ws = self.workgroup_size();
        let u = self.u32t();
        let x = self.m.spec_op(u, op::COMPOSITE_EXTRACT, &[ws, 0]);
        let y = self.m.spec_op(u, op::COMPOSITE_EXTRACT, &[ws, 1]);
        match comp {
            0 => self.sb(op::U_MOD, lin, x),
            1 => {
                let q = self.sb(op::U_DIV, lin, x);
                self.sb(op::U_MOD, q, y)
            }
            _ => {
                let xy = self.m.spec_op(u, op::I_MUL, &[x, y]);
                self.sb(op::U_DIV, lin, xy)
            }
        }
    }

    /// A record for the next warp primitive of the instruction: the same
    /// variable in both passes.
    fn rec(&mut self, r: Rec, init: u32) -> u32 {
        let st = self.st();
        let k = *st.used.get(&r).unwrap_or(&0);
        st.used.insert(r, k + 1);
        let var = match self.sv().recs.get(&(r, k)) {
            Some(&v) => v,
            None => {
                let v = self.m.id();
                if r == Rec::Words {
                    self.st().words.push(v);
                } else {
                    self.locals.push(v);
                }
                self.st().recs.insert((r, k), v);
                v
            }
        };
        if r != Rec::Words && self.sv().pass == Pass::Collect {
            let c = self.m.c_u32(init);
            self.st().inits.push((var, c));
        }
        var
    }

    fn word_at(&mut self, var: u32, lane: u32) -> u32 {
        let u = self.u32t();
        let p = self.m.t_ptr(super::FUNCTION, u);
        self.m.emit(op::ACCESS_CHAIN, p, &[var, lane])
    }

    /// The lanes that execute the warp operation.
    pub(crate) fn simt_active(&mut self) -> u32 {
        match self.sv().pass {
            Pass::Replay => {
                let g = self.sv().gact;
                self.ld(g)
            }
            Pass::Collect => self.m.c_u32(0),
            Pass::Lane => {
                let e = self.sv().exec;
                self.ld(e)
            }
        }
    }

    pub(crate) fn simt_ballot(&mut self, pred: u32) -> u32 {
        let var = self.rec(Rec::Ballot, 0);
        if self.sv().pass != Pass::Collect {
            return self.ld(var);
        }
        let act = self.sv().act;
        let lane = self.sv().lane;
        let b = self.boolt();
        let on = self.m.emit(op::LOGICAL_AND, b, &[act, pred]);
        let bit = self.bit_of(lane);
        let z = self.m.c_u32(0);
        let u = self.u32t();
        let v = self.select(u, on, bit, z);
        self.or_into(var, v);
        z
    }

    fn record(&mut self, v: u32) -> Option<u32> {
        let var = self.rec(Rec::Words, 0);
        if self.sv().pass == Pass::Collect {
            let lane = self.sv().lane;
            let p = self.word_at(var, lane);
            self.m.emit_void(op::STORE, &[p, v]);
            return None;
        }
        Some(var)
    }

    pub(crate) fn simt_shuffle(&mut self, v: u32, l: u32) -> u32 {
        match self.record(v) {
            None => self.m.c_u32(0),
            Some(var) => {
                let k31 = self.m.c_u32(31);
                let l = self.sb(op::BITWISE_AND, l, k31);
                let p = self.word_at(var, l);
                let x = self.ld(p);
                let act = self.simt_active();
                let sh = self.sb(op::SHIFT_RIGHT_LOGICAL, act, l);
                let one = self.m.c_u32(1);
                let bit = self.sb(op::BITWISE_AND, sh, one);
                let on = self.scmp(op::I_EQUAL, bit, one);
                let z = self.m.c_u32(0);
                let u = self.u32t();
                self.select(u, on, x, z)
            }
        }
    }

    pub(crate) fn simt_gather(&mut self, v: u32) -> Vec<u32> {
        match self.record(v) {
            None => vec![self.m.c_u32(0); 32],
            Some(var) => (0..32)
                .map(|k| {
                    let l = self.m.c_u32(k);
                    let p = self.word_at(var, l);
                    self.ld(p)
                })
                .collect(),
        }
    }

    pub(crate) fn simt_all_equal(&mut self, v: u32) -> u32 {
        let Some(var) = self.record(v) else {
            return self.m.c_bool(false);
        };
        let act = self.simt_active();
        let first = self.lsb(act);
        let p = self.word_at(var, first);
        let x = self.ld(p);
        let b = self.boolt();
        let mut ok = self.m.c_bool(true);
        for k in 0..32 {
            let l = self.m.c_u32(k);
            let p = self.word_at(var, l);
            let y = self.ld(p);
            let eq = self.scmp(op::I_EQUAL, x, y);
            let bit = self.m.c_u32(1 << k);
            let on = self.sb(op::BITWISE_AND, act, bit);
            let z = self.m.c_u32(0);
            let off = self.scmp(op::I_EQUAL, on, z);
            let fine = self.m.emit(op::LOGICAL_OR, b, &[off, eq]);
            ok = self.m.emit(op::LOGICAL_AND, b, &[ok, fine]);
        }
        ok
    }

    /// REDUX over the executing lanes; `o` is the subgroup opcode of the
    /// reduction.
    pub(crate) fn simt_reduce(&mut self, o: u16, v: u32) -> u32 {
        use super::mem::spv;
        let (init, f): (u32, Option<u16>) = match o {
            spv::GROUP_AND => (!0, Some(op::BITWISE_AND)),
            spv::GROUP_OR => (0, Some(op::BITWISE_OR)),
            spv::GROUP_XOR => (0, Some(op::BITWISE_XOR)),
            spv::GROUP_I_ADD => (0, Some(op::I_ADD)),
            spv::GROUP_S_MIN => (0x7fff_ffff, None),
            spv::GROUP_U_MIN => (!0, None),
            spv::GROUP_S_MAX => (0x8000_0000, None),
            _ => (0, None),
        };
        let var = self.rec(Rec::Reduce, init);
        if self.sv().pass != Pass::Collect {
            return self.ld(var);
        }
        let acc = self.ld(var);
        let u = self.u32t();
        let n = match f {
            Some(f) => self.sb(f, acc, v),
            None => {
                let g = match o {
                    spv::GROUP_S_MIN => glsl::S_MIN,
                    spv::GROUP_U_MIN => glsl::U_MIN,
                    spv::GROUP_S_MAX => glsl::S_MAX,
                    _ => glsl::U_MAX,
                };
                self.ext(u, g, &[acc, v])
            }
        };
        let act = self.sv().act;
        let r = self.select(u, act, n, acc);
        self.m.emit_void(op::STORE, &[var, r]);
        self.m.c_u32(0)
    }

    /// In [`Pass::Collect`], the instruction's guard decides whether the
    /// lane takes part; the instruction is emitted either way.
    pub(crate) fn simt_collect_guard(&mut self, guard: Option<u32>) {
        let lane = self.sv().lane;
        let act = guard.unwrap_or_else(|| self.m.c_bool(true));
        let gact = self.sv().gact;
        let lb = self.bit_of(lane);
        let z = self.m.c_u32(0);
        let u = self.u32t();
        let v = self.select(u, act, lb, z);
        self.or_into(gact, v);
        self.st().act = act;
    }

    fn set_pass(&mut self, p: Pass) {
        let st = self.st();
        st.pass = p;
        st.used.clear();
    }

    /// A loop over the lanes set in `mask`, lowest first.
    fn lanes_of(&mut self, mask: u32, body: impl FnOnce(&mut Ctx) -> Res<()>) -> Res<()> {
        let todo = self.sv().todo;
        self.m.emit_void(op::STORE, &[todo, mask]);
        let (h, bl, cont, merge) = (self.m.id(), self.m.id(), self.m.id(), self.m.id());
        self.m.emit_void(op::BRANCH, &[h]);
        self.m.emit_void(op::LABEL, &[h]);
        let t = self.ld(todo);
        let z = self.m.c_u32(0);
        let nz = self.scmp(op::I_NOT_EQUAL, t, z);
        self.m.emit_void(op::LOOP_MERGE, &[merge, cont, 0]);
        self.m.emit_void(op::BRANCH_CONDITIONAL, &[nz, bl, merge]);
        self.m.emit_void(op::LABEL, &[bl]);
        let l = self.lsb(t);
        let one = self.m.c_u32(1);
        let t1 = self.sb(op::I_SUB, t, one);
        let rest = self.sb(op::BITWISE_AND, t, t1);
        self.m.emit_void(op::STORE, &[todo, rest]);
        self.st().lane = l;
        body(self)?;
        self.m.emit_void(op::BRANCH, &[cont]);
        self.m.emit_void(op::LABEL, &[cont]);
        self.m.emit_void(op::BRANCH, &[h]);
        self.m.emit_void(op::LABEL, &[merge]);
        Ok(())
    }

    /// `body` once, as the lowest lane of `mask`, if any.
    fn first_of(&mut self, mask: u32, body: impl FnOnce(&mut Ctx) -> Res<()>) -> Res<()> {
        let z = self.m.c_u32(0);
        let nz = self.scmp(op::I_NOT_EQUAL, mask, z);
        let (then, merge) = (self.m.id(), self.m.id());
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m.emit_void(op::BRANCH_CONDITIONAL, &[nz, then, merge]);
        self.m.emit_void(op::LABEL, &[then]);
        let l = self.lsb(mask);
        self.st().lane = l;
        body(self)?;
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
        Ok(())
    }

    fn exec(&mut self) -> u32 {
        let e = self.sv().exec;
        self.ld(e)
    }

    fn set_exec(&mut self, v: u32) {
        let e = self.sv().exec;
        self.m.emit_void(op::STORE, &[e, v]);
    }

    /// A warp operation: records over all lanes, then results for the lanes
    /// that executed it.
    fn cross_inst(&mut self, i: &Inst, w: crate::Word, pc: u64, uniform: bool) -> Res<()> {
        let gact = self.sv().gact;
        let z = self.m.c_u32(0);
        self.m.emit_void(op::STORE, &[gact, z]);
        self.st().inits.clear();
        let at = self.m.body.len();
        self.set_pass(Pass::Collect);
        let e = self.exec();
        self.lanes_of(e, |c| c.guarded(i, w, pc))?;
        let mut words = Vec::new();
        for (var, init) in std::mem::take(&mut self.st().inits) {
            crate::spirv::inst(&mut words, op::STORE, &[var, init]);
        }
        self.m.body.splice(at..at, words);
        self.set_pass(Pass::Replay);
        let g = self.ld(gact);
        if uniform {
            self.first_of(g, |c| c.guarded(i, w, pc))?;
        } else {
            self.lanes_of(g, |c| c.guarded(i, w, pc))?;
        }
        self.set_pass(Pass::Lane);
        Ok(())
    }
}

/// Code a lane loop runs after the block's instructions: the branch
/// condition.
type Tail<'a> = Option<&'a dyn Fn(&mut Ctx) -> Res<()>>;

#[derive(Clone, Copy)]
enum Dest {
    /// The lanes wait at a mask variable.
    Arrive(u32),
    Die,
    Inline(usize),
}

struct Lp {
    header: usize,
    exits: Vec<usize>,
    body: Vec<bool>,
    cont: u32,
    exit_vars: Vec<u32>,
}

struct Emit<'a> {
    g: Graph,
    code: &'a Code,
    emitted: usize,
    /// Per selector variable and value: the lanes that set it and have
    /// not reached its dispatch yet.
    selected: HashMap<(usize, u32), u32>,
}

impl Emit<'_> {
    /// The mask of the lanes that set selector variable `var` to `k`.
    fn selected_lanes(&mut self, c: &mut Ctx, var: usize, k: u32) -> u32 {
        *self
            .selected
            .entry((var, k))
            .or_insert_with(|| c.mask_var())
    }

    fn resolve(&self, t: usize, follow: Option<(usize, u32)>, lp: Option<&Lp>) -> Res<Dest> {
        if t == EXIT {
            return Ok(Dest::Die);
        }
        if let Some(l) = lp {
            if t == l.header {
                return Ok(Dest::Arrive(l.cont));
            }
            if let Some(k) = l.exits.iter().position(|e| *e == t) {
                return Ok(Dest::Arrive(l.exit_vars[k]));
            }
        }
        if let Some((f, v)) = follow
            && f == t
        {
            return Ok(Dest::Arrive(v));
        }
        if let Some(l) = lp
            && !l.body[t]
        {
            return bad("branch out of more than one loop");
        }
        Ok(Dest::Inline(t))
    }

    fn merge_after(&mut self, ctx: Option<usize>, b: usize, skip: &[bool]) -> Option<usize> {
        let mut p = self.g.ipdom(ctx, b);
        let mut steps = 0;
        while let Some(x) = p
            && skip[x]
        {
            steps += 1;
            if steps > self.g.blocks.len() {
                return None;
            }
            p = self.g.ipdom(ctx, x);
        }
        p
    }

    /// The block's instructions, in runs; the last run also gives each
    /// lane's branch condition to `tail`.
    fn content(&mut self, c: &mut Ctx, blk: &Block, tail: Tail<'_>) -> Res<()> {
        let code = self.code;
        let mut run: Vec<usize> = Vec::new();
        let (mut read, mut write) = (false, false);
        let flush = |c: &mut Ctx, run: &mut Vec<usize>, tail: Tail<'_>| {
            if run.is_empty() && tail.is_none() {
                return Ok(());
            }
            let ks = std::mem::take(run);
            let e = c.exec();
            c.lanes_of(e, |c| {
                for k in ks {
                    c.guarded(&code.insts[k], code.words[k], 16 * k as u64)?;
                }
                match tail {
                    Some(t) => t(c),
                    None => Ok(()),
                }
            })
        };
        for k in blk.first..blk.end {
            let i = &code.insts[k];
            let (w, pc) = (code.words[k], 16 * k as u64);
            let kind = if c.warp_idioms.per_lane.contains(&k) {
                Kind::Lane(Mem::None)
            } else {
                kind(i)
            };
            match kind {
                Kind::Lane(m) => {
                    if m != Mem::None && (write || (m == Mem::Write && read)) {
                        flush(c, &mut run, None)?;
                        read = false;
                        write = false;
                    }
                    read |= m == Mem::Read;
                    write |= m == Mem::Write;
                    run.push(k);
                }
                k2 => {
                    flush(c, &mut run, None)?;
                    read = false;
                    write = false;
                    match k2 {
                        Kind::Once => {
                            let e = c.exec();
                            c.first_of(e, |c| c.guarded(i, w, pc))?;
                        }
                        Kind::Cross { uniform } => c.cross_inst(i, w, pc, uniform)?,
                        _ if i.guard.is_some() => {
                            let e = c.exec();
                            c.first_of(e, |c| c.guarded(i, w, pc))?;
                        }
                        _ => c.guarded(i, w, pc)?,
                    }
                }
            }
        }
        flush(c, &mut run, tail)
    }

    /// Runs each target with its mask (a variable), meeting at `pd`;
    /// returns where the region goes on.
    fn fork(
        &mut self,
        c: &mut Ctx,
        arms: &[(usize, u32)],
        pd: Option<usize>,
        follow: Option<(usize, u32)>,
        lp: Option<&Lp>,
    ) -> Res<Option<usize>> {
        let real = pd.filter(|&m| {
            follow.is_none_or(|(f, _)| f != m)
                && lp.is_none_or(|l| l.header != m && !l.exits.contains(&m))
        });
        let merge = match real {
            Some(_) => {
                let v = c.mask_var();
                let z = c.m.c_u32(0);
                c.m.emit_void(op::STORE, &[v, z]);
                v
            }
            None => 0,
        };
        let arm_follow = match real {
            Some(r) => Some((r, merge)),
            None => follow,
        };
        for &(tg, mv) in arms {
            let d = if Some(tg) == real {
                Dest::Arrive(merge)
            } else {
                self.resolve(tg, arm_follow, lp)?
            };
            let m = c.ld(mv);
            match d {
                Dest::Arrive(v) => c.or_into(v, m),
                Dest::Die => {}
                Dest::Inline(b) => {
                    let z = c.m.c_u32(0);
                    let nz = c.scmp(op::I_NOT_EQUAL, m, z);
                    let (then, end) = (c.m.id(), c.m.id());
                    c.m.emit_void(op::SELECTION_MERGE, &[end, 0]);
                    c.m.emit_void(op::BRANCH_CONDITIONAL, &[nz, then, end]);
                    c.m.emit_void(op::LABEL, &[then]);
                    c.set_exec(m);
                    self.region(c, b, arm_follow, lp)?;
                    c.m.emit_void(op::BRANCH, &[end]);
                    c.m.emit_void(op::LABEL, &[end]);
                }
            }
        }
        if real.is_some() {
            let m = c.ld(merge);
            c.set_exec(m);
        }
        Ok(real)
    }

    fn arrive(c: &mut Ctx, d: Dest) {
        if let Dest::Arrive(v) = d {
            let e = c.exec();
            c.or_into(v, e);
        }
    }

    /// A loop's masks, all empty.
    fn lp(c: &mut Ctx, cur: usize, body: Vec<bool>, exits: Vec<usize>) -> Lp {
        let z = c.m.c_u32(0);
        let cont = c.mask_var();
        c.m.emit_void(op::STORE, &[cont, z]);
        let mut exit_vars = Vec::new();
        for _ in &exits {
            let v = c.mask_var();
            c.m.emit_void(op::STORE, &[v, z]);
            exit_vars.push(v);
        }
        Lp {
            header: cur,
            exits,
            body,
            cont,
            exit_vars,
        }
    }

    fn region(
        &mut self,
        c: &mut Ctx,
        mut cur: usize,
        follow: Option<(usize, u32)>,
        lp: Option<&Lp>,
    ) -> Res<()> {
        loop {
            self.emitted += 1;
            if self.emitted > 4 * self.g.blocks.len() + 64 {
                return bad("control flow needs too much duplication");
            }
            if let Some((body, exits)) = self.g.headers.get(&cur).cloned()
                && lp.is_none_or(|l| l.header != cur)
            {
                let inner = Emit::lp(c, cur, body, exits);
                let (h, bodyl, cont, merge) = (c.m.id(), c.m.id(), c.m.id(), c.m.id());
                c.m.emit_void(op::BRANCH, &[h]);
                c.m.emit_void(op::LABEL, &[h]);
                c.m.emit_void(op::LOOP_MERGE, &[merge, cont, 0]);
                c.m.emit_void(op::BRANCH, &[bodyl]);
                c.m.emit_void(op::LABEL, &[bodyl]);
                self.region(c, cur, None, Some(&inner))?;
                c.m.emit_void(op::BRANCH, &[cont]);
                c.m.emit_void(op::LABEL, &[cont]);
                let again = c.ld(inner.cont);
                c.set_exec(again);
                let z = c.m.c_u32(0);
                c.m.emit_void(op::STORE, &[inner.cont, z]);
                let nz = c.scmp(op::I_NOT_EQUAL, again, z);
                c.m.emit_void(op::BRANCH_CONDITIONAL, &[nz, h, merge]);
                c.m.emit_void(op::LABEL, &[merge]);
                match inner.exits.len() {
                    0 => return Ok(()),
                    1 => {
                        let e = c.ld(inner.exit_vars[0]);
                        c.set_exec(e);
                        match self.resolve(inner.exits[0], follow, lp)? {
                            Dest::Inline(e) => {
                                cur = e;
                                continue;
                            }
                            d => {
                                Emit::arrive(c, d);
                                return Ok(());
                            }
                        }
                    }
                    _ => {
                        let arms: Vec<(usize, u32)> = inner
                            .exits
                            .iter()
                            .copied()
                            .zip(inner.exit_vars.iter().copied())
                            .collect();
                        let pd = self.merge_after(lp.map(|l| l.header), cur, &inner.body);
                        match self.fork(c, &arms, pd, follow, lp)? {
                            Some(r) => {
                                cur = r;
                                continue;
                            }
                            None => return Ok(()),
                        }
                    }
                }
            }
            let blk = self.g.blocks[cur].clone();
            let arms: Vec<(usize, u32)> = match &blk.term {
                Term::Jump(t) => {
                    self.content(c, &blk, None)?;
                    match self.resolve(*t, follow, lp)? {
                        Dest::Inline(t) => {
                            cur = t;
                            continue;
                        }
                        d => {
                            Emit::arrive(c, d);
                            return Ok(());
                        }
                    }
                }
                Term::Branch(conds, t, f) => {
                    let mt = c.mask_var();
                    let z = c.m.c_u32(0);
                    c.m.emit_void(op::STORE, &[mt, z]);
                    let tail = |c: &mut Ctx| -> Res<()> {
                        let v = cond(c, conds)?;
                        let lane = c.sv().lane;
                        let bit = c.bit_of(lane);
                        let z = c.m.c_u32(0);
                        let u = c.u32t();
                        let b = c.select(u, v, bit, z);
                        c.or_into(mt, b);
                        Ok(())
                    };
                    self.content(c, &blk, Some(&tail))?;
                    let mf = c.mask_var();
                    let e = c.exec();
                    let tm = c.ld(mt);
                    let u = c.u32t();
                    let ntm = c.m.emit(op::NOT, u, &[tm]);
                    let fm = c.sb(op::BITWISE_AND, e, ntm);
                    c.m.emit_void(op::STORE, &[mf, fm]);
                    if t == f {
                        vec![(*t, mf), (*t, mt)]
                    } else {
                        vec![(*t, mt), (*f, mf)]
                    }
                }
                Term::Set(var, k, d) => {
                    let mv = self.selected_lanes(c, *var, *k);
                    let e = c.exec();
                    c.or_into(mv, e);
                    match self.resolve(*d, follow, lp)? {
                        Dest::Inline(t) => {
                            cur = t;
                            continue;
                        }
                        d => {
                            Emit::arrive(c, d);
                            return Ok(());
                        }
                    }
                }
                Term::Dispatch(var, ts) => {
                    let e = c.exec();
                    let z = c.m.c_u32(0);
                    let mut arms = Vec::new();
                    for (k, &t) in ts.iter().enumerate() {
                        let mv = self.selected_lanes(c, *var, k as u32);
                        let m = c.ld(mv);
                        c.m.emit_void(op::STORE, &[mv, z]);
                        let a = c.sb(op::BITWISE_AND, m, e);
                        let av = c.mask_var();
                        c.m.emit_void(op::STORE, &[av, a]);
                        arms.push((t, av));
                    }
                    arms
                }
                Term::Switch(sel, base, cases) => {
                    let mut targets: Vec<usize> = Vec::new();
                    for (_, b) in cases {
                        if !targets.contains(b) {
                            targets.push(*b);
                        }
                    }
                    targets.push(EXIT);
                    let vars: Vec<u32> = targets.iter().map(|_| c.mask_var()).collect();
                    let z = c.m.c_u32(0);
                    for &v in &vars {
                        c.m.emit_void(op::STORE, &[v, z]);
                    }
                    let tail = |c: &mut Ctx| -> Res<()> {
                        let s = selector(c, *sel, *base);
                        let b = c.boolt();
                        let mut any = c.m.c_bool(false);
                        let mut hits = Vec::new();
                        for t in &targets[..targets.len() - 1] {
                            let mut hit = c.m.c_bool(false);
                            for (lit, tb) in cases {
                                if tb == t {
                                    let k = c.m.c_u32(*lit);
                                    let eq = c.scmp(op::I_EQUAL, s, k);
                                    hit = c.m.emit(op::LOGICAL_OR, b, &[hit, eq]);
                                }
                            }
                            any = c.m.emit(op::LOGICAL_OR, b, &[any, hit]);
                            hits.push(hit);
                        }
                        hits.push(c.m.emit(op::LOGICAL_NOT, b, &[any]));
                        let lane = c.sv().lane;
                        let bit = c.bit_of(lane);
                        let z = c.m.c_u32(0);
                        let u = c.u32t();
                        for (k, h) in hits.into_iter().enumerate() {
                            let v = c.select(u, h, bit, z);
                            c.or_into(vars[k], v);
                        }
                        Ok(())
                    };
                    self.content(c, &blk, Some(&tail))?;
                    targets.into_iter().zip(vars).collect()
                }
            };
            let pd = self.g.ipdom(lp.map(|l| l.header), cur);
            let arms = merge_duplicates(c, arms);
            match self.fork(c, &arms, pd, follow, lp)? {
                Some(r) => cur = r,
                None => return Ok(()),
            }
        }
    }
}

/// Arms to the same target become one arm with the union of the masks.
fn merge_duplicates(c: &mut Ctx, arms: Vec<(usize, u32)>) -> Vec<(usize, u32)> {
    let mut out: Vec<(usize, u32)> = Vec::new();
    for (t, v) in arms {
        match out.iter().find(|(x, _)| *x == t) {
            Some(&(_, into)) => {
                let m = c.ld(v);
                c.or_into(into, m);
            }
            None => out.push((t, v)),
        }
    }
    out
}

fn cond(c: &mut Ctx, conds: &[Operand]) -> Res<u32> {
    let mut acc: Option<u32> = None;
    for p in conds {
        let v = c.pred_operand(p)?;
        acc = Some(match acc {
            None => v,
            Some(a) => {
                let t = c.boolt();
                c.m.emit(op::LOGICAL_AND, t, &[a, v])
            }
        });
    }
    Ok(acc.unwrap_or_else(|| c.m.c_bool(true)))
}

fn selector(c: &mut Ctx, sel: Sel, base: u32) -> u32 {
    let r = match sel {
        Sel::Reg(n) => c.load_reg(n),
        Sel::UReg(n) => c.load_ureg(n),
    };
    let b = c.m.c_u32(base);
    c.sb(op::I_ADD, r, b)
}

/// Emits the program's body for [`WarpPath::Simt`], starting with label
/// `first`. Shared joins (`split`) are refused; the BSSY regions (funneled
/// to their targets where needed) and BAR sites are checked as on the other
/// paths.
///
/// [`WarpPath::Simt`]: super::WarpPath::Simt
pub(crate) fn emit(c: &mut Ctx, code: &Code, first: u32, split: bool) -> Res<()> {
    if split {
        return bad("shared joins on the software warp path");
    }
    super::io::prescan(c, &code.insts);
    let mut g = super::cfg::structure(code, false, true)?;
    if code.insts.iter().any(|i| i.mnemonic == "BAR") {
        g.check_barriers(code, false)?;
    }
    c.m.emit_void(op::LABEL, &[first]);
    let u = c.u32t();
    let idx = c.builtin(BUILTIN_LOCAL_INVOCATION_INDEX, INPUT, u);
    let warp = c.m.emit(op::LOAD, u, &[idx]);
    let exec = c.m.id();
    let gact = c.m.id();
    let todo = c.m.id();
    c.locals.extend([exec, gact, todo]);
    c.simt = Some(State {
        pass: Pass::Lane,
        lane: 0,
        act: 0,
        exec,
        gact,
        todo,
        warp,
        recs: HashMap::new(),
        used: HashMap::new(),
        inits: Vec::new(),
        words: Vec::new(),
    });
    let e = c.existing(warp);
    c.set_exec(e);
    let mut em = Emit {
        g,
        code,
        emitted: 0,
        selected: HashMap::new(),
    };
    em.region(c, 0, None, None)?;
    c.m.emit_void(op::RETURN, &[]);
    Ok(())
}

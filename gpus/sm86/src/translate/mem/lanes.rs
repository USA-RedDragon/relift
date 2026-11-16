//! The lanes of a guest warp on any host.
//!
//! Every warp operation goes through the primitives here: the guest lane,
//! ballot, shuffle from a lane, the votes and the reductions. The path is
//! picked per program from the host profile ([`WarpPath`]):
//!
//! - [`WarpPath::Subgroup`]: a warp is a 32-wide subgroup.
//! - [`WarpPath::Cluster`]: a warp is a 32-lane cluster of a subgroup whose
//!   size is a multiple of 32; the lane is the subgroup invocation modulo 32
//!   and ballots read the cluster's word of the subgroup ballot.
//! - [`WarpPath::Shared`]: compute only. A warp is 32 consecutive local
//!   invocations; values cross lanes through a workgroup array of one word
//!   per invocation, and ballots and reductions are atomics on one word per
//!   warp, with workgroup barriers in between. Barriers need every
//!   invocation of the workgroup, so the path is taken only when each warp
//!   operation sits in workgroup-uniform control flow; all lanes that exist
//!   are then active.
//! - [`WarpPath::Quad`] and [`WarpPath::Host`]: the host's own subgroups.
//!   Shuffles that stay in a 2x2 quad are exact through the subgroup's
//!   quads; anything wider is listed as inexact.

use super::spv;
use crate::program::Stage;
use crate::spirv::{glsl, op};
use crate::translate::{Ctx, INPUT, Warp, WarpPath};
use crate::{Inst, Operand};

const BUILTIN_LOCAL_INVOCATION_INDEX: u32 = 29;
const BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID: u32 = 41;
const SCOPE_WORKGROUP: u32 = 2;
const SCOPE_SUBGROUP: u32 = 3;
const SEM_ACQUIRE_RELEASE: u32 = 0x8;
const SEM_WORKGROUP: u32 = 0x100;
const WORKGROUP: u32 = 4;
const CAP_GROUP_NON_UNIFORM: u32 = 61;
const CAP_VOTE: u32 = 62;
const CAP_ARITHMETIC: u32 = 63;
const CAP_BALLOT: u32 = 64;
const CAP_SHUFFLE: u32 = 65;
const CAP_CLUSTERED: u32 = 67;
const VECTOR_EXTRACT_DYNAMIC: u16 = 77;
const LANES_KEY: u32 = 0x2_0010;
const WORDS_KEY: u32 = 0x2_0011;
const CLUSTERED_REDUCE: u32 = 3;

/// The SHFL operand pattern stays inside a 2x2 quad: the segment mask
/// covers lane bits 2 to 4 (and a butterfly flips only bits 0 and 1), or a
/// butterfly of bits 0 and 1 over the whole warp.
pub(crate) fn shfl_is_quad(i: &Inst) -> bool {
    let imm = |k: usize| match i.ops.get(k) {
        Some(Operand::Int(v)) => Some(*v as u64),
        Some(Operand::Hex(v)) => Some(*v),
        _ => None,
    };
    let (Some(b), Some(c)) = (imm(3), imm(4)) else {
        return false;
    };
    let seg = (c >> 8) & 0x1f;
    let clamp = c & 0x1f;
    let bfly = i.has("BFLY");
    if bfly && b & 0x1f >= 4 {
        return false;
    }
    seg & 0x1c == 0x1c || (bfly && seg == 0 && clamp == 0x1f)
}

/// A warp operation, and whether it stays in a quad.
pub(crate) fn warp_op(i: &Inst) -> Option<bool> {
    match i.mnemonic {
        "SHFL" => Some(shfl_is_quad(i)),
        "VOTE" | "VOTEU" | "REDUX" | "MATCH" => Some(false),
        "S2R" | "S2UR" | "CS2R" => match i.ops.get(1) {
            Some(Operand::Sym(s))
                if matches!(
                    s.as_str(),
                    "SR_LANEID"
                        | "SR_EQMASK"
                        | "SR_LTMASK"
                        | "SR_LEMASK"
                        | "SR_GTMASK"
                        | "SR_GEMASK"
                ) =>
            {
                Some(false)
            }
            _ => None,
        },
        _ => None,
    }
}

fn branch_target(i: &Inst) -> Option<usize> {
    i.ops.iter().find_map(|o| match o {
        Operand::Target(t) if *t >= 0 => Some((*t / 16) as usize),
        _ => None,
    })
}

/// For each instruction, whether control flow may reach it with only part
/// of the workgroup: inside a BSSY region, after a guarded EXIT, RET or
/// KILL, or between a guarded branch and its target.
pub(crate) fn divergent(insts: &[Inst]) -> Vec<bool> {
    let n = insts.len();
    let mut div = vec![false; n];
    for (k, i) in insts.iter().enumerate() {
        let guarded = matches!(
            i.guard,
            Some(Operand::Pred(p, _)) | Some(Operand::UPred(p, _)) if p != 7
        );
        let range = match i.mnemonic {
            "BSSY" => branch_target(i).map(|t| (k + 1, t)),
            "EXIT" | "RET" | "KILL" if guarded => Some((k + 1, n)),
            "BRA" | "BRX" | "JMX" if guarded || i.has("DIV") => {
                branch_target(i).map(|t| if t <= k { (t, k + 1) } else { (k + 1, t) })
            }
            "BRX" | "JMX" => Some((k + 1, n)),
            _ => None,
        };
        if let Some((lo, hi)) = range {
            for d in div.iter_mut().take(hi.min(n)).skip(lo) {
                *d = true;
            }
        }
    }
    div
}

impl Ctx {
    /// Picks how warp operations are carried out, from the stage's
    /// subgroups and where the operations sit.
    pub(crate) fn warp_plan(&mut self, insts: &[Inst]) {
        let ops: Vec<(usize, bool)> = insts
            .iter()
            .enumerate()
            .filter_map(|(k, i)| warp_op(i).map(|q| (k, q)))
            .collect();
        self.iface.host.warp_ops = ops.len() as u32;
        if ops.is_empty() {
            return;
        }
        let warp = match self.stage {
            Stage::Compute => self.host.warp_compute,
            Stage::Fragment => self.host.warp_fragment,
            _ => self.host.warp_graphics,
        };
        let path = match warp {
            Warp::Fixed32 => WarpPath::Subgroup,
            Warp::Unchecked32 => {
                self.iface.host.check_warp32 = true;
                WarpPath::Subgroup
            }
            Warp::Wide => {
                self.iface.host.check_warp32 = true;
                WarpPath::Cluster
            }
            Warp::Narrow(_) | Warp::Varies | Warp::Absent => {
                let div = divergent(insts);
                let uniform = ops
                    .iter()
                    .all(|&(k, _)| !div[k] && insts[k].guard.is_none());
                let wide = ops.iter().filter(|&&(_, q)| !q).count();
                if self.stage == Stage::Compute && uniform {
                    self.iface.host.warp_shared_bytes_per_warp = 132;
                    WarpPath::Shared
                } else if wide == 0 && warp != Warp::Absent {
                    WarpPath::Quad
                } else {
                    let why = if warp == Warp::Absent {
                        "the stage has no subgroup operations"
                    } else if self.stage == Stage::Compute {
                        "a warp operation is in divergent control flow"
                    } else {
                        "the stage cannot have 32-wide subgroups"
                    };
                    self.iface.host.inexact.push(format!(
                        "{wide} warp-wide operations on {}: {why}",
                        self.host.name
                    ));
                    WarpPath::Host
                }
            }
        };
        self.iface.host.warp = path;
    }

    fn warp_path(&self) -> WarpPath {
        self.iface.host.warp
    }

    fn sg_id(&mut self) -> u32 {
        self.cap(CAP_GROUP_NON_UNIFORM);
        let u = self.u32t();
        let var = self.builtin(BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID, INPUT, u);
        self.m.emit(op::LOAD, u, &[var])
    }

    fn local_index(&mut self) -> u32 {
        let u = self.u32t();
        let var = self.builtin(BUILTIN_LOCAL_INVOCATION_INDEX, INPUT, u);
        self.m.emit(op::LOAD, u, &[var])
    }

    fn lb(&mut self, o: u16, a: u32, b: u32) -> u32 {
        let u = self.u32t();
        self.m.emit(o, u, &[a, b])
    }

    fn lk(&mut self, o: u16, a: u32, k: u32) -> u32 {
        let kk = self.m.c_u32(k);
        self.lb(o, a, kk)
    }

    fn lcmp(&mut self, o: u16, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(o, t, &[a, b])
    }

    /// The guest lane, 0 to 31.
    pub(crate) fn lane_id(&mut self) -> u32 {
        match self.warp_path() {
            WarpPath::Shared => {
                let i = self.local_index();
                self.lk(op::BITWISE_AND, i, 31)
            }
            WarpPath::Cluster => {
                let s = self.sg_id();
                self.lk(op::BITWISE_AND, s, 31)
            }
            _ => self.sg_id(),
        }
    }

    /// The host subgroup invocation that holds guest lane `l` of this
    /// invocation's warp.
    fn host_lane(&mut self, l: u32) -> u32 {
        match self.warp_path() {
            WarpPath::Cluster => {
                let s = self.sg_id();
                let base = self.lk(op::BITWISE_AND, s, !31);
                self.lb(op::BITWISE_OR, base, l)
            }
            _ => l,
        }
    }

    fn barrier(&mut self) {
        let exec = self.m.c_u32(SCOPE_WORKGROUP);
        let sem = self.m.c_u32(SEM_ACQUIRE_RELEASE | SEM_WORKGROUP);
        self.m.emit_void(op::CONTROL_BARRIER, &[exec, exec, sem]);
    }

    /// The number of local invocations, a specialization constant.
    fn invocations(&mut self) -> u32 {
        let ws = self.workgroup_size();
        let u = self.u32t();
        let x = self.m.spec_op(u, op::COMPOSITE_EXTRACT, &[ws, 0]);
        let y = self.m.spec_op(u, op::COMPOSITE_EXTRACT, &[ws, 1]);
        let z = self.m.spec_op(u, op::COMPOSITE_EXTRACT, &[ws, 2]);
        let xy = self.m.spec_op(u, op::I_MUL, &[x, y]);
        self.m.spec_op(u, op::I_MUL, &[xy, z])
    }

    fn shared_array(&mut self, key: u32) -> u32 {
        if let Some(&v) = self.builtins.get(&key) {
            return v;
        }
        let u = self.u32t();
        let n = self.invocations();
        let len = if key == WORDS_KEY {
            let k31 = self.m.c_u32(31);
            let k5 = self.m.c_u32(5);
            let r = self.m.spec_op(u, op::I_ADD, &[n, k31]);
            self.m.spec_op(u, op::SHIFT_RIGHT_LOGICAL, &[r, k5])
        } else {
            n
        };
        let arr = self.m.t_array_of(u, len);
        let p = self.m.t_ptr(WORKGROUP, arr);
        let var = self.m.global(p, WORKGROUP);
        self.builtins.insert(key, var);
        self.globals.push(var);
        var
    }

    fn shared_at(&mut self, key: u32, idx: u32) -> u32 {
        let var = self.shared_array(key);
        let u = self.u32t();
        let p = self.m.t_ptr(WORKGROUP, u);
        self.m.emit(op::ACCESS_CHAIN, p, &[var, idx])
    }

    /// This warp's word in the shared ballot and reduction array.
    fn warp_word(&mut self) -> u32 {
        let i = self.local_index();
        let w = self.lk(op::SHIFT_RIGHT_LOGICAL, i, 5);
        self.shared_at(WORDS_KEY, w)
    }

    /// Publishes `v` for the other lanes of the warp.
    fn put(&mut self, v: u32) {
        self.barrier();
        let i = self.local_index();
        let p = self.shared_at(LANES_KEY, i);
        self.m.emit_void(op::STORE, &[p, v]);
        self.barrier();
    }

    /// The value lane `l` published.
    fn get(&mut self, l: u32) -> u32 {
        let i = self.local_index();
        let base = self.lk(op::BITWISE_AND, i, !31);
        let at = self.lb(op::BITWISE_OR, base, l);
        let p = self.shared_at(LANES_KEY, at);
        let u = self.u32t();
        self.m.emit(op::LOAD, u, &[p])
    }

    /// Every lane of the warp combines `v` into one word with the atomic
    /// `atomic`, starting from `init`.
    fn combine(&mut self, atomic: u16, init: u32, v: u32) -> u32 {
        self.barrier();
        let lane = self.lane_id();
        let z = self.m.c_u32(0);
        let first = self.lcmp(op::I_EQUAL, lane, z);
        let then = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[first, then, merge]);
        self.m.emit_void(op::LABEL, &[then]);
        let w = self.warp_word();
        let k = self.m.c_u32(init);
        self.m.emit_void(op::STORE, &[w, k]);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
        self.barrier();
        let w = self.warp_word();
        let u = self.u32t();
        let scope = self.m.c_u32(SCOPE_WORKGROUP);
        let sem = self.m.c_u32(0);
        self.m.emit(atomic, u, &[w, scope, sem, v]);
        self.barrier();
        let w = self.warp_word();
        self.m.emit(op::LOAD, u, &[w])
    }

    /// The lanes of this warp that exist (all of them active).
    fn shared_active(&mut self) -> u32 {
        let n = self.invocations();
        let i = self.local_index();
        let base = self.lk(op::BITWISE_AND, i, !31);
        let left = self.lb(op::I_SUB, n, base);
        let k32 = self.m.c_u32(32);
        let full = self.lcmp(op::U_GREATER_THAN_EQUAL, left, k32);
        let one = self.m.c_u32(1);
        let sh = self.lk(op::BITWISE_AND, left, 31);
        let b = self.lb(op::SHIFT_LEFT_LOGICAL, one, sh);
        let part = self.lb(op::I_SUB, b, one);
        let all = self.m.c_u32(!0);
        let u = self.u32t();
        self.select(u, full, all, part)
    }

    /// The 32-bit mask of lanes for which `pred` holds, among the lanes
    /// that are active.
    pub(crate) fn ballot(&mut self, pred: u32) -> u32 {
        let u = self.u32t();
        if self.warp_path() == WarpPath::Shared {
            let lane = self.lane_id();
            let one = self.m.c_u32(1);
            let bit = self.lb(op::SHIFT_LEFT_LOGICAL, one, lane);
            let z = self.m.c_u32(0);
            let v = self.select(u, pred, bit, z);
            return self.combine(spv::ATOMIC_OR, 0, v);
        }
        self.cap(CAP_GROUP_NON_UNIFORM);
        self.cap(CAP_BALLOT);
        let v4 = self.m.t_vec(u, 4);
        let scope = self.m.c_u32(SCOPE_SUBGROUP);
        let b = self.m.emit(spv::GROUP_BALLOT, v4, &[scope, pred]);
        if self.warp_path() == WarpPath::Cluster {
            let s = self.sg_id();
            let w = self.lk(op::SHIFT_RIGHT_LOGICAL, s, 5);
            return self.m.emit(VECTOR_EXTRACT_DYNAMIC, u, &[b, w]);
        }
        self.m.emit(op::COMPOSITE_EXTRACT, u, &[b, 0])
    }

    /// The active lanes of the warp.
    pub(crate) fn active(&mut self) -> u32 {
        if self.warp_path() == WarpPath::Shared {
            return self.shared_active();
        }
        let t = self.m.c_bool(true);
        self.ballot(t)
    }

    /// `v` as guest lane `l` (0 to 31) of the warp holds it.
    pub(crate) fn shuffle(&mut self, v: u32, l: u32) -> u32 {
        if self.warp_path() == WarpPath::Shared {
            self.put(v);
            return self.get(l);
        }
        self.cap(CAP_GROUP_NON_UNIFORM);
        self.cap(CAP_SHUFFLE);
        let src = self.host_lane(l);
        let u = self.u32t();
        let scope = self.m.c_u32(SCOPE_SUBGROUP);
        self.m.emit(spv::GROUP_SHUFFLE, u, &[scope, v, src])
    }

    /// `v` from each of the 32 lanes of the warp.
    pub(crate) fn gather(&mut self, v: u32) -> Vec<u32> {
        if self.warp_path() == WarpPath::Shared {
            self.put(v);
            return (0..32)
                .map(|k| {
                    let l = self.m.c_u32(k);
                    self.get(l)
                })
                .collect();
        }
        (0..32)
            .map(|k| {
                let l = self.m.c_u32(k);
                self.shuffle(v, l)
            })
            .collect()
    }

    /// `v` from the invocation in quad position `q` of this invocation's
    /// host quad.
    pub(crate) fn quad_shuffle(&mut self, v: u32, q: u32) -> u32 {
        self.cap(CAP_GROUP_NON_UNIFORM);
        self.cap(CAP_SHUFFLE);
        let s = self.sg_id();
        let base = self.lk(op::BITWISE_AND, s, !3);
        let q = self.lk(op::BITWISE_AND, q, 3);
        let src = self.lb(op::BITWISE_OR, base, q);
        let u = self.u32t();
        let scope = self.m.c_u32(SCOPE_SUBGROUP);
        self.m.emit(spv::GROUP_SHUFFLE, u, &[scope, v, src])
    }

    /// Shuffles stay in the host's quads rather than the warp.
    pub(crate) fn quad_only(&self) -> bool {
        matches!(self.warp_path(), WarpPath::Quad | WarpPath::Host)
    }

    /// VOTE.ALL, VOTE.ANY and VOTE.EQ of `p` over the active lanes, with
    /// the vote opcode of the subgroup path.
    pub(crate) fn vote(&mut self, o: u16, p: u32) -> u32 {
        let b = self.boolt();
        match self.warp_path() {
            WarpPath::Shared | WarpPath::Cluster => {
                let m = self.ballot(p);
                let act = self.active();
                let z = self.m.c_u32(0);
                match o {
                    spv::GROUP_ALL => self.lcmp(op::I_EQUAL, m, act),
                    spv::GROUP_ANY => self.lcmp(op::I_NOT_EQUAL, m, z),
                    _ => {
                        let all = self.lcmp(op::I_EQUAL, m, act);
                        let none = self.lcmp(op::I_EQUAL, m, z);
                        self.m.emit(op::LOGICAL_OR, b, &[all, none])
                    }
                }
            }
            _ => {
                self.cap(CAP_GROUP_NON_UNIFORM);
                self.cap(CAP_VOTE);
                let scope = self.m.c_u32(SCOPE_SUBGROUP);
                self.m.emit(o, b, &[scope, p])
            }
        }
    }

    /// All active lanes hold the same `v`.
    pub(crate) fn all_equal(&mut self, v: u32) -> u32 {
        let b = self.boolt();
        match self.warp_path() {
            WarpPath::Shared | WarpPath::Cluster => {
                let act = self.active();
                let u = self.u32t();
                let first = self.ext(u, glsl::FIND_I_LSB, &[act]);
                let x = self.shuffle(v, first);
                let eq = self.lcmp(op::I_EQUAL, v, x);
                let m = self.ballot(eq);
                self.lcmp(op::I_EQUAL, m, act)
            }
            _ => {
                self.cap(CAP_GROUP_NON_UNIFORM);
                self.cap(CAP_VOTE);
                let scope = self.m.c_u32(SCOPE_SUBGROUP);
                self.m.emit(spv::GROUP_ALL_EQUAL, b, &[scope, v])
            }
        }
    }

    /// REDUX: the reduction `o` (a subgroup arithmetic opcode) of `v` over
    /// the active lanes.
    pub(crate) fn reduce(&mut self, o: u16, v: u32) -> u32 {
        let u = self.u32t();
        match self.warp_path() {
            WarpPath::Shared => {
                let (atomic, init) = match o {
                    spv::GROUP_AND => (spv::ATOMIC_AND, !0),
                    spv::GROUP_OR => (spv::ATOMIC_OR, 0),
                    spv::GROUP_XOR => (spv::ATOMIC_XOR, 0),
                    spv::GROUP_I_ADD => (spv::ATOMIC_I_ADD, 0),
                    spv::GROUP_S_MIN => (spv::ATOMIC_S_MIN, 0x7fff_ffff),
                    spv::GROUP_U_MIN => (spv::ATOMIC_U_MIN, !0),
                    spv::GROUP_S_MAX => (spv::ATOMIC_S_MAX, 0x8000_0000),
                    _ => (spv::ATOMIC_U_MAX, 0),
                };
                self.combine(atomic, init, v)
            }
            WarpPath::Cluster => {
                self.cap(CAP_GROUP_NON_UNIFORM);
                self.cap(CAP_CLUSTERED);
                let scope = self.m.c_u32(SCOPE_SUBGROUP);
                let k32 = self.m.c_u32(32);
                self.m.emit(o, u, &[scope, CLUSTERED_REDUCE, v, k32])
            }
            _ => {
                self.cap(CAP_GROUP_NON_UNIFORM);
                self.cap(CAP_ARITHMETIC);
                let scope = self.m.c_u32(SCOPE_SUBGROUP);
                self.m.emit(o, u, &[scope, 0, v])
            }
        }
    }
}

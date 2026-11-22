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
//! - [`WarpPath::Simt`]: compute only, a guest warp is one invocation that
//!   runs all 32 lanes (see the `simt` module); the primitives read the
//!   other lanes' records directly.
//! - [`WarpPath::Quad`] and [`WarpPath::Host`]: the host's own subgroups.
//!   Shuffles that stay in a 2x2 quad are exact through the subgroup's
//!   quads; anything wider is listed as inexact.
//! - [`WarpPath::Invariant`]: the host's own subgroups for a program whose
//!   warp-wide operations are all idioms whose result does not depend on
//!   which lanes share a warp (see `uniform`). A warp is the aligned
//!   32-lane group of the subgroup (all of it when smaller): lanes and
//!   ballots as in the cluster path, shuffles that the analysis proved
//!   stay in the reader's quad through the host quad.

use super::spv;
use crate::program::Stage;
use crate::spirv::{glsl, op};
use crate::translate::{Ctx, INPUT, MeshPath, Warp, WarpPath};
use crate::{Inst, Operand};
use std::collections::HashSet;

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
const STAGE_KEY: u32 = 0x2_0012;
const BUILTIN_SUBGROUP_SIZE: u32 = 36;
const BUILTIN_SUBGROUP_ID: u32 = 40;
/// Words per lane that [`Ctx::stage_quads`] holds.
pub(crate) const STAGE_REGS: usize = 6;

/// The SHFL operand pattern stays inside a 2x2 quad: the segment mask
/// covers lane bits 2 to 4, so the source lane keeps the quad bits of the
/// lane whatever the lane operand (a butterfly must also flip only bits 0
/// and 1), or a butterfly of bits 0 and 1 over the whole warp.
pub(crate) fn shfl_is_quad(i: &Inst) -> bool {
    let imm = |k: usize| match i.ops.get(k) {
        Some(Operand::Int(v)) => Some(*v as u64),
        Some(Operand::Hex(v)) => Some(*v),
        _ => None,
    };
    let Some(c) = imm(4) else {
        return false;
    };
    let seg = (c >> 8) & 0x1f;
    let clamp = c & 0x1f;
    if !i.has("BFLY") {
        return seg & 0x1c == 0x1c;
    }
    let Some(b) = imm(3) else {
        return false;
    };
    b & 0x1f < 4 && (seg & 0x1c == 0x1c || (seg == 0 && clamp == 0x1f))
}

/// A warp operation, and whether it stays in a quad.
pub(crate) fn warp_op(i: &Inst) -> Option<bool> {
    match i.mnemonic {
        "SHFL" => Some(shfl_is_quad(i)),
        "MOV"
            if !matches!(
                i.ops.get(2),
                None | Some(Operand::Hex(0xf) | Operand::Int(0xf))
            ) =>
        {
            Some(true)
        }
        "VOTE" | "VOTEU" | "REDUX" | "MATCH" | "LDSM" | "MOVM" | "HMMA" => Some(false),
        "R2UR" if r2ur_votes(i) => Some(false),
        "BMOV" if reads_active(i) => Some(false),
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

/// A warp operation whose result depends on which lanes are active.
pub(crate) fn reads_lanes(i: &Inst) -> bool {
    matches!(i.mnemonic, "SHFL" | "VOTE" | "VOTEU" | "REDUX" | "MATCH")
        || reads_active(i)
        || r2ur_votes(i)
}

/// `R2UR P, UR, R`: the predicate says whether every active lane holds the
/// same value.
pub(crate) fn r2ur_votes(i: &Inst) -> bool {
    i.mnemonic == "R2UR" && matches!(i.ops.first(), Some(Operand::Pred(..)))
}

/// `BMOV R, MACTIVE`: the mask of the active lanes.
pub(crate) fn reads_active(i: &Inst) -> bool {
    i.mnemonic == "BMOV"
        && matches!(i.ops.as_slice(), [Operand::Reg(r, _), Operand::Sym(s)] if *r != 255 && s == "MACTIVE")
}

/// `Bn`, a convergence barrier register, as BMOV names it.
pub(crate) fn barrier_name(o: &Operand) -> Option<u8> {
    match o {
        Operand::Sym(s) => s.strip_prefix('B')?.parse().ok().filter(|n| *n < 16),
        Operand::Barrier(n) => Some(*n),
        _ => None,
    }
}

/// A BMOV that changes a convergence barrier: a write to `Bn`, or a
/// `.CLEAR` read of one.
pub(crate) fn writes_barrier(i: &Inst) -> bool {
    i.mnemonic == "BMOV"
        && (i.ops.first().and_then(barrier_name).is_some()
            || (i.has("CLEAR") && i.ops.get(1).and_then(barrier_name).is_some()))
}

/// Whether instruction `k` is `S2R Rn, SR_LANEID` whose value only names
/// the vertex of TessCoord loads (`ALD.O a[0x2f0], Rn`, which the
/// translation reads without it) before Rn is written again or the program
/// exits, all in straight-line code.
fn tess_coord_lane(insts: &[Inst], k: usize) -> bool {
    let i = &insts[k];
    if i.mnemonic != "S2R" || !matches!(i.ops.get(1), Some(Operand::Sym(s)) if s == "SR_LANEID") {
        return false;
    }
    let Some(Operand::Reg(r, _)) = i.ops.first() else {
        return false;
    };
    let r = *r;
    fn near(o: &Operand, r: u8, wide: bool) -> bool {
        match o {
            Operand::Reg(n, _) => *n == r || (wide && *n < r && r - *n < 4),
            Operand::Attr { index, .. } | Operand::CBuf { index, .. } => {
                index.as_deref().is_some_and(|i| near(i, r, wide))
            }
            Operand::Mem { base, .. } => base.as_deref().is_some_and(|b| near(b, r, wide)),
            Operand::Sym(s) => s
                .split(|ch: char| !ch.is_ascii_alphanumeric())
                .filter_map(|t| t.strip_prefix('R')?.parse::<u8>().ok())
                .any(|n| n == r || (wide && n < r && r - n < 4)),
            _ => false,
        }
    }
    for j in insts.iter().skip(k + 1) {
        if j.mnemonic == "EXIT" && j.guard.is_none() {
            return true;
        }
        if matches!(
            j.mnemonic,
            "BRA" | "BRX" | "BRXU" | "JMX" | "JMP" | "CALL" | "RET" | "EXIT" | "KILL"
        ) {
            return false;
        }
        let coord = j.mnemonic == "ALD"
            && j.has("O")
            && !j.has("P")
            && matches!(j.ops.get(1), Some(Operand::Attr { index: None, offset, .. }) if (0x2f0..0x2f8).contains(offset))
            && matches!(j.ops.get(2), Some(Operand::Reg(n, _)) if *n == r);
        let scalar = matches!(
            j.mnemonic,
            "ISETP"
                | "FSETP"
                | "PRMT"
                | "IADD3"
                | "LOP3"
                | "SHF"
                | "FMUL"
                | "FADD"
                | "FFMA"
                | "MOV"
                | "SEL"
                | "FSEL"
                | "FMNMX"
                | "IMNMX"
                | "MUFU"
                | "I2FP"
                | "F2I"
                | "PLOP3"
        ) || (j.mnemonic == "IMAD" && !j.has("WIDE"));
        let wide = !scalar
            || j.mods
                .iter()
                .any(|m| matches!(*m, "64" | "96" | "128" | "F64"));
        let near = |o: &Operand| near(o, r, wide);
        let reads = if coord {
            false
        } else {
            j.ops.iter().skip(1).any(near)
                || j.ops
                    .first()
                    .is_some_and(|o| !matches!(o, Operand::Reg(..)) && near(o))
        };
        if reads || j.guard.as_ref().is_some_and(near) {
            return false;
        }
        if j.guard.is_none() && matches!(j.ops.first(), Some(Operand::Reg(n, _)) if *n == r) {
            return true;
        }
    }
    false
}

fn branch_target(i: &Inst) -> Option<usize> {
    i.ops.iter().find_map(|o| match o {
        Operand::Target(t) if *t >= 0 => Some((*t / 16) as usize),
        _ => None,
    })
}

/// The `BMOV MACTIVE, Bn` (with or without `.PQUAD`) whose Bn holds the
/// lanes a `BMOV Bn, MACTIVE` saved earlier in the same straight-line
/// code, where no branch target, Bn write or instruction that can change
/// the active lanes (other than such a `BMOV.PQUAD MACTIVE`, which the
/// host does not apply) lies between: the active lanes there are the saved
/// ones, so a restore changes nothing and `.PQUAD` starts from them.
pub(crate) fn mactive_from_saved(insts: &[Inst]) -> HashSet<usize> {
    let targets: HashSet<usize> = insts.iter().filter_map(branch_target).collect();
    let writes_active = |i: &Inst| {
        i.mnemonic == "BMOV" && matches!(i.ops.first(), Some(Operand::Sym(s)) if s == "MACTIVE")
    };
    let mut out = HashSet::new();
    for (k, i) in insts.iter().enumerate() {
        if !writes_active(i) {
            continue;
        }
        let Some(n) = i.ops.get(1).and_then(barrier_name) else {
            continue;
        };
        for j in (0..k).rev() {
            let x = &insts[j];
            if targets.contains(&(j + 1)) {
                break;
            }
            let saves = x.mnemonic == "BMOV"
                && x.ops.first().and_then(barrier_name) == Some(n)
                && matches!(x.ops.get(1), Some(Operand::Sym(s)) if s == "MACTIVE")
                && x.guard.is_none();
            if saves {
                out.insert(k);
                break;
            }
            let writes_n = (x.mnemonic == "BMOV" || x.mnemonic == "BSSY")
                && x.ops.first().and_then(barrier_name) == Some(n);
            let flow = matches!(
                x.mnemonic,
                "BRA"
                    | "BRX"
                    | "JMX"
                    | "JMP"
                    | "CALL"
                    | "RET"
                    | "EXIT"
                    | "KILL"
                    | "BSYNC"
                    | "BREAK"
                    | "WARPSYNC"
                    | "BPT"
                    | "YIELD"
            );
            if writes_n || flow || (writes_active(x) && !x.has("PQUAD")) {
                break;
            }
        }
    }
    out
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
    /// subgroups and where the operations sit. The BRA.DIV and BRA.CONV
    /// taken as converged are counted in `code.fast_paths`.
    pub(crate) fn warp_plan(&mut self, code: &crate::translate::Code) {
        let insts = &code.insts;
        let fast_paths = code.fast_paths;
        self.divergent = divergent(insts);
        self.mactive_from_saved = mactive_from_saved(insts);
        let outputs = self.output_registers();
        let plan = crate::translate::uniform::plan(code, &outputs);
        let all: Vec<(usize, bool)> = insts
            .iter()
            .enumerate()
            .filter(|&(k, _)| {
                let free = self.stage == Stage::TessEval && tess_coord_lane(insts, k);
                if free {
                    self.io.vtg.lane_free.insert(16 * k as u64);
                }
                !free
            })
            .filter_map(|(k, i)| warp_op(i).map(|q| (k, q)))
            .collect();
        let ops: Vec<(usize, bool)> = all
            .into_iter()
            .filter(|(k, _)| !plan.per_lane.contains(k))
            .collect();
        self.iface.host.warp_ops = ops.len() as u32;
        self.iface.host.warp_invariant = plan.per_lane.len() as u32;
        if ops.is_empty() {
            self.warp_idioms = plan;
            return;
        }
        let kernel = self.stage == Stage::Compute || self.stage.is_mesh_pipeline();
        let warp = match self.stage {
            Stage::Compute => self.host.warp_compute,
            Stage::Fragment => self.host.warp_fragment,
            Stage::Task | Stage::Mesh if self.io.mesh.path == MeshPath::Compute => {
                self.host.warp_compute
            }
            Stage::Task | Stage::Mesh => self.host.warp_mesh,
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
                if kernel && uniform {
                    self.iface.host.warp_shared_bytes_per_warp = 132;
                    WarpPath::Shared
                } else if wide == 0 && warp != Warp::Absent {
                    WarpPath::Quad
                } else if plan.invariant && warp != Warp::Absent {
                    WarpPath::Invariant
                } else if kernel {
                    WarpPath::Simt
                } else {
                    let why = if warp == Warp::Absent {
                        "the stage has no subgroup operations"
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
        if !matches!(path, WarpPath::Shared | WarpPath::Simt) {
            self.reconvergence_plan(insts, fast_paths);
        }
        let quads = ops.iter().any(|&(_, q)| q) || !plan.quad_shfl.is_empty();
        self.iface.host.whole_quads = self.stage == Stage::Fragment
            && quads
            && matches!(path, WarpPath::Quad | WarpPath::Invariant | WarpPath::Host);
        self.warp_idioms = plan;
    }

    /// The registers the fragment epilogue reads: the enabled components
    /// of each color target the SPH enables (four registers apart), then
    /// the sample mask and depth.
    fn output_registers(&self) -> Vec<u8> {
        if self.stage != Stage::Fragment {
            return Vec::new();
        }
        let targets = self.sph_words[18];
        let flags = self.sph_words[19];
        let mut out = Vec::new();
        let mut reg = 0u8;
        for t in 0..8 {
            let mask = (targets >> (4 * t)) & 0xf;
            if mask == 0 {
                continue;
            }
            out.extend((0..4u8).filter(|c| mask >> c & 1 == 1).map(|c| reg + c));
            reg += 4;
        }
        if flags & 1 != 0 {
            out.push(reg);
        }
        if flags & 2 != 0 {
            out.push(reg + 1);
        }
        out
    }

    /// Asks for maximal reconvergence when a warp operation that reads
    /// other lanes may follow divergent control flow (a guarded instruction,
    /// a BSSY region, a guarded branch or EXIT) or the program takes a
    /// BRA.DIV or BRA.CONV fast path. Without it SPIR-V does not promise
    /// that invocations which split come together again where the guest's
    /// lanes do, so on a host without it the program is inexact.
    fn reconvergence_plan(&mut self, insts: &[Inst], fast_paths: usize) {
        let div = &self.divergent;
        let first = (0..insts.len()).find(|&k| {
            div[k]
                || matches!(
                    insts[k].guard,
                    Some(Operand::Pred(p, _)) | Some(Operand::UPred(p, _)) if p != 7
                )
        });
        let n = insts
            .iter()
            .enumerate()
            .filter(|(k, i)| reads_lanes(i) && (fast_paths > 0 || first.is_some_and(|f| *k >= f)))
            .count();
        if n == 0 {
            return;
        }
        if self.host.maximal_reconvergence {
            self.iface.host.max_reconvergence = true;
        } else {
            self.iface.host.inexact.push(format!(
                "{n} warp operations after divergent control flow on {}: no maximal reconvergence",
                self.host.name
            ));
        }
    }

    /// The host has subgroup operations in this stage.
    pub(crate) fn host_subgroups(&self) -> bool {
        let warp = match self.stage {
            Stage::Compute => self.host.warp_compute,
            Stage::Fragment => self.host.warp_fragment,
            _ => self.host.warp_graphics,
        };
        warp != Warp::Absent
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

    pub(crate) fn local_index(&mut self) -> u32 {
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
            WarpPath::Simt => self.simt.as_ref().map_or(0, |s| s.lane),
            _ if self.stage.is_mesh_pipeline() => self.mesh_guest_lane(),
            WarpPath::Shared => {
                let i = self.local_index();
                self.lk(op::BITWISE_AND, i, 31)
            }
            WarpPath::Cluster | WarpPath::Invariant => {
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
            WarpPath::Cluster | WarpPath::Invariant => {
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
        if self.warp_path() == WarpPath::Simt {
            return self.simt_ballot(pred);
        }
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
        if matches!(self.warp_path(), WarpPath::Cluster | WarpPath::Invariant) {
            let s = self.sg_id();
            let w = self.lk(op::SHIFT_RIGHT_LOGICAL, s, 5);
            return self.m.emit(VECTOR_EXTRACT_DYNAMIC, u, &[b, w]);
        }
        self.m.emit(op::COMPOSITE_EXTRACT, u, &[b, 0])
    }

    /// The active lanes of the warp.
    pub(crate) fn active(&mut self) -> u32 {
        if self.warp_path() == WarpPath::Simt {
            return self.simt_active();
        }
        if self.warp_path() == WarpPath::Shared {
            return self.shared_active();
        }
        let t = self.m.c_bool(true);
        self.ballot(t)
    }

    /// `v` as guest lane `l` (0 to 31) of the warp holds it.
    pub(crate) fn shuffle(&mut self, v: u32, l: u32) -> u32 {
        if self.warp_path() == WarpPath::Simt {
            return self.simt_shuffle(v, l);
        }
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

    /// `v` as each of the guest lanes `ls` holds it, publishing `v` once on
    /// the workgroup path.
    pub(crate) fn shuffle_set(&mut self, v: u32, ls: &[u32]) -> Vec<u32> {
        if self.warp_path() == WarpPath::Shared {
            self.put(v);
            return ls.iter().map(|&l| self.get(l)).collect();
        }
        ls.iter().map(|&l| self.shuffle(v, l)).collect()
    }

    /// Whether [`Ctx::stage_quads`] works on this program's warp path.
    pub(crate) fn can_stage_quads(&self) -> bool {
        matches!(
            self.warp_path(),
            WarpPath::Subgroup | WarpPath::Cluster | WarpPath::Shared
        ) && !self.stage.is_mesh_pipeline()
    }

    /// The warp's index in the workgroup and the lane, for the staging
    /// array of [`Ctx::stage_quads`].
    fn stage_warp_lane(&mut self) -> (u32, u32) {
        match self.warp_path() {
            WarpPath::Shared => {
                let i = self.local_index();
                let w = self.lk(op::SHIFT_RIGHT_LOGICAL, i, 5);
                let l = self.lk(op::BITWISE_AND, i, 31);
                (w, l)
            }
            path => {
                let u = self.u32t();
                let var = self.builtin(BUILTIN_SUBGROUP_ID, INPUT, u);
                let sub = self.m.emit(op::LOAD, u, &[var]);
                let s = self.sg_id();
                if path == WarpPath::Cluster {
                    let var = self.builtin(BUILTIN_SUBGROUP_SIZE, INPUT, u);
                    let size = self.m.emit(op::LOAD, u, &[var]);
                    let per = self.lk(op::SHIFT_RIGHT_LOGICAL, size, 5);
                    let base = self.lb(op::I_MUL, sub, per);
                    let c = self.lk(op::SHIFT_RIGHT_LOGICAL, s, 5);
                    let w = self.lb(op::I_ADD, base, c);
                    let l = self.lk(op::BITWISE_AND, s, 31);
                    (w, l)
                } else {
                    (sub, s)
                }
            }
        }
    }

    /// A barrier among the lanes of the warp that orders workgroup memory:
    /// the workgroup's on the shared path, the subgroup's otherwise.
    fn stage_barrier(&mut self) {
        if self.warp_path() == WarpPath::Shared {
            self.barrier();
            return;
        }
        let exec = self.m.c_u32(SCOPE_SUBGROUP);
        let sem = self.m.c_u32(SEM_ACQUIRE_RELEASE | SEM_WORKGROUP);
        self.m.emit_void(op::CONTROL_BARRIER, &[exec, exec, sem]);
    }

    /// Up to [`STAGE_REGS`] words of every lane of the warp through
    /// workgroup memory, written once: each read `(i, n)` gives the
    /// four-word vector of word `regs[i]` of lanes `4n` to `4n + 3`, `n`
    /// being a value from 0 to 7. Only where [`Ctx::can_stage_quads`].
    pub(crate) fn stage_quads(&mut self, regs: &[u32], reads: &[(usize, u32)]) -> Vec<u32> {
        let u = self.u32t();
        let v4 = self.m.t_vec(u, 4);
        let var = if let Some(&v) = self.builtins.get(&STAGE_KEY) {
            v
        } else {
            let n = self.invocations();
            let k31 = self.m.c_u32(31);
            let k5 = self.m.c_u32(5);
            let r = self.m.spec_op(u, op::I_ADD, &[n, k31]);
            let warps = self.m.spec_op(u, op::SHIFT_RIGHT_LOGICAL, &[r, k5]);
            let per = self.m.c_u32(8 * STAGE_REGS as u32);
            let len = self.m.spec_op(u, op::I_MUL, &[warps, per]);
            let arr = self.m.t_array_of(v4, len);
            let p = self.m.t_ptr(WORKGROUP, arr);
            let var = self.m.global(p, WORKGROUP);
            self.builtins.insert(STAGE_KEY, var);
            self.globals.push(var);
            var
        };
        let (w, lane) = self.stage_warp_lane();
        let base = self.lk(op::I_MUL, w, 8 * STAGE_REGS as u32);
        let g = self.lk(op::SHIFT_RIGHT_LOGICAL, lane, 2);
        let q = self.lk(op::BITWISE_AND, lane, 3);
        let pu = self.m.t_ptr(WORKGROUP, u);
        let pv = self.m.t_ptr(WORKGROUP, v4);
        self.stage_barrier();
        for (i, &r) in regs.iter().enumerate() {
            let at = self.lk(op::I_ADD, base, 8 * i as u32);
            let at = self.lb(op::I_ADD, at, g);
            let p = self.m.emit(op::ACCESS_CHAIN, pu, &[var, at, q]);
            self.m.emit_void(op::STORE, &[p, r]);
        }
        self.stage_barrier();
        reads
            .iter()
            .map(|&(i, n)| {
                let at = self.lk(op::I_ADD, base, 8 * i as u32);
                let at = self.lb(op::I_ADD, at, n);
                let p = self.m.emit(op::ACCESS_CHAIN, pv, &[var, at]);
                self.m.emit(op::LOAD, v4, &[p])
            })
            .collect()
    }

    /// `v` from each of the 32 lanes of the warp.
    pub(crate) fn gather(&mut self, v: u32) -> Vec<u32> {
        if self.warp_path() == WarpPath::Simt {
            return self.simt_gather(v);
        }
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
        matches!(
            self.warp_path(),
            WarpPath::Quad | WarpPath::Invariant | WarpPath::Host
        )
    }

    /// VOTE.ALL, VOTE.ANY and VOTE.EQ of `p` over the active lanes, with
    /// the vote opcode of the subgroup path.
    pub(crate) fn vote(&mut self, o: u16, p: u32) -> u32 {
        let b = self.boolt();
        match self.warp_path() {
            WarpPath::Shared | WarpPath::Cluster | WarpPath::Simt => {
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
            WarpPath::Simt => self.simt_all_equal(v),
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
            WarpPath::Simt => self.simt_reduce(o, v),
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

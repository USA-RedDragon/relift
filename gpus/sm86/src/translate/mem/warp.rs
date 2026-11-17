//! Warp and block level operations: SHFL, VOTE, VOTEU, REDUX, MATCH, MOVM,
//! the barriers and fences, and the special registers that describe a lane.
//!
//! The lanes of a warp come from [`super::lanes`], which maps them to the
//! host's subgroups or to workgroup memory as the host profile allows.
//! Block barriers become workgroup barriers whatever their id; partial
//! barriers (an arrival count) are not supported.

use super::spv;
use crate::program::Stage;
use crate::spirv::op;
use crate::translate::{Ctx, Res, bad};
use crate::{Inst, Operand};

const SCOPE_DEVICE: u32 = 1;
const SCOPE_WORKGROUP: u32 = 2;
const SCOPE_SUBGROUP: u32 = 3;
const SEM_ACQUIRE_RELEASE: u32 = 0x8;
const SEM_UNIFORM: u32 = 0x40;
const SEM_WORKGROUP: u32 = 0x100;
const SEM_IMAGE: u32 = 0x800;

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "S2R" | "S2UR" => return s2r(c, i),
        "CS2R" => return cs2r(c, i),
        "SHFL" => shfl(c, i),
        "VOTE" | "VOTEU" => vote(c, i),
        "REDUX" => redux(c, i),
        "MATCH" => match_(c, i),
        "MOVM" => movm(c, i),
        "BAR" => bar(c, i),
        "BMOV" => bmov(c, i),
        "WARPSYNC" => warpsync(c, i),
        "MEMBAR" => membar(c, i),
        "ERRBAR" | "CCTL" | "CCTLL" | "CCTLT" | "LDGDEPBAR" => Ok(()),
        _ => return None,
    })
}

impl Ctx {
    fn bin(&mut self, o: u16, a: u32, b: u32) -> u32 {
        let u = self.u32t();
        self.m.emit(o, u, &[a, b])
    }

    fn cmp(&mut self, o: u16, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(o, t, &[a, b])
    }
}

fn lane_mask(c: &mut Ctx, name: &str) -> Option<u32> {
    if !matches!(
        name,
        "SR_EQMASK" | "SR_LTMASK" | "SR_LEMASK" | "SR_GEMASK" | "SR_GTMASK"
    ) {
        return None;
    }
    let lane = c.lane_id();
    let one = c.m.c_u32(1);
    let eq = c.bin(op::SHIFT_LEFT_LOGICAL, one, lane);
    let lt = c.bin(op::I_SUB, eq, one);
    Some(match name {
        "SR_EQMASK" => eq,
        "SR_LTMASK" => lt,
        "SR_LEMASK" => c.bin(op::BITWISE_OR, lt, eq),
        "SR_GEMASK" => {
            let u = c.u32t();
            c.m.emit(op::NOT, u, &[lt])
        }
        "SR_GTMASK" => {
            let le = c.bin(op::BITWISE_OR, lt, eq);
            let u = c.u32t();
            c.m.emit(op::NOT, u, &[le])
        }
        _ => return None,
    })
}

fn s2r(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    let Operand::Sym(name) = &i.ops[1] else {
        return None;
    };
    let v = match name.as_str() {
        "SR_LANEID" if c.io.vtg.lane_free.contains(&c.pc) => c.m.c_u32(0),
        "SR_LANEID" => c.lane_id(),
        "SR_CLOCKLO" | "SR_CLOCK" => match c.guest_clock() {
            Ok((lo, _)) => lo,
            Err(e) => return Some(Err(e)),
        },
        "SR_CLOCKHI" => match c.guest_clock() {
            Ok((_, hi)) => hi,
            Err(e) => return Some(Err(e)),
        },
        "SR_SMEMSZ" => {
            let words = c.m.spec_u32(
                super::scratch::SHARED_DEFAULT_WORDS,
                super::scratch::SHARED_SPEC,
            );
            let two = c.m.c_u32(2);
            c.bin(op::SHIFT_LEFT_LOGICAL, words, two)
        }
        "SR_VIRTID"
        | "SR_VIRTUALSMID"
        | "SR_THREAD_KILL"
        | "SR_VARIABLE_RATE"
        | "SR_NLATC"
        | "SR_SW_SCRATCH"
        | "SR_GLOBALERRORSTATUS"
        | "SR_WARPERRORSTATUS" => c.m.c_u32(0),
        n => lane_mask(c, n)?,
    };
    Some(c.store_dst(&i.ops[0], v))
}

fn cs2r(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    let Operand::Sym(name) = &i.ops[1] else {
        return None;
    };
    if !matches!(name.as_str(), "SR_CLOCKLO" | "SR_CLOCK") {
        return None;
    }
    let (lo, hi) = match c.guest_clock() {
        Ok(v) => v,
        Err(e) => return Some(Err(e)),
    };
    Some(if i.has("32") {
        c.store_dst(&i.ops[0], lo)
    } else {
        c.store_pair(&i.ops[0], lo, hi)
    })
}

fn shfl(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.ops.len() != 5 {
        return bad("SHFL operands");
    }
    let pd = match &i.ops[0] {
        Operand::Pred(n, false) => *n,
        o => return bad(format!("SHFL predicate {o}")),
    };
    let d = Ctx::dst_reg(&i.ops[1])?;
    if d == 255 && pd == 7 {
        return Ok(());
    }
    let mode = i
        .mods
        .iter()
        .find(|m| matches!(**m, "IDX" | "UP" | "DOWN" | "BFLY"))
        .copied();
    let Some(mode) = mode else {
        return bad("SHFL mode");
    };
    let a = c.usrc(&i.ops[2])?;
    let b = c.usrc(&i.ops[3])?;
    let cl = c.usrc(&i.ops[4])?;
    let quad = c.quad_only() && super::lanes::shfl_is_quad(i);
    let in_quad =
        quad || (c.quad_only() && c.warp_idioms.quad_shfl.contains(&((c.pc / 16) as usize)));
    let lane = c.lane_id();
    let lane = if quad {
        let k3 = c.m.c_u32(3);
        c.bin(op::BITWISE_AND, lane, k3)
    } else {
        lane
    };
    let k31 = c.m.c_u32(31);
    let k8 = c.m.c_u32(8);
    let bval = c.bin(op::BITWISE_AND, b, k31);
    let clamp = c.bin(op::BITWISE_AND, cl, k31);
    let sh = c.bin(op::SHIFT_RIGHT_LOGICAL, cl, k8);
    let seg = c.bin(op::BITWISE_AND, sh, k31);
    let u = c.u32t();
    let nseg = c.m.emit(op::NOT, u, &[seg]);
    let hi_part = c.bin(op::BITWISE_AND, clamp, nseg);
    let lo_part = c.bin(op::BITWISE_AND, lane, seg);
    let max_lane = c.bin(op::BITWISE_OR, lo_part, hi_part);
    let (j, valid) = match mode {
        "UP" => {
            let j = c.bin(op::I_SUB, lane, bval);
            let no_wrap = c.cmp(op::U_GREATER_THAN_EQUAL, lane, bval);
            let in_seg = c.cmp(op::U_GREATER_THAN_EQUAL, j, max_lane);
            let t = c.boolt();
            (j, c.m.emit(op::LOGICAL_AND, t, &[no_wrap, in_seg]))
        }
        "DOWN" => {
            let j = c.bin(op::I_ADD, lane, bval);
            (j, c.cmp(op::U_LESS_THAN_EQUAL, j, max_lane))
        }
        "BFLY" => {
            let j = c.bin(op::BITWISE_XOR, lane, bval);
            (j, c.cmp(op::U_LESS_THAN_EQUAL, j, max_lane))
        }
        _ => {
            let keep = c.bin(op::BITWISE_AND, bval, nseg);
            let j = c.bin(op::BITWISE_OR, lo_part, keep);
            (j, c.cmp(op::U_LESS_THAN_EQUAL, j, max_lane))
        }
    };
    let src = c.select(u, valid, j, lane);
    let v = if in_quad {
        c.quad_shuffle(a, src)
    } else {
        c.shuffle(a, src)
    };
    c.store_reg(d, v);
    c.store_pred(pd, valid);
    Ok(())
}

fn vote(c: &mut Ctx, i: &Inst) -> Res<()> {
    let (dst, pd, ps) = match i.ops.as_slice() {
        [d, p, s] => (Some(d), p, s),
        [p, s] => (None, p, s),
        _ => return bad("VOTE operands"),
    };
    let p = c.pred_operand(ps)?;
    let at = (c.pc / 16) as usize;
    if c.warp_idioms.per_lane.contains(&at) {
        let r = if i.has("EQ") { c.m.c_bool(true) } else { p };
        return c.store_pdst(pd, r);
    }
    if let Some(d) = dst {
        let m = c.ballot(p);
        c.store_dst(d, m)?;
    }
    let o = if i.has("ALL") {
        spv::GROUP_ALL
    } else if i.has("ANY") {
        spv::GROUP_ANY
    } else if i.has("EQ") {
        spv::GROUP_ALL_EQUAL
    } else {
        return bad("VOTE mode");
    };
    let r = c.vote(o, p);
    c.store_pdst(pd, r)
}

fn redux(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.ops.len() != 2 {
        return bad("REDUX operands");
    }
    let signed = i.has("S32");
    let o = if i.has("AND") {
        spv::GROUP_AND
    } else if i.has("OR") {
        spv::GROUP_OR
    } else if i.has("XOR") {
        spv::GROUP_XOR
    } else if i.has("SUM") {
        spv::GROUP_I_ADD
    } else if i.has("MIN") {
        if signed {
            spv::GROUP_S_MIN
        } else {
            spv::GROUP_U_MIN
        }
    } else if i.has("MAX") {
        if signed {
            spv::GROUP_S_MAX
        } else {
            spv::GROUP_U_MAX
        }
    } else {
        return bad("REDUX operation");
    };
    let v = c.usrc(&i.ops[1])?;
    let r = c.reduce(o, v);
    c.store_dst(&i.ops[0], r)
}

fn match_any(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.ops.len() != 2 {
        return bad("MATCH operands");
    }
    let (lo, hi) = if i.has("U64") {
        c.src_pair(&i.ops[1])?
    } else {
        (c.usrc(&i.ops[1])?, c.m.c_u32(0))
    };
    let u = c.u32t();
    let ls = c.gather(lo);
    let hs = c.gather(hi);
    let mut mask = c.m.c_u32(0);
    for k in 0..32 {
        let (l, h) = (ls[k as usize], hs[k as usize]);
        let el = c.cmp(op::I_EQUAL, l, lo);
        let eh = c.cmp(op::I_EQUAL, h, hi);
        let b = c.boolt();
        let eq = c.m.emit(op::LOGICAL_AND, b, &[el, eh]);
        let bit = c.m.c_u32(1 << k);
        let zero = c.m.c_u32(0);
        let v = c.select(u, eq, bit, zero);
        mask = c.bin(op::BITWISE_OR, mask, v);
    }
    let live = c.active();
    let mask = c.bin(op::BITWISE_AND, mask, live);
    c.store_dst(&i.ops[0], mask)
}

fn match_(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.has("ANY") {
        return match_any(c, i);
    }
    if !i.has("ALL") || i.ops.len() != 3 {
        return bad("MATCH form");
    }
    let (lo, hi) = if i.has("U64") {
        c.src_pair(&i.ops[2])?
    } else {
        (c.usrc(&i.ops[2])?, c.m.c_u32(0))
    };
    let b = c.boolt();
    let e0 = c.all_equal(lo);
    let e1 = c.all_equal(hi);
    let eq = c.m.emit(op::LOGICAL_AND, b, &[e0, e1]);
    c.store_pdst(&i.ops[0], eq)?;
    if !matches!(i.ops[1], Operand::Reg(255, _)) {
        let all = c.active();
        let zero = c.m.c_u32(0);
        let u = c.u32t();
        let m = c.select(u, eq, all, zero);
        c.store_dst(&i.ops[1], m)?;
    }
    Ok(())
}

/// BMOV. Moves between convergence barriers, and from a register or an
/// immediate into one, change only where lanes wait at BSYNC; the control
/// flow module rejects them where that matters. `MACTIVE` reads the active
/// lanes. Every other thread state (exited and killed lanes, the trap and
/// at-exit state, the barrier stack) and a barrier read into a register
/// are not modeled.
fn bmov(c: &mut Ctx, i: &Inst) -> Res<()> {
    use super::lanes::barrier_name;
    let [d, s] = i.ops.as_slice() else {
        return bad(format!("BMOV {i}"));
    };
    let plain = matches!(
        s,
        Operand::Reg(..) | Operand::UReg(..) | Operand::Hex(_) | Operand::Int(_)
    );
    let active = matches!(s, Operand::Sym(x) if x == "MACTIVE");
    match d {
        Operand::Reg(255, _) if barrier_name(s).is_some() => Ok(()),
        Operand::Sym(x) if x == "ATEXIT_PC" => Ok(()),
        d if barrier_name(d).is_some() && (barrier_name(s).is_some() || plain || active) => Ok(()),
        Operand::Reg(..) if active => {
            let m = c.active();
            c.store_dst(d, m)
        }
        _ => bad(format!("BMOV {d}, {s}")),
    }
}

/// WARPSYNC waits for the lanes of its mask to reach it and orders their
/// memory accesses. Where the warp has not diverged the lanes are already
/// together; in compute a subgroup barrier orders their shared memory. In
/// divergent flow the wait can take lanes from another path, which no host
/// can do, so it is reported.
fn warpsync(c: &mut Ctx, i: &Inst) -> Res<()> {
    let k = (c.pc / 16) as usize;
    let guarded = matches!(
        i.guard,
        Some(Operand::Pred(p, _)) | Some(Operand::UPred(p, _)) if p != 7
    );
    if guarded || c.divergent.get(k).copied().unwrap_or(true) {
        let why = "WARPSYNC in divergent control flow";
        if !c.iface.host.inexact.iter().any(|s| s == why) {
            c.iface.host.inexact.push(why.into());
        }
        return Ok(());
    }
    if c.stage == crate::program::Stage::Compute {
        let exec = c.m.c_u32(SCOPE_SUBGROUP);
        let sem = c.m.c_u32(SEM_ACQUIRE_RELEASE | SEM_WORKGROUP);
        c.m.emit_void(op::CONTROL_BARRIER, &[exec, exec, sem]);
    }
    Ok(())
}

/// MOVM.16.MT88: transposes an 8x8 matrix of 16-bit elements held as
/// LDSM leaves it (lane `t` has row `t / 4`, columns `2 * (t % 4)` and the
/// next): lane `t` gets the elements of rows `2 * (t % 4)` and the next at
/// column `t / 4`.
fn movm(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["16", "MT88"])?;
    let d = Ctx::dst_reg(&i.ops[0])?;
    let v = c.usrc(&i.ops[1])?;
    let lane = c.lane_id();
    let k2 = c.m.c_u32(2);
    let k3 = c.m.c_u32(3);
    let k1 = c.m.c_u32(1);
    let k16 = c.m.c_u32(16);
    let col = c.bin(op::SHIFT_RIGHT_LOGICAL, lane, k2);
    let q = c.bin(op::BITWISE_AND, lane, k3);
    let row = c.bin(op::SHIFT_LEFT_LOGICAL, q, k1);
    let half = c.bin(op::BITWISE_AND, col, k1);
    let pos = c.bin(op::I_MUL, half, k16);
    let part = c.bin(op::SHIFT_RIGHT_LOGICAL, col, k1);
    let mut h = [0; 2];
    for (k, h) in h.iter_mut().enumerate() {
        let kk = c.m.c_u32(k as u32);
        let r = c.bin(op::I_ADD, row, kk);
        let base = c.bin(op::SHIFT_LEFT_LOGICAL, r, k2);
        let src = c.bin(op::BITWISE_OR, base, part);
        let w = c.shuffle(v, src);
        let u = c.u32t();
        *h = c.m.emit(op::BIT_FIELD_U_EXTRACT, u, &[w, pos, k16]);
    }
    let hi = c.bin(op::SHIFT_LEFT_LOGICAL, h[1], k16);
    let r = c.bin(op::BITWISE_OR, h[0], hi);
    c.store_reg(d, r);
    Ok(())
}

fn bar(c: &mut Ctx, i: &Inst) -> Res<()> {
    for m in &i.mods {
        if !matches!(*m, "SYNC" | "SYNCALL" | "DEFER_BLOCKING") {
            return bad(format!("BAR.{m}"));
        }
    }
    let counted = match i.ops.get(1) {
        None | Some(Operand::Reg(255, _)) | Some(Operand::Hex(0)) | Some(Operand::Int(0)) => false,
        Some(_) => true,
    };
    if counted {
        return bad("BAR with a thread count");
    }
    if c.stage == Stage::TessControl {
        crate::translate::io::vtg::tcs_barrier(c);
        return Ok(());
    }
    let exec = c.m.c_u32(SCOPE_WORKGROUP);
    let sem =
        c.m.c_u32(SEM_ACQUIRE_RELEASE | SEM_UNIFORM | SEM_WORKGROUP | SEM_IMAGE);
    c.m.emit_void(op::CONTROL_BARRIER, &[exec, exec, sem]);
    Ok(())
}

/// MEMBAR: an acquire-release fence on buffer, workgroup and image memory
/// at the fence's scope (`.CTA`, `.SM` and `.VC` the workgroup in a
/// compute program, the device elsewhere; `.GPU` the device). NVIDIA's
/// compiler gives a workgroup-scope fence on all memory (GLSL's
/// `groupMemoryBarrier`) `.VC` and one on shared memory alone none. Vulkan
/// has no sequentially consistent fence and, without its memory model, no
/// scope wider than the device, so `.SC` (an acquire-release fence then)
/// and `.SYS` (a device fence then) are listed inexact.
/// The accesses a fence orders are atomics where they must be coherent
/// (see [`super::order`]).
fn membar(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut scope = None;
    for m in &i.mods {
        match *m {
            "CTA" | "SM" | "VC" => scope = Some(c.cta_scope()),
            "GPU" => scope = Some(SCOPE_DEVICE),
            "SYS" => {
                c.inexact("MEMBAR.SYS: system scope as device scope".into());
                scope = Some(SCOPE_DEVICE);
            }
            "SC" => c.inexact("MEMBAR.SC: acquire-release, not sequentially consistent".into()),
            "ALL" | "MMIO" => {}
            m => return bad(format!("MEMBAR.{m}")),
        }
    }
    let Some(scope) = scope else {
        return bad("MEMBAR scope");
    };
    let s = c.m.c_u32(scope);
    let sem =
        c.m.c_u32(SEM_ACQUIRE_RELEASE | SEM_UNIFORM | SEM_WORKGROUP | SEM_IMAGE);
    c.m.emit_void(op::MEMORY_BARRIER, &[s, sem]);
    Ok(())
}

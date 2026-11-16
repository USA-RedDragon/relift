//! Warp and block level operations: SHFL, VOTE, VOTEU, REDUX, MATCH, the
//! barriers and fences, and the special registers that describe a lane.
//!
//! A warp is a Vulkan subgroup, so the translated shader only means what the
//! guest meant when the device runs it with a subgroup size of 32 (the
//! runtime must require that size for any program that reads
//! `SR_LANEID` or uses these instructions). Lane `n` is the subgroup
//! invocation `n`. Block barriers become workgroup barriers whatever their
//! id; partial barriers (an arrival count) are not supported.

use super::spv;
use crate::spirv::op;
use crate::translate::{Ctx, INPUT, Res, bad};
use crate::{Inst, Operand};

const BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID: u32 = 41;
const SCOPE_DEVICE: u32 = 1;
const SCOPE_WORKGROUP: u32 = 2;
const SCOPE_SUBGROUP: u32 = 3;
const SEM_ACQUIRE_RELEASE: u32 = 0x8;
const SEM_UNIFORM: u32 = 0x40;
const SEM_WORKGROUP: u32 = 0x100;
const SEM_CROSS_WORKGROUP: u32 = 0x200;
const CAP_GROUP_NON_UNIFORM: u32 = 61;
const CAP_VOTE: u32 = 62;
const CAP_ARITHMETIC: u32 = 63;
const CAP_BALLOT: u32 = 64;
const CAP_SHUFFLE: u32 = 65;
const CAP_SHADER_CLOCK: u32 = 5055;

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "S2R" | "S2UR" => return s2r(c, i),
        "CS2R" => return cs2r(c, i),
        "SHFL" => shfl(c, i),
        "VOTE" | "VOTEU" => vote(c, i),
        "REDUX" => redux(c, i),
        "MATCH" => match_(c, i),
        "BAR" => bar(c, i),
        "MEMBAR" => membar(c, i),
        "ERRBAR" | "CCTL" | "CCTLL" | "CCTLT" | "LDGDEPBAR" => Ok(()),
        _ => return None,
    })
}

impl Ctx {
    pub(crate) fn lane_id(&mut self) -> u32 {
        self.cap(CAP_GROUP_NON_UNIFORM);
        let u = self.u32t();
        let var = self.builtin(BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID, INPUT, u);
        self.m.emit(op::LOAD, u, &[var])
    }

    /// The 32-bit mask of lanes for which `pred` holds, among the lanes
    /// that are active.
    fn ballot(&mut self, pred: u32) -> u32 {
        self.cap(CAP_GROUP_NON_UNIFORM);
        self.cap(CAP_BALLOT);
        let u = self.u32t();
        let v4 = self.m.t_vec(u, 4);
        let scope = self.m.c_u32(SCOPE_SUBGROUP);
        let b = self.m.emit(spv::GROUP_BALLOT, v4, &[scope, pred]);
        self.m.emit(op::COMPOSITE_EXTRACT, u, &[b, 0])
    }

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
        "SR_LANEID" => c.lane_id(),
        "SR_CLOCKLO" | "SR_CLOCK" => {
            let (lo, _) = clock(c);
            lo
        }
        "SR_CLOCKHI" => clock(c).1,
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
    let (lo, hi) = clock(c);
    Some(if i.has("32") {
        c.store_dst(&i.ops[0], lo)
    } else {
        c.store_pair(&i.ops[0], lo, hi)
    })
}

/// The SM clock as (low, high) words, from the shader clock extension.
fn clock(c: &mut Ctx) -> (u32, u32) {
    c.cap(CAP_SHADER_CLOCK);
    c.exts.insert("SPV_KHR_shader_clock");
    let u = c.u32t();
    let v2 = c.m.t_vec(u, 2);
    let scope = c.m.c_u32(SCOPE_DEVICE);
    let t = c.m.emit(spv::READ_CLOCK, v2, &[scope]);
    let lo = c.m.emit(op::COMPOSITE_EXTRACT, u, &[t, 0]);
    let hi = c.m.emit(op::COMPOSITE_EXTRACT, u, &[t, 1]);
    (lo, hi)
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
    c.cap(CAP_GROUP_NON_UNIFORM);
    c.cap(CAP_SHUFFLE);
    let a = c.usrc(&i.ops[2])?;
    let b = c.usrc(&i.ops[3])?;
    let cl = c.usrc(&i.ops[4])?;
    let lane = c.lane_id();
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
    let scope = c.m.c_u32(SCOPE_SUBGROUP);
    let v = c.m.emit(spv::GROUP_SHUFFLE, u, &[scope, a, src]);
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
    c.cap(CAP_GROUP_NON_UNIFORM);
    c.cap(CAP_VOTE);
    let p = c.pred_operand(ps)?;
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
    let b = c.boolt();
    let scope = c.m.c_u32(SCOPE_SUBGROUP);
    let r = c.m.emit(o, b, &[scope, p]);
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
    c.cap(CAP_GROUP_NON_UNIFORM);
    c.cap(CAP_ARITHMETIC);
    let v = c.usrc(&i.ops[1])?;
    let u = c.u32t();
    let scope = c.m.c_u32(SCOPE_SUBGROUP);
    let r = c.m.emit(o, u, &[scope, 0, v]);
    c.store_dst(&i.ops[0], r)
}

fn match_any(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.ops.len() != 2 {
        return bad("MATCH operands");
    }
    c.cap(CAP_GROUP_NON_UNIFORM);
    c.cap(CAP_SHUFFLE);
    let (lo, hi) = if i.has("U64") {
        c.src_pair(&i.ops[1])?
    } else {
        (c.usrc(&i.ops[1])?, c.m.c_u32(0))
    };
    let u = c.u32t();
    let scope = c.m.c_u32(SCOPE_SUBGROUP);
    let mut mask = c.m.c_u32(0);
    for k in 0..32 {
        let lane = c.m.c_u32(k);
        let l = c.m.emit(spv::GROUP_SHUFFLE, u, &[scope, lo, lane]);
        let h = c.m.emit(spv::GROUP_SHUFFLE, u, &[scope, hi, lane]);
        let el = c.cmp(op::I_EQUAL, l, lo);
        let eh = c.cmp(op::I_EQUAL, h, hi);
        let b = c.boolt();
        let eq = c.m.emit(op::LOGICAL_AND, b, &[el, eh]);
        let bit = c.m.c_u32(1 << k);
        let zero = c.m.c_u32(0);
        let v = c.select(u, eq, bit, zero);
        mask = c.bin(op::BITWISE_OR, mask, v);
    }
    let t = c.m.c_bool(true);
    let live = c.ballot(t);
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
    c.cap(CAP_GROUP_NON_UNIFORM);
    c.cap(CAP_VOTE);
    let (lo, hi) = if i.has("U64") {
        c.src_pair(&i.ops[2])?
    } else {
        (c.usrc(&i.ops[2])?, c.m.c_u32(0))
    };
    let b = c.boolt();
    let scope = c.m.c_u32(SCOPE_SUBGROUP);
    let e0 = c.m.emit(spv::GROUP_ALL_EQUAL, b, &[scope, lo]);
    let e1 = c.m.emit(spv::GROUP_ALL_EQUAL, b, &[scope, hi]);
    let eq = c.m.emit(op::LOGICAL_AND, b, &[e0, e1]);
    c.store_pdst(&i.ops[0], eq)?;
    if !matches!(i.ops[1], Operand::Reg(255, _)) {
        let t = c.m.c_bool(true);
        let all = c.ballot(t);
        let zero = c.m.c_u32(0);
        let u = c.u32t();
        let m = c.select(u, eq, all, zero);
        c.store_dst(&i.ops[1], m)?;
    }
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
    let exec = c.m.c_u32(SCOPE_WORKGROUP);
    let sem = c.m.c_u32(SEM_ACQUIRE_RELEASE | SEM_UNIFORM | SEM_WORKGROUP);
    c.m.emit_void(op::CONTROL_BARRIER, &[exec, exec, sem]);
    Ok(())
}

/// MEMBAR. Vulkan has no sequentially consistent fence, so MEMBAR.SC is
/// an acquire-release fence too.
fn membar(c: &mut Ctx, i: &Inst) -> Res<()> {
    let scope = if i.has("CTA") {
        SCOPE_WORKGROUP
    } else {
        SCOPE_DEVICE
    };
    let order = SEM_ACQUIRE_RELEASE;
    let s = c.m.c_u32(scope);
    let sem =
        c.m.c_u32(order | SEM_UNIFORM | SEM_WORKGROUP | SEM_CROSS_WORKGROUP);
    c.m.emit_void(op::MEMORY_BARRIER, &[s, sem]);
    Ok(())
}

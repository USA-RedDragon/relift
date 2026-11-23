//! Memory: global loads and stores through buffer device addresses, shared
//! and local memory, atomics, barriers and warp operations.
//!
//! A global address is a 64-bit GPU virtual address; the runtime gives
//! every guest memory pool a Vulkan buffer whose device address is the one
//! the game sees, so LDG/STG become physical storage buffer accesses.
//! Generic `LD`/`ST`/`ATOM` are treated as global: the shared and local
//! windows of the generic address space are not modeled, so they are
//! listed in `HostReport::inexact`. How global accesses are ordered is in
//! [`order`]. The asynchronous copy LDGSTS is a load and a store done at
//! once, so its barriers (LDGDEPBAR, DEPBAR) have nothing to wait for.
//!
//! The submodules own the other spaces: [`scratch`] (shared and local
//! memory), [`atomic`] (ATOM, ATOMG, ATOMS, RED) and [`warp`] (SHFL, VOTE,
//! barriers, special registers).

mod addr;
mod atomic;
mod atomic64;
mod clock;
pub(crate) mod lanes;
pub(crate) mod order;
mod scratch;
mod segments;
pub(crate) mod slots;
mod warp;

use super::{Ctx, PHYSICAL_STORAGE_BUFFER, Res, bad};
use crate::spirv::op;
use crate::{Inst, Operand};
use order::Order;

const ALIGNED: u32 = 2;
/// `OpAtomicOr`.
const ATOMIC_OR: u16 = 242;
/// Device scope, for the written-page marks.
const SCOPE_DEVICE_MARK: u32 = 1;
const SCOPE_DEVICE: u32 = 1;

/// SPIR-V opcodes the memory family uses that the shared opcode table does
/// not carry.
pub(super) mod spv {
    pub const ATOMIC_LOAD: u16 = 227;
    pub const ATOMIC_EXCHANGE: u16 = 229;
    pub const ATOMIC_COMPARE_EXCHANGE: u16 = 230;
    pub const ATOMIC_I_ADD: u16 = 234;
    pub const ATOMIC_S_MIN: u16 = 236;
    pub const ATOMIC_U_MIN: u16 = 237;
    pub const ATOMIC_S_MAX: u16 = 238;
    pub const ATOMIC_U_MAX: u16 = 239;
    pub const ATOMIC_AND: u16 = 240;
    pub const ATOMIC_OR: u16 = 241;
    pub const ATOMIC_XOR: u16 = 242;
    pub const LOOP_MERGE: u16 = 246;
    pub const GROUP_ALL: u16 = 334;
    pub const GROUP_ANY: u16 = 335;
    pub const GROUP_ALL_EQUAL: u16 = 336;
    pub const GROUP_BALLOT: u16 = 339;
    pub const GROUP_SHUFFLE: u16 = 345;
    pub const GROUP_I_ADD: u16 = 349;
    pub const GROUP_S_MIN: u16 = 353;
    pub const GROUP_U_MIN: u16 = 354;
    pub const GROUP_S_MAX: u16 = 356;
    pub const GROUP_U_MAX: u16 = 357;
    pub const GROUP_AND: u16 = 359;
    pub const GROUP_OR: u16 = 360;
    pub const GROUP_XOR: u16 = 361;
    pub const READ_CLOCK: u16 = 5056;
}

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    if let Some(r) = warp::inst(c, i) {
        return Some(r);
    }
    if let Some(r) = scratch::inst(c, i) {
        return Some(r);
    }
    if let Some(r) = atomic::inst(c, i) {
        return Some(r);
    }
    if matches!(i.mnemonic, "LD" | "ST") {
        c.generic(i);
    }
    Some(match i.mnemonic {
        "LDG" | "LD" => ldg(c, i),
        "STG" | "ST" => stg(c, i),
        "LDGSTS" => ldgsts(c, i),
        _ => return None,
    })
}

/// A parsed `[...]` address: register base (and whether it is a 64-bit
/// pair or a 32-bit offset), a scale shift of the register (`.X4` is 2),
/// uniform register pair base, constant offset.
#[derive(Debug, Default)]
pub(crate) struct Addr {
    pub reg: Option<(u8, bool)>,
    pub shift: u32,
    pub ureg: Option<u8>,
    pub offset: i64,
}

pub(crate) fn parse_addr(s: &str, wide_default: bool) -> Res<Addr> {
    let s = match s.strip_prefix("desc[") {
        Some(r) => match r.split_once("][") {
            Some((_, rest)) => format!("[{rest}"),
            None => return bad(format!("address {s}")),
        },
        None => s.to_string(),
    };
    let s = s.as_str();
    let Some(inner) = s.strip_prefix('[').and_then(|s| s.strip_suffix(']')) else {
        return bad(format!("address {s}"));
    };
    let mut a = Addr::default();
    let mut rest = inner.replace("-0x", "+-0x");
    if rest.starts_with('+') {
        rest.remove(0);
    }
    for t in rest.split('+') {
        if t.is_empty() || t == "RZ" || t == "RZ.X4" || t == "RZ.X8" || t == "RZ.X16" {
            continue;
        }
        if let Some(h) = t.strip_prefix("-0x") {
            a.offset -= i64::from_str_radix(h, 16)
                .map_err(|_| super::Unsupported(format!("address {s}")))?;
        } else if let Some(h) = t.strip_prefix("0x") {
            a.offset += i64::from_str_radix(h, 16)
                .map_err(|_| super::Unsupported(format!("address {s}")))?;
        } else if let Some(u) = t.strip_prefix("UR") {
            if u == "Z" {
                continue;
            }
            a.ureg = Some(
                u.parse()
                    .map_err(|_| super::Unsupported(format!("address {s}")))?,
            );
        } else if let Some(r) = t.strip_prefix('R') {
            let (n, wide) = match r.split_once('.') {
                Some((n, "64")) => (n, true),
                Some((n, "U32")) => (n, false),
                Some((n, "X4")) => {
                    a.shift = 2;
                    (n, false)
                }
                Some((n, "X8")) => {
                    a.shift = 3;
                    (n, false)
                }
                Some((n, "X16")) => {
                    a.shift = 4;
                    (n, false)
                }
                Some(_) => return bad(format!("address {s}")),
                None => (r, wide_default),
            };
            a.reg = Some((
                n.parse()
                    .map_err(|_| super::Unsupported(format!("address {s}")))?,
                wide,
            ));
        } else {
            return bad(format!("address {s}"));
        }
    }
    Ok(a)
}

impl Ctx {
    /// A generic access, carried out as a global one.
    pub(crate) fn generic(&mut self, i: &Inst) {
        self.inexact(format!(
            "{}: generic address as global (shared and local windows not modeled)",
            i.mnemonic
        ));
    }

    /// The 64-bit global address of a memory operand.
    pub(crate) fn global_address(&mut self, a: &Addr) -> Res<u32> {
        let mut parts = Vec::new();
        if let Some(u) = a.ureg {
            let lo = self.load_ureg(u);
            let hi = self.load_ureg(u + 1);
            parts.push(self.addr_pair(lo, hi));
        }
        if let Some((r, wide)) = a.reg {
            let lo = self.load_reg(r);
            parts.push(if wide {
                let hi = self.load_reg(r + 1);
                self.addr_pair(lo, hi)
            } else {
                self.addr_zext(lo)
            });
        }
        if a.offset != 0 || parts.is_empty() {
            parts.push(self.addr_const(a.offset as u64));
        }
        let mut acc = parts[0];
        for &p in &parts[1..] {
            acc = self.addr_add(acc, p);
        }
        Ok(acc)
    }

    /// A physical storage buffer pointer to `ty` at 64-bit address `addr`.
    /// Without buffer device addresses on the host, a pointer into the
    /// guest memory segments instead (see [`segments`]).
    pub(crate) fn global_ptr(&mut self, addr: u32, ty: u32) -> u32 {
        if !self.host.bda {
            return self.segment_ptr(addr, ty);
        }
        self.iface.host.global = super::GlobalPath::Bda;
        self.iface.global_memory = true;
        let p = self.m.t_ptr(PHYSICAL_STORAGE_BUFFER, ty);
        if self.host.int64 {
            self.m.emit(op::CONVERT_U_TO_PTR, p, &[addr])
        } else {
            self.m.emit(op::BITCAST, p, &[addr])
        }
    }

    /// As [`Self::global_ptr`], for a pointer the program stores or does an
    /// atomic read-modify-write through: the interface records that the
    /// program writes global memory, so a host that mirrors the memory
    /// knows to copy it back.
    pub(crate) fn global_ptr_w(&mut self, addr: u32, ty: u32) -> u32 {
        self.iface.global_store = true;
        if self.host.mirror_writes && self.host.bda {
            self.iface.marks_written_pages = true;
            self.mark_written(addr);
            let last = self.addr_add_const(addr, 15);
            self.mark_written(last);
        }
        self.global_ptr(addr, ty)
    }

    /// Sets the bit of the page of `addr` in the written-page bitmap (see
    /// [`super::io::PUSH_WRITTEN_PAGES`]) where it is not set yet. A write
    /// of up to 16 bytes at `a` touches the pages of `a` and `a + 15` only.
    fn mark_written(&mut self, addr: u32) {
        let u = self.u32t();
        let lo = self.addr_low(addr);
        let page = self.sh(op::SHIFT_RIGHT_LOGICAL, lo, 12);
        let page = self.and_k(page, super::io::WRITTEN_PAGE_BITS - 1);
        let word = self.sh(op::SHIFT_RIGHT_LOGICAL, page, 5);
        let shift = self.and_k(page, 31);
        let one = self.kc(1);
        let bit = self.ib(op::SHIFT_LEFT_LOGICAL, one, shift);
        let off = self.sh(op::SHIFT_LEFT_LOGICAL, word, 2);
        let (blo, bhi) = self.push_written_pages();
        let base = self.addr_pair(blo, bhi);
        let off = self.addr_zext(off);
        let at = self.addr_add(base, off);
        let p = self.global_ptr(at, u);
        let old = self.m.emit(op::LOAD, u, &[p, ALIGNED, 4]);
        let has = self.ib(op::BITWISE_AND, old, bit);
        let clear = self.cmp_k(op::I_EQUAL, has, 0);
        let set = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[clear, set, merge]);
        self.m.emit_void(op::LABEL, &[set]);
        let scope = self.m.c_u32(SCOPE_DEVICE_MARK);
        let sem = self.m.c_u32(0);
        self.m.emit(ATOMIC_OR, u, &[p, scope, sem, bit]);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
    }

    /// Extracts the sub-word `size` at byte address `addr` from the 32-bit
    /// word that contains it.
    pub(crate) fn extract_sub(&mut self, word: u32, addr: u32, size: Size) -> u32 {
        let u = self.u32t();
        let three = self.m.c_u32(3);
        let eight = self.m.c_u32(8);
        let lane = if size.bytes() == 1 {
            self.m.emit(op::BITWISE_AND, u, &[addr, three])
        } else {
            let two = self.m.c_u32(2);
            self.m.emit(op::BITWISE_AND, u, &[addr, two])
        };
        let off = self.m.emit(op::I_MUL, u, &[lane, eight]);
        let bits = self.m.c_u32(8 * size.bytes());
        let opc = if size.signed() {
            op::BIT_FIELD_S_EXTRACT
        } else {
            op::BIT_FIELD_U_EXTRACT
        };
        self.m.emit(opc, u, &[word, off, bits])
    }

    /// The (clear mask, insert bits) pair that put the low `size` bits of
    /// `val` at byte address `addr` of its containing word.
    pub(crate) fn insert_sub(&mut self, val: u32, addr: u32, size: Size) -> (u32, u32) {
        let u = self.u32t();
        let three = self.m.c_u32(3);
        let eight = self.m.c_u32(8);
        let lane = if size.bytes() == 1 {
            self.m.emit(op::BITWISE_AND, u, &[addr, three])
        } else {
            let two = self.m.c_u32(2);
            self.m.emit(op::BITWISE_AND, u, &[addr, two])
        };
        let off = self.m.emit(op::I_MUL, u, &[lane, eight]);
        let mask = self.m.c_u32(if size.bytes() == 1 { 0xff } else { 0xffff });
        let vm = self.m.emit(op::BITWISE_AND, u, &[val, mask]);
        let bits = self.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[vm, off]);
        let sm = self.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[mask, off]);
        let keep = self.m.emit(op::NOT, u, &[sm]);
        (keep, bits)
    }
}

/// An access width.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Size {
    U8,
    S8,
    U16,
    S16,
    W32,
    W64,
    W128,
}

impl Size {
    pub(crate) fn words(self) -> u32 {
        match self {
            Size::W64 => 2,
            Size::W128 => 4,
            _ => 1,
        }
    }

    pub(crate) fn bytes(self) -> u32 {
        match self {
            Size::U8 | Size::S8 => 1,
            Size::U16 | Size::S16 => 2,
            s => 4 * s.words(),
        }
    }

    pub(crate) fn sub(self) -> bool {
        self.bytes() < 4
    }

    pub(crate) fn signed(self) -> bool {
        matches!(self, Size::S8 | Size::S16)
    }
}

/// What the modifiers of a memory instruction say.
pub(crate) struct Mods {
    pub size: Size,
    pub order: Order,
}

pub(crate) fn mods_of(i: &Inst) -> Res<Mods> {
    let mut r = Mods {
        size: Size::W32,
        order: Order::Weak,
    };
    for m in &i.mods {
        match *m {
            "E" | "EF" | "EL" | "LU" | "EU" | "NA" | "BYPASS" => {}
            "U8" => r.size = Size::U8,
            "S8" => r.size = Size::S8,
            "U16" => r.size = Size::U16,
            "S16" => r.size = Size::S16,
            "64" => r.size = Size::W64,
            "128" => r.size = Size::W128,
            m if m.starts_with("LTC") => {}
            m => match Order::of(m) {
                Some(o) => r.order = o,
                None => return bad(format!("{}.{m}", i.mnemonic)),
            },
        }
    }
    Ok(r)
}

pub(crate) fn addr_operand(i: &Inst, k: usize) -> Res<Addr> {
    match i.ops.get(k) {
        Some(Operand::Sym(s)) => parse_addr(s, i.has("E")),
        o => bad(format!("{} address {o:?}", i.mnemonic)),
    }
}

fn vec_ty(c: &mut Ctx, n: u32) -> u32 {
    let u = c.u32t();
    if n == 1 { u } else { c.m.t_vec(u, n) }
}

fn ldg(c: &mut Ctx, i: &Inst) -> Res<()> {
    let m = mods_of(i)?;
    if i.ops.len() != 2 || !matches!(i.ops[0], Operand::Reg(..)) {
        return bad(format!("{} form", i.mnemonic));
    }
    let d = Ctx::dst_reg(&i.ops[0])?;
    let a = addr_operand(i, 1)?;
    let addr = c.global_address(&a)?;
    let scope = c.access_scope(i, m.order);
    let vals = if m.size.sub() {
        let u = c.u32t();
        let wa = c.addr_align(addr, 2);
        let w = match scope {
            Some(s) => c.atomic_load_words(wa, 1, m.order, s)[0],
            None => {
                let p = c.global_ptr(wa, u);
                c.m.emit(op::LOAD, u, &[p, ALIGNED, 4])
            }
        };
        let lo = c.addr_low(addr);
        vec![c.extract_sub(w, lo, m.size)]
    } else if let Some(s) = scope {
        c.atomic_load_words(addr, m.size.words(), m.order, s)
    } else {
        let n = m.size.words();
        let ty = vec_ty(c, n);
        let p = match c.slot_ptr(&a, ty, 4 * n) {
            Some(p) => p,
            None => c.global_ptr(addr, ty),
        };
        let v = c.m.emit(op::LOAD, ty, &[p, ALIGNED, 4 * n]);
        let u = c.u32t();
        (0..n)
            .map(|k| {
                if n == 1 {
                    v
                } else {
                    c.m.emit(op::COMPOSITE_EXTRACT, u, &[v, k])
                }
            })
            .collect()
    };
    store_words(c, d, &vals)
}

/// Register `k` of the register tuple that starts at `base`: every word of
/// an RZ tuple is RZ (reads zero, writes vanish).
pub(crate) fn tuple_reg(base: u8, k: u32) -> Res<u8> {
    if base == 255 {
        return Ok(255);
    }
    u8::try_from(u32::from(base) + k)
        .map_err(|_| super::Unsupported(format!("register tuple R{base} past RZ")))
}

pub(crate) fn store_words(c: &mut Ctx, d: u8, vals: &[u32]) -> Res<()> {
    for (k, &w) in vals.iter().enumerate() {
        c.store_reg(tuple_reg(d, k as u32)?, w);
    }
    Ok(())
}

/// The register operand `k` of a memory instruction with `n` operands.
pub(crate) fn reg_at(i: &Inst, n: usize, k: usize) -> Res<u8> {
    if i.ops.len() != n {
        return bad(format!("{} operands", i.mnemonic));
    }
    Ctx::dst_reg(&i.ops[k])
}

fn stg(c: &mut Ctx, i: &Inst) -> Res<()> {
    let m = mods_of(i)?;
    let s = reg_at(i, 2, 1)?;
    let a = addr_operand(i, 0)?;
    let addr = c.global_address(&a)?;
    let n = m.size.words();
    let mut parts = Vec::new();
    for k in 0..n {
        parts.push(c.load_reg(tuple_reg(s, k)?));
    }
    let scope = c.access_scope(i, m.order);
    if m.size.sub() {
        let u = c.u32t();
        let wa = c.addr_align(addr, 2);
        let lo = c.addr_low(addr);
        let (keep, bits) = c.insert_sub(parts[0], lo, m.size);
        let p = c.global_ptr_w(wa, u);
        c.cas_insert(p, scope.unwrap_or(SCOPE_DEVICE), keep, bits);
        return Ok(());
    }
    if let Some(s) = scope {
        c.atomic_store_words(addr, &parts, m.order, s);
        return Ok(());
    }
    let ty = vec_ty(c, n);
    let v = if n == 1 {
        parts[0]
    } else {
        c.m.emit(op::COMPOSITE_CONSTRUCT, ty, &parts)
    };
    let p = c.global_ptr_w(addr, ty);
    c.m.emit_void(op::STORE, &[p, v, ALIGNED, 4 * n]);
    Ok(())
}

/// LDGSTS: copy 4, 8 or 16 bytes from global to shared memory. With the
/// predicate false the shared bytes become zero; with `.ZFILL` the low four
/// bits of the source address say how many bytes at the end of the 16 are
/// zero, the rest coming from the 16-byte aligned address below it.
fn ldgsts(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut n = 1;
    let mut zfill = false;
    for m in &i.mods {
        match *m {
            "E" | "BYPASS" | "LTC128B" => {}
            "64" => n = 2,
            "128" => n = 4,
            "ZFILL" => zfill = true,
            m => return bad(format!("LDGSTS.{m}")),
        }
    }
    if zfill && n != 4 {
        return bad("LDGSTS.ZFILL size");
    }
    let (Some(Operand::Sym(ds)), Some(Operand::Sym(gs))) = (i.ops.first(), i.ops.get(1)) else {
        return bad("LDGSTS operands");
    };
    let da = parse_addr(ds, false)?;
    let ga = parse_addr(gs, true)?;
    let sa = c.scratch_address(&da, 4 * n, scratch::Space::Shared)?;
    let mut ga_addr = c.global_address(&ga)?;
    let pred = match i.ops.get(2) {
        Some(o) => Some(c.pred_operand(o)?),
        None => None,
    };
    let u = c.u32t();
    let zero = c.m.c_u32(0);
    let slots: Vec<u32> = (0..n)
        .map(|k| c.scratch_word(scratch::Space::Shared, sa, k))
        .collect();
    if pred.is_some() {
        for &p in &slots {
            c.m.emit_void(op::STORE, &[p, zero]);
        }
    }
    let merge = pred.map(|p| {
        let then = c.m.id();
        let merge = c.m.id();
        c.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        c.m.emit_void(op::BRANCH_CONDITIONAL, &[p, then, merge]);
        c.m.emit_void(op::LABEL, &[then]);
        merge
    });
    let keep = if zfill {
        let lo = c.addr_low(ga_addr);
        let k15 = c.m.c_u32(15);
        let cut = c.m.emit(op::BITWISE_AND, u, &[lo, k15]);
        let k16 = c.m.c_u32(16);
        ga_addr = c.addr_align(ga_addr, 4);
        Some(c.m.emit(op::I_SUB, u, &[k16, cut]))
    } else {
        None
    };
    let words = match c.access_scope(i, Order::Weak) {
        Some(s) => c.atomic_load_words(ga_addr, n, Order::Weak, s),
        None => {
            let ty = vec_ty(c, n);
            let gp = c.global_ptr(ga_addr, ty);
            let v = c.m.emit(op::LOAD, ty, &[gp, ALIGNED, 4 * n]);
            (0..n)
                .map(|k| {
                    if n == 1 {
                        v
                    } else {
                        c.m.emit(op::COMPOSITE_EXTRACT, u, &[v, k])
                    }
                })
                .collect()
        }
    };
    for (k, (&p, &w)) in slots.iter().zip(&words).enumerate() {
        let mut w = w;
        if let Some(keep) = keep {
            let base = c.m.c_u32(4 * k as u32);
            let b = c.boolt();
            let past = c.m.emit(op::U_LESS_THAN_EQUAL, b, &[keep, base]);
            let left = c.m.emit(op::I_SUB, u, &[keep, base]);
            let k4 = c.m.c_u32(4);
            let whole = c.m.emit(op::U_GREATER_THAN_EQUAL, b, &[left, k4]);
            let k8 = c.m.c_u32(8);
            let bits = c.m.emit(op::I_MUL, u, &[left, k8]);
            let one = c.m.c_u32(1);
            let top = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[one, bits]);
            let mask = c.m.emit(op::I_SUB, u, &[top, one]);
            let all = c.m.c_u32(!0);
            let mask = c.select(u, whole, all, mask);
            let mask = c.select(u, past, zero, mask);
            w = c.m.emit(op::BITWISE_AND, u, &[w, mask]);
        }
        c.m.emit_void(op::STORE, &[p, w]);
    }
    if let Some(merge) = merge {
        c.m.emit_void(op::BRANCH, &[merge]);
        c.m.emit_void(op::LABEL, &[merge]);
    }
    Ok(())
}

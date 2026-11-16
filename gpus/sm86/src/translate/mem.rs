//! Memory: global loads and stores through buffer device addresses, shared
//! and local memory, atomics, barriers and warp operations.
//!
//! A global address is a 64-bit GPU virtual address; the runtime gives
//! every guest memory pool a Vulkan buffer whose device address is the one
//! the game sees, so LDG/STG become physical storage buffer accesses.
//! Generic `LD`/`ST`/`ATOM` are treated as global: the shared and local
//! windows of the generic address space are not modeled.
//!
//! The submodules own the other spaces: [`scratch`] (shared and local
//! memory), [`atomic`] (ATOM, ATOMG, ATOMS, RED) and [`warp`] (SHFL, VOTE,
//! barriers, special registers).

mod atomic;
mod atomic64;
mod clock;
pub(crate) mod lanes;
mod scratch;
mod segments;
mod warp;

use super::{Ctx, PHYSICAL_STORAGE_BUFFER, Res, bad};
use crate::spirv::op;
use crate::{Inst, Operand};

const VOLATILE: u32 = 1;
const ALIGNED: u32 = 2;
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
    Some(match i.mnemonic {
        "LDG" | "LD" => ldg(c, i),
        "STG" | "ST" => stg(c, i),
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
    fn u64_of(&mut self, lo: u32, hi: u32) -> u32 {
        let u = self.u32t();
        let v2 = self.m.t_vec(u, 2);
        let pair = self.m.emit(op::COMPOSITE_CONSTRUCT, v2, &[lo, hi]);
        let t = self.m.t_u64();
        self.m.emit(op::BITCAST, t, &[pair])
    }

    /// The 64-bit global address of a memory operand.
    pub(crate) fn global_address(&mut self, a: &Addr) -> Res<u32> {
        self.cap(11);
        let t = self.m.t_u64();
        let mut parts = Vec::new();
        if let Some(u) = a.ureg {
            let lo = self.load_ureg(u);
            let hi = self.load_ureg(u + 1);
            parts.push(self.u64_of(lo, hi));
        }
        if let Some((r, wide)) = a.reg {
            let lo = self.load_reg(r);
            parts.push(if wide {
                let hi = self.load_reg(r + 1);
                self.u64_of(lo, hi)
            } else {
                self.m.emit(op::U_CONVERT, t, &[lo])
            });
        }
        if a.offset != 0 || parts.is_empty() {
            parts.push(self.m.c_u64(a.offset as u64));
        }
        let mut acc = parts[0];
        for &p in &parts[1..] {
            acc = self.m.emit(op::I_ADD, t, &[acc, p]);
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
        self.m.emit(op::CONVERT_U_TO_PTR, p, &[addr])
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
    pub volatile: bool,
}

pub(crate) fn mods_of(i: &Inst) -> Res<Mods> {
    let mut r = Mods {
        size: Size::W32,
        volatile: false,
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
            m if m.starts_with("STRONG.GPU")
                || m.starts_with("STRONG.SYS")
                || m.starts_with("MMIO") =>
            {
                r.volatile = true;
            }
            m if m.starts_with("STRONG") || m.starts_with("CONSTANT") || m.starts_with("LTC") => {}
            m => return bad(format!("{}.{m}", i.mnemonic)),
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

fn mem_flags(volatile: bool) -> u32 {
    if volatile {
        VOLATILE | ALIGNED
    } else {
        ALIGNED
    }
}

fn ldg(c: &mut Ctx, i: &Inst) -> Res<()> {
    let m = mods_of(i)?;
    if i.ops.len() != 2 || !matches!(i.ops[0], Operand::Reg(..)) {
        return bad(format!("{} form", i.mnemonic));
    }
    let d = Ctx::dst_reg(&i.ops[0])?;
    let a = addr_operand(i, 1)?;
    let addr = c.global_address(&a)?;
    let vals = if m.size.sub() {
        let u = c.u32t();
        let t = c.m.t_u64();
        let mask = c.m.c_u64(!3);
        let wa = c.m.emit(op::BITWISE_AND, t, &[addr, mask]);
        let p = c.global_ptr(wa, u);
        let w = c.m.emit(op::LOAD, u, &[p, mem_flags(m.volatile), 4]);
        let lo = c.m.emit(op::U_CONVERT, u, &[addr]);
        vec![c.extract_sub(w, lo, m.size)]
    } else {
        let n = m.size.words();
        let ty = vec_ty(c, n);
        let p = c.global_ptr(addr, ty);
        let v = c.m.emit(op::LOAD, ty, &[p, mem_flags(m.volatile), 4 * n]);
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
    if m.size.sub() {
        let u = c.u32t();
        let t = c.m.t_u64();
        let mask = c.m.c_u64(!3);
        let wa = c.m.emit(op::BITWISE_AND, t, &[addr, mask]);
        let lo = c.m.emit(op::U_CONVERT, u, &[addr]);
        let (keep, bits) = c.insert_sub(parts[0], lo, m.size);
        let p = c.global_ptr(wa, u);
        let scope = c.m.c_u32(SCOPE_DEVICE);
        let sem = c.m.c_u32(0);
        c.m.emit(spv::ATOMIC_AND, u, &[p, scope, sem, keep]);
        c.m.emit(spv::ATOMIC_OR, u, &[p, scope, sem, bits]);
        return Ok(());
    }
    let ty = vec_ty(c, n);
    let v = if n == 1 {
        parts[0]
    } else {
        c.m.emit(op::COMPOSITE_CONSTRUCT, ty, &parts)
    };
    let p = c.global_ptr(addr, ty);
    c.m.emit_void(op::STORE, &[p, v, mem_flags(m.volatile), 4 * n]);
    Ok(())
}

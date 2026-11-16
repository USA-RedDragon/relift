//! Memory: global loads and stores through buffer device addresses.
//!
//! A global address is a 64-bit GPU virtual address; the runtime gives
//! every guest memory pool a Vulkan buffer whose device address is the one
//! the game sees, so LDG/STG become physical storage buffer accesses.

use super::{Ctx, PHYSICAL_STORAGE_BUFFER, Res, bad};
use crate::spirv::op;
use crate::{Inst, Operand};

const ALIGNED: u32 = 2;

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "LDG" => ldg(c, i),
        "STG" => stg(c, i),
        _ => return None,
    })
}

/// A parsed `[...]` address: register base (and whether it is a 64-bit
/// pair or a 32-bit offset), uniform register pair base, constant offset.
#[derive(Debug, Default)]
pub(crate) struct Addr {
    pub reg: Option<(u8, bool)>,
    pub ureg: Option<u8>,
    pub offset: i64,
}

pub(crate) fn parse_addr(s: &str, wide_default: bool) -> Res<Addr> {
    let Some(inner) = s.strip_prefix('[').and_then(|s| s.strip_suffix(']')) else {
        return bad(format!("address {s}"));
    };
    let mut a = Addr::default();
    let mut rest = inner.replace("-0x", "+-0x");
    if rest.starts_with('+') {
        rest.remove(0);
    }
    for t in rest.split('+') {
        if t == "RZ" {
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
    pub(crate) fn global_ptr(&mut self, addr: u32, ty: u32) -> u32 {
        self.iface.global_memory = true;
        let p = self.m.t_ptr(PHYSICAL_STORAGE_BUFFER, ty);
        self.m.emit(op::CONVERT_U_TO_PTR, p, &[addr])
    }
}

fn words_of(i: &Inst) -> Res<u32> {
    let mut n = 1;
    for m in &i.mods {
        match *m {
            "E" | "STRONG.SM" | "STRONG.GPU" | "STRONG.SYS" | "CONSTANT" | "EF" | "EL" | "LU"
            | "EU" | "NA" => {}
            "64" => n = 2,
            "128" => n = 4,
            m if m.starts_with("STRONG") || m.starts_with("CONSTANT") || m.starts_with("LTC") => {}
            m => return bad(format!("{}.{m}", i.mnemonic)),
        }
    }
    Ok(n)
}

fn addr_operand(i: &Inst, k: usize) -> Res<Addr> {
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
    let n = words_of(i)?;
    if i.ops.len() != 2 {
        return bad("LDG with a predicate");
    }
    let d = Ctx::dst_reg(&i.ops[0])?;
    let a = addr_operand(i, 1)?;
    let addr = c.global_address(&a)?;
    let ty = vec_ty(c, n);
    let p = c.global_ptr(addr, ty);
    let v = c.m.emit(op::LOAD, ty, &[p, ALIGNED, 4 * n]);
    let u = c.u32t();
    for k in 0..n {
        let w = if n == 1 {
            v
        } else {
            c.m.emit(op::COMPOSITE_EXTRACT, u, &[v, k])
        };
        if d != 255 {
            c.store_reg(d + k as u8, w);
        }
    }
    Ok(())
}

fn stg(c: &mut Ctx, i: &Inst) -> Res<()> {
    let n = words_of(i)?;
    let a = addr_operand(i, 0)?;
    let s = Ctx::dst_reg(&i.ops[1])?;
    let addr = c.global_address(&a)?;
    let ty = vec_ty(c, n);
    let mut parts = Vec::new();
    for k in 0..n {
        parts.push(if s == 255 {
            c.m.c_u32(0)
        } else {
            c.load_reg(s + k as u8)
        });
    }
    let v = if n == 1 {
        parts[0]
    } else {
        c.m.emit(op::COMPOSITE_CONSTRUCT, ty, &parts)
    };
    let p = c.global_ptr(addr, ty);
    c.m.emit_void(op::STORE, &[p, v, ALIGNED, 4 * n]);
    Ok(())
}

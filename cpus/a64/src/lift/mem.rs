use relift_ir::{Builder, Endian, Order, Value, Width};

use super::{End, LiftError, LiftFn, read_x, read_x_sp, write_x};
use crate::decode::{Encoding, Insn};

pub(super) fn lifter(enc: Encoding) -> Option<LiftFn> {
    use Encoding::*;
    let f: LiftFn = match enc {
        STLXR_SR32_ldstexclr | STLXR_SR64_ldstexclr => store_exclusive,
        LDAXR_LR32_ldstexclr | LDAXR_LR64_ldstexclr => load_exclusive,
        LDR_32_ldst_pos | LDR_64_ldst_pos | STR_32_ldst_pos | STR_64_ldst_pos => ldst_pos,
        _ => return None,
    };
    Some(f)
}

fn load(b: &mut Builder, addr: Value, w: Width, order: Order) -> Value {
    b.load(addr, w, Endian::Little, order)
}

/// Loads or stores `x<t>` at `base + offset` per the `size`/`opc` fields.
fn single(insn: Insn, b: &mut Builder, offset: Value) -> Result<End, LiftError> {
    let size = insn.get("size");
    let opc = insn.get("opc");
    let (n, t) = (insn.get("Rn"), insn.get("Rt"));
    let w = Width::new(8 << size);
    let base = read_x_sp(b, n, Width::W64);
    let addr = b.add(base, offset);
    match opc {
        0 => {
            let v = read_x(b, t, w);
            b.store(addr, v, Endian::Little, Order::Plain);
        }
        1 => {
            let v = load(b, addr, w, Order::Plain);
            write_x(b, t, v);
        }
        2 | 3 => {
            let v = load(b, addr, w, Order::Plain);
            let to = if opc == 2 { Width::W64 } else { Width::W32 };
            let s = b.sext(v, to);
            write_x(b, t, s);
        }
        _ => return Err(LiftError::Undefined(insn.encoding)),
    }
    Ok(End::Next)
}

fn ldst_pos(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let off = u128::from(insn.get("imm12")) << insn.get("size");
    let o = b.konst(Width::W64, off);
    single(insn, b, o)
}

fn excl_width(insn: &Insn) -> Width {
    if insn.get("size") == 3 {
        Width::W64
    } else {
        Width::W32
    }
}

fn load_exclusive(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = excl_width(&insn);
    let order = if insn.get("o0") == 1 {
        Order::Acquire
    } else {
        Order::Plain
    };
    let addr = read_x_sp(b, insn.get("Rn"), Width::W64);
    let v = b.load_exclusive(addr, w, Endian::Little, order);
    write_x(b, insn.get("Rt"), v);
    Ok(End::Next)
}

fn store_exclusive(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = excl_width(&insn);
    let (s, n, t) = (insn.get("Rs"), insn.get("Rn"), insn.get("Rt"));
    if s == t || (s == n && n != 31) {
        return Err(LiftError::Unpredictable(insn.encoding));
    }
    let order = if insn.get("o0") == 1 {
        Order::Release
    } else {
        Order::Plain
    };
    let addr = read_x_sp(b, n, Width::W64);
    let v = read_x(b, t, w);
    let ok = b.store_exclusive(addr, v, Endian::Little, order);
    let failed = b.not(ok);
    let status = b.zext(failed, Width::W32);
    write_x(b, s, status);
    Ok(End::Next)
}

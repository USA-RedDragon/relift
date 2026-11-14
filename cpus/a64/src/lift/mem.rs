use relift_ir::{Builder, Endian, Order, Value, Width};

use super::{End, LiftError, LiftFn, read_x, read_x_sp, sext_imm, write_x, write_x_sp};
use crate::decode::{Encoding, Insn};

/// The addressing families of the integer single-register loads and stores.
const FAMILIES: [(&str, LiftFn); 5] = [
    ("_ldst_pos", ldst_pos),
    ("_ldst_immpre", ldst_immpre),
    ("_ldst_immpost", ldst_immpost),
    ("_ldst_unscaled", ldst_unscaled),
    ("_ldst_regoff", ldst_regoff),
];

pub(super) fn lifter(enc: Encoding) -> Option<LiftFn> {
    use Encoding::*;
    let f: LiftFn = match enc {
        STP_32_ldstpair_post | LDP_32_ldstpair_post | STP_64_ldstpair_post
        | LDP_64_ldstpair_post | STP_32_ldstpair_off | LDP_32_ldstpair_off
        | STP_64_ldstpair_off | LDP_64_ldstpair_off | STP_32_ldstpair_pre | LDP_32_ldstpair_pre
        | STP_64_ldstpair_pre | LDP_64_ldstpair_pre => ldstpair,
        STXR_SR32_ldstexclr | STLXR_SR32_ldstexclr | STXR_SR64_ldstexclr | STLXR_SR64_ldstexclr => {
            store_exclusive
        }
        LDXR_LR32_ldstexclr | LDAXR_LR32_ldstexclr | LDXR_LR64_ldstexclr | LDAXR_LR64_ldstexclr => {
            load_exclusive
        }
        LDAR_LR32_ldstord | LDAR_LR64_ldstord | STLR_SL32_ldstord | STLR_SL64_ldstord => ordered,
        _ => return integer_family(enc),
    };
    Some(f)
}

/// The general-register forms of the single-register families: bit 26 (the
/// SIMD selector) fixed to 0, and not a prefetch.
fn integer_family(enc: Encoding) -> Option<LiftFn> {
    let name = enc.name();
    let vr = 1u32 << 26;
    if enc.mask() & vr == 0 || enc.value() & vr != 0 || name.contains("PRF") {
        return None;
    }
    FAMILIES
        .iter()
        .find(|(suffix, _)| name.ends_with(suffix))
        .map(|(_, f)| *f)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    Offset,
    Pre,
    Post,
}

fn load(b: &mut Builder, addr: Value, w: Width, order: Order) -> Value {
    b.load(addr, w, Endian::Little, order)
}

/// Loads or stores `x<t>` at `base + offset` per the `size`/`opc` fields,
/// with writeback for the pre and post modes.
fn single(insn: Insn, b: &mut Builder, offset: Value, mode: Mode) -> Result<End, LiftError> {
    let size = insn.get("size");
    let opc = insn.get("opc");
    let (n, t) = (insn.get("Rn"), insn.get("Rt"));
    let wback = mode != Mode::Offset;
    if wback && n == t && n != 31 {
        return Err(LiftError::Unpredictable(insn.encoding));
    }
    let w = Width::new(8 << size);
    let base = read_x_sp(b, n, Width::W64);
    let moved = b.add(base, offset);
    let addr = if mode == Mode::Post { base } else { moved };
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
    if wback {
        write_x_sp(b, n, moved);
    }
    Ok(End::Next)
}

fn ldst_pos(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let off = u128::from(insn.get("imm12")) << insn.get("size");
    let o = b.konst(Width::W64, off);
    single(insn, b, o, Mode::Offset)
}

fn imm9(insn: &Insn, b: &mut Builder) -> Value {
    let off = sext_imm(insn.get("imm9"), 9) as u64;
    b.konst(Width::W64, u128::from(off))
}

fn ldst_immpre(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let o = imm9(&insn, b);
    single(insn, b, o, Mode::Pre)
}

fn ldst_immpost(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let o = imm9(&insn, b);
    single(insn, b, o, Mode::Post)
}

fn ldst_unscaled(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let o = imm9(&insn, b);
    single(insn, b, o, Mode::Offset)
}

fn ldst_regoff(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let option = insn.get("option");
    let (w, signed) = match option {
        0b010 => (Width::W32, false),
        0b011 => (Width::W64, false),
        0b110 => (Width::W32, true),
        0b111 => (Width::W64, true),
        _ => return Err(LiftError::Undefined(insn.encoding)),
    };
    let shift = if insn.get("S") == 1 {
        insn.get("size")
    } else {
        0
    };
    let m = read_x(b, insn.get("Rm"), w);
    let ext = if signed {
        b.sext(m, Width::W64)
    } else {
        b.zext(m, Width::W64)
    };
    let o = if shift == 0 {
        ext
    } else {
        let k = b.konst(Width::W64, u128::from(shift));
        b.bin(relift_ir::BinOp::Shl, ext, k)
    };
    single(insn, b, o, Mode::Offset)
}

fn ldstpair(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let name = insn.encoding.name();
    let mode = if name.ends_with("_post") {
        Mode::Post
    } else if name.ends_with("_pre") {
        Mode::Pre
    } else {
        Mode::Offset
    };
    let is_load = insn.get("L") == 1;
    let (n, t, t2) = (insn.get("Rn"), insn.get("Rt"), insn.get("Rt2"));
    let wback = mode != Mode::Offset;
    if (is_load && t == t2) || (wback && (t == n || t2 == n) && n != 31) {
        return Err(LiftError::Unpredictable(insn.encoding));
    }
    let scale = 2 + (insn.get("opc") >> 1);
    let w = Width::new(8 << scale);
    let off = (sext_imm(insn.get("imm7"), 7) << scale) as u64;
    let base = read_x_sp(b, n, Width::W64);
    let o = b.konst(Width::W64, u128::from(off));
    let moved = b.add(base, o);
    let addr = if mode == Mode::Post { base } else { moved };
    let step = b.konst(Width::W64, u128::from(w.bits() / 8));
    let addr2 = b.add(addr, step);
    if is_load {
        let v1 = load(b, addr, w, Order::Plain);
        let v2 = load(b, addr2, w, Order::Plain);
        write_x(b, t, v1);
        write_x(b, t2, v2);
    } else {
        let v1 = read_x(b, t, w);
        let v2 = read_x(b, t2, w);
        b.store(addr, v1, Endian::Little, Order::Plain);
        b.store(addr2, v2, Endian::Little, Order::Plain);
    }
    if wback {
        write_x_sp(b, n, moved);
    }
    Ok(End::Next)
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

fn ordered(insn: Insn, b: &mut Builder) -> Result<End, LiftError> {
    let w = excl_width(&insn);
    let addr = read_x_sp(b, insn.get("Rn"), Width::W64);
    if insn.get("L") == 1 {
        let v = load(b, addr, w, Order::Acquire);
        write_x(b, insn.get("Rt"), v);
    } else {
        let v = read_x(b, insn.get("Rt"), w);
        b.store(addr, v, Endian::Little, Order::Release);
    }
    Ok(End::Next)
}

//! C types and expressions for IR values.
//!
//! A value of width `w` lives in the smallest unsigned C type that holds it
//! and always holds its value modulo 2^w. Arithmetic runs in the wide type
//! (`uint64_t` up to 64 bits, `rl_u128` above), never in a promoted `int`.

use relift_ir::{BinOp, Block, CmpOp, Endian, ExtKind, FpOp, Op, Order, Reg, UnOp, Value, Width};

use relift_guest::Frontend;

/// The C type of a value of width `w`.
pub(crate) fn cty(w: Width) -> &'static str {
    match w.bits() {
        0..=8 => "uint8_t",
        9..=16 => "uint16_t",
        17..=32 => "uint32_t",
        33..=64 => "uint64_t",
        _ => "rl_u128",
    }
}

fn wide(w: Width) -> &'static str {
    if w.bits() <= 64 {
        "uint64_t"
    } else {
        "rl_u128"
    }
}

fn natural(w: Width) -> bool {
    matches!(w.bits(), 8 | 16 | 32 | 64 | 128)
}

/// `e` (in the wide type) reduced to width `w`, as the initializer of a
/// local of its C type: a natural width needs nothing, the conversion to
/// the local's type reduces it; other widths are masked.
fn fit(e: &str, w: Width) -> String {
    if natural(w) {
        e.to_string()
    } else {
        format!("({e}) & {}", mask(w))
    }
}

fn mask(w: Width) -> String {
    if w.bits() <= 32 {
        format!("{:#x}u", w.mask())
    } else if w.bits() <= 64 {
        format!("UINT64_C({:#x})", w.mask())
    } else {
        format!("rl_e_m128({})", w.bits())
    }
}

/// A C constant of width `w`.
pub(crate) fn lit(v: u128, w: Width) -> String {
    let v = v & w.mask();
    if w.bits() <= 64 {
        format!("UINT64_C({v:#x})")
    } else if v >> 64 == 0 {
        format!("((rl_u128)UINT64_C({:#x}))", v as u64)
    } else if v == u128::MAX {
        "(~(rl_u128)0)".into()
    } else {
        format!(
            "(((rl_u128)UINT64_C({:#x}) << 64) | UINT64_C({:#x}))",
            (v >> 64) as u64,
            v as u64
        )
    }
}

/// A C constant of width `w` whose type is the C type of the width, so it
/// can stand wherever a local of that width would.
pub(crate) fn typed_lit(v: u128, w: Width) -> String {
    let v = v & w.mask();
    match cty(w) {
        "uint8_t" | "uint16_t" => format!("(({}){v:#x})", cty(w)),
        "uint32_t" => format!("UINT32_C({v:#x})"),
        _ => lit(v, w),
    }
}

/// The C expressions that stand for values of `b` in place of a local:
/// constants, and reads of registers the block does not write (sets apply
/// when the block exits, so such a read would otherwise see the entry
/// value through its local anyway).
pub(crate) fn inlined(b: &Block) -> Vec<Option<String>> {
    let written: Vec<Reg> = b
        .insts
        .iter()
        .filter_map(|i| match i.op {
            Op::Set(r, _) => Some(r),
            _ => None,
        })
        .collect();
    b.insts
        .iter()
        .map(|i| match i.op {
            Op::Const(c) => Some(typed_lit(c, i.width.unwrap_or(Width::W1))),
            Op::Get(r) if !written.contains(&r) => Some(reg(r)),
            _ => None,
        })
        .collect()
}

/// The C local of value `v`.
pub(crate) fn val(v: Value) -> String {
    format!("v{}", v.0)
}

/// The C local of register `r`.
pub(crate) fn reg(r: Reg) -> String {
    format!("r{}", r.index())
}

fn sx(e: &str, w: Width) -> String {
    // The natural widths convert through the signed type of the width;
    // clang and zig cc reduce modulo 2^n.
    match w.bits() {
        8 => return format!("(int64_t)(int8_t)({e})"),
        16 => return format!("(int64_t)(int16_t)({e})"),
        32 => return format!("(int64_t)(int32_t)({e})"),
        64 => return format!("(int64_t)({e})"),
        128 => return format!("(rl_e_s128)({e})"),
        _ => {}
    }
    if w.bits() <= 64 {
        format!("rl_e_sx64((uint64_t)({e}), {})", w.bits())
    } else {
        format!("rl_e_sx128((rl_u128)({e}), {})", w.bits())
    }
}

/// What one instruction emits.
pub(crate) enum Emitted {
    /// A value: its C expression.
    Value(String),
    /// A statement with no value.
    Stmt(String),
    /// A register write, applied when the block exits.
    Set(Reg, String),
    /// A guard: leave for slow path `code` when the value is 1.
    Guard(Value, u32),
}

/// The C for instruction `i` of `b`, or why it cannot be expressed.
///
/// `inl` holds the expressions that stand for some values in place of
/// their locals ([`inlined`]).
pub(crate) fn emit_op(
    fe: &dyn Frontend,
    b: &Block,
    i: usize,
    inl: &[Option<String>],
) -> Result<Emitted, String> {
    let inst = &b.insts[i];
    let wv = |v: Value| b.insts[v.index()].width.unwrap_or(Width::W1);
    let w = inst.width.unwrap_or(Width::W1);
    let u = wide(w);
    let val = |v: Value| inl[v.index()].clone().unwrap_or_else(|| val(v));
    let a = |v: &Value| {
        if cty(wv(*v)) == u {
            val(*v)
        } else {
            format!("({u}){}", val(*v))
        }
    };
    let e = match &inst.op {
        Op::Const(c) => fit(&lit(*c, w), w),
        Op::Get(r) => reg(*r),
        Op::Set(r, v) => return Ok(Emitted::Set(*r, val(*v))),
        Op::Guard(c, code) => return Ok(Emitted::Guard(*c, *code)),
        Op::Un(op, x) => {
            let x64 = w.bits() <= 64;
            let ax = a(x);
            let n = w.bits();
            let e = match op {
                UnOp::Not if n == 1 => return Ok(Emitted::Value(format!("!{}", val(*x)))),
                UnOp::Not => format!("~{ax}"),
                UnOp::Neg => format!("({u})0 - {ax}"),
                UnOp::Clz if x64 => format!("rl_e_clz64({ax}, {n})"),
                UnOp::Clz => format!("rl_e_clz128({ax}, {n})"),
                UnOp::Ctz if x64 => format!("rl_e_ctz64({ax}, {n})"),
                UnOp::Ctz => format!("rl_e_ctz128({ax}, {n})"),
                UnOp::Popcount if x64 => format!("(uint64_t)__builtin_popcountll({ax})"),
                UnOp::Popcount => format!("rl_e_popcount128({ax})"),
                UnOp::BitReverse if x64 => format!("rl_e_rbit64({ax}) >> {}", 64 - n),
                UnOp::BitReverse => format!("rl_e_rbit128({ax}) >> {}", 128 - n),
                UnOp::ByteSwap => match n {
                    8 => ax,
                    16 => format!("__builtin_bswap16((uint16_t){})", val(*x)),
                    32 => format!("__builtin_bswap32((uint32_t){})", val(*x)),
                    64 => format!("__builtin_bswap64({ax})"),
                    128 => format!("rl_e_bswap128({ax})"),
                    _ if x64 => format!("__builtin_bswap64({ax}) >> {}", 64 - n),
                    _ => format!("rl_e_bswap128({ax}) >> {}", 128 - n),
                },
            };
            fit(&e, w)
        }
        Op::Bin(op, x, y) => {
            let (ax, ay) = (a(x), a(y));
            let n = w.bits();
            let x64 = n <= 64;
            let amt = val(*y);
            // A constant shift amount below the width needs no range check.
            let k = match b.insts[y.index()].op {
                Op::Const(k) if k < u128::from(n) => Some(k),
                _ => None,
            };
            let e = match (op, k) {
                (BinOp::Shl, Some(k)) => format!("{ax} << {k}"),
                (BinOp::LShr, Some(k)) => format!("{ax} >> {k}"),
                (BinOp::AShr, Some(k)) => format!("({u})({} >> {k})", sx(&val(*x), w)),
                _ => match op {
                    BinOp::Add => format!("{ax} + {ay}"),
                    BinOp::Sub => format!("{ax} - {ay}"),
                    BinOp::Mul => format!("{ax} * {ay}"),
                    BinOp::And => format!("{ax} & {ay}"),
                    BinOp::Or => format!("{ax} | {ay}"),
                    BinOp::Xor => format!("{ax} ^ {ay}"),
                    BinOp::UDiv => format!("{ay} ? {ax} / {ay} : 0"),
                    BinOp::URem => format!("{ay} ? {ax} % {ay} : {ax}"),
                    BinOp::SDiv if x64 => format!("rl_e_sdiv64({ax}, {ay}, {n})"),
                    BinOp::SDiv => format!("rl_e_sdiv128({ax}, {ay}, {n})"),
                    BinOp::SRem if x64 => format!("rl_e_srem64({ax}, {ay}, {n})"),
                    BinOp::SRem => format!("rl_e_srem128({ax}, {ay}, {n})"),
                    BinOp::Shl => format!("{amt} >= {n} ? 0 : {ax} << (unsigned){amt}"),
                    BinOp::LShr => format!("{amt} >= {n} ? 0 : {ax} >> (unsigned){amt}"),
                    BinOp::AShr => {
                        let s = sx(&val(*x), w);
                        format!("({u})({s} >> ({amt} >= {n} ? {} : (unsigned){amt}))", n - 1)
                    }
                    BinOp::RotR if x64 => format!("rl_e_ror64({ax}, (unsigned)({amt} % {n}), {n})"),
                    BinOp::RotR => format!("rl_e_ror128({ax}, (unsigned)({amt} % {n}), {n})"),
                },
            };
            fit(&e, w)
        }
        Op::Cmp(op, x, y) => {
            let wx = wv(*x);
            let (vx, vy) = (val(*x), val(*y));
            match op {
                CmpOp::Eq => format!("{vx} == {vy}"),
                CmpOp::Ne => format!("{vx} != {vy}"),
                CmpOp::Ult => format!("{vx} < {vy}"),
                CmpOp::Ule => format!("{vx} <= {vy}"),
                CmpOp::Slt => format!("{} < {}", sx(&vx, wx), sx(&vy, wx)),
                CmpOp::Sle => format!("{} <= {}", sx(&vx, wx), sx(&vy, wx)),
            }
        }
        Op::Select(c, x, y) => format!("{} ? {} : {}", val(*c), val(*x), val(*y)),
        Op::Ext(ExtKind::Zero, x) => format!("({}){}", cty(w), val(*x)),
        Op::Ext(ExtKind::Sign, x) => {
            let s = sx(&val(*x), wv(*x));
            let s = if w.bits() > 64 && wv(*x).bits() <= 64 {
                format!("(rl_e_s128){s}")
            } else {
                s
            };
            fit(&format!("({u}){s}"), w)
        }
        Op::Extract(x, 0) => fit(&a(x), w),
        Op::Extract(x, lo) => {
            let ux = wide(wv(*x));
            fit(&format!("(({ux}){} >> {lo})", val(*x)), w)
        }
        Op::Concat(h, l) => {
            let wl = wv(*l).bits();
            fit(&format!("(({u}){} << {wl}) | ({u}){}", val(*h), val(*l)), w)
        }
        Op::Load {
            addr,
            endian,
            order,
        } => {
            let n = bytes(w)?;
            let ad = val(*addr);
            let raw = match order {
                Order::Plain => format!("rl_ld{}(mem, {ad})", n * 8),
                _ if n <= 8 => format!("rl_ldar(mem, {ad}, {n})"),
                _ => return Err("an ordered 128-bit load".into()),
            };
            swap(&fit(&raw, w), w, *endian)?
        }
        Op::Store {
            addr,
            value,
            endian,
            order,
        } => {
            let wv = wv(*value);
            let n = bytes(wv)?;
            let v = swap(&val(*value), wv, *endian)?;
            let ad = val(*addr);
            return Ok(Emitted::Stmt(match order {
                Order::Plain => format!("rl_st{}(mem, {ad}, {v})", n * 8),
                _ if n <= 8 => format!("rl_stlr(mem, {ad}, {n}, (uint64_t)({v}))"),
                _ => return Err("an ordered 128-bit store".into()),
            }));
        }
        Op::LoadExclusive {
            addr,
            endian,
            order,
        } => {
            little(*endian)?;
            let n = bytes(w)?;
            let (m, ord) = (fe.monitor(), ord(*order));
            let ad = val(*addr);
            if n == 16 {
                format!(
                    "({{ uint64_t hi_; uint64_t lo_ = rl_ldxp64({m}, mem, {ad}, &hi_, {ord}); ((rl_u128)hi_ << 64) | lo_; }})"
                )
            } else {
                fit(&format!("rl_ldx({m}, mem, {ad}, {n}, {ord})"), w)
            }
        }
        Op::StoreExclusive {
            addr,
            value,
            endian,
            order,
        } => {
            little(*endian)?;
            let n = bytes(wv(*value))?;
            let (m, ord) = (fe.monitor(), ord(*order));
            let (ad, v) = (val(*addr), val(*value));
            if n == 16 {
                format!(
                    "(uint8_t)!rl_stxp64({m}, mem, {ad}, (uint64_t){v}, (uint64_t)({v} >> 64), {ord})"
                )
            } else {
                format!("(uint8_t)!rl_stx({m}, mem, {ad}, {n}, (uint64_t){v}, {ord})")
            }
        }
        Op::ClearExclusive => return Ok(Emitted::Stmt(format!("rl_clrex({})", fe.monitor()))),
        Op::Fence(o) => {
            return Ok(Emitted::Stmt(
                match o {
                    Order::Plain => "(void)0",
                    Order::Acquire => "rl_dmb_ld()",
                    Order::Release => "rl_dmb_st()",
                    Order::AcqRel | Order::SeqCst => "rl_dmb_full()",
                }
                .into(),
            ));
        }
        Op::Fp(op, args) => {
            let from = args.first().map(|v| wv(*v)).unwrap_or(w);
            let exprs: Vec<String> = args.iter().map(|v| val(*v)).collect();
            match fe.fp(*op, from, w, &exprs, &|r| reg(r)) {
                Some(e) => fit(&e, w),
                None => {
                    let sign = lit(1u128 << (w.bits() - 1), w);
                    let x = exprs.first().cloned().unwrap_or_default();
                    match op {
                        FpOp::Neg => fit(&format!("({u}){x} ^ {sign}"), w),
                        FpOp::Abs => fit(&format!("({u}){x} & ~{sign}"), w),
                        _ => return Err(format!("FP operation {op:?}")),
                    }
                }
            }
        }
        Op::Intrinsic(name, args) => {
            let exprs: Vec<String> = args.iter().map(|v| val(*v)).collect();
            let e = fe
                .intrinsic(name, &exprs)
                .ok_or_else(|| format!("intrinsic {name}"))?;
            if inst.width.is_none() {
                return Ok(Emitted::Stmt(e));
            }
            fit(&e, w)
        }
        Op::Tick(_) | Op::TickIf(..) => return Err("a guest clock tick".into()),
        Op::IoLoad { .. } | Op::IoStore { .. } => return Err("an I/O access".into()),
        Op::Sync => return Err("a sync point".into()),
        Op::Crypto(..) => return Err("a cipher step".into()),
    };
    Ok(Emitted::Value(e))
}

fn bytes(w: Width) -> Result<u32, String> {
    match w.bytes() {
        Some(n @ (1 | 2 | 4 | 8 | 16)) => Ok(n),
        _ => Err(format!("a {}-bit memory access", w.bits())),
    }
}

fn little(e: Endian) -> Result<(), String> {
    match e {
        Endian::Little => Ok(()),
        Endian::Big => Err("a big-endian exclusive access".into()),
    }
}

fn swap(e: &str, w: Width, endian: Endian) -> Result<String, String> {
    Ok(match (endian, w.bits()) {
        (Endian::Little, _) | (Endian::Big, 8) => e.to_string(),
        (Endian::Big, 16) => format!("__builtin_bswap16({e})"),
        (Endian::Big, 32) => format!("__builtin_bswap32({e})"),
        (Endian::Big, 64) => format!("__builtin_bswap64({e})"),
        (Endian::Big, _) => format!("rl_e_bswap128({e})"),
    })
}

fn ord(o: Order) -> &'static str {
    match o {
        Order::Plain => "RL_ORD_PLAIN",
        _ => "RL_ORD_SC",
    }
}

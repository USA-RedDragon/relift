//! C types and expressions for IR values.
//!
//! A value of width `w` lives in the smallest unsigned C type that holds it
//! and always holds its value modulo 2^w. Arithmetic runs in the wide type
//! (`uint64_t` up to 64 bits, `rl_u128` above), never in a promoted `int`.
//! A 128-bit register, and most 128-bit values ([`vectors`]), live in a
//! vector of two 64-bit lanes instead, so they stay in the host's vector
//! registers and their lanes are read and built with vector operations;
//! the context keeps `rl_u128` fields.

use relift_ir::{
    AtomicOp, BinOp, Block, CmpOp, Endian, ExtKind, FpOp, IoMap, Op, Order, Reg, UnOp, Value, Width,
};

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

/// The vector type of 128-bit registers and of the 128-bit values that
/// [`vectors`] gives it, two 64-bit lanes (lane 0 the low half).
pub(crate) const VQ: &str = "rl_e_vq";

/// The C type of register `r` of width `w`: a 128-bit register is a
/// vector ([`VQ`]), the rest as [`cty`].
pub(crate) fn reg_ty(w: Width) -> &'static str {
    if w.bits() == 128 { VQ } else { cty(w) }
}

/// Which 128-bit values of `b` live in the vector type ([`VQ`]) instead of
/// `rl_u128`: register reads, 16-byte loads and 64-bit lane concatenations
/// always do; constants, zero extensions and bitwise operations do unless
/// a use needs the integer form (an add, a shift, a compare and other
/// operations without a vector form, which then convert). With `regs`
/// false the registers keep `rl_u128` ([`crate::Options::regs_in_context`])
/// and so does every value.
pub(crate) fn vectors(b: &Block, regs: bool) -> Vec<bool> {
    if !regs {
        return vec![false; b.insts.len()];
    }
    let wv = |v: Value| b.insts[v.index()].width.unwrap_or(Width::W1).bits();
    let zero = |v: Value| matches!(b.insts[v.index()].op, Op::Const(k) if k.get() == 0);
    let mut vec: Vec<bool> = b
        .insts
        .iter()
        .map(|i| {
            i.width.is_some_and(|w| w.bits() == 128)
                && match &i.op {
                    Op::Get(_) | Op::Const(_) | Op::Select(..) => true,
                    Op::Load { order, endian, .. } => {
                        *order == Order::Plain && *endian == Endian::Little
                    }
                    Op::Concat(h, l) => (wv(*h) == 64 || zero(*h)) && wv(*l) <= 64,
                    Op::Ext(ExtKind::Zero, x) => wv(*x) <= 64,
                    Op::Bin(BinOp::And | BinOp::Or | BinOp::Xor, ..) | Op::Un(UnOp::Not, _) => true,
                    _ => false,
                }
        })
        .collect();
    loop {
        let mut changed = false;
        for i in 0..b.insts.len() {
            let flexible = |vec: &[bool], v: Value| match b.insts[v.index()].op {
                Op::Get(_) | Op::Load { .. } | Op::Concat(..) => false,
                _ => vec[v.index()],
            };
            let scalar_use: Vec<Value> = match &b.insts[i].op {
                Op::Set(..) | Op::Store { .. } | Op::Extract(..) => Vec::new(),
                Op::Bin(BinOp::And | BinOp::Or | BinOp::Xor, ..)
                | Op::Un(UnOp::Not, _)
                | Op::Select(..)
                    if vec[i] =>
                {
                    Vec::new()
                }
                Op::Select(_, x, y) => vec![*x, *y],
                op => op.operands(),
            };
            for v in scalar_use {
                if flexible(&vec, v) {
                    vec[v.index()] = false;
                    changed = true;
                }
            }
            if vec[i]
                && match &b.insts[i].op {
                    Op::Bin(_, x, y) | Op::Select(_, x, y) => !vec[x.index()] && !vec[y.index()],
                    Op::Un(_, x) => !vec[x.index()],
                    _ => false,
                }
            {
                vec[i] = false;
                changed = true;
            }
        }
        if !changed {
            return vec;
        }
    }
}

/// `e`, a value in the representation `from` (vector or not), in the
/// representation `to`.
pub(crate) fn conv(e: String, from: bool, to: bool) -> String {
    match (from, to) {
        (true, false) => format!("rl_e_v2u({e})"),
        (false, true) => format!("rl_e_u2v({e})"),
        _ => e,
    }
}

/// A 128-bit constant as a vector literal.
fn vlit(v: u128) -> String {
    format!(
        "RL_E_VQ(UINT64_C({:#x}), UINT64_C({:#x}))",
        v as u64,
        (v >> 64) as u64
    )
}

/// The C expressions that stand for values of `b` in place of a local:
/// constants, and reads of registers the block does not write (sets apply
/// when the block exits, so such a read would otherwise see the entry
/// value through its local anyway).
pub(crate) fn inlined(b: &Block, vec: &[bool]) -> Vec<Option<String>> {
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
        .enumerate()
        .map(|(k, i)| match i.op {
            Op::Const(c) if vec[k] => Some(vlit(c.get())),
            Op::Const(c) => Some(typed_lit(c.get(), i.width.unwrap_or(Width::W1))),
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

/// How the values of one block are written: `inl` holds the expressions
/// that stand for some values in place of their locals ([`inlined`]),
/// `vec` which values are vectors ([`vectors`]) and `vregs` whether the
/// 128-bit registers are.
pub(crate) struct Lowering<'a> {
    pub(crate) inl: &'a [Option<String>],
    pub(crate) vec: &'a [bool],
    pub(crate) vregs: bool,
}

/// The C for instruction `i` of `b`, or why it cannot be expressed.
///
/// `zero` holds the registers a guard earlier in the block has shown to be
/// zero (guards leave when their [`Frontend::guard_state`] is not).
pub(crate) fn emit_op(
    fe: &dyn Frontend,
    b: &Block,
    i: usize,
    lw: &Lowering<'_>,
    zero: &[Reg],
    io: &IoMap,
) -> Result<Emitted, String> {
    let Lowering { inl, vec, vregs } = *lw;
    let inst = &b.insts[i];
    let wv = |v: Value| b.insts[v.index()].width.unwrap_or(Width::W1);
    let w = inst.width.unwrap_or(Width::W1);
    let u = wide(w);
    let raw = |v: Value| inl[v.index()].clone().unwrap_or_else(|| val(v));
    let vv = |v: Value| conv(raw(v), vec[v.index()], true);
    let val = |v: Value| conv(raw(v), vec[v.index()], false);
    if vec[i] {
        let e = match &inst.op {
            Op::Const(c) => vlit(c.get()),
            Op::Get(r) => reg(*r),
            Op::Select(c, x, y) => format!("{} ? {} : {}", val(*c), vv(*x), vv(*y)),
            Op::Bin(BinOp::And, x, y) => format!("{} & {}", vv(*x), vv(*y)),
            Op::Bin(BinOp::Or, x, y) => format!("{} | {}", vv(*x), vv(*y)),
            Op::Bin(BinOp::Xor, x, y) => format!("{} ^ {}", vv(*x), vv(*y)),
            Op::Un(UnOp::Not, x) => format!("~{}", vv(*x)),
            Op::Ext(ExtKind::Zero, x) => format!("RL_E_VQ({}, 0)", val(*x)),
            Op::Concat(_, l) if wv(*l).bits() < 64 => format!("RL_E_VQ({}, 0)", val(*l)),
            Op::Concat(h, l) => format!("RL_E_VQ({}, {})", val(*l), val(*h)),
            Op::Load { addr, .. } => format!("rl_e_u2v(rl_ld128(mem, {}))", val(*addr)),
            op => return Err(format!("no vector form of {op:?}")),
        };
        return Ok(Emitted::Value(e));
    }
    if let Op::Extract(x, lo) = inst.op
        && vec[x.index()]
        && let Some(e) = lane(&raw(x), lo, w)
    {
        return Ok(Emitted::Value(e));
    }
    let a = |v: &Value| {
        if cty(wv(*v)) == u {
            val(*v)
        } else {
            format!("({u}){}", val(*v))
        }
    };
    let e = match &inst.op {
        Op::Const(c) => fit(&lit(c.get(), w), w),
        Op::Get(r) => reg(*r),
        Op::Set(r, v) if vregs && wv(*v).bits() == 128 => return Ok(Emitted::Set(*r, vv(*v))),
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
                Op::Const(k) if k.get() < u128::from(n) => Some(k.get()),
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
            let state = |r: Reg| {
                if zero.contains(&r) {
                    "0u".to_string()
                } else {
                    reg(r)
                }
            };
            match fe.fp(*op, from, w, &exprs, &state) {
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
        Op::Intrinsic(call) => {
            let name = call.name;
            let exprs: Vec<String> = call.args.iter().map(|v| val(*v)).collect();
            let e = fe
                .intrinsic(name, &exprs)
                .ok_or_else(|| format!("intrinsic {name}"))?;
            if inst.width.is_none() {
                return Ok(Emitted::Stmt(e));
            }
            fit(&e, w)
        }
        Op::Tick(_) | Op::TickIf(..) | Op::IoLoad { .. } | Op::IoStore { .. } | Op::Sync => {
            return crate::time::emit(fe, b, i, &val, io);
        }
        Op::Crypto(op, x) => fe
            .crypto(*op, &val(*x))
            .ok_or_else(|| format!("cipher step {op:?}"))?,
        Op::AtomicRmw {
            op,
            addr,
            value,
            endian,
            order,
        } => {
            little(*endian)?;
            let n = bytes(w)?;
            let (ad, v, o) = (val(*addr), val(*value), ord(*order));
            let k = atomic_name(*op);
            if n == 16 {
                format!("rl_atomic128(mem, {ad}, {k}, {v}, {o})")
            } else {
                fit(
                    &format!("rl_atomic(mem, {ad}, {n}, {k}, (uint64_t){v}, {o})"),
                    w,
                )
            }
        }
        Op::CompareSwap {
            addr,
            expected,
            value,
            endian,
            order,
        } => {
            little(*endian)?;
            let n = bytes(w)?;
            let (ad, e, v, o) = (val(*addr), val(*expected), val(*value), ord(*order));
            if n == 16 {
                format!("rl_cas128(mem, {ad}, {e}, {v}, {o})")
            } else {
                fit(
                    &format!("rl_cas(mem, {ad}, {n}, (uint64_t){e}, (uint64_t){v}, {o})"),
                    w,
                )
            }
        }
    };
    Ok(Emitted::Value(e))
}

/// Bits `lo..lo + w` of the vector `x` (`w` at most 64) as a lane read of
/// the narrowest lane view that holds them, or `None` when they cross a
/// 64-bit lane.
fn lane(x: &str, lo: u8, w: Width) -> Option<String> {
    let (lo, n) = (u32::from(lo), w.bits());
    for bits in [8, 16, 32, 64] {
        if lo % bits == 0 && n <= bits {
            return Some(fit(&format!("rl_e_lane{bits}({x}, {})", lo / bits), w));
        }
    }
    (lo % 64 + n <= 64).then(|| {
        fit(
            &format!("(rl_e_lane64({x}, {}) >> {})", lo / 64, lo % 64),
            w,
        )
    })
}

fn atomic_name(op: AtomicOp) -> &'static str {
    match op {
        AtomicOp::Add => "RL_AT_ADD",
        AtomicOp::Clear => "RL_AT_CLR",
        AtomicOp::Xor => "RL_AT_EOR",
        AtomicOp::Or => "RL_AT_SET",
        AtomicOp::SMax => "RL_AT_SMAX",
        AtomicOp::SMin => "RL_AT_SMIN",
        AtomicOp::UMax => "RL_AT_UMAX",
        AtomicOp::UMin => "RL_AT_UMIN",
        AtomicOp::Swap => "RL_AT_SWP",
    }
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

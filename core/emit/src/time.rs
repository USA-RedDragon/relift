//! The guest clock, I/O accesses and sync points (docs/abi.md section 13).
//!
//! The clock's budget lives in the C local `budget` of the function; the
//! function stores it to the context before every runtime call and reloads
//! it after (see `RL_STORE` and `RL_RELOAD` in `func.rs`).

use relift_guest::Frontend;
use relift_ir::{Block, Endian, IoKind, IoMap, Op, Value, Width};

use crate::value::Emitted;

/// The C local that holds the clock's budget.
pub(crate) const BUDGET: &str = "budget";

/// Whether `b` uses the clock, an I/O access or a sync point.
pub(crate) fn uses_clock(b: &Block) -> bool {
    b.insts.iter().any(|i| {
        matches!(
            i.op,
            Op::Tick(_) | Op::TickIf(..) | Op::IoLoad { .. } | Op::IoStore { .. } | Op::Sync
        )
    })
}

/// The runtime calls an access of `n` bytes makes, as `(offset, size)` in
/// address order, each of a supported size (1, 2, 4 or 8) that does not
/// cross an 8-byte boundary of the value.
fn pieces(n: u32) -> Vec<(u32, u32)> {
    let mut out = Vec::new();
    let mut off = 0;
    while off < n {
        let left = n - off;
        let room = 8 - off % 8;
        let size = [8, 4, 2, 1]
            .into_iter()
            .find(|s| *s <= left && *s <= room)
            .unwrap_or(1);
        out.push((off, size));
        off += size;
    }
    out
}

fn bytes(w: Width) -> Result<u32, String> {
    match w.bytes() {
        Some(n) if (1..=16).contains(&n) => Ok(n),
        _ => Err(format!("a {}-bit I/O access", w.bits())),
    }
}

fn ty(n: u32) -> &'static str {
    if n <= 8 { "uint64_t" } else { "rl_u128" }
}

fn mask(size: u32) -> String {
    format!("UINT64_C({:#x})", u64::MAX >> (64 - 8 * size.min(8)))
}

/// `e` with its `n` low bytes in the opposite order.
fn bswap(e: &str, n: u32) -> String {
    match n {
        1 => e.to_string(),
        2 => format!("__builtin_bswap16((uint16_t)({e}))"),
        4 => format!("__builtin_bswap32((uint32_t)({e}))"),
        8 => format!("__builtin_bswap64((uint64_t)({e}))"),
        16 => format!("rl_e_bswap128((rl_u128)({e}))"),
        _ if n < 8 => format!("(__builtin_bswap64((uint64_t)({e})) >> {})", 64 - 8 * n),
        _ => format!("(rl_e_bswap128((rl_u128)({e})) >> {})", 128 - 8 * n),
    }
}

/// The C condition that an access of `n` bytes at `a` (a `uint64_t`) goes to
/// the device hook; `None` when it never does.
fn device_cond(
    io: &IoMap,
    bits: u32,
    n: u32,
    kind: IoKind,
    write: bool,
    a: &str,
) -> Option<String> {
    if kind == IoKind::Device {
        return Some("1".into());
    }
    let parts: Vec<String> = io
        .device_intervals(bits, write)
        .iter()
        .map(|(first, last)| {
            let lo = first.saturating_sub(u64::from(n) - 1);
            if lo > 0 {
                format!("({a} <= UINT64_C({last:#x}) && {a} >= UINT64_C({lo:#x}))")
            } else {
                format!("{a} <= UINT64_C({last:#x})")
            }
        })
        .collect();
    (!parts.is_empty()).then(|| parts.join(" || "))
}

/// The C for a tick, I/O access or sync point `b.insts[i]`.
pub(crate) fn emit(
    fe: &dyn Frontend,
    b: &Block,
    i: usize,
    val: &dyn Fn(Value) -> String,
    io: &IoMap,
) -> Result<Emitted, String> {
    let inst = &b.insts[i];
    let what = match inst.op {
        Op::Tick(_) | Op::TickIf(..) => "a guest clock tick",
        Op::Sync => "a sync point",
        _ => "an I/O access",
    };
    let clk = fe.clock().ok_or_else(|| what.to_string())?;
    let bits = b.addr_width.bits().min(64);
    let wv = |v: Value| b.insts[v.index()].width.unwrap_or(Width::W1);
    Ok(match &inst.op {
        Op::Tick(c) => Emitted::Stmt(format!("{BUDGET} -= INT64_C({c})")),
        Op::TickIf(c, k) => {
            Emitted::Stmt(format!("{BUDGET} -= (int64_t){} * INT64_C({k})", val(*c)))
        }
        Op::Sync => Emitted::Stmt("(void)0".into()),
        Op::IoLoad { addr, endian, kind } => {
            let n = bytes(inst.width.unwrap_or(Width::W8))?;
            let t = ty(n);
            let plain = match n {
                1 | 2 | 4 | 8 | 16 => format!("r_ = rl_ld{}(mem, a_);", 8 * n),
                _ => format!("r_ = 0; memcpy(&r_, mem + a_, {n});"),
            };
            let mut s = format!("({{ uint64_t a_ = (uint64_t){}; {t} r_;", val(*addr));
            match device_cond(io, bits, n, *kind, false, "a_") {
                None => s.push_str(&format!(" {plain}")),
                Some(cond) => {
                    s.push_str(&format!(
                        " if ({cond}) {{ {clk}.{BUDGET} = {BUDGET}; r_ = 0;"
                    ));
                    for (off, size) in pieces(n) {
                        let call =
                            format!("rl_io_read(c, mem, a_ + {off}, {size}) & {}", mask(size));
                        s.push_str(&format!(" r_ |= (({t})({call})) << {};", 8 * off));
                    }
                    s.push_str(&format!(
                        " {BUDGET} = {clk}.{BUDGET}; }} else {{ {plain} }}"
                    ));
                }
            }
            let r = if *endian == Endian::Big {
                bswap("r_", n)
            } else {
                "r_".into()
            };
            s.push_str(&format!(" {r}; }})"));
            Emitted::Value(s)
        }
        Op::IoStore {
            addr,
            value,
            endian,
            kind,
        } => {
            let n = bytes(wv(*value))?;
            let t = ty(n);
            let v = format!("({t}){}", val(*value));
            let d = if *endian == Endian::Big {
                bswap(&v, n)
            } else {
                v
            };
            let plain = match n {
                1 | 2 | 4 | 8 | 16 => format!("rl_st{}(mem, a_, d_);", 8 * n),
                _ => format!("memcpy(mem + a_, &d_, {n});"),
            };
            let mut s = format!(
                "({{ uint64_t a_ = (uint64_t){}; {t} d_ = ({t})({d});",
                val(*addr)
            );
            match device_cond(io, bits, n, *kind, true, "a_") {
                None => s.push_str(&format!(" {plain}")),
                Some(cond) => {
                    s.push_str(&format!(" if ({cond}) {{ {clk}.{BUDGET} = {BUDGET};"));
                    for (off, size) in pieces(n) {
                        s.push_str(&format!(
                            " rl_io_write(c, mem, a_ + {off}, {size}, (uint64_t)((d_ >> {}) & {}));",
                            8 * off,
                            mask(size)
                        ));
                    }
                    s.push_str(&format!(
                        " {BUDGET} = {clk}.{BUDGET}; }} else {{ {plain} }}"
                    ));
                }
            }
            s.push_str(" (void)0; })");
            Emitted::Stmt(s)
        }
        _ => return Err(what.to_string()),
    })
}

/// The statements of a preemption point: when another host thread ended
/// the budget of the preemption clock `clk`, call `rl_preempt`. The
/// registers stay in their locals: the scheduler only waits, so storing
/// them would keep values alive (flags above all) that the code drops.
pub(crate) fn preempt(clk: &str) -> String {
    format!("    if (RL_UNLIKELY(rl_time_spent(&{clk})))\n        rl_preempt(c);\n")
}

/// The statements of a sync point: when the budget is spent, store the
/// context, call `rl_sync` with the exit target `next` and reload.
pub(crate) fn sync(next: &str) -> String {
    format!(
        "    if (RL_UNLIKELY({BUDGET} <= 0)) {{\n        RL_STORE();\n        rc = rl_sync(c, mem, {next});\n        if (RL_UNLIKELY(rc))\n            return RL_PENDING;\n        RL_RELOAD();\n    }}\n"
    )
}

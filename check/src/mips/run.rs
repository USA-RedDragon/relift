//! Lifting a case with `relift-cpu-mips` and running the block on the IR
//! interpreter.

use std::collections::BTreeMap;

use relift_cpu_mips::lift::{self, EXC_GUARD, FP_SLOW, IN_DELAY};
use relift_cpu_mips::regs;
use relift_ir::interp::{self, Error, FlatMemory, Intrinsics, State};
use relift_ir::{Block, Flow, Reg};

use super::cases::{Case, DATA_LEN, DATA_PA, PC};

/// The CP0 registers the check compares, by the oracle's names and the
/// model's.
pub const CP0: [(&str, &str); 11] = [
    ("epc", "CP0EPC"),
    ("watchlo", "CP0WatchLo"),
    ("taglo", "CP0TagLo"),
    ("entryhi", "CP0EntryHi"),
    ("pagemask", "CP0PageMask"),
    ("index", "CP0Index"),
    ("cause", "CP0Cause"),
    ("lladdr", "CP0LLAddr"),
    ("errorepc", "CP0ErrorEPC"),
    ("status", "CP0Status"),
    ("compare", "CP0Compare"),
];

/// The machine state after a unit, from either side.
pub struct Out {
    pub gpr: [u64; 32],
    pub hi: u64,
    pub lo: u64,
    pub fpr: [u64; 32],
    pub fcr31: u32,
    pub cop0: BTreeMap<String, u64>,
    pub data: Vec<u8>,
    pub pc: u64,
    /// The Cause.ExcCode of the exception the unit raised.
    pub exc: Option<u32>,
    /// The exception came from a delay slot.
    pub delay: bool,
    /// The block left for the FPU slow path.
    pub slow: bool,
}

/// A lifted case.
pub struct Lifted {
    pub block: Block,
    pub encs: Vec<&'static str>,
}

/// Lifts the words of `c` at [`PC`].
pub fn lift(c: &Case) -> Result<Lifted, String> {
    let slot = c.words.get(1).copied().unwrap_or(0);
    let l = lift::lift(PC, c.words[0], slot).map_err(|e| e.to_string())?;
    Ok(Lifted {
        encs: l.encs.iter().map(|e| e.name()).collect(),
        block: l.block,
    })
}

/// The runtime's FPU and CP0 intrinsics, as far as the fast path needs them:
/// the FCR31 cause and flag bits of an operation on normal operands, and a
/// compare's condition bit.
struct Fpu;

impl Intrinsics for Fpu {
    fn call(&mut self, name: &'static str, a: &[u128]) -> Result<u128, Error> {
        match name {
            "mips_random" => return Ok(31),
            "mips_tlb" | "mips_cache" => return Ok(0),
            "mips_fcr31" | "mips_fcmp" => {}
            _ => return Err(Error::UnknownIntrinsic(name)),
        }
        let (op, fmt, fcr) = (a[0] as u32, a[1] as u32, a[2] as u32);
        let (x, y) = (a[3] as u64, a[4] as u64);
        let mut cause = 0u32;
        if name == "mips_fcmp" {
            let (lt, eq, un) = if fmt == 0x10 {
                let (p, q) = (f32::from_bits(x as u32), f32::from_bits(y as u32));
                (p < q, p == q, p.is_nan() || q.is_nan())
            } else {
                let (p, q) = (f64::from_bits(x), f64::from_bits(y));
                (p < q, p == q, p.is_nan() || q.is_nan())
            };
            let c = (op & 4 != 0 && lt) || (op & 2 != 0 && eq) || (op & 1 != 0 && un);
            let fcr = (fcr & !(1 << 23) & !(0x3F << 12)) | (u32::from(c) << 23);
            return Ok(u128::from(fcr));
        }
        let single = fmt == 0x10;
        match op {
            0..=4 => {
                macro_rules! flags {
                    ($t:ty, $p:expr, $q:expr) => {{
                        let (p, q): ($t, $t) = ($p, $q);
                        let r = match op {
                            0 => p + q,
                            1 => p - q,
                            2 => p * q,
                            3 => p / q,
                            _ => p.sqrt(),
                        };
                        let inexact = match op {
                            0 | 1 => {
                                let q = if op == 1 { -q } else { q };
                                let bb = r - p;
                                ((p - (r - bb)) + (q - bb)) != 0.0
                            }
                            2 => p.mul_add(q, -r) != 0.0,
                            3 => r.mul_add(q, -p) != 0.0,
                            _ => r.mul_add(r, -p) != 0.0,
                        };
                        if op == 3 && q == 0.0 {
                            8
                        } else if r.is_infinite() {
                            1 | 4
                        } else {
                            u32::from(inexact)
                        }
                    }};
                }
                cause = if single {
                    flags!(f32, f32::from_bits(x as u32), f32::from_bits(y as u32))
                } else {
                    flags!(f64, f64::from_bits(x), f64::from_bits(y))
                };
            }
            32 | 33 if fmt == 0x14 || fmt == 0x15 => {
                let i = if fmt == 0x14 {
                    i64::from(x as u32 as i32)
                } else {
                    x as i64
                };
                let back = if op == 32 {
                    (i as f32) as i64
                } else {
                    (i as f64) as i64
                };
                if back != i {
                    cause |= 1;
                }
            }
            32 => {
                let p = f64::from_bits(x);
                if (p as f32) as f64 != p {
                    cause |= 1;
                }
            }
            8..=15 | 36 | 37 => {
                let p = if single {
                    f64::from(f32::from_bits(x as u32))
                } else {
                    f64::from_bits(x)
                };
                let r = match op & 3 {
                    0 => p.round_ties_even(),
                    1 => p.trunc(),
                    2 => p.ceil(),
                    _ => p.floor(),
                };
                let r = if op >= 36 { p.round_ties_even() } else { r };
                let wide = op <= 11 || op == 37;
                let lim = if wide { 2f64.powi(53) } else { 2f64.powi(31) };
                if r >= lim || r < -lim {
                    cause |= 16;
                } else if r != p {
                    cause |= 1;
                }
            }
            _ => {}
        }
        let fcr = (fcr & !(0x3F << 12)) | (cause << 12) | (cause << 2) | (fcr & 0x7C);
        Ok(u128::from(fcr))
    }
}

fn slot_of(name: &str) -> Reg {
    regs::slot(name).unwrap_or_else(|| panic!("no register {name}"))
}

/// Runs the lifted block on the state of `c`.
pub fn run_ir(c: &Case, block: &Block) -> Result<Out, String> {
    let mut s = State::new(regs::regs().len());
    let set = |s: &mut State, r: Reg, v: u64| s.regs[r.index()] = u128::from(v);
    for i in 0..32u8 {
        set(&mut s, regs::gpr(i), c.gpr[i as usize]);
        set(&mut s, regs::fpr(i), c.fpr[i as usize]);
    }
    set(&mut s, regs::HI, c.hi);
    set(&mut s, regs::LO, c.lo);
    set(&mut s, regs::FCR31, u64::from(c.fcr31));
    set(&mut s, regs::CP0_STATUS, u64::from(c.status));
    for (n, v) in &c.cop0 {
        if let Some((_, slot)) = CP0.iter().find(|(o, _)| o == n) {
            set(&mut s, slot_of(slot), *v);
        }
    }
    set(&mut s, regs::CP0_CONFIG, 0x0000_0000_7000_8000);
    let mut mem = FlatMemory {
        base: 0,
        bytes: vec![0; 0x80_0000],
    };
    mem.bytes[DATA_PA as usize..DATA_PA as usize + DATA_LEN].copy_from_slice(&c.data);
    let o = interp::run(block, &mut s, &mut mem, &mut Fpu).map_err(|e| e.to_string())?;
    let get = |r: Reg| s.regs[r.index()] as u64;
    let mut out = Out {
        gpr: [0; 32],
        hi: get(regs::HI),
        lo: get(regs::LO),
        fpr: [0; 32],
        fcr31: get(regs::FCR31) as u32,
        cop0: BTreeMap::new(),
        data: mem.bytes[DATA_PA as usize..DATA_PA as usize + DATA_LEN].to_vec(),
        pc: o.next,
        exc: None,
        delay: false,
        slow: false,
    };
    for i in 0..32u8 {
        out.gpr[i as usize] = get(regs::gpr(i));
        out.fpr[i as usize] = get(regs::fpr(i));
    }
    for (n, slot) in CP0 {
        out.cop0.insert(n.into(), get(slot_of(slot)));
    }
    if let Flow::Trap(code) = o.flow {
        out.delay = code & IN_DELAY != 0;
        let code = code & !IN_DELAY;
        if code == FP_SLOW {
            out.slow = true;
        } else {
            out.exc = Some(code - EXC_GUARD);
        }
    }
    Ok(out)
}

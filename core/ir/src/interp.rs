//! The reference interpreter: the definition of every operation's meaning.

use std::fmt;

use crate::{
    AddrClass, BinOp, Block, CmpOp, Endian, ExtKind, Flow, FpOp, FpRound, Inst, IoKind, IoMap,
    Next, Op, UnOp, Width,
};

/// A failed guest memory access.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fault {
    /// The first address of the access.
    pub addr: u64,
    /// The size in bytes.
    pub size: u32,
    /// Whether it was a write.
    pub write: bool,
}

/// Guest memory as the interpreter sees it: byte-addressed reads and writes
/// that may fault.
pub trait GuestMemory {
    /// Fills `buf` from `addr` upward.
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault>;
    /// Writes `data` from `addr` upward.
    fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), Fault>;
}

/// Named intrinsics the frontend and runtime define.
pub trait Intrinsics {
    /// Evaluates `name` on `args`, giving the result (0 when it has none).
    fn call(&mut self, name: &'static str, args: &[u128]) -> Result<u128, Error>;
}

/// The devices of a hardware-register guest as the interpreter sees them:
/// the guest clock, I/O accesses and sync points.
///
/// Guests without devices use [`NoDevices`], and [`run`] runs with it; the
/// operations of this interface then never occur or behave as plain memory.
pub trait Devices {
    /// Advances the guest clock by `cycles`, for [`Op::Tick`] and a taken
    /// [`Op::TickIf`].
    fn tick(&mut self, cycles: u64);

    /// The guest's address-range classifier. An [`Op::IoLoad`] or
    /// [`Op::IoStore`] of kind [`IoKind::Any`] whose bytes are all memory
    /// in it reads or writes `mem` directly; any other goes to
    /// [`Devices::io_read`] or [`Devices::io_write`].
    fn map(&self) -> &IoMap;

    /// A device read of `buf.len()` bytes at `addr`, in memory order (byte
    /// 0 at `addr`). `mem` is the guest memory, for devices that are partly
    /// memory.
    fn io_read(
        &mut self,
        mem: &mut dyn GuestMemory,
        addr: u64,
        buf: &mut [u8],
    ) -> Result<(), Fault>;

    /// A device write of `data` at `addr`, in memory order.
    fn io_write(&mut self, mem: &mut dyn GuestMemory, addr: u64, data: &[u8]) -> Result<(), Fault>;

    /// A sync point ([`Op::Sync`]) at an exit to `next`, after the block's
    /// register writes are committed: runs the devices up to the current
    /// cycle and decides whether an interrupt is taken. A taken one gives
    /// the handler entry, after the hook has made the entry's effects on
    /// `state`, `mem` and the clock (with `next` as the return address);
    /// `None` continues at `next`.
    fn sync(
        &mut self,
        state: &mut State,
        mem: &mut dyn GuestMemory,
        next: u64,
    ) -> Result<Option<u64>, Error>;
}

static MEMORY_ONLY: IoMap = IoMap::memory();

/// [`Devices`] of a guest without any: the clock is ignored, the map is all
/// memory, a device access (only an [`IoKind::Device`] one reaches it)
/// reads or writes guest memory, and no interrupt is ever taken.
#[derive(Clone, Copy, Debug, Default)]
pub struct NoDevices;

impl Devices for NoDevices {
    fn tick(&mut self, _cycles: u64) {}

    fn map(&self) -> &IoMap {
        &MEMORY_ONLY
    }

    fn io_read(
        &mut self,
        mem: &mut dyn GuestMemory,
        addr: u64,
        buf: &mut [u8],
    ) -> Result<(), Fault> {
        mem.read(addr, buf)
    }

    fn io_write(&mut self, mem: &mut dyn GuestMemory, addr: u64, data: &[u8]) -> Result<(), Fault> {
        mem.write(addr, data)
    }

    fn sync(
        &mut self,
        _state: &mut State,
        _mem: &mut dyn GuestMemory,
        _next: u64,
    ) -> Result<Option<u64>, Error> {
        Ok(None)
    }
}

/// An [`Intrinsics`] that knows no intrinsic.
#[derive(Clone, Copy, Debug, Default)]
pub struct NoIntrinsics;

impl Intrinsics for NoIntrinsics {
    fn call(&mut self, name: &'static str, _args: &[u128]) -> Result<u128, Error> {
        Err(Error::UnknownIntrinsic(name))
    }
}

/// The address and size the exclusive monitor is armed for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Monitor {
    /// The address.
    pub addr: u64,
    /// The size in bytes.
    pub size: u32,
}

/// Guest state: one value per register-file slot, and the exclusive monitor.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct State {
    /// Slot values, indexed like the register file; each fits its slot width.
    pub regs: Vec<u128>,
    /// The exclusive monitor, `None` when clear.
    pub monitor: Option<Monitor>,
}

impl State {
    /// All slots zero and the monitor clear.
    pub fn new(slots: usize) -> State {
        State {
            regs: vec![0; slots],
            monitor: None,
        }
    }
}

/// Why a block could not run to its exit.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// A memory access faulted. Register writes are discarded; stores
    /// before the fault have happened.
    Fault(Fault),
    /// An intrinsic the environment does not know.
    UnknownIntrinsic(&'static str),
    /// The block does not verify.
    Invalid(String),
    /// The state has the wrong number of slots.
    StateSize {
        /// Slots in the register file.
        expected: usize,
        /// Slots in the state.
        found: usize,
    },
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Fault(x) => write!(
                f,
                "{} of {} bytes at {:#x} faulted",
                if x.write { "write" } else { "read" },
                x.size,
                x.addr
            ),
            Error::UnknownIntrinsic(n) => write!(f, "unknown intrinsic {n}"),
            Error::Invalid(m) => write!(f, "invalid block: {m}"),
            Error::StateSize { expected, found } => {
                write!(f, "state has {found} slots, the register file {expected}")
            }
        }
    }
}

impl std::error::Error for Error {}

/// Where a block went.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Outcome {
    /// The next program counter.
    pub next: u64,
    /// The kind of transfer.
    pub flow: Flow,
    /// `Some(pc)` when the block's sync point took an interrupt: `next` is
    /// then the handler entry and `pc` the exit's target, where the handler
    /// returns to; `flow` stays the exit's.
    pub resume: Option<u64>,
}

fn sext(v: u128, w: Width) -> i128 {
    let s = 128 - w.bits();
    ((v << s) as i128) >> s
}

/// Evaluates a one-operand operation on a value of width `w`.
pub fn eval_un(op: UnOp, a: u128, w: Width) -> u128 {
    let m = w.mask();
    let bits = w.bits();
    let r = match op {
        UnOp::Not => !a,
        UnOp::Neg => a.wrapping_neg(),
        UnOp::Clz => u128::from(a.leading_zeros() - (128 - bits)),
        UnOp::Ctz => u128::from(a.trailing_zeros().min(bits)),
        UnOp::Popcount => u128::from(a.count_ones()),
        UnOp::BitReverse => a.reverse_bits() >> (128 - bits),
        UnOp::ByteSwap => a.swap_bytes() >> (128 - bits),
    };
    r & m
}

/// Evaluates a two-operand operation; `a` has width `w`, `b` is read
/// unsigned for shifts and has width `w` otherwise.
pub fn eval_bin(op: BinOp, a: u128, b: u128, w: Width) -> u128 {
    let m = w.mask();
    let bits = u128::from(w.bits());
    let r = match op {
        BinOp::Add => a.wrapping_add(b),
        BinOp::Sub => a.wrapping_sub(b),
        BinOp::Mul => a.wrapping_mul(b),
        BinOp::UDiv => a.checked_div(b).unwrap_or(0),
        BinOp::URem => a.checked_rem(b).unwrap_or(a),
        BinOp::SDiv => {
            let (x, y) = (sext(a, w), sext(b, w));
            if y == 0 { 0 } else { x.wrapping_div(y) as u128 }
        }
        BinOp::SRem => {
            let (x, y) = (sext(a, w), sext(b, w));
            if y == 0 {
                a
            } else if y == -1 {
                0
            } else {
                (x % y) as u128
            }
        }
        BinOp::And => a & b,
        BinOp::Or => a | b,
        BinOp::Xor => a ^ b,
        BinOp::Shl => {
            if b >= bits {
                0
            } else {
                a << b
            }
        }
        BinOp::LShr => {
            if b >= bits {
                0
            } else {
                a >> b
            }
        }
        BinOp::AShr => {
            let x = sext(a, w);
            (x >> b.min(bits - 1)) as u128
        }
        BinOp::RotR => {
            let k = (b % bits) as u32;
            if k == 0 {
                a
            } else {
                (a >> k) | (a << (w.bits() - k))
            }
        }
    };
    r & m
}

/// Evaluates a comparison of two values of width `w`.
pub fn eval_cmp(op: CmpOp, a: u128, b: u128, w: Width) -> bool {
    match op {
        CmpOp::Eq => a == b,
        CmpOp::Ne => a != b,
        CmpOp::Ult => a < b,
        CmpOp::Ule => a <= b,
        CmpOp::Slt => sext(a, w) < sext(b, w),
        CmpOp::Sle => sext(a, w) <= sext(b, w),
    }
}

/// Evaluates a floating-point operation on `arg_w`-bit operands (32 or 64)
/// giving a `w`-bit result; the two differ only for conversions.
pub fn eval_fp(op: FpOp, args: &[u128], arg_w: Width, w: Width) -> u128 {
    let f = |i: usize| -> f64 {
        if arg_w == Width::W32 {
            f64::from(f32::from_bits(args[i] as u32))
        } else {
            f64::from_bits(args[i] as u64)
        }
    };
    let round = |x: f64, r: FpRound| match r {
        FpRound::TiesEven => x.round_ties_even(),
        FpRound::TiesAway => x.round(),
        FpRound::Floor => x.floor(),
        FpRound::Ceil => x.ceil(),
        FpRound::Zero => x.trunc(),
    };
    let to_bits = |x: f64| -> u128 {
        if w == Width::W32 {
            u128::from((x as f32).to_bits())
        } else {
            u128::from(x.to_bits())
        }
    };
    match op {
        FpOp::ToSInt(r) => {
            let x = round(f(0), r);
            return if w == Width::W32 {
                u128::from(x as i32 as u32)
            } else {
                u128::from(x as i64 as u64)
            };
        }
        FpOp::ToUInt(r) => {
            let x = round(f(0), r);
            return if w == Width::W32 {
                u128::from(x as u32)
            } else {
                u128::from(x as u64)
            };
        }
        FpOp::FromSInt => {
            let i = if arg_w == Width::W32 {
                i64::from(args[0] as u32 as i32)
            } else {
                args[0] as u64 as i64
            };
            return if w == Width::W32 {
                u128::from((i as f32).to_bits())
            } else {
                u128::from((i as f64).to_bits())
            };
        }
        FpOp::FromUInt => {
            let i = if arg_w == Width::W32 {
                u64::from(args[0] as u32)
            } else {
                args[0] as u64
            };
            return if w == Width::W32 {
                u128::from((i as f32).to_bits())
            } else {
                u128::from((i as f64).to_bits())
            };
        }
        FpOp::Convert => return to_bits(f(0)),
        _ => {}
    }
    if w == Width::W32 {
        let f = |i: usize| f32::from_bits(args[i] as u32);
        let r = match op {
            FpOp::Add => f(0) + f(1),
            FpOp::Sub => f(0) - f(1),
            FpOp::Mul => f(0) * f(1),
            FpOp::Div => f(0) / f(1),
            FpOp::Sqrt => f(0).sqrt(),
            FpOp::MulAdd => f(0).mul_add(f(1), f(2)),
            FpOp::Neg => return args[0] ^ (1 << 31),
            FpOp::Abs => return args[0] & !(1 << 31),
            FpOp::RoundInt(r) => {
                let x = f(0);
                match r {
                    FpRound::TiesEven => x.round_ties_even(),
                    FpRound::TiesAway => x.round(),
                    FpRound::Floor => x.floor(),
                    FpRound::Ceil => x.ceil(),
                    FpRound::Zero => x.trunc(),
                }
            }
            _ => unreachable!("conversions return above"),
        };
        u128::from(r.to_bits())
    } else {
        let f = |i: usize| f64::from_bits(args[i] as u64);
        let r = match op {
            FpOp::Add => f(0) + f(1),
            FpOp::Sub => f(0) - f(1),
            FpOp::Mul => f(0) * f(1),
            FpOp::Div => f(0) / f(1),
            FpOp::Sqrt => f(0).sqrt(),
            FpOp::MulAdd => f(0).mul_add(f(1), f(2)),
            FpOp::Neg => return args[0] ^ (1 << 63),
            FpOp::Abs => return args[0] & !(1 << 63),
            FpOp::RoundInt(r) => round(f(0), r),
            _ => unreachable!("conversions return above"),
        };
        u128::from(r.to_bits())
    }
}

fn to_bytes(v: u128, n: usize, endian: Endian) -> Vec<u8> {
    let le = v.to_le_bytes();
    let mut out = le[..n].to_vec();
    if endian == Endian::Big {
        out.reverse();
    }
    out
}

fn from_bytes(buf: &[u8], endian: Endian) -> u128 {
    let mut le = [0u8; 16];
    le[..buf.len()].copy_from_slice(buf);
    if endian == Endian::Big {
        le[..buf.len()].reverse();
    }
    u128::from_le_bytes(le)
}

fn routes_to_device(
    map: &IoMap,
    kind: IoKind,
    addr: u64,
    size: u32,
    write: bool,
    addr_width: Width,
) -> bool {
    kind == IoKind::Device || map.classify(addr, size, write, addr_width) == AddrClass::Device
}

/// Runs `block` on `state` and `mem`, with no devices ([`NoDevices`]).
///
/// `Get` reads the state at entry; `Set`s are committed together when the
/// block reaches its exit, so a fault leaves registers unchanged. Memory
/// operations and the monitor take effect in order.
pub fn run(
    block: &Block,
    state: &mut State,
    mem: &mut dyn GuestMemory,
    intrinsics: &mut dyn Intrinsics,
) -> Result<Outcome, Error> {
    run_with(block, state, mem, intrinsics, &mut NoDevices)
}

/// Runs `block` like [`run`], with `devices` for the clock, I/O and sync
/// points.
///
/// Clock ticks and I/O accesses take effect in order with memory
/// operations, so a fault leaves the ticks and accesses before it applied.
/// A sync point runs [`Devices::sync`] after the exit's register writes
/// are committed; when it takes an interrupt, the outcome's `next` is the
/// handler entry and `resume` the exit's target.
pub fn run_with(
    block: &Block,
    state: &mut State,
    mem: &mut dyn GuestMemory,
    intrinsics: &mut dyn Intrinsics,
    devices: &mut dyn Devices,
) -> Result<Outcome, Error> {
    block.verify().map_err(|e| Error::Invalid(e.to_string()))?;
    if state.regs.len() != block.regs.len() {
        return Err(Error::StateSize {
            expected: block.regs.len(),
            found: state.regs.len(),
        });
    }
    let mut vals: Vec<u128> = Vec::with_capacity(block.insts.len());
    let mut writes: Vec<(usize, u128)> = Vec::new();
    let addr_mask = block.addr_width.mask();
    for inst in &block.insts {
        let v = |x: crate::Value| vals[x.index()];
        let width = inst.width.unwrap_or(Width::W1);
        let r: u128 = match &inst.op {
            Op::Const(c) => *c,
            Op::Get(reg) => state.regs[reg.index()] & width.mask(),
            Op::Set(reg, x) => {
                writes.push((reg.index(), v(*x)));
                0
            }
            Op::Un(op, a) => eval_un(*op, v(*a), width),
            Op::Bin(op, a, b) => eval_bin(*op, v(*a), v(*b), width),
            Op::Cmp(op, a, b) => {
                let w = block.width_of(*a).unwrap_or(Width::W1);
                u128::from(eval_cmp(*op, v(*a), v(*b), w))
            }
            Op::Select(c, a, b) => {
                if v(*c) != 0 {
                    v(*a)
                } else {
                    v(*b)
                }
            }
            Op::Ext(kind, a) => {
                let aw = block.width_of(*a).unwrap_or(Width::W1);
                match kind {
                    ExtKind::Zero => v(*a),
                    ExtKind::Sign => (sext(v(*a), aw) as u128) & width.mask(),
                }
            }
            Op::Extract(a, lo) => (v(*a) >> lo) & width.mask(),
            Op::Concat(h, l) => {
                let lw = block.width_of(*l).unwrap_or(Width::W1).bits();
                ((v(*h) << lw) | v(*l)) & width.mask()
            }
            Op::Load {
                addr,
                endian,
                order: _,
            }
            | Op::LoadExclusive {
                addr,
                endian,
                order: _,
            } => {
                let a = (v(*addr) & addr_mask) as u64;
                let n = width.bits() / 8;
                let mut buf = vec![0u8; n as usize];
                mem.read(a, &mut buf).map_err(Error::Fault)?;
                if matches!(inst.op, Op::LoadExclusive { .. }) {
                    state.monitor = Some(Monitor { addr: a, size: n });
                }
                from_bytes(&buf, *endian)
            }
            Op::Store {
                addr,
                value,
                endian,
                order: _,
            } => {
                let a = (v(*addr) & addr_mask) as u64;
                let n = block.width_of(*value).unwrap_or(Width::W8).bits() / 8;
                mem.write(a, &to_bytes(v(*value), n as usize, *endian))
                    .map_err(Error::Fault)?;
                0
            }
            Op::StoreExclusive {
                addr,
                value,
                endian,
                order: _,
            } => {
                let a = (v(*addr) & addr_mask) as u64;
                let n = block.width_of(*value).unwrap_or(Width::W8).bits() / 8;
                let armed = state.monitor == Some(Monitor { addr: a, size: n });
                state.monitor = None;
                if armed {
                    mem.write(a, &to_bytes(v(*value), n as usize, *endian))
                        .map_err(Error::Fault)?;
                }
                u128::from(armed)
            }
            Op::ClearExclusive => {
                state.monitor = None;
                0
            }
            Op::Fence(_) => 0,
            Op::Guard(c, code) => {
                if v(*c) != 0 {
                    return Ok(Outcome {
                        next: block.pc,
                        flow: Flow::Trap(*code),
                        resume: None,
                    });
                }
                0
            }
            Op::Fp(op, args) => {
                let a: Vec<u128> = args.iter().map(|x| v(*x)).collect();
                let aw = block.insts[args[0].index()].width.unwrap_or(width);
                eval_fp(*op, &a, aw, width)
            }
            Op::Intrinsic(name, args) => {
                let a: Vec<u128> = args.iter().map(|x| v(*x)).collect();
                intrinsics.call(name, &a)? & width.mask()
            }
            Op::Tick(n) => {
                devices.tick(u64::from(*n));
                0
            }
            Op::TickIf(c, n) => {
                if v(*c) != 0 {
                    devices.tick(u64::from(*n));
                }
                0
            }
            Op::IoLoad { addr, endian, kind } => {
                let a = (v(*addr) & addr_mask) as u64;
                let n = width.bits() / 8;
                let mut buf = vec![0u8; n as usize];
                if routes_to_device(devices.map(), *kind, a, n, false, block.addr_width) {
                    devices.io_read(mem, a, &mut buf)
                } else {
                    mem.read(a, &mut buf)
                }
                .map_err(Error::Fault)?;
                from_bytes(&buf, *endian)
            }
            Op::IoStore {
                addr,
                value,
                endian,
                kind,
            } => {
                let a = (v(*addr) & addr_mask) as u64;
                let n = block.width_of(*value).unwrap_or(Width::W8).bits() / 8;
                let data = to_bytes(v(*value), n as usize, *endian);
                if routes_to_device(devices.map(), *kind, a, n, true, block.addr_width) {
                    devices.io_write(mem, a, &data)
                } else {
                    mem.write(a, &data)
                }
                .map_err(Error::Fault)?;
                0
            }
            Op::Sync => 0,
        };
        vals.push(r);
    }
    let next = match block.exit.next {
        Next::Const(t) => t,
        Next::Select {
            cond,
            taken,
            not_taken,
        } => {
            if vals[cond.index()] != 0 {
                taken
            } else {
                not_taken
            }
        }
        Next::Dynamic(x) => vals[x.index()] as u64,
    };
    for (slot, value) in writes {
        state.regs[slot] = value;
    }
    let next = next & (addr_mask as u64);
    let sync = matches!(block.insts.last(), Some(Inst { op: Op::Sync, .. }));
    if sync && let Some(entry) = devices.sync(state, mem, next)? {
        return Ok(Outcome {
            next: entry & (addr_mask as u64),
            flow: block.exit.flow,
            resume: Some(next),
        });
    }
    Ok(Outcome {
        next,
        flow: block.exit.flow,
        resume: None,
    })
}

/// A flat byte array at a base address, for tests and simple guests.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FlatMemory {
    /// The guest address of `bytes[0]`.
    pub base: u64,
    /// The contents.
    pub bytes: Vec<u8>,
}

impl FlatMemory {
    fn range(&self, addr: u64, n: usize, write: bool) -> Result<std::ops::Range<usize>, Fault> {
        let fault = Fault {
            addr,
            size: n as u32,
            write,
        };
        let off = addr.checked_sub(self.base).ok_or(fault)?;
        let off = usize::try_from(off).map_err(|_| fault)?;
        let end = off.checked_add(n).ok_or(fault)?;
        if end > self.bytes.len() {
            return Err(fault);
        }
        Ok(off..end)
    }
}

impl GuestMemory for FlatMemory {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault> {
        let r = self.range(addr, buf.len(), false)?;
        buf.copy_from_slice(&self.bytes[r]);
        Ok(())
    }

    fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), Fault> {
        let r = self.range(addr, data.len(), true)?;
        self.bytes[r].copy_from_slice(data);
        Ok(())
    }
}

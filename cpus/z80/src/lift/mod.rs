//! Lifting Z80 instructions to `relift-ir` from the templates generated
//! from the Sail model (`cpus/z80/sail/z80.sail`).
//!
//! One instruction is one block, with two kinds of synthetic address:
//!
//! * A conditional `CALL` or `RET` is two blocks: the part up to the
//!   decision, and a continuation for the taken case at the instruction's
//!   address plus [`HALF`], because the IR has no conditional store or pop.
//! * An interrupt entry (NMI, IM 0 with 0xFF on the bus, IM 1, IM 2) is the
//!   block at [`ENTRY`] plus [`Entry`]; it reads the address it returns to
//!   from [`regs::PC`](crate::regs::PC), which the runtime stores first.
//!
//! The data accesses are `IoLoad` and `IoStore` operations of kind `Any`,
//! so the guest's [`IoMap`] decides (at lift time for a constant address)
//! whether one reaches a device; port accesses are the intrinsics
//! [`PORT_IN`] and [`PORT_OUT`]. The clock is charged in T-states up to the
//! start of each access's machine cycle before the access, and the rest at
//! the end. `EI`'s delay is the model's `EIDelay` register: the runtime
//! takes no maskable interrupt while it is set.
//!
//! Control transfers end in a sync point, and so do `HALT` and the
//! instructions that set IFF1; other instructions follow the
//! [`SyncPolicy`].

mod generated;
mod rt;

use core::fmt;

use relift_ir::{Block, Builder, Exit, IoMap, Op, Width, classify_io};

use crate::decode::{DecodeError, Group, decode};
use crate::regs;
use rt::St;

/// Added to the address of a conditional `CALL` or `RET` to name the block of
/// its taken case. The low 16 bits are the address of the instruction.
pub const HALF: u64 = 0x1_0000;

/// The address of the interrupt entry blocks: [`ENTRY`] plus the
/// [`Entry`].
pub const ENTRY: u64 = 0x2_0000;

/// The intrinsic of an I/O port read: `(port) -> byte`, the port a 16-bit
/// value (the Z80 drives the whole address bus).
pub const PORT_IN: &str = "z80_in";

/// The intrinsic of an I/O port write: `(port, byte)`.
pub const PORT_OUT: &str = "z80_out";

/// The intrinsic `HALT` calls once its T-states are charged: the per-CPU
/// layer waits until an interrupt is taken (the CPU runs NOPs, which count
/// in R), and the block goes on to the instruction after `HALT`.
pub const HALT: &str = "z80_halt";

/// An interrupt entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Entry {
    /// The non-maskable interrupt: a call to 0x0066.
    Nmi = 0,
    /// Mode 0 with 0xFF on the data bus: `RST 38H`.
    Im0 = 1,
    /// Mode 1: a call to 0x0038.
    Im1 = 2,
    /// Mode 2 with 0xFF on the data bus: a call through the word at
    /// `I:0xFF`.
    Im2 = 3,
}

impl Entry {
    /// The entry of a maskable interrupt in mode `im` (0 to 2).
    pub fn maskable(im: u8) -> Option<Entry> {
        Some(match im {
            0 => Entry::Im0,
            1 => Entry::Im1,
            2 => Entry::Im2,
            _ => return None,
        })
    }

    /// The address of its block.
    pub fn addr(self) -> u64 {
        ENTRY + self as u64
    }
}

/// Where a block's sync point goes, besides control transfers, `HALT` and
/// the instructions that set IFF1.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SyncPolicy {
    /// Nowhere else: interrupts are taken at the end of each basic block.
    Blocks,
    /// Also after each instruction that stores to something that may be a
    /// device, or writes a port.
    #[default]
    IoStores,
    /// After every instruction: exact interrupt timing.
    Every,
}

/// What lifting needs to know besides the instruction.
#[derive(Clone, Debug)]
pub struct Options {
    /// Which addresses are memory and which reach a device.
    pub map: IoMap,
    /// Where sync points go.
    pub sync: SyncPolicy,
}

impl Options {
    /// The default policy over `map`.
    pub fn new(map: IoMap) -> Options {
        Options {
            map,
            sync: SyncPolicy::default(),
        }
    }
}

/// Why an instruction was not lifted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LiftError {
    /// The bytes end inside an instruction.
    Decode(DecodeError),
    /// A combination the templates cannot express.
    Unsupported(&'static str),
    /// A synthetic address that names no block.
    Address(u64),
}

impl fmt::Display for LiftError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LiftError::Decode(e) => e.fmt(f),
            LiftError::Unsupported(what) => write!(f, "{what} is not lifted"),
            LiftError::Address(a) => write!(f, "{a:#x} names no block"),
        }
    }
}

impl core::error::Error for LiftError {}

impl From<DecodeError> for LiftError {
    fn from(e: DecodeError) -> LiftError {
        LiftError::Decode(e)
    }
}

/// A generated template: emits the instruction's operations.
type GenFn = fn(u32, &mut Builder, &mut St) -> Result<(), rt::LiftError>;

/// One lifted block with what the template recorded.
pub struct Lifted {
    /// The block.
    pub block: Block,
    /// The instruction length in bytes (0 for an entry).
    pub len: u16,
    /// The instruction bytes fetched: offset from the instruction's address
    /// and the T-state of their machine cycle.
    pub fetches: Vec<(u16, u32)>,
}

/// Runs the template of `code` in `group` for the instruction `bytes` at
/// `pc`; `taken` lifts the taken continuation of a conditional `CALL` or
/// `RET`.
fn run(group: Group, code: u8, bytes: [u8; 4], pc: u16, taken: bool) -> Result<Lifted, LiftError> {
    let template = generated::template(group as u8, code)
        .ok_or(LiftError::Unsupported("an opcode without a template"))?;
    let mut b = Builder::new(regs::regs(), Width::W16, u64::from(pc));
    let elapsed = if taken {
        let mut probe = Builder::new(regs::regs(), Width::W16, u64::from(pc));
        let mut st = St::new(bytes, pc, false, 0);
        let Ok(()) = template(u32::from(code), &mut probe, &mut st);
        st.finish(&mut probe)
            .map_err(LiftError::Unsupported)?
            .elapsed
    } else {
        0
    };
    let mut st = St::new(bytes, pc, taken, elapsed);
    let Ok(()) = template(u32::from(code), &mut b, &mut st);
    let fetches = st.fetches.clone();
    let fin = st.finish(&mut b).map_err(LiftError::Unsupported)?;
    let len = if taken || group == Group::Accept {
        0
    } else {
        fin.len
    };
    let mut block = b.finish(
        u32::from(len),
        Exit {
            next: fin.next,
            flow: fin.flow,
        },
    );
    if taken {
        block.pc = HALF | u64::from(pc);
    }
    Ok(Lifted {
        block,
        len,
        fetches,
    })
}

/// Places the sync point of `block` and classifies its accesses.
fn finish(mut l: Lifted, opts: &Options, half: bool, control: bool) -> Lifted {
    let mut block = relift_ir::dce(&classify_io(&l.block, &opts.map));
    let sets_iff1 = block
        .insts
        .iter()
        .any(|i| matches!(i.op, Op::Set(r, _) if r == regs::IFF1));
    let stores = || {
        block.insts.iter().any(|i| match &i.op {
            Op::IoStore { .. } => true,
            Op::Intrinsic(c) => c.name == PORT_OUT,
            _ => false,
        })
    };
    let sync = !half
        && (control
            || sets_iff1
            || match opts.sync {
                SyncPolicy::Blocks => false,
                SyncPolicy::IoStores => stores(),
                SyncPolicy::Every => true,
            });
    if sync {
        block.insts.push(relift_ir::Inst {
            op: Op::Sync,
            width: None,
        });
    }
    l.block = block;
    l
}

/// Lifts the block at `pc` as the frontend does: a plain address is the
/// instruction there; the instruction's address plus [`HALF`] is the taken
/// continuation of a conditional `CALL` or `RET`; [`ENTRY`] plus an
/// [`Entry`] is an interrupt entry. `bytes` start at the instruction (any
/// for an entry); four are enough for any instruction.
///
/// # Errors
///
/// The bytes end inside an instruction, or the address names no block.
pub fn lift_bytes(bytes: &[u8], pc: u64, opts: &Options) -> Result<Lifted, LiftError> {
    if pc & !0xFFFF == ENTRY {
        let code = u8::try_from(pc & 0xFFFF)
            .ok()
            .filter(|c| *c < 4)
            .ok_or(LiftError::Address(pc))?;
        let l = run(Group::Accept, code, [0; 4], 0, false)?;
        let mut l = finish(l, opts, false, true);
        l.block.pc = pc;
        return Ok(l);
    }
    let taken = match pc & !0xFFFF {
        0 => false,
        HALF => true,
        _ => return Err(LiftError::Address(pc)),
    };
    let real = (pc & 0xFFFF) as u16;
    let (group, code) = decode(bytes)?;
    let mut window = [0u8; 4];
    for (w, b) in window.iter_mut().zip(bytes) {
        *w = *b;
    }
    let plain = run(group, code, window, real, false)?;
    let is_half = matches!(plain.block.exit.next, relift_ir::Next::Select { taken, .. } if taken == HALF | u64::from(real));
    if taken && !is_half {
        return Err(LiftError::Address(pc));
    }
    let l = if taken {
        run(group, code, window, real, true)?
    } else {
        plain
    };
    let after = u64::from(real.wrapping_add(l.len));
    let control = l
        .block
        .insts
        .iter()
        .any(|i| matches!(&i.op, Op::Intrinsic(c) if c.name == HALT))
        || !matches!(l.block.exit.flow, relift_ir::Flow::Jump)
        || !matches!(l.block.exit.next, relift_ir::Next::Const(n) if n == after);
    Ok(finish(l, opts, is_half && !taken, control))
}

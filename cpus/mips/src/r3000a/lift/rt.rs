//! The run-time side of the R3000A templates: `St` holds the lift-time
//! state of one block over the shared core (`crate::common::rt`) and turns
//! the cut calls of the templates into IR.
//!
//! - The model is 64-bit and the register file 32-bit: a read of a general
//!   register, HI or LO is the slot sign-extended to 64 bits, and a write
//!   keeps the low word. MIPS I keeps every register sign-extended, so the
//!   model's 32-bit operations see the values they expect.
//! - Addresses are 32 bits: the PC the templates see is the sign-extended
//!   instruction address, and a branch target keeps its low word.
//! - Memory is little-endian; translation, COP0 and the load delay slot
//!   are not in the stub.

use relift_ir::{Builder, Endian, Reg, Value, Width};

use crate::common::rt::Core;
pub use crate::common::rt::{Access, EXC_GUARD, Exc, IN_DELAY, Target, sext, sint};
use crate::r3000a::regs;

/// The lift-time state of one block.
pub struct St {
    core: Core,
}

impl St {
    /// The state for the instruction at `pc`.
    pub fn new(pc: u64) -> St {
        St {
            core: Core::new(pc, None, Endian::Little),
        }
    }

    pub(crate) fn core(&mut self) -> &mut Core {
        &mut self.core
    }

    /// The exception raised whenever the block runs, if any.
    pub fn exception_static(&self) -> Option<Exc> {
        self.core.exception_static()
    }

    /// The instruction's address, sign-extended to the model's 64 bits.
    pub fn pc(&self) -> u128 {
        sext(self.core.pc(), 32, 64)
    }

    /// Whether the instruction is a delay slot.
    pub fn in_delay(&self) -> bool {
        self.core.in_delay()
    }

    fn named(name: &str) -> Reg {
        regs::slot(name).unwrap_or_else(|| panic!("the register file has no {name}"))
    }

    fn get64(&mut self, b: &mut Builder, r: Reg) -> Value {
        let v = self.core.get(b, r);
        b.sext(v, Width::W64)
    }

    fn set32(&mut self, b: &mut Builder, r: Reg, v: Value, g: Option<Value>) {
        let v = b.trunc(v, Width::W32);
        self.core.set(b, r, v, g);
    }

    /// General register `n`, sign-extended.
    pub fn gpr(&mut self, b: &mut Builder, n: u8) -> Value {
        if n == 0 {
            return b.konst(Width::W64, 0);
        }
        self.get64(b, regs::gpr(n))
    }

    /// Writes the low word of `v` to general register `n` (a write to `r0`
    /// is dropped).
    pub fn set_gpr(&mut self, b: &mut Builder, n: u8, v: Value, g: Option<Value>) {
        if n != 0 {
            self.set32(b, regs::gpr(n), v, g);
        }
    }

    /// The model register `name` (HI or LO), sign-extended.
    pub fn reg(&mut self, b: &mut Builder, name: &str) -> Value {
        self.get64(b, St::named(name))
    }

    /// Writes the low word of `v` to the model register `name`.
    pub fn set_reg(&mut self, b: &mut Builder, name: &str, v: Value, g: Option<Value>) {
        self.set32(b, St::named(name), v, g);
    }

    /// Raises `e` under `g`; `_addr` is the BadVaddr of an address error,
    /// which the runtime recomputes.
    pub fn exception(&mut self, b: &mut Builder, e: Exc, _addr: Option<Value>, g: Option<Value>) {
        self.core.exception(b, e, g);
    }

    /// A branch to `t` under `g`, after the delay slot; the target keeps
    /// its low word.
    pub fn branch(&mut self, b: &mut Builder, t: Target, g: Option<Value>) {
        let t = match t {
            Target::Const(a) => Target::Const(a & 0xFFFF_FFFF),
            Target::Dynamic(v) => Target::Dynamic(b.trunc(v, Width::W32)),
        };
        self.core.branch(b, t, g);
    }

    /// A branch not taken: the slot runs, then the next instruction.
    pub fn delay_slot(&mut self, _b: &mut Builder, _g: Option<Value>) {
        self.core.delay_slot();
    }

    /// Writes the pending registers, in slot order.
    pub fn finish(&mut self, b: &mut Builder) {
        self.core.finish(b);
    }
}

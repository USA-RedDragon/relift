//! The Z80 frontend of the Master System and Game Gear machine.
//!
//! Every access goes to the machine (`rl_sms_read`, `rl_sms_write`,
//! `rl_sms_in`, `rl_sms_out`), and every block ends in a sync point
//! (`rl_sms_sync`) that closes the instruction and runs the boundary of the
//! next one. The sync call carries the kind of transfer, the return address
//! of a call, and the unit the block was lifted from: a ROM bank as one
//! slot shows it, or a group of code in work RAM. When the exit may go on in
//! lifted code of the same unit without dispatch, the info says so, and the
//! runtime checks that the slot still shows the bank (or the RAM still holds
//! the bytes) before it lets the code go on.

use relift_cpu_z80::{HALT, IFF, PORT_IN, PORT_OUT, SyncPolicy, Z80};
use relift_guest::{Frontend, Home, LiftFailure, WasmIntrinsic};
use relift_ir::{
    AddrClass, Block, CryptoOp, Flow, FpOp, Inst, IoMap, Next, Op, Reg, RegFile, Width,
};

/// The sync kind of a block that falls through or jumps.
pub const SYNC_JUMP: u32 = 0;
/// The sync kind of a call; bits 16 to 31 hold the return address.
pub const SYNC_CALL: u32 = 1;
/// The sync kind of a return.
pub const SYNC_RETURN: u32 = 2;
/// Set when the exit may go on in lifted code of the block's unit directly.
pub const SYNC_CHECK: u32 = 4;
/// The shift of the unit's slot (0 to 2, or [`SLOT_RAM`]).
pub const SYNC_SLOT_SHIFT: u32 = 3;
/// The shift of the unit's bank or RAM group plus one.
pub const SYNC_UNIT_SHIFT: u32 = 5;
/// The shift of a call's return address.
pub const SYNC_RET_SHIFT: u32 = 16;
/// The slot number of a unit of code in work RAM.
pub const SLOT_RAM: u8 = 3;
/// The first address of work RAM.
pub const RAM: u16 = 0xC000;
/// The first address cartridge RAM can be mapped at.
pub const CART_RAM: u16 = 0x8000;

/// The code a unit holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum UnitTag {
    /// The interrupt entry blocks, which belong to no ROM bank.
    Entries,
    /// A ROM bank as a slot (0 to 2) shows it.
    Rom {
        /// The bank.
        bank: u32,
        /// The slot.
        slot: u8,
    },
    /// A group of code in work RAM or cartridge RAM.
    Ram {
        /// The group's index.
        group: u32,
    },
}

impl UnitTag {
    fn bits(self) -> u32 {
        match self {
            UnitTag::Entries => 0,
            UnitTag::Rom { bank, slot } => {
                u32::from(slot) << SYNC_SLOT_SHIFT | ((bank + 1) & 0x7FF) << SYNC_UNIT_SHIFT
            }
            UnitTag::Ram { group } => {
                u32::from(SLOT_RAM) << SYNC_SLOT_SHIFT | ((group + 1) & 0x7FF) << SYNC_UNIT_SHIFT
            }
        }
    }

    /// Whether code of this unit sits at the CPU address `a`.
    #[must_use]
    pub fn holds(self, a: u64) -> bool {
        match self {
            UnitTag::Entries => false,
            UnitTag::Rom { slot, .. } => a < u64::from(RAM) && (a >> 14) as u8 == slot,
            UnitTag::Ram { .. } => (u64::from(CART_RAM)..0x1_0000).contains(&a),
        }
    }
}

/// The address map: every byte reaches the machine.
#[must_use]
pub fn device_map() -> IoMap {
    IoMap {
        ranges: Vec::new(),
        default: AddrClass::Device,
    }
}

/// The sync info word of `b`, lifted for `unit` (the `SYNC_*` constants).
#[must_use]
pub fn sync_info(b: &Block, unit: UnitTag) -> u32 {
    let kind = match b.exit.flow {
        Flow::Call { ret } => SYNC_CALL | (((ret & 0xFFFF) as u32) << SYNC_RET_SHIFT),
        Flow::Return => SYNC_RETURN,
        Flow::Jump | Flow::Trap(_) => SYNC_JUMP,
    };
    let targets = match b.exit.next {
        Next::Const(t) => vec![t],
        Next::Select {
            taken, not_taken, ..
        } => vec![taken, not_taken],
        Next::Dynamic(_) => Vec::new(),
    };
    let direct = targets.iter().any(|&t| t <= 0xFFFF && unit.holds(t))
        || matches!(b.exit.flow, Flow::Call { ret } if unit.holds(ret & 0xFFFF));
    kind | if direct { SYNC_CHECK } else { 0 } | unit.bits()
}

/// The frontend for the code of one unit. Code in work RAM is lifted with
/// its operand bytes read when it runs, so only its opcode bytes need to
/// match the bytes it was lifted from.
#[derive(Clone, Debug)]
pub struct Sms {
    inner: Z80,
    unit: UnitTag,
}

impl Sms {
    /// The frontend that lifts code of `unit`.
    #[must_use]
    pub fn new(unit: UnitTag) -> Sms {
        Sms {
            inner: Z80 {
                map: device_map(),
                sync: SyncPolicy::Every,
                live_operands: matches!(unit, UnitTag::Ram { .. }),
            },
            unit,
        }
    }
}

impl Frontend for Sms {
    fn granule(&self) -> u64 {
        self.inner.granule()
    }

    fn regs(&self) -> RegFile {
        self.inner.regs()
    }

    fn stack_pointer(&self) -> Option<Reg> {
        self.inner.stack_pointer()
    }

    fn preserved(&self) -> Vec<Reg> {
        self.inner.preserved()
    }

    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        let mut b = self.inner.lift(bytes, pc)?;
        if !matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)) {
            b.insts.push(Inst {
                op: Op::Sync,
                width: None,
            });
        }
        Ok(b)
    }

    fn abi_headers(&self) -> &'static [&'static str] {
        &["relift/z80/abi.h", "relift/sms/run.h"]
    }

    fn home(&self, reg: Reg) -> Home {
        self.inner.home(reg)
    }

    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        Some(match (name, args) {
            (PORT_IN, [port]) => format!("rl_sms_in(c, (uint16_t)({port}))"),
            (PORT_OUT, [port, v]) => {
                format!("rl_sms_out(c, (uint16_t)({port}), (uint8_t)({v}))")
            }
            (IFF, [v]) => format!("(void)({v})"),
            (HALT, [iff1, r]) => {
                format!("rl_sms_halt(c, mem, (uint8_t)({iff1}), (uint8_t)({r}))")
            }
            _ => return None,
        })
    }

    fn wasm_intrinsic(&self, name: &str) -> Option<WasmIntrinsic> {
        self.inner.wasm_intrinsic(name)
    }

    fn clock(&self) -> Option<&'static str> {
        self.inner.clock()
    }

    fn io_calls(&self) -> (&'static str, &'static str) {
        ("rl_sms_read", "rl_sms_write")
    }

    fn sync_call(&self, b: &Block, next: &str) -> Option<String> {
        Some(format!(
            "rl_sms_sync(c, mem, {next}, UINT32_C({:#x}))",
            sync_info(b, self.unit)
        ))
    }

    fn trap(&self, code: u32) -> String {
        self.inner.trap(code)
    }

    fn undefined(&self, pc: u64, insn: u64) -> String {
        self.inner.undefined(pc, insn)
    }

    fn monitor(&self) -> &'static str {
        self.inner.monitor()
    }

    fn fp(
        &self,
        op: FpOp,
        from: Width,
        to: Width,
        args: &[String],
        reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        self.inner.fp(op, from, to, args, reg)
    }

    fn crypto(&self, op: CryptoOp, arg: &str) -> Option<String> {
        self.inner.crypto(op, arg)
    }

    fn fp_state(&self) -> Vec<Reg> {
        self.inner.fp_state()
    }

    fn guard_state(&self, code: u32) -> Option<Reg> {
        self.inner.guard_state(code)
    }

    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        self.inner.guard_slow(code, pc, insn)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const BANK3: UnitTag = UnitTag::Rom { bank: 3, slot: 2 };

    #[test]
    fn every_block_ends_in_a_sync_point_and_every_access_reaches_the_machine() {
        let fe = Sms::new(BANK3);
        for bytes in [
            &[0x00][..],
            &[0x3A, 0x00, 0xC0][..],
            &[0xED, 0xB0][..],
            &[0xDB, 0xBF][..],
        ] {
            let b = fe.lift(bytes, 0x8000).expect("lifts");
            assert!(
                matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)),
                "{bytes:x?}"
            );
            assert!(
                !b.insts
                    .iter()
                    .any(|i| matches!(i.op, Op::Load { .. } | Op::Store { .. })),
                "{bytes:x?}"
            );
        }
    }

    #[test]
    fn the_sync_info_names_the_unit_and_checks_direct_exits_into_it() {
        let fe = Sms::new(BANK3);
        let call = fe.lift(&[0xCD, 0x00, 0x90], 0x8100).expect("lifts");
        assert_eq!(
            sync_info(&call, BANK3),
            SYNC_CALL
                | SYNC_CHECK
                | (0x8103 << SYNC_RET_SHIFT)
                | (2 << SYNC_SLOT_SHIFT)
                | (4 << SYNC_UNIT_SHIFT)
        );
        let jp = fe.lift(&[0xC3, 0x00, 0x00], 0x8100).expect("lifts");
        assert_eq!(
            sync_info(&jp, BANK3),
            SYNC_JUMP | (2 << SYNC_SLOT_SHIFT) | (4 << SYNC_UNIT_SHIFT)
        );
        let ret = fe.lift(&[0xC9], 0x8100).expect("lifts");
        assert_eq!(sync_info(&ret, BANK3) & 3, SYNC_RETURN);
        let ram = UnitTag::Ram { group: 0 };
        let jr = fe.lift(&[0x18, 0xFE], 0xC100).expect("lifts");
        assert_eq!(
            sync_info(&jr, ram),
            SYNC_JUMP | SYNC_CHECK | (3 << SYNC_SLOT_SHIFT) | (1 << SYNC_UNIT_SHIFT)
        );
    }
}

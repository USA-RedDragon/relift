//! The SM83 frontend of the exact Game Boy mode.
//!
//! The device library (`guests/gb/devices`) owns the bus and the
//! instruction boundary, so lifted code makes every access through the
//! runtime (`rl_gb_io_read`, `rl_gb_io_write`), ends every block in a sync
//! point that closes the instruction and opens the next one
//! (`rl_gb_sync`), and leaves `EI`, `DI`, `HALT` and `STOP` to the
//! library's own CPU (`rl_gb_trap`). The sync call carries what the
//! runtime checks the host frames against: the kind of transfer, the
//! return address of a call, and the bank a direct transfer into the
//! switchable window assumes.

use std::collections::BTreeSet;

use relift_cpu_sm83::{HALF, OPERAND, Options, Sm83, SyncPolicy, decode, irq, lift_bytes};
use relift_guest::{Frontend, Home, LiftFailure, WasmIntrinsic};
use relift_ir::{
    AddrClass, Block, Builder, CryptoOp, Exit, Flow, FpOp, Inst, IoMap, Next, Op, Reg, RegFile,
    Width,
};

/// The sync kind of a block that falls through or jumps.
pub const SYNC_JUMP: u32 = 0;
/// The sync kind of a call; bits 16 to 31 hold the return address.
pub const SYNC_CALL: u32 = 1;
/// The sync kind of a return.
pub const SYNC_RETURN: u32 = 2;
/// Set in the sync info when the exit may reach lifted window code of the
/// block's bank directly (a goto or a call by name), so the runtime checks
/// that the bank is still mapped.
pub const SYNC_CHECK: u32 = 8;
/// The shift of the block's bank plus one in the sync info (0: fixed code).
pub const SYNC_BANK_SHIFT: u32 = 4;
/// The shift of a call's return address in the sync info.
pub const SYNC_RET_SHIFT: u32 = 16;

/// The first address of RAM: code from here on is lifted with its operand
/// bytes read when it runs (`rl_gb_operand`), and the runtime picks the
/// lifted instruction by the opcode it fetched.
pub const RAM: u64 = 0x8000;

/// The opcodes the library's CPU runs through `rl_gb_trap`.
pub const NATIVE: [u8; 4] = [0xF3, 0xFB, 0x76, 0x10];

/// The exact-mode frontend.
#[derive(Clone, Debug)]
pub struct Gb {
    inner: Sm83,
    window: BTreeSet<(u64, u64)>,
}

impl Default for Gb {
    fn default() -> Gb {
        Gb::new()
    }
}

/// The address map of the exact mode: every byte reaches the device library.
#[must_use]
pub fn device_map() -> IoMap {
    IoMap {
        ranges: Vec::new(),
        default: AddrClass::Device,
    }
}

/// The sync info word of `b` (see the `SYNC_*` constants); `window` holds
/// the `(bank, address)` of every lifted window instruction and entry.
#[must_use]
pub fn sync_info(b: &Block, window: &BTreeSet<(u64, u64)>) -> u32 {
    let kind = match b.exit.flow {
        Flow::Call { ret } => SYNC_CALL | (((ret & 0xFFFF) as u32) << SYNC_RET_SHIFT),
        Flow::Return => SYNC_RETURN,
        Flow::Jump | Flow::Trap(_) => SYNC_JUMP,
    };
    let Some(k) = b.bank else {
        return kind;
    };
    let targets = match b.exit.next {
        Next::Const(t) => vec![t],
        Next::Select {
            taken, not_taken, ..
        } => vec![taken, not_taken],
        Next::Dynamic(_) => Vec::new(),
    };
    let direct = targets
        .iter()
        .any(|&t| (0x4000..0x8000).contains(&t) && window.contains(&(k, t)));
    kind | if direct { SYNC_CHECK } else { 0 } | ((((k as u32) + 1) & 0xFFF) << SYNC_BANK_SHIFT)
}

impl Gb {
    /// The frontend.
    #[must_use]
    pub fn new() -> Gb {
        Gb::with_window(BTreeSet::new())
    }

    /// The frontend that emits code knowing `window`, the `(bank, address)`
    /// of every lifted window instruction and entry ([`sync_info`]).
    #[must_use]
    pub fn with_window(window: BTreeSet<(u64, u64)>) -> Gb {
        Gb {
            inner: Sm83 {
                map: device_map(),
                sync: SyncPolicy::Every,
            },
            window,
        }
    }

    fn native(bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        let ins = decode(bytes).map_err(|e| LiftFailure {
            insn: bytes.first().copied().map_or(0, u64::from),
            message: e.to_string(),
        })?;
        let len = u64::from(ins.length());
        let op = u32::from(bytes[0]);
        let next = (pc + len) & 0xFFFF;
        let b = Builder::new(relift_cpu_sm83::regs::regs(), Width::W16, pc);
        Ok(b.finish(
            len as u32,
            Exit {
                next: Next::Const(next),
                flow: Flow::Trap((op << 16) | next as u32),
            },
        ))
    }
}

impl Frontend for Gb {
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
        if pc < HALF
            && let Some(&op) = bytes.first()
            && NATIVE.contains(&op)
        {
            return Gb::native(bytes, pc);
        }
        let mut b = if pc & 0xFFFF >= RAM {
            let opts = Options {
                map: self.inner.map.clone(),
                sync: self.inner.sync,
                live_operands: true,
            };
            lift_bytes(bytes, pc, &opts).map_err(|e| LiftFailure {
                insn: bytes
                    .iter()
                    .take(3)
                    .fold(0u64, |acc, b| (acc << 8) | u64::from(*b)),
                message: e.to_string(),
            })?
        } else {
            self.inner.lift(bytes, pc)?
        };
        if !matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)) {
            b.insts.push(Inst {
                op: Op::Sync,
                width: None,
            });
        }
        Ok(b)
    }

    fn abi_headers(&self) -> &'static [&'static str] {
        &["relift/sm83/abi.h", "relift/gb/exact.h"]
    }

    fn home(&self, reg: Reg) -> Home {
        self.inner.home(reg)
    }

    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        match name {
            irq::RETI => Some("rl_gb_reti(c)".into()),
            OPERAND => Some(format!("rl_gb_operand(c, {})", args.first()?)),
            _ => self.inner.intrinsic(name, args),
        }
    }

    fn wasm_intrinsic(&self, name: &str) -> Option<WasmIntrinsic> {
        self.inner.wasm_intrinsic(name)
    }

    fn trap(&self, code: u32) -> String {
        format!("rl_gb_trap(c, mem, {code:#x}u)")
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

    fn clock(&self) -> Option<&'static str> {
        self.inner.clock()
    }

    fn io_calls(&self) -> (&'static str, &'static str) {
        ("rl_gb_io_read", "rl_gb_io_write")
    }

    fn sync_call(&self, b: &Block, next: &str) -> Option<String> {
        Some(format!(
            "rl_gb_sync(c, mem, {next}, UINT32_C({:#x}))",
            sync_info(b, &self.window)
        ))
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

    #[test]
    fn ei_di_halt_and_stop_run_on_the_library_cpu() {
        let fe = Gb::new();
        for (bytes, len) in [
            (&[0xFB, 0x00][..], 1u64),
            (&[0xF3][..], 1),
            (&[0x76][..], 1),
            (&[0x10, 0x00][..], 2),
        ] {
            let b = fe.lift(bytes, 0x150).expect("lifts");
            assert!(b.insts.is_empty());
            assert_eq!(b.exit.next, Next::Const(0x150 + len));
            assert_eq!(
                b.exit.flow,
                Flow::Trap((u32::from(bytes[0]) << 16) | (0x150 + len) as u32)
            );
        }
    }

    #[test]
    fn every_other_block_ends_in_a_sync_point() {
        let fe = Gb::new();
        for bytes in [
            &[0x00][..],
            &[0x3E, 0x12][..],
            &[0xC4, 0x00, 0x40][..],
            &[0xC8][..],
        ] {
            let b = fe.lift(bytes, 0x200).expect("lifts");
            assert!(
                matches!(b.insts.last().map(|i| &i.op), Some(Op::Sync)),
                "{bytes:x?}"
            );
        }
        let half = fe.lift(&[0xC4, 0x00, 0x40], HALF | 0x200).expect("lifts");
        assert!(matches!(half.insts.last().map(|i| &i.op), Some(Op::Sync)));
    }

    #[test]
    fn the_sync_info_names_calls_returns_and_the_assumed_bank() {
        let fe = Gb::new();
        let mut call = fe.lift(&[0xCD, 0x00, 0x50], 0x4100).expect("lifts");
        call.bank = Some(7);
        let none = BTreeSet::new();
        assert_eq!(
            sync_info(&call, &none),
            SYNC_CALL | (0x4103 << SYNC_RET_SHIFT) | (8 << SYNC_BANK_SHIFT)
        );
        let lifted: BTreeSet<(u64, u64)> = [(7, 0x5000)].into();
        assert_eq!(
            sync_info(&call, &lifted),
            SYNC_CALL | SYNC_CHECK | (0x4103 << SYNC_RET_SHIFT) | (8 << SYNC_BANK_SHIFT)
        );
        let ret = fe.lift(&[0xC9], 0x4100).expect("lifts");
        assert_eq!(sync_info(&ret, &none), SYNC_RETURN);
        let mut jp_hl = fe.lift(&[0xE9], 0x4100).expect("lifts");
        jp_hl.bank = Some(7);
        assert_eq!(
            sync_info(&jp_hl, &lifted),
            SYNC_JUMP | (8 << SYNC_BANK_SHIFT)
        );
    }

    #[test]
    fn every_access_reaches_the_device_library() {
        let fe = Gb::new();
        let b = fe.lift(&[0xEA, 0x00, 0xC0], 0x200).expect("lifts");
        let io = b
            .insts
            .iter()
            .filter(|i| matches!(i.op, Op::IoStore { .. }));
        assert_eq!(io.count(), 1);
        assert!(
            !b.insts
                .iter()
                .any(|i| matches!(i.op, Op::Store { .. } | Op::Load { .. }))
        );
    }
}

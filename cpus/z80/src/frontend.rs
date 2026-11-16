//! The Z80 [`Frontend`]: lifting through [`crate::lift`], and the context
//! layout of `runtime/cpus/z80` (`relift/z80/abi.h`).
//!
//! Each register slot is a field of the context with the slot's name, the
//! eight flag slots are the bits of the byte `f` (S Z Y H X P/V N C from bit
//! 7 to bit 0, as the CPU has F), the shadow pairs `af_`, `bc_`, `de_` and
//! `hl_` are 16-bit fields, and the slot of the interrupt entries' return
//! address is the field `ret`.

use relift_guest::{Frontend, Home, LiftFailure, WasmIntrinsic};
use relift_ir::{Block, CryptoOp, FpOp, IoMap, Reg, RegFile, Width};

use crate::{HALT, IFF, Options, PORT_IN, PORT_OUT, SyncPolicy, lift_bytes, regs};

/// The Z80 frontend for a machine whose address space is classified by
/// `map` (the guest's [`IoMap`]).
#[derive(Clone, Debug)]
pub struct Z80 {
    /// Which addresses are memory and which reach a device.
    pub map: IoMap,
    /// Where sync points go besides control transfers.
    pub sync: SyncPolicy,
}

impl Z80 {
    /// A frontend over `map`.
    pub fn new(map: IoMap) -> Z80 {
        Z80 {
            map,
            sync: SyncPolicy::default(),
        }
    }
}

impl Frontend for Z80 {
    fn granule(&self) -> u64 {
        1
    }

    fn regs(&self) -> RegFile {
        regs::regs()
    }

    fn stack_pointer(&self) -> Option<Reg> {
        Some(regs::SP)
    }

    fn preserved(&self) -> Vec<Reg> {
        Vec::new()
    }

    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        let opts = Options {
            map: self.map.clone(),
            sync: self.sync,
        };
        lift_bytes(bytes, pc, &opts)
            .map(|l| l.block)
            .map_err(|e| LiftFailure {
                insn: bytes
                    .iter()
                    .take(4)
                    .fold(0u64, |acc, b| (acc << 8) | u64::from(*b)),
                message: e.to_string(),
            })
    }

    fn abi_headers(&self) -> &'static [&'static str] {
        &["relift/z80/abi.h"]
    }

    fn home(&self, reg: Reg) -> Home {
        if let Some(i) = regs::F_BITS.iter().position(|r| *r == reg) {
            return Home::Flag {
                word: "c->f".into(),
                bit: 7 - i as u32,
            };
        }
        Home::Field(format!("c->{}", regs::DECLS[reg.index()].name))
    }

    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        Some(match (name, args) {
            (PORT_IN, [port]) => format!("rl_z80_in(c, (uint16_t)({port}))"),
            (PORT_OUT, [port, v]) => format!("rl_z80_out(c, (uint16_t)({port}), (uint8_t)({v}))"),
            (IFF, [v]) => format!("rl_z80_iff(c, (uint8_t)({v}))"),
            (HALT, [iff1, r]) => format!("rl_z80_halt(c, mem, (uint8_t)({iff1}), (uint8_t)({r}))"),
            _ => return None,
        })
    }

    fn wasm_intrinsic(&self, name: &str) -> Option<WasmIntrinsic> {
        matches!(name, PORT_IN | PORT_OUT | HALT | IFF).then_some(WasmIntrinsic::Import)
    }

    fn clock(&self) -> Option<&'static str> {
        Some("c->time")
    }

    fn trap(&self, code: u32) -> String {
        format!("rl_trap(c, mem, {code:#x}u)")
    }

    fn undefined(&self, pc: u64, insn: u64) -> String {
        format!("rl_undefined(c, mem, UINT64_C({pc:#x}), {insn:#x}u)")
    }

    fn monitor(&self) -> &'static str {
        "NULL"
    }

    fn fp(
        &self,
        _op: FpOp,
        _from: Width,
        _to: Width,
        _args: &[String],
        _reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        None
    }

    fn crypto(&self, _op: CryptoOp, _arg: &str) -> Option<String> {
        None
    }

    fn fp_state(&self) -> Vec<Reg> {
        Vec::new()
    }

    fn guard_state(&self, _code: u32) -> Option<Reg> {
        None
    }

    fn guard_slow(&self, _code: u32, _pc: u64, _insn: u64) -> Option<String> {
        None
    }
}

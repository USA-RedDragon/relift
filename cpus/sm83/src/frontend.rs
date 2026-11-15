//! The SM83 [`Frontend`]: lifting through [`crate::lift`].
//!
//! The context layout in [`Frontend::home`] and the C names are provisional:
//! `runtime/cpus/sm83` does not exist yet.

use relift_guest::{Frontend, Home, LiftFailure, WasmIntrinsic};
use relift_ir::{Block, CryptoOp, FpOp, IoMap, Reg, RegFile, Width};

use crate::{Options, SyncPolicy, irq, lift_bytes, regs};

/// The SM83 frontend for a machine whose address space is classified by
/// `map` (the guest's [`IoMap`]).
#[derive(Clone, Debug)]
pub struct Sm83 {
    /// Which addresses are memory and which reach a device.
    pub map: IoMap,
    /// Where sync points go besides control transfers.
    pub sync: SyncPolicy,
}

impl Sm83 {
    /// A frontend over `map`.
    pub fn new(map: IoMap) -> Sm83 {
        Sm83 {
            map,
            sync: SyncPolicy::default(),
        }
    }
}

impl Frontend for Sm83 {
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
        let insn = bytes.first().copied().map_or(0, u64::from);
        let opts = Options {
            map: self.map.clone(),
            sync: self.sync,
            live_operands: false,
        };
        lift_bytes(bytes, pc, &opts).map_err(|e| LiftFailure {
            insn: bytes
                .iter()
                .take(3)
                .fold(0u64, |acc, b| (acc << 8) | u64::from(*b))
                .max(insn),
            message: e.to_string(),
        })
    }

    fn abi_headers(&self) -> &'static [&'static str] {
        &["relift/sm83/abi.h"]
    }

    fn home(&self, reg: Reg) -> Home {
        match reg.index() {
            0..=6 => Home::Field(format!(
                "c->{}",
                ["a", "b", "c", "d", "e", "h", "l"][reg.index()]
            )),
            7 => Home::Field("c->sp".into()),
            i => Home::Flag {
                word: "c->f".into(),
                bit: 7 - (i as u32 - 8),
            },
        }
    }

    fn intrinsic(&self, name: &str, _args: &[String]) -> Option<String> {
        Some(
            match name {
                irq::EI => "(rl_sm83_ei(c), rl_sm83_ei_commit(c))",
                irq::DI => "rl_sm83_di(c)",
                irq::RETI => "rl_sm83_reti(c)",
                irq::HALT => {
                    "((c->ints && c->ints->pending(c->ints->opaque)) ? 1u : ((void)rl_sm83_halt(c, mem), 0u))"
                }
                irq::STOP => "rl_sm83_stop(c, mem)",
                _ => return None,
            }
            .to_string(),
        )
    }

    fn wasm_intrinsic(&self, name: &str) -> Option<WasmIntrinsic> {
        matches!(name, irq::EI | irq::DI | irq::RETI | irq::HALT | irq::STOP)
            .then_some(WasmIntrinsic::Import)
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

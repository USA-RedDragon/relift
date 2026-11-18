//! `relift-gbrom ir`: the IR of SM83 instructions as the Game Boy guest
//! lifts them.

use std::io::Write;

use clap::Args;
use relift_cpu_sm83::decode;
use relift_gb::frontend::Gb;
use relift_guest::Frontend;
use relift_ir::dce;

use crate::BoxError;

/// Arguments of `relift-gbrom ir`.
#[derive(Args, Debug)]
pub struct IrArgs {
    /// Instruction bytes in hex (`0x` optional), one instruction after the
    /// other from `--pc`: `f0 40` is `LDH A,(0x40)`.
    #[arg(required = true)]
    pub bytes: Vec<String>,
    /// The address of the first instruction.
    #[arg(long, default_value = "0x150", value_parser = hex)]
    pub pc: u64,
    /// The code bank to record on each block (a ROM bank number).
    #[arg(long)]
    pub bank: Option<u64>,
    /// Remove dead values before printing.
    #[arg(long)]
    pub dce: bool,
}

fn hex(s: &str) -> Result<u64, String> {
    u64::from_str_radix(s.trim_start_matches("0x"), 16).map_err(|e| e.to_string())
}

/// Run `relift-gbrom ir`.
///
/// # Errors
///
/// A byte string is not hex, an instruction is not defined or ends after
/// the last byte, or stdout cannot be written.
pub fn run(a: &IrArgs) -> Result<(), BoxError> {
    let mut bytes = Vec::new();
    for s in &a.bytes {
        let s = s.trim_start_matches("0x");
        if s.len() % 2 != 0 || !s.is_ascii() {
            return Err(format!("{s}: not whole hex bytes").into());
        }
        for i in (0..s.len()).step_by(2) {
            bytes.push(u8::from_str_radix(&s[i..i + 2], 16).map_err(|e| format!("{s}: {e}"))?);
        }
    }
    let frontend = Gb::new();
    let mut out = std::io::stdout().lock();
    let mut at = 0;
    while at < bytes.len() {
        let rest = &bytes[at..];
        let insn = decode(rest)?;
        let len = usize::from(insn.length());
        let pc = a.pc + at as u64;
        let hexed: String = rest[..len].iter().map(|b| format!("{b:02x}")).collect();
        write!(out, "; {pc:#x} {hexed} {:?}", insn.opcode.mnemonic)?;
        if let Some(bank) = a.bank {
            write!(out, " bank {bank}")?;
        }
        writeln!(out)?;
        let mut block = frontend.lift(rest, pc).map_err(|e| e.message)?;
        block.bank = a.bank;
        let block = if a.dce { dce(&block) } else { block };
        write!(out, "{block}")?;
        at += len;
    }
    Ok(())
}

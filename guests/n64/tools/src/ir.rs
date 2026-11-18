//! `relift-n64rom ir`: the IR of MIPS and RSP instructions as the Nintendo
//! 64 guest lifts them.

use std::io::Write;

use clap::Args;
use relift_guest::Frontend;
use relift_ir::dce;
use relift_n64::frontend::N64;
use relift_n64::rsp::Rsp;

use crate::BoxError;

/// Arguments of `relift-n64rom ir`.
#[derive(Args, Debug)]
pub struct IrArgs {
    /// Instruction words in hex (`0x` optional), lifted at `--pc` onward.
    /// A branch is lifted with the word after it as its delay slot.
    #[arg(required = true)]
    pub words: Vec<String>,
    /// The address of the first word.
    #[arg(long, default_value = "0x80001000", value_parser = hex)]
    pub pc: u64,
    /// Lift as the RSP (IMEM addresses, vector unit) instead of the VR4300.
    #[arg(long)]
    pub rsp: bool,
    /// Remove dead values before printing.
    #[arg(long)]
    pub dce: bool,
}

fn hex(s: &str) -> Result<u64, String> {
    u64::from_str_radix(s.trim_start_matches("0x"), 16).map_err(|e| e.to_string())
}

/// Run `relift-n64rom ir`.
///
/// # Errors
///
/// A word is not hex, an instruction does not lift, or stdout cannot be
/// written.
pub fn run(a: &IrArgs) -> Result<(), BoxError> {
    let mut bytes = Vec::new();
    for s in &a.words {
        let w =
            u32::from_str_radix(s.trim_start_matches("0x"), 16).map_err(|e| format!("{s}: {e}"))?;
        bytes.extend_from_slice(&w.to_be_bytes());
    }
    bytes.extend_from_slice(&[0; 4]);
    let mut out = std::io::stdout().lock();
    let mut at = 0;
    while at < a.words.len() * 4 {
        let rest = &bytes[at..];
        let pc = a.pc + at as u64;
        let word = u32::from_be_bytes([rest[0], rest[1], rest[2], rest[3]]);
        writeln!(out, "; {pc:#x} {word:08x}")?;
        let block = if a.rsp {
            Rsp.lift(rest, pc)
        } else {
            N64.lift(rest, pc)
        }
        .map_err(|e| e.message)?;
        let len = block.len as usize;
        let block = if a.dce { dce(&block) } else { block };
        write!(out, "{block}")?;
        at += len.max(4);
    }
    Ok(())
}

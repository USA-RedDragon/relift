//! `relift-ir`: print the `relift-ir` lift of A64 instructions, given as hex
//! words or taken from an NSO by symbol or address.

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use relift_cpu_arm64::cpu::{Cpu, Va};
use relift_cpu_arm64::decode::{Features, decode_for};
use relift_cpu_arm64::lift::lift;
use relift_ir::dce;
use relift_switch_formats::nso::Nso;
use relift_switch_formats::storage::read_all;
use relift_switch_tools::{BoxError, input, print_json, run};
use serde_json::json;

/// Print the IR of A64 instructions.
#[derive(Parser, Debug)]
#[command(name = "relift-ir", version)]
struct Cli {
    /// Instruction words in hex (`0x` optional), lifted at `--pc` onward.
    words: Vec<String>,
    /// The address of the first word.
    #[arg(long, default_value = "0x1000", value_parser = hex)]
    pc: u64,
    /// Take the instructions from this NSO (an image offset is an address).
    #[arg(long, conflicts_with = "words")]
    nso: Option<PathBuf>,
    /// With `--nso`: lift the symbol of this name, over its size.
    #[arg(long, requires = "nso", conflicts_with = "addr")]
    sym: Option<String>,
    /// With `--nso`: lift from this address.
    #[arg(long, requires = "nso", value_parser = hex)]
    addr: Option<u64>,
    /// With `--addr`, or a symbol of unknown size: how many instructions.
    #[arg(long, default_value_t = 16)]
    count: u64,
    /// The CPU's architecture features: a base (`cortex-a57`, `armv8.0`,
    /// `all`) and features such as `FEAT_LSE`, comma-separated.
    #[arg(long, default_value = "cortex-a57", value_parser = Features::parse)]
    features: Features,
    /// Remove dead values before printing.
    #[arg(long)]
    dce: bool,
    /// Print only the instruction lines that contain this text, with each
    /// block's header and exit line.
    #[arg(long)]
    grep: Option<String>,
    /// Print JSON.
    #[arg(long)]
    json: bool,
}

fn hex(s: &str) -> Result<u64, String> {
    u64::from_str_radix(s.trim_start_matches("0x"), 16).map_err(|e| e.to_string())
}

/// `text` with only the instruction lines containing `grep`, when given.
fn filter(text: &str, grep: Option<&str>) -> String {
    let Some(grep) = grep else {
        return text.to_owned();
    };
    let mut out = String::new();
    for line in text.lines() {
        let inner = line.starts_with("  ") && !line.starts_with("  exit ");
        if !inner || line.contains(grep) {
            out.push_str(line);
            out.push('\n');
        }
    }
    out
}

fn main() -> ExitCode {
    run::<Cli>("relift-ir", body)
}

fn body(cli: Cli) -> Result<(), BoxError> {
    let words = words(&cli)?;
    let mut out = Vec::new();
    for (pc, word) in words {
        let encoding = decode_for(word, &cli.features).map(|i| i.encoding.name());
        let cpu = Cpu::new(cli.features, Va::BITS39);
        let (ir, error) = match lift(word, pc, &cpu) {
            Ok(b) => {
                let b = if cli.dce { dce(&b) } else { b };
                (Some(filter(&b.to_string(), cli.grep.as_deref())), None)
            }
            Err(e) => (None, Some(e.to_string())),
        };
        out.push(json!({
            "addr": format!("{pc:#x}"),
            "word": format!("{word:08x}"),
            "encoding": encoding,
            "ir": ir,
            "error": error,
        }));
    }
    if cli.json {
        return print_json(&serde_json::Value::Array(out));
    }
    let mut w = std::io::stdout().lock();
    for v in &out {
        let s = |k: &str| v[k].as_str().unwrap_or("-").to_string();
        writeln!(w, "; {} {} {}", s("addr"), s("word"), s("encoding"))?;
        match v["ir"].as_str() {
            Some(ir) => write!(w, "{ir}")?,
            None => writeln!(w, "; error: {}", s("error"))?,
        }
    }
    Ok(())
}

/// The `(address, word)` pairs to lift.
fn words(cli: &Cli) -> Result<Vec<(u64, u32)>, BoxError> {
    let Some(path) = &cli.nso else {
        if cli.words.is_empty() {
            return Err("give instruction words, or --nso with --sym or --addr".into());
        }
        let mut v = Vec::new();
        for (i, s) in cli.words.iter().enumerate() {
            let w = u32::from_str_radix(s.trim_start_matches("0x"), 16)
                .map_err(|e| format!("{s}: {e}"))?;
            v.push((cli.pc + 4 * i as u64, w));
        }
        return Ok(v);
    };
    let nso = Nso::parse(&read_all(&*input(path)?)?)?;
    let (start, count) = match (&cli.sym, cli.addr) {
        (Some(name), _) => {
            let s = nso
                .symbols()?
                .into_iter()
                .find(|s| &s.name == name && s.is_defined())
                .ok_or_else(|| format!("no defined symbol {name}"))?;
            let n = if s.size == 0 { cli.count } else { s.size / 4 };
            (s.value, n)
        }
        (None, Some(a)) => (a, cli.count),
        (None, None) => return Err("--nso needs --sym or --addr".into()),
    };
    let image = nso.flat_image();
    let mut v = Vec::new();
    for i in 0..count {
        let a = start + 4 * i;
        let o = usize::try_from(a)?;
        let Some(b) = image.get(o..o + 4) else {
            break;
        };
        v.push((a, u32::from_le_bytes([b[0], b[1], b[2], b[3]])));
    }
    Ok(v)
}

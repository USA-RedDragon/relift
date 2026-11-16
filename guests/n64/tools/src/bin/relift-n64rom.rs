//! `relift-n64rom`: print the header of a Nintendo 64 ROM with its byte
//! order, boot variant and checksum check, list its Yay0, Yaz0 and MIO0
//! streams, or write it back as big-endian.

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Args, Parser, Subcommand};
use relift_n64_formats::{Cic, CrcCheck, Rom};
use relift_n64_tools::{BoxError, input, output, print_json, run};
use serde_json::{Value, json};

/// Inspect and normalize Nintendo 64 ROM images. Reads bytes only; the
/// boot code is hashed to name the variant and never interpreted.
#[derive(Parser, Debug)]
#[command(name = "relift-n64rom", version)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Args, Debug)]
struct Common {
    /// The ROM image in any byte order, or - for stdin.
    rom: PathBuf,
    /// Print JSON.
    #[arg(long)]
    json: bool,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Show the byte order, header, boot variant and checksum check.
    Info {
        #[command(flatten)]
        common: Common,
        /// Check the checksum as this boot variant (such as 6102) instead of
        /// the detected one, for images with a custom boot code.
        #[arg(long, value_name = "CIC")]
        cic: Option<String>,
        /// Exit with code 1 when the checksum check does not pass.
        #[arg(long)]
        check: bool,
    },
    /// List the Yay0, Yaz0 and MIO0 streams that decode completely.
    Streams {
        #[command(flatten)]
        common: Common,
    },
    /// Write the image as big-endian (`.z64`).
    Normalize {
        /// The ROM image in any byte order, or - for stdin.
        rom: PathBuf,
        /// The file to write.
        out: PathBuf,
    },
}

fn open(path: &std::path::Path) -> Result<Rom, BoxError> {
    let bytes = input(path)?;
    Rom::new(&bytes).map_err(|e| format!("{}: {e}", path.display()).into())
}

fn hex8(v: u32) -> String {
    format!("{v:08X}")
}

fn crc_json(c: CrcCheck) -> Value {
    match c {
        CrcCheck::Good => json!({"status": "ok"}),
        CrcCheck::Bad((a, b)) => json!({
            "status": "mismatch",
            "computed1": hex8(a),
            "computed2": hex8(b),
        }),
        CrcCheck::Unavailable(why) => json!({"status": "unavailable", "reason": why}),
    }
}

fn info(common: &Common, cic_arg: Option<&str>, check: bool) -> Result<(), BoxError> {
    let rom = open(&common.rom)?;
    let h = rom.header();
    let forced = match cic_arg {
        Some(n) => Some(Cic::parse(n).ok_or_else(|| format!("--cic: unknown variant {n}"))?),
        None => None,
    };
    let detected = rom.cic();
    let cic = forced.or(detected);
    let crc = rom.check_crc(forced);
    let entry = cic.map(|c| c.entry(h.entry));
    if common.json {
        print_json(&json!({
            "byte_order": rom.order.name(),
            "bytes": rom.bytes.len(),
            "pi_config": hex8(h.pi_config),
            "clock_rate": hex8(h.clock_rate),
            "entry": hex8(h.entry),
            "entry_adjusted": entry.map(hex8),
            "release": hex8(h.release),
            "crc1": hex8(h.crc1),
            "crc2": hex8(h.crc2),
            "name": h.name_text(),
            "media": h.media,
            "game_id": h.game_id,
            "region": h.region,
            "version": h.version,
            "cic": cic.map(Cic::name),
            "cic_forced": forced.is_some(),
            "crc": crc_json(crc),
        }))?;
    } else {
        let mut out = std::io::stdout().lock();
        writeln!(out, "byte order: {}", rom.order.name())?;
        writeln!(out, "size: {} bytes", rom.bytes.len())?;
        writeln!(out, "name: {}", h.name_text())?;
        writeln!(out, "pi config: {}", hex8(h.pi_config))?;
        writeln!(out, "clock rate: {}", hex8(h.clock_rate))?;
        match entry {
            Some(e) => writeln!(out, "entry: {} (boot jumps to {})", hex8(h.entry), hex8(e))?,
            None => writeln!(out, "entry: {}", hex8(h.entry))?,
        }
        writeln!(out, "release: {}", hex8(h.release))?;
        writeln!(out, "media: ${:02X}", h.media)?;
        writeln!(
            out,
            "game id: {}{}",
            relift_n64_formats::header::Header::printable(h.game_id[0]),
            relift_n64_formats::header::Header::printable(h.game_id[1])
        )?;
        writeln!(out, "region: ${:02X}, version: {}", h.region, h.version)?;
        match (cic, forced.is_some()) {
            (Some(c), true) => writeln!(out, "boot variant: {} (forced)", c.name())?,
            (Some(c), false) => writeln!(out, "boot variant: {}", c.name())?,
            (None, _) => writeln!(out, "boot variant: unknown")?,
        }
        let crcs = format!("{} {}", hex8(h.crc1), hex8(h.crc2));
        match crc {
            CrcCheck::Good => writeln!(out, "crc: {crcs} ok")?,
            CrcCheck::Bad((a, b)) => {
                writeln!(
                    out,
                    "crc: {crcs} MISMATCH, computed {} {}",
                    hex8(a),
                    hex8(b)
                )?;
            }
            CrcCheck::Unavailable(why) => writeln!(out, "crc: {crcs} not checked ({why})")?,
        }
    }
    if check && crc != CrcCheck::Good {
        return Err("the checksum check did not pass".into());
    }
    Ok(())
}

fn streams(common: &Common) -> Result<(), BoxError> {
    let found = open(&common.rom)?.streams();
    if common.json {
        let list: Vec<Value> = found
            .iter()
            .map(|s| json!({"offset": s.offset, "format": s.kind.name(), "decoded_size": s.decoded_size}))
            .collect();
        return print_json(&json!({"streams": list}));
    }
    let mut out = std::io::stdout().lock();
    for s in &found {
        writeln!(out, "{:08X} {} {}", s.offset, s.kind.name(), s.decoded_size)?;
    }
    if found.is_empty() {
        writeln!(out, "no streams found")?;
    }
    Ok(())
}

fn main() -> ExitCode {
    run::<Cli>("relift-n64rom", |cli| match cli.command {
        Command::Info { common, cic, check } => info(&common, cic.as_deref(), check),
        Command::Streams { common } => streams(&common),
        Command::Normalize { rom, out } => {
            let rom = open(&rom)?;
            output(&out, &rom.bytes)
        }
    })
}

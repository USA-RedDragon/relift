//! `relift-smsrom`: print the layout, header, checksums, system and mapper
//! guess of a Master System, Game Gear or SG-1000 ROM.

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use relift_sms_formats::Cartridge;
use relift_sms_tools::{BoxError, input, print_json, run};
use serde_json::{Value, json};

/// Inspect Master System, Game Gear and SG-1000 ROM images.
#[derive(Parser, Debug)]
#[command(name = "relift-smsrom", version)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Print the size, header, checksums, system and mapper guess.
    Info {
        /// The ROM image, or - for stdin.
        file: PathBuf,
        /// Print JSON.
        #[arg(long)]
        json: bool,
    },
}

fn main() -> ExitCode {
    run::<Cli>("relift-smsrom", |cli| match cli.command {
        Command::Info { file, json } => {
            let rom = input(&file)?;
            let ext = file.extension().and_then(|e| e.to_str());
            let cart =
                Cartridge::parse(&rom, ext).map_err(|e| format!("{}: {e}", file.display()))?;
            if json {
                print_json(&to_json(&cart))
            } else {
                print_text(&cart)
            }
        }
    })
}

fn hex(b: &[u8]) -> String {
    b.iter()
        .map(|x| format!("{x:02X}"))
        .collect::<Vec<_>>()
        .join(" ")
}

fn to_json(c: &Cartridge) -> Value {
    let l = &c.layout;
    let header = c.header.as_ref().map(|h| {
        json!({
            "offset": h.offset,
            "reserved": hex(&h.reserved),
            "product_code": h.product_code,
            "version": h.version,
            "region_code": h.region_code,
            "region": h.region.map(|r| r.name()),
            "size_code": h.size_code,
            "size_bytes": h.size_bytes,
            "checksum": {
                "stored": h.checksum,
                "computed": h.computed_checksum,
                "ok": h.checksum_ok,
            },
        })
    });
    let cm = c.codemasters.as_ref().map(|m| {
        json!({
            "banks": m.banks,
            "date": hex(&m.date),
            "checksum": m.checksum,
            "checksum_negated": m.checksum_negated,
            "computed": m.computed,
            "pair_ok": m.pair_ok,
            "ok": m.checksum_ok,
        })
    });
    json!({
        "image_bytes": l.image_bytes,
        "copier_header": l.copier_header,
        "rom_bytes": l.rom_bytes,
        "banks": l.banks,
        "partial_bank": l.partial_bank,
        "power_of_two": l.power_of_two,
        "system": c.system.map(|(s, _)| s.name()),
        "system_source": c.system.map(|(_, s)| s.name()),
        "header": header,
        "codemasters": cm,
        "mapper": {
            "kind": c.mapper.kind.name(),
            "reason": c.mapper.reason,
            "sega_stores": c.mapper.sega_stores,
            "codemasters_stores": c.mapper.codemasters_stores,
            "korea_stores": c.mapper.korea_stores,
        },
    })
}

fn print_text(c: &Cartridge) -> Result<(), BoxError> {
    let l = &c.layout;
    let ok = |b: bool| if b { "ok" } else { "MISMATCH" };
    let mut out = std::io::stdout().lock();
    match c.system {
        Some((s, src)) => writeln!(out, "system: {} (from {})", s.name(), src.name())?,
        None => writeln!(out, "system: unknown")?,
    }
    writeln!(
        out,
        "rom: {} bytes, {} banks{}{}{}",
        l.rom_bytes,
        l.banks,
        if l.partial_bank {
            ", partial last bank"
        } else {
            ""
        },
        if l.power_of_two {
            ""
        } else {
            ", not a power of two"
        },
        if l.copier_header {
            ", 512-byte copier header skipped"
        } else {
            ""
        },
    )?;
    match &c.header {
        None => writeln!(out, "header: none")?,
        Some(h) => {
            writeln!(out, "header: TMR SEGA at ${:04X}", h.offset)?;
            writeln!(out, "  reserved: {}", hex(&h.reserved))?;
            writeln!(out, "  product code: {}", h.product_code)?;
            writeln!(out, "  version: {}", h.version)?;
            match h.region {
                Some(r) => writeln!(out, "  region: {} (${:X})", r.name(), h.region_code)?,
                None => writeln!(out, "  region: unknown (${:X})", h.region_code)?,
            }
            match h.size_bytes {
                Some(n) => writeln!(out, "  size code: ${:X} ({n} bytes)", h.size_code)?,
                None => writeln!(out, "  size code: ${:X} (unused)", h.size_code)?,
            }
            match h.computed_checksum {
                Some(v) => writeln!(
                    out,
                    "  checksum: ${:04X} computed ${v:04X} {}",
                    h.checksum,
                    ok(h.checksum_ok)
                )?,
                None => writeln!(
                    out,
                    "  checksum: ${:04X} not computable from the size code",
                    h.checksum
                )?,
            }
        }
    }
    match &c.codemasters {
        Some(m) if m.pair_ok => {
            writeln!(
                out,
                "codemasters header: banks {}, date {}",
                m.banks,
                hex(&m.date)
            )?;
            writeln!(
                out,
                "  checksum: ${:04X} computed ${:04X} {}",
                m.checksum,
                m.computed,
                ok(m.checksum_ok)
            )?;
        }
        _ => writeln!(out, "codemasters header: none")?,
    }
    writeln!(
        out,
        "mapper: {} ({})",
        c.mapper.kind.name(),
        c.mapper.reason
    )?;
    writeln!(
        out,
        "  stores: {} sega, {} codemasters, {} korea",
        c.mapper.sega_stores, c.mapper.codemasters_stores, c.mapper.korea_stores
    )?;
    Ok(())
}

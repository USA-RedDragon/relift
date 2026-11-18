//! `relift-gbrom`: print the header of a Game Boy or Game Boy Color ROM and
//! validate its checksums, list its functions per bank, or print the IR of
//! SM83 instructions.

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use relift_gb_formats::Cartridge;
use relift_gb_tools::functions::{self, FunctionsArgs};
use relift_gb_tools::ir::{self, IrArgs};
use relift_gb_tools::{BoxError, input, print_json, run};
use serde_json::{Value, json};

/// Show a ROM's cartridge header and check its logo, checksums and size.
#[derive(Parser, Debug)]
#[command(name = "relift-gbrom", version, args_conflicts_with_subcommands = true)]
struct Cli {
    #[command(subcommand)]
    command: Option<Command>,
    /// The ROM image, or - for stdin.
    #[arg(required = true)]
    rom: Option<PathBuf>,
    /// Print JSON.
    #[arg(long)]
    json: bool,
    /// Exit with code 1 when any check fails.
    #[arg(long)]
    check: bool,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Find the functions of a ROM across its mapper banks: per bank, with
    /// the transfers that cannot be resolved and the code run from RAM.
    Functions(FunctionsArgs),
    /// Print the IR of SM83 instructions given as hex bytes, as the Game Boy
    /// guest lifts them.
    Ir(IrArgs),
}

fn main() -> ExitCode {
    run::<Cli>("relift-gbrom", |cli| {
        let rom_path = match (cli.command, cli.rom) {
            (Some(Command::Functions(a)), _) => return functions::run(&a),
            (Some(Command::Ir(a)), _) => return ir::run(&a),
            (None, Some(p)) => p,
            (None, None) => return Err("a ROM path is required".into()),
        };
        let rom = input(&rom_path)?;
        let cart = Cartridge::parse(&rom).map_err(|e| format!("{}: {e}", rom_path.display()))?;
        if cli.json {
            print_json(&to_json(&cart))?;
        } else {
            print_text(&cart)?;
        }
        if cli.check && !cart.checks().all() {
            return Err("one or more checks failed".into());
        }
        Ok(())
    })
}

fn hex(b: &[u8]) -> String {
    b.iter()
        .map(|x| format!("{x:02X}"))
        .collect::<Vec<_>>()
        .join(" ")
}

fn licensee(cart: &Cartridge) -> String {
    let h = &cart.header;
    if h.old_licensee == 0x33 {
        let s: String = h
            .new_licensee
            .iter()
            .map(|b| {
                if b.is_ascii_graphic() {
                    char::from(*b)
                } else {
                    '?'
                }
            })
            .collect();
        format!("new:{s}")
    } else {
        format!("old:{:02X}", h.old_licensee)
    }
}

fn to_json(cart: &Cartridge) -> Value {
    let h = &cart.header;
    let k = cart.checks();
    json!({
        "title": h.title,
        "manufacturer": h.manufacturer,
        "cgb": h.cgb.name(),
        "cgb_byte": h.cgb_byte,
        "sgb": h.sgb,
        "cartridge_type": h.mapper.code,
        "mapper": cart.mapper().name(),
        "features": {
            "ram": h.mapper.features.ram,
            "battery": h.mapper.features.battery,
            "rtc": h.mapper.features.rtc,
            "rumble": h.mapper.features.rumble,
            "sensor": h.mapper.features.sensor,
        },
        "rom_size_code": h.rom_size_code,
        "rom_banks": h.layout.rom_banks,
        "rom_bytes": h.layout.rom_bytes(),
        "ram_size_code": h.ram_size_code,
        "ram_banks": h.layout.ram_banks,
        "ram_bytes": h.layout.ram_bytes,
        "entry": hex(&h.entry),
        "entry_jump": h.entry_jump(),
        "destination": if h.destination == 0 { "japan" } else { "overseas" },
        "licensee": licensee(cart),
        "version": h.version,
        "image_bytes": cart.len,
        "header_checksum": {
            "stored": h.header_checksum,
            "computed": h.header_checksum_computed,
            "ok": k.header_checksum,
        },
        "global_checksum": {
            "stored": h.global_checksum,
            "computed": cart.global_checksum_computed,
            "ok": k.global_checksum,
        },
        "logo_ok": k.logo,
        "size_ok": k.size,
    })
}

fn print_text(cart: &Cartridge) -> Result<(), BoxError> {
    let h = &cart.header;
    let k = cart.checks();
    let ok = |b: bool| if b { "ok" } else { "MISMATCH" };
    let f = h.mapper.features;
    let mut extras = Vec::new();
    for (on, name) in [
        (f.ram, "ram"),
        (f.battery, "battery"),
        (f.rtc, "rtc"),
        (f.rumble, "rumble"),
        (f.sensor, "sensor"),
    ] {
        if on {
            extras.push(name);
        }
    }
    let mut out = std::io::stdout().lock();
    writeln!(out, "title: {}", h.title)?;
    if let Some(m) = &h.manufacturer {
        writeln!(out, "manufacturer: {m}")?;
    }
    writeln!(out, "cgb: {} (${:02X})", h.cgb.name(), h.cgb_byte)?;
    writeln!(out, "sgb: {}", if h.sgb { "yes" } else { "no" })?;
    writeln!(
        out,
        "cartridge type: ${:02X} {} [{}]",
        h.mapper.code,
        cart.mapper().name(),
        extras.join(" ")
    )?;
    writeln!(
        out,
        "rom: {} banks, {} bytes (code ${:02X})",
        h.layout.rom_banks,
        h.layout.rom_bytes(),
        h.rom_size_code
    )?;
    writeln!(
        out,
        "ram: {} banks, {} bytes (code ${:02X})",
        h.layout.ram_banks, h.layout.ram_bytes, h.ram_size_code
    )?;
    match h.entry_jump() {
        Some(t) => writeln!(out, "entry: {} (jp ${t:04X})", hex(&h.entry))?,
        None => writeln!(out, "entry: {}", hex(&h.entry))?,
    }
    writeln!(
        out,
        "destination: {}",
        if h.destination == 0 {
            "japan"
        } else {
            "overseas"
        }
    )?;
    writeln!(out, "licensee: {}", licensee(cart))?;
    writeln!(out, "version: {}", h.version)?;
    writeln!(out, "logo: {}", ok(k.logo))?;
    writeln!(
        out,
        "header checksum: ${:02X} computed ${:02X} {}",
        h.header_checksum,
        h.header_checksum_computed,
        ok(k.header_checksum)
    )?;
    writeln!(
        out,
        "global checksum: ${:04X} computed ${:04X} {}",
        h.global_checksum,
        cart.global_checksum_computed,
        ok(k.global_checksum)
    )?;
    writeln!(out, "size: {} bytes {}", cart.len, ok(k.size))?;
    Ok(())
}

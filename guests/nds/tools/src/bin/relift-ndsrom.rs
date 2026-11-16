//! `relift-ndsrom`: print the header of a Nintendo DS ROM and check its
//! CRCs and logo hash, list and hash its units, extract them, list its
//! files, and find the functions of every unit.

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Args, Parser, Subcommand};
use relift_nds_formats::checksum::{LOGO_SHA256, hex, parse_sha256};
use relift_nds_formats::header::{Binary, Table};
use relift_nds_formats::{Checks, Header, Rom};
use relift_nds_tools::functions::{self, FunctionsArgs};
use relift_nds_tools::units::{self, Unit};
use relift_nds_tools::{BoxError, input, print_json, run};
use serde_json::{Value, json};

/// Inspect a decrypted Nintendo DS ROM.
#[derive(Parser, Debug)]
#[command(name = "relift-ndsrom", version)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Args, Debug)]
struct RomArg {
    /// The ROM image, or - for stdin.
    rom: PathBuf,
    /// Print JSON.
    #[arg(long)]
    json: bool,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Show the cartridge header and the results of its checks.
    Header {
        #[command(flatten)]
        rom: RomArg,
        /// Exit with code 1 when a check fails.
        #[arg(long)]
        check: bool,
        /// The SHA-256 of the logo field to compare against, 64 hex
        /// digits; default is the hash of the real logo.
        #[arg(long, value_name = "HEX")]
        logo_sha256: Option<String>,
    },
    /// List the static binaries, autoload blocks and overlays with their
    /// addresses, sizes, compression and the SHA-256 of each decompressed
    /// unit.
    Units(RomArg),
    /// Write every decompressed unit into a directory, with a units.json.
    Extract {
        /// The ROM image, or - for stdin.
        rom: PathBuf,
        /// The directory to write into.
        outdir: PathBuf,
    },
    /// List the files of the file system with their sizes.
    Files(RomArg),
    /// Find the functions of every unit: one discovery run per static
    /// binary and per overlay, repeated while calls between units give
    /// new entries.
    Functions(FunctionsArgs),
}

fn main() -> ExitCode {
    run::<Cli>("relift-ndsrom", |cli| match cli.command {
        Command::Header {
            rom,
            check,
            logo_sha256,
        } => header(&rom, check, logo_sha256.as_deref()),
        Command::Units(a) => units_cmd(&a),
        Command::Extract { rom, outdir } => {
            let bytes = input(&rom)?;
            let parsed = parse(&bytes, &rom)?;
            let n = units::extract(&parsed, &outdir)?;
            writeln!(
                std::io::stdout().lock(),
                "extracted {n} units to {}",
                outdir.display()
            )?;
            Ok(())
        }
        Command::Files(a) => files(&a),
        Command::Functions(a) => functions::run(&a),
    })
}

fn parse<'a>(bytes: &'a [u8], path: &std::path::Path) -> Result<Rom<'a>, BoxError> {
    Rom::parse(bytes).map_err(|e| format!("{}: {e}", path.display()).into())
}

fn word_hex(b: u32) -> String {
    format!("{b:08X}")
}

fn binary_json(b: &Binary) -> Value {
    json!({
        "rom_offset": b.rom_offset,
        "entry": word_hex(b.entry),
        "ram": word_hex(b.ram),
        "size": b.size,
    })
}

fn table_json(t: &Table) -> Value {
    json!({ "offset": t.offset, "size": t.size })
}

fn header_json(h: &Header, k: &Checks) -> Value {
    json!({
        "title": h.title,
        "game_code": h.game_code,
        "maker": h.maker,
        "unit_code": h.unit_code,
        "seed_select": h.seed_select,
        "capacity": h.capacity,
        "version": h.version,
        "autostart": h.autostart,
        "arm9": binary_json(&h.arm9),
        "arm7": binary_json(&h.arm7),
        "fnt": table_json(&h.fnt),
        "fat": table_json(&h.fat),
        "overlay_table9": table_json(&h.ovt9),
        "overlay_table7": table_json(&h.ovt7),
        "total_size": h.total_size,
        "header_size": h.header_size,
        "image_bytes": h.len,
        "logo_sha256": hex(&h.logo_sha256),
        "logo_ok": k.logo,
        "logo_crc": { "stored": h.logo_crc, "computed": h.logo_crc_computed, "ok": k.logo_crc },
        "header_crc": { "stored": h.header_crc, "computed": h.header_crc_computed, "ok": k.header_crc },
        "secure_crc": {
            "stored": h.secure_crc,
            "computed": h.secure_crc_computed,
            "ok": k.secure_crc,
        },
    })
}

fn header(a: &RomArg, check: bool, logo: Option<&str>) -> Result<(), BoxError> {
    let expected = match logo {
        Some(s) => parse_sha256(s).ok_or("--logo-sha256 needs 64 hex digits")?,
        None => LOGO_SHA256,
    };
    let bytes = input(&a.rom)?;
    let h = Header::parse(&bytes).map_err(|e| format!("{}: {e}", a.rom.display()))?;
    let k = h.checks_against(&expected);
    if a.json {
        print_json(&header_json(&h, &k))?;
    } else {
        header_text(&h, &k)?;
    }
    if check && !k.all() {
        return Err("one or more checks failed".into());
    }
    Ok(())
}

fn header_text(h: &Header, k: &Checks) -> Result<(), BoxError> {
    let ok = |b: bool| if b { "ok" } else { "MISMATCH" };
    let mut out = std::io::stdout().lock();
    writeln!(out, "title: {}", h.title)?;
    writeln!(out, "game code: {}", h.game_code)?;
    writeln!(out, "maker: {}", h.maker)?;
    writeln!(out, "version: {}", h.version)?;
    writeln!(
        out,
        "unit code: ${:02X}, seed select: ${:02X}, capacity: ${:02X}, autostart: ${:02X}",
        h.unit_code, h.seed_select, h.capacity, h.autostart
    )?;
    for (n, b) in [("arm9", &h.arm9), ("arm7", &h.arm7)] {
        writeln!(
            out,
            "{n}: rom ${:X} entry ${:08X} ram ${:08X} size ${:X}",
            b.rom_offset, b.entry, b.ram, b.size
        )?;
    }
    for (n, t) in [
        ("fnt", &h.fnt),
        ("fat", &h.fat),
        ("arm9 overlay table", &h.ovt9),
        ("arm7 overlay table", &h.ovt7),
    ] {
        writeln!(out, "{n}: offset ${:X} size ${:X}", t.offset, t.size)?;
    }
    writeln!(
        out,
        "total size: ${:X}, header size: ${:X}",
        h.total_size, h.header_size
    )?;
    writeln!(out, "image: {} bytes", h.len)?;
    writeln!(out, "logo: sha256 {} {}", hex(&h.logo_sha256), ok(k.logo))?;
    writeln!(
        out,
        "logo crc: ${:04X} computed ${:04X} {}",
        h.logo_crc,
        h.logo_crc_computed,
        ok(k.logo_crc)
    )?;
    writeln!(
        out,
        "header crc: ${:04X} computed ${:04X} {}",
        h.header_crc,
        h.header_crc_computed,
        ok(k.header_crc)
    )?;
    match (h.secure_crc_computed, k.secure_crc) {
        (Some(c), Some(good)) => writeln!(
            out,
            "secure area crc: ${:04X} computed ${c:04X} {}",
            h.secure_crc,
            ok(good)
        )?,
        _ => writeln!(
            out,
            "secure area crc: ${:04X} (image too short)",
            h.secure_crc
        )?,
    }
    Ok(())
}

fn unit_json(u: &Unit) -> Value {
    json!({
        "name": u.name,
        "kind": u.kind,
        "cpu": u.cpu.name(),
        "id": u.id,
        "addr": word_hex(u.addr),
        "size": u.bytes.len(),
        "bss": u.bss,
        "compressed": u.compressed,
        "sha256": u.sha256(),
    })
}

fn units_cmd(a: &RomArg) -> Result<(), BoxError> {
    let bytes = input(&a.rom)?;
    let rom = parse(&bytes, &a.rom)?;
    let list = units::collect(&rom)?;
    if a.json {
        return print_json(&Value::Array(list.iter().map(unit_json).collect()));
    }
    let mut out = std::io::stdout().lock();
    for u in &list {
        writeln!(
            out,
            "{:<22} {:<8} {:<4} addr ${:08X} size ${:X} bss ${:X}{} sha256 {}",
            u.name,
            u.kind,
            u.cpu.name(),
            u.addr,
            u.bytes.len(),
            u.bss,
            if u.compressed { " compressed" } else { "" },
            u.sha256()
        )?;
    }
    Ok(())
}

fn files(a: &RomArg) -> Result<(), BoxError> {
    let bytes = input(&a.rom)?;
    let rom = parse(&bytes, &a.rom)?;
    let list = rom.files()?;
    if a.json {
        return print_json(&Value::Array(
            list.iter()
                .map(|f| json!({ "path": f.path, "id": f.id, "offset": f.entry.start, "size": f.entry.size() }))
                .collect(),
        ));
    }
    let mut out = std::io::stdout().lock();
    for f in &list {
        writeln!(out, "{:>10} {}", f.entry.size(), f.path)?;
    }
    Ok(())
}

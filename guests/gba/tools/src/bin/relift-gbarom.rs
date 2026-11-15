//! `relift-gbarom`: print the header of a Game Boy Advance ROM, validate
//! its logo hash, complement and size, and scan it for save library
//! strings, or list its functions.

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use relift_gba_formats::checksum::{LOGO_SHA256, hex, parse_sha256};
use relift_gba_formats::header::MAX_ROM;
use relift_gba_formats::save::{self, SaveString};
use relift_gba_formats::{Header, Rom};
use relift_gba_tools::functions::{self, FunctionsArgs};
use relift_gba_tools::{BoxError, input, print_json, run};
use serde_json::{Value, json};

/// Show a ROM's cartridge header, check its logo hash, complement and size,
/// and list the save library strings it carries and the multiboot entry
/// of an image that fits work RAM.
#[derive(Parser, Debug)]
#[command(
    name = "relift-gbarom",
    version,
    args_conflicts_with_subcommands = true
)]
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
    /// The SHA-256 of the logo field to compare against, 64 hex digits;
    /// default is the hash of the real logo.
    #[arg(long, value_name = "HEX")]
    logo_sha256: Option<String>,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Find the functions of a ROM across ARM and Thumb code, with the code
    /// copied to work RAM and the transfers that cannot be resolved.
    Functions(FunctionsArgs),
}

fn main() -> ExitCode {
    run::<Cli>("relift-gbarom", |cli| {
        let rom_path = match (cli.command, cli.rom) {
            (Some(Command::Functions(a)), _) => return functions::run(&a),
            (None, Some(p)) => p,
            (None, None) => return Err("a ROM path is required".into()),
        };
        let expected = match &cli.logo_sha256 {
            Some(s) => parse_sha256(s).ok_or("--logo-sha256 needs 64 hex digits")?,
            None => LOGO_SHA256,
        };
        let bytes = input(&rom_path)?;
        let header = Header::parse(&bytes).map_err(|e| format!("{}: {e}", rom_path.display()))?;
        let strings = save::scan(&bytes);
        let report = Report {
            header: &header,
            checks: header.checks_against(&expected),
            strings: &strings,
            eeprom_spans: Rom::new(&bytes).eeprom_spans_region(),
        };
        if cli.json {
            print_json(&report.to_json())?;
        } else {
            report.print_text()?;
        }
        if cli.check && !report.checks.all() {
            return Err("one or more checks failed".into());
        }
        Ok(())
    })
}

struct Report<'a> {
    header: &'a Header,
    checks: relift_gba_formats::Checks,
    strings: &'a [SaveString],
    eeprom_spans: bool,
}

fn word_hex(b: u32) -> String {
    format!("{b:08X}")
}

impl Report<'_> {
    fn save_json(&self) -> Value {
        let kind = save::kind_of(self.strings);
        json!({
            "strings": self.strings.iter().map(|s| json!({
                "string": format!("{}{}", s.prefix, s.version),
                "prefix": s.prefix,
                "version": s.version,
                "type": s.kind.name(),
                "offset": s.offset,
            })).collect::<Vec<_>>(),
            "type": kind.map(save::SaveKind::name),
            "sizes": kind.map(save::SaveKind::sizes),
            "ambiguous": kind.is_none() && !self.strings.is_empty(),
        })
    }

    fn to_json(&self) -> Value {
        let h = self.header;
        let k = self.checks;
        json!({
            "title": h.title,
            "game_code": h.game_code,
            "maker": h.maker,
            "version": h.version,
            "unit_code": h.unit_code,
            "device_type": h.device_type,
            "fixed": h.fixed,
            "entry": word_hex(h.entry),
            "entry_target": h.entry_target(),
            "multiboot_target": h.multiboot_target(),
            "image_bytes": h.len,
            "power_of_two": h.len.is_power_of_two(),
            "eeprom_spans_region": self.eeprom_spans,
            "complement": {
                "stored": h.complement,
                "computed": h.complement_computed,
                "ok": k.complement,
            },
            "logo_sha256": hex(&h.logo_sha256),
            "logo_ok": k.logo,
            "fixed_ok": k.fixed,
            "size_ok": k.size,
            "save": self.save_json(),
        })
    }

    fn print_text(&self) -> Result<(), BoxError> {
        let h = self.header;
        let k = self.checks;
        let ok = |b: bool| if b { "ok" } else { "MISMATCH" };
        let mut out = std::io::stdout().lock();
        writeln!(out, "title: {}", h.title)?;
        writeln!(out, "game code: {}", h.game_code)?;
        writeln!(out, "maker: {}", h.maker)?;
        writeln!(out, "version: {}", h.version)?;
        writeln!(
            out,
            "unit code: ${:02X}, device type: ${:02X}",
            h.unit_code, h.device_type
        )?;
        match h.entry_target() {
            Some(t) => writeln!(out, "entry: {} (b ${t:08X})", word_hex(h.entry))?,
            None => writeln!(out, "entry: {}", word_hex(h.entry))?,
        }
        if let (Some(w), Some(t)) = (h.multiboot_entry, h.multiboot_target()) {
            writeln!(out, "multiboot entry: {} (b ${t:08X})", word_hex(w))?;
        }
        writeln!(out, "logo: sha256 {} {}", hex(&h.logo_sha256), ok(k.logo))?;
        writeln!(out, "fixed byte: ${:02X} {}", h.fixed, ok(k.fixed))?;
        writeln!(
            out,
            "complement: ${:02X} computed ${:02X} {}",
            h.complement,
            h.complement_computed,
            ok(k.complement)
        )?;
        writeln!(
            out,
            "size: {} bytes{} {} (limit {MAX_ROM})",
            h.len,
            if h.len.is_power_of_two() {
                ""
            } else {
                ", not a power of two"
            },
            ok(k.size)
        )?;
        if self.strings.is_empty() {
            writeln!(out, "save: no library string found")?;
        }
        for s in self.strings {
            writeln!(
                out,
                "save: {}{} at ${:X} ({})",
                s.prefix,
                s.version,
                s.offset,
                s.kind.name()
            )?;
        }
        if let Some(kind) = save::kind_of(self.strings) {
            let sizes: Vec<String> = kind.sizes().iter().map(usize::to_string).collect();
            writeln!(
                out,
                "save type: {} ({} bytes)",
                kind.name(),
                sizes.join(" or ")
            )?;
            if kind == save::SaveKind::Eeprom {
                writeln!(
                    out,
                    "eeprom region: {}",
                    if self.eeprom_spans {
                        "whole 0x0D mirror"
                    } else {
                        "last 256 bytes of 0x0D only"
                    }
                )?;
            }
        } else if !self.strings.is_empty() {
            writeln!(out, "save type: ambiguous")?;
        }
        Ok(())
    }
}

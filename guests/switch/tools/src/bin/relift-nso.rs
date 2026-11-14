//! `relift-nso`: show, list the symbols of and decompress an NSO.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use relift_switch_formats::nso::Nso;
use relift_switch_formats::storage::read_all;
use relift_switch_tools::{BoxError, input, output, print_json, run};
use serde_json::json;

/// Inspect an NSO executable module.
#[derive(Parser, Debug)]
#[command(name = "relift-nso", version)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Print the module name and path, build ID, segments, bss, MOD0 and
    /// the symbol count.
    Show {
        /// The NSO, or - for stdin.
        nso: PathBuf,
        /// Print JSON.
        #[arg(long)]
        json: bool,
    },
    /// List the dynamic symbols: name, value, size, type, binding, defined.
    Symbols {
        /// The NSO, or - for stdin.
        nso: PathBuf,
        /// Print JSON.
        #[arg(long)]
        json: bool,
    },
    /// Write the NSO uncompressed (hactool layout), or flat with --flat.
    Decompress {
        /// The NSO, or - for stdin.
        nso: PathBuf,
        /// The output file, or - for stdout.
        #[arg(short, long)]
        output: PathBuf,
        /// Write the flat module image with .bss zero-filled instead.
        #[arg(long)]
        flat: bool,
    },
}

fn main() -> ExitCode {
    run::<Cli>("relift-nso", |cli| match cli.command {
        Command::Show { nso, json } => show(&open(&nso)?, json),
        Command::Symbols { nso, json } => symbols(&open(&nso)?, json),
        Command::Decompress {
            nso,
            output: out,
            flat,
        } => {
            let n = open(&nso)?;
            let bytes = if flat {
                n.flat_image()
            } else {
                n.to_uncompressed()
            };
            output(Some(&out), &bytes)
        }
    })
}

fn open(path: &Path) -> Result<Nso, BoxError> {
    Ok(Nso::parse(&read_all(&*input(path)?)?)?)
}

fn show(n: &Nso, as_json: bool) -> Result<(), BoxError> {
    let mod0 = n.mod0()?.map(|m| {
        json!({
            "offset": m.offset,
            "dynamic": m.dynamic,
            "bss_start": m.bss_start,
            "bss_end": m.bss_end,
            "eh_frame_hdr_start": m.eh_frame_hdr_start,
            "eh_frame_hdr_end": m.eh_frame_hdr_end,
            "module_object": m.module_object,
        })
    });
    let names = ["text", "rodata", "data"];
    let segments: Vec<_> = n
        .segments
        .iter()
        .zip(names)
        .map(|(s, name)| json!({ "name": name, "memory_offset": s.memory_offset, "size": s.size }))
        .collect();
    let v = json!({
        "module_name": n.module_name(),
        "module_path": n.module_path(),
        "build_id": n.build_id_hex(),
        "segments": segments,
        "bss_size": n.bss_size,
        "mod0": mod0,
        "symbol_count": n.symbols()?.len(),
    });
    if as_json {
        return print_json(&v);
    }
    let mut w = std::io::stdout().lock();
    let text = |k: &str| {
        v[k].as_str()
            .map_or_else(|| "-".to_string(), str::to_string)
    };
    writeln!(w, "module name: {}", text("module_name"))?;
    writeln!(w, "module path: {}", text("module_path"))?;
    writeln!(w, "build id: {}", text("build_id"))?;
    for s in n.segments.iter().zip(names) {
        writeln!(
            w,
            "{}: offset {:#x} size {:#x}",
            s.1, s.0.memory_offset, s.0.size
        )?;
    }
    writeln!(w, "bss: {:#x}", n.bss_size)?;
    match n.mod0()? {
        Some(m) => writeln!(w, "mod0: offset {:#x} dynamic {:#x}", m.offset, m.dynamic)?,
        None => writeln!(w, "mod0: none")?,
    }
    writeln!(w, "symbols: {}", v["symbol_count"])?;
    Ok(())
}

fn symbols(n: &Nso, as_json: bool) -> Result<(), BoxError> {
    let syms = n.symbols()?;
    let kind = |info: u8| match info & 0xf {
        0 => "notype",
        1 => "object",
        2 => "func",
        3 => "section",
        4 => "file",
        6 => "tls",
        _ => "other",
    };
    let bind = |info: u8| match info >> 4 {
        0 => "local",
        1 => "global",
        2 => "weak",
        _ => "other",
    };
    if as_json {
        let list: Vec<_> = syms
            .iter()
            .map(|s| {
                json!({
                    "name": s.name,
                    "value": s.value,
                    "size": s.size,
                    "type": kind(s.info),
                    "binding": bind(s.info),
                    "defined": s.is_defined(),
                })
            })
            .collect();
        return print_json(&json!(list));
    }
    let mut w = std::io::stdout().lock();
    for s in &syms {
        writeln!(
            w,
            "{}\t{:#x}\t{}\t{}\t{}\t{}",
            s.name,
            s.value,
            s.size,
            kind(s.info),
            bind(s.info),
            if s.is_defined() {
                "defined"
            } else {
                "undefined"
            }
        )?;
    }
    Ok(())
}

//! `relift-nro`: inspect and unpack NRO executables.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use relift_cache::write_if_changed;
use relift_switch_formats::nro::Nro;
use relift_switch_tools::{BoxError, input, print_json, run};
use serde_json::json;

/// Inspect and unpack NRO executables.
#[derive(Parser, Debug)]
#[command(name = "relift-nro", version)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Describe an NRO.
    Show {
        /// The NRO file, or - for stdin.
        input: PathBuf,
        /// Print JSON (the only format; accepted for scripts).
        #[arg(long)]
        json: bool,
    },
    /// Write the segments, the flat image and the assets to a directory.
    Extract {
        /// The NRO file, or - for stdin.
        input: PathBuf,
        /// The directory to fill.
        #[arg(short, long)]
        output: PathBuf,
    },
}

fn main() -> ExitCode {
    run::<Cli>("relift-nro", |cli| match cli.command {
        Command::Show { input: path, .. } => show(&path),
        Command::Extract {
            input: path,
            output,
        } => extract(&path, &output),
    })
}

fn show(path: &Path) -> Result<(), BoxError> {
    let n = Nro::open(input(path)?)?;
    let names = ["text", "rodata", "data"];
    let segments: Vec<_> = n
        .segments
        .iter()
        .zip(names)
        .map(|(s, name)| json!({ "name": name, "offset": s.memory_offset, "size": s.size }))
        .collect();
    let symbols: Vec<_> = n
        .symbols()?
        .iter()
        .map(|s| json!({ "name": s.name, "value": s.value, "size": s.size, "imported": s.section == 0 }))
        .collect();
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
    let asset = |a: relift_switch_formats::nro::AssetSection| json!({ "present": a.is_present(), "offset": a.offset, "size": a.size });
    let assets = n.assets.map(|a| {
        json!({
            "offset": a.header_offset,
            "version": a.version,
            "icon": asset(a.icon),
            "nacp": asset(a.nacp),
            "romfs": asset(a.romfs),
        })
    });
    print_json(&json!({
        "version": n.version,
        "size": n.size,
        "flags": n.flags,
        "segments": segments,
        "bss_size": n.bss_size,
        "build_id": n.build_id_hex(),
        "dynstr": { "offset": n.dynstr.0, "size": n.dynstr.1 },
        "dynsym": { "offset": n.dynsym.0, "size": n.dynsym.1 },
        "mod0": mod0,
        "symbols": symbols,
        "assets": assets,
    }))
}

fn extract(path: &Path, dir: &Path) -> Result<(), BoxError> {
    let n = Nro::open(input(path)?)?;
    std::fs::create_dir_all(dir)?;
    for (i, name) in ["text.bin", "rodata.bin", "data.bin"]
        .into_iter()
        .enumerate()
    {
        write_if_changed(&dir.join(name), n.segment_bytes(i)?)?;
    }
    write_if_changed(&dir.join("image.bin"), &n.flat_image())?;
    if let Some(icon) = n.icon()? {
        write_if_changed(&dir.join("icon.jpg"), &icon)?;
    }
    if let Some(nacp) = n.nacp()? {
        write_if_changed(&dir.join("control.nacp"), &nacp)?;
    }
    if let Some(r) = n.romfs()? {
        r.extract(&dir.join("romfs"))?;
    }
    Ok(())
}

//! `relift-n64rom`: print the header of a Nintendo 64 ROM with its byte
//! order, boot variant and checksum check, list its Yay0, Yaz0 and MIO0
//! streams, list the code static discovery finds, or write it back as
//! big-endian.

use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Args, Parser, Subcommand};
use relift_n64_analyze::{Boot, Report, RspOrigin, Segment, SegmentOrigin, Source, analyze_with};
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
    /// Find the code: function entries, code units by source and link
    /// address, RSP images and what stays unknown.
    Functions {
        #[command(flatten)]
        common: Common,
        /// Boot as this variant (such as 6102) instead of the detected one.
        #[arg(long, value_name = "CIC")]
        cic: Option<String>,
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

fn source_json(s: Source) -> Value {
    match s {
        Source::Rom => json!("rom"),
        Source::Stream(i) => json!({"stream": i}),
    }
}

fn source_text(s: Source) -> String {
    match s {
        Source::Rom => "rom".to_owned(),
        Source::Stream(i) => format!("stream{i}"),
    }
}

fn segment_json(s: &Segment) -> Value {
    let origin = match s.origin {
        SegmentOrigin::Boot => json!("boot"),
        SegmentOrigin::Elf => json!("elf"),
        SegmentOrigin::PiDma { site } => json!({"pi_dma": hex8(site)}),
        SegmentOrigin::Region { votes } => json!({"region": votes}),
        SegmentOrigin::Vector { from } => json!({"vector": hex8(from)}),
        SegmentOrigin::Table { at } => json!({"table": at}),
    };
    json!({
        "source": source_json(s.source),
        "offset": s.offset,
        "len": s.len,
        "link": s.link.map(hex8),
        "origin": origin,
    })
}

fn boot_json(b: Boot) -> Value {
    match b {
        Boot::Standard(c) => json!({"variant": c.name()}),
        Boot::Elf(off) => json!({"elf": off}),
        Boot::Unknown => json!("unknown"),
    }
}

fn report_json(r: &Report) -> Value {
    json!({
        "boot": boot_json(r.boot),
        "entry": hex8(r.entry),
        "streams": r.streams.iter().map(|s| json!({
            "offset": s.offset, "format": s.kind.name(), "decoded_size": s.decoded_size,
        })).collect::<Vec<_>>(),
        "segments": r.segments.iter().map(segment_json).collect::<Vec<_>>(),
        "units": r.units.iter().map(|u| json!({
            "segment": u.segment,
            "source": source_json(u.source),
            "offset": u.offset,
            "len": u.len,
            "link": u.link.map(hex8),
            "hash": format!("{:016x}", u.hash),
            "reloc": u.reloc,
            "swept": u.swept,
        })).collect::<Vec<_>>(),
        "functions": r.functions.iter().map(|f| json!({
            "segment": f.segment, "addr": hex8(f.addr), "origin": f.origin.name(),
        })).collect::<Vec<_>>(),
        "unresolved": r.unresolved.iter().map(|u| json!({
            "segment": u.segment, "site": hex8(u.site), "kind": u.kind.name(), "detail": u.detail,
        })).collect::<Vec<_>>(),
        "pi_dma": r.pi_dma.iter().map(|d| json!({
            "segment": d.segment, "site": hex8(d.site), "dram": hex8(d.dram),
            "cart": hex8(d.cart), "len": d.len,
        })).collect::<Vec<_>>(),
        "rsp": r.rsp.iter().map(|i| json!({
            "source": source_json(i.source),
            "offset": i.offset,
            "len": i.len,
            "imem": i.imem,
            "hash": format!("{:016x}", i.hash),
            "origin": match i.origin {
                RspOrigin::SpDma { site } => json!({"sp_dma": hex8(site)}),
                RspOrigin::Scan => json!("scan"),
            },
        })).collect::<Vec<_>>(),
        "stats": {
            "run_words": r.stats.run_words,
            "swept_words": r.stats.swept_words,
            "regions": r.stats.regions,
            "linked_regions": r.stats.linked_regions,
            "runs": r.stats.runs,
            "rounds": r.stats.rounds,
            "rejected": r.stats.rejected,
        },
    })
}

fn functions(common: &Common, cic_arg: Option<&str>) -> Result<(), BoxError> {
    let bytes = input(&common.rom)?;
    let cic = match cic_arg {
        Some(n) => Some(Cic::parse(n).ok_or_else(|| format!("--cic: unknown variant {n}"))?),
        None => None,
    };
    let r = analyze_with(&bytes, relift_n64_analyze::Options { cic })
        .map_err(|e| format!("{}: {e}", common.rom.display()))?;
    if common.json {
        return print_json(&report_json(&r));
    }
    let mut out = std::io::stdout().lock();
    let boot = match r.boot {
        Boot::Standard(c) => format!("boot variant {}", c.name()),
        Boot::Elf(off) => format!("ELF image at {off:#x}"),
        Boot::Unknown => "unknown boot variant".to_owned(),
    };
    writeln!(out, "entry: {} ({boot})", hex8(r.entry))?;
    let s = r.stats;
    writeln!(
        out,
        "code: {} words by runs, {} by the sweep, in {} units of {} segments",
        s.run_words,
        s.swept_words,
        r.units.len(),
        r.segments.len()
    )?;
    writeln!(
        out,
        "regions: {} ({} linked), streams: {}, rounds: {}, runs: {}, rejected roots: {}",
        s.regions,
        s.linked_regions,
        r.streams.len(),
        s.rounds,
        s.runs,
        s.rejected
    )?;
    for d in &r.pi_dma {
        writeln!(
            out,
            "pi dma at {}: cart {} to {} ({} bytes)",
            hex8(d.site),
            hex8(d.cart),
            hex8(d.dram),
            d.len
        )?;
    }
    for i in &r.rsp {
        let how = match i.origin {
            RspOrigin::SpDma { site } => format!("sp dma at {}", hex8(site)),
            RspOrigin::Scan => "scan".to_owned(),
        };
        writeln!(
            out,
            "rsp: {} {:08X} {} bytes, hash {:016x} ({how})",
            source_text(i.source),
            i.offset,
            i.len,
            i.hash
        )?;
    }
    for f in &r.functions {
        let seg = &r.segments[f.segment as usize];
        let at = seg.source_offset(f.addr).unwrap_or(seg.offset);
        writeln!(
            out,
            "{} {} {at:08X} {}",
            hex8(f.addr),
            source_text(seg.source),
            f.origin.name()
        )?;
    }
    writeln!(out, "unresolved: {}", r.unresolved.len())?;
    for u in &r.unresolved {
        writeln!(out, "  {} {} {}", hex8(u.site), u.kind.name(), u.detail)?;
    }
    Ok(())
}

fn main() -> ExitCode {
    run::<Cli>("relift-n64rom", |cli| match cli.command {
        Command::Info { common, cic, check } => info(&common, cic.as_deref(), check),
        Command::Streams { common } => streams(&common),
        Command::Functions { common, cic } => functions(&common, cic.as_deref()),
        Command::Normalize { rom, out } => {
            let rom = open(&rom)?;
            output(&out, &rom.bytes)
        }
    })
}

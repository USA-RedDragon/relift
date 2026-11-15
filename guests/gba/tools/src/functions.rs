//! `relift-gbarom functions`: the functions of a ROM across ARM and Thumb
//! code, per region.

use std::io::Write;
use std::path::PathBuf;

use clap::Args;
use relift_gba_analyze::{
    CopyKind, EdgeKind, Function, Loc, Origin, Region, Report, analyze, analyze_multiboot,
};
use serde_json::{Value, json};

use crate::{BoxError, input, print_json};

/// Arguments of `relift-gbarom functions`.
#[derive(Args, Debug)]
pub struct FunctionsArgs {
    /// The ROM image, or - for stdin.
    pub rom: PathBuf,
    /// Print JSON.
    #[arg(long)]
    pub json: bool,
    /// Leave out the list of functions.
    #[arg(long)]
    pub summary: bool,
    /// With --json, list every instruction of each function.
    #[arg(long)]
    pub instructions: bool,
    /// Exit with code 1 when a transfer is unresolved.
    #[arg(long)]
    pub strict: bool,
    /// Run the image as a multiboot program: loaded to work RAM at
    /// 0x02000000 and entered at its entry word at 0xC0, with no cartridge.
    #[arg(long)]
    pub multiboot: bool,
}

/// Run the subcommand.
///
/// # Errors
///
/// The file cannot be read or analyzed, or `--strict` found an unresolved
/// transfer.
pub fn run(args: &FunctionsArgs) -> Result<(), BoxError> {
    let rom = input(&args.rom)?;
    let report = if args.multiboot {
        analyze_multiboot(&rom)
    } else {
        analyze(&rom)
    }
    .map_err(|e| format!("{}: {e}", args.rom.display()))?;
    if args.json {
        print_json(&to_json(&report, args.summary, args.instructions))?;
    } else {
        print_text(&report, args.summary)?;
    }
    if args.strict && !report.unresolved.is_empty() {
        return Err(format!("{} unresolved transfers", report.unresolved.len()).into());
    }
    Ok(())
}

fn origin_name(o: Origin) -> &'static str {
    match o {
        Origin::Entry => "entry",
        Origin::Irq => "irq",
        Origin::Called => "called",
        Origin::Jumped => "jumped",
        Origin::AddressTaken => "address-taken",
    }
}

fn edge_name(k: EdgeKind) -> &'static str {
    match k {
        EdgeKind::Call => "call",
        EdgeKind::LinkedJump => "linked-jump",
        EdgeKind::Via => "via",
        EdgeKind::Pointer => "pointer",
        EdgeKind::Tail => "tail",
    }
}

fn copy_name(k: CopyKind) -> String {
    match k {
        CopyKind::CpuSet => "cpuset".to_owned(),
        CopyKind::CpuFastSet => "cpufastset".to_owned(),
        CopyKind::Dma(n) => format!("dma{n}"),
        CopyKind::Run => "run".to_owned(),
        CopyKind::Load => "load".to_owned(),
    }
}

fn loc_json(l: Loc) -> Value {
    json!({
        "addr": l.addr,
        "state": if l.thumb { "thumb" } else { "arm" },
        "copy": l.copy,
        "text": l.to_string(),
    })
}

fn function_json(f: &Function, instructions: bool) -> Value {
    let bytes: usize = f.instructions.values().map(|n| usize::from(*n)).sum();
    let mut v = json!({
        "entry": loc_json(f.entry),
        "origin": origin_name(f.origin),
        "instruction_count": f.instructions.len(),
        "bytes": bytes,
        "returns": f.returns,
        "edges": f.edges.iter().map(|e| json!({
            "site": loc_json(e.site), "target": loc_json(e.target), "kind": edge_name(e.kind),
        })).collect::<Vec<_>>(),
        "tables": f.tables.iter().map(|t| json!({
            "site": loc_json(t.site), "check": loc_json(t.check), "max": t.max,
            "targets": t.targets.iter().map(|l| loc_json(*l)).collect::<Vec<_>>(),
        })).collect::<Vec<_>>(),
    });
    if instructions {
        v["instructions"] = f
            .instructions
            .iter()
            .map(|(l, n)| json!([l.addr, if l.thumb { "T" } else { "A" }, l.copy, n]))
            .collect();
    }
    v
}

fn to_json(report: &Report, summary: bool, instructions: bool) -> Value {
    let regions: Vec<Value> = report
        .coverage()
        .iter()
        .map(|(r, c)| json!({"region": r.name(), "arm": c.arm, "thumb": c.thumb, "bytes": c.bytes}))
        .collect();
    let kinds: serde_json::Map<String, Value> = report
        .unresolved_by_kind()
        .iter()
        .map(|(k, n)| (k.name().to_owned(), json!(n)))
        .collect();
    let functions: Vec<Value> = if summary {
        Vec::new()
    } else {
        report
            .functions
            .values()
            .map(|f| function_json(f, instructions))
            .collect()
    };
    json!({
        "function_count": report.functions.len(),
        "regions": regions,
        "irq_handlers": report.irq_handlers.iter().map(|l| loc_json(*l)).collect::<Vec<_>>(),
        "copies": report.copies.iter().enumerate().map(|(i, c)| json!({
            "index": i, "site": loc_json(c.site), "kind": copy_name(c.kind),
            "src": c.src, "dst": c.dst, "len": c.len(),
        })).collect::<Vec<_>>(),
        "swis": report.swis.iter().map(|s| json!({"site": loc_json(s.site), "number": s.number})).collect::<Vec<_>>(),
        "data": report.data.iter().map(|(a, n)| json!([a, n])).collect::<Vec<_>>(),
        "conflicts": report.conflicts.iter().map(|l| loc_json(*l)).collect::<Vec<_>>(),
        "unresolved_by_kind": kinds,
        "unresolved": report.unresolved.iter().map(|u| json!({
            "at": loc_json(u.at), "kind": u.kind.name(), "detail": u.detail,
        })).collect::<Vec<_>>(),
        "functions": functions,
    })
}

fn print_text(report: &Report, summary: bool) -> Result<(), BoxError> {
    let mut out = std::io::stdout().lock();
    writeln!(out, "{} functions", report.functions.len())?;
    for (r, c) in report.coverage() {
        writeln!(
            out,
            "{}: {} ARM and {} Thumb instructions, {} bytes",
            r.name(),
            c.arm,
            c.thumb,
            c.bytes
        )?;
    }
    for (i, c) in report.copies.iter().enumerate() {
        let src = c.src.map_or(String::new(), |s| format!(" from ${s:08X}"));
        writeln!(
            out,
            "copy {i}: {} at {}{src} to ${:08X}, {} bytes",
            copy_name(c.kind),
            c.site,
            c.dst,
            c.len()
        )?;
    }
    for l in &report.irq_handlers {
        writeln!(out, "irq handler: {l}")?;
    }
    if !summary {
        for f in report.functions.values() {
            let region = f.entry.region();
            let tag = if region == Region::Rom {
                String::new()
            } else {
                format!(" ({})", region.name())
            };
            writeln!(
                out,
                "  {} {}{tag}: {} instructions{}",
                f.entry,
                origin_name(f.origin),
                f.instructions.len(),
                if f.returns { "" } else { ", no return" }
            )?;
        }
    }
    let kinds = report.unresolved_by_kind();
    if !kinds.is_empty() {
        let parts: Vec<String> = kinds
            .iter()
            .map(|(k, n)| format!("{} {n}", k.name()))
            .collect();
        writeln!(out, "unresolved: {}", parts.join(", "))?;
        for u in &report.unresolved {
            writeln!(out, "  {} {}: {}", u.at, u.kind.name(), u.detail)?;
        }
    }
    Ok(())
}

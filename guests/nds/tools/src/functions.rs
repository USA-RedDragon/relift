//! `relift-ndsrom functions`: the functions of every unit of a ROM, one
//! discovery run per unit.

use std::io::Write;
use std::path::{Path, PathBuf};

use clap::Args;
use relift_nds_analyze::{CopyKind, EdgeKind, Function, Loc, Origin, Program, Report, UnitReport};
use relift_nds_formats::Rom;
use relift_nds_formats::checksum::hex;
use serde_json::{Value, json};

use crate::{BoxError, input, print_json};

/// Arguments of `relift-ndsrom functions`.
#[derive(Args, Debug)]
pub struct FunctionsArgs {
    /// The ROM image, or - for stdin.
    pub rom: PathBuf,
    /// Print JSON.
    #[arg(long)]
    pub json: bool,
    /// Also write each unit's full report, every instruction included,
    /// to `DIR/TAG-UNIT.out.json` (UNIT: `static`, `arm7`, `ov<id>`).
    #[arg(long, value_name = "DIR")]
    pub out: Option<PathBuf>,
    /// The file name prefix for `--out`.
    #[arg(long, default_value = "functions")]
    pub tag: String,
    /// Runs at a time.
    #[arg(long, default_value_t = 4)]
    pub jobs: usize,
}

/// Run the subcommand.
///
/// # Errors
///
/// The file cannot be read or parsed, a unit does not decode, or a
/// report cannot be written.
pub fn run(args: &FunctionsArgs) -> Result<(), BoxError> {
    let bytes = input(&args.rom)?;
    let rom = Rom::parse(&bytes).map_err(|e| format!("{}: {e}", args.rom.display()))?;
    let program = Program::read(&rom).map_err(|e| format!("{}: {e}", args.rom.display()))?;
    if let Some(dir) = &args.out {
        std::fs::create_dir_all(dir)?;
    }
    let progress = |r: &UnitReport| {
        eprintln!(
            "{} run {}: {} functions, {} instructions, {:.1} s",
            r.id.name(),
            r.runs,
            r.report.functions.len(),
            r.report.instructions().len(),
            r.elapsed.as_secs_f64()
        );
    };
    let reports = program.analyze(args.jobs, &progress);
    if let Some(dir) = &args.out {
        for r in &reports {
            write_unit(dir, &args.tag, r)?;
        }
    }
    if args.json {
        print_json(&json!({
            "units": reports.iter().map(summary_json).collect::<Vec<_>>(),
        }))?;
    } else {
        let mut out = std::io::stdout().lock();
        for r in &reports {
            writeln!(
                out,
                "{} {:08X}-{:08X}: {} functions, {} instructions, {} runs, {} seeds",
                r.id.name(),
                r.range.0,
                r.range.1,
                r.report.functions.len(),
                r.report.instructions().len(),
                r.runs,
                r.seeds.len()
            )?;
        }
    }
    Ok(())
}

fn write_unit(dir: &Path, tag: &str, r: &UnitReport) -> Result<(), BoxError> {
    let mut v = report_json(&r.report);
    if let (Value::Object(m), Value::Object(s)) = (&mut v, summary_json(r)) {
        m.extend(s);
    }
    let path = dir.join(format!("{tag}-{}.out.json", r.id.name()));
    relift_cache::write_if_changed(&path, &serde_json::to_vec(&v)?)
        .map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(())
}

fn summary_json(r: &UnitReport) -> Value {
    let kinds: serde_json::Map<String, Value> = r
        .report
        .unresolved_by_kind()
        .iter()
        .map(|(k, n)| (k.name().to_owned(), json!(n)))
        .collect();
    json!({
        "unit": r.id.name(),
        "cpu": r.id.cpu().name(),
        "base": r.range.0,
        "end": r.range.1,
        "sha256": r.sha256.map(|h| hex(&h)),
        "runs": r.runs,
        "seeds": r.seeds.len(),
        "elapsed_ms": r.elapsed.as_millis(),
        "function_count": r.report.functions.len(),
        "instruction_count": r.report.instructions().len(),
        "unresolved_by_kind": kinds,
    })
}

fn origin_name(o: Origin) -> &'static str {
    match o {
        Origin::Entry => "entry",
        Origin::Irq => "irq",
        Origin::Called => "called",
        Origin::Jumped => "jumped",
        Origin::AddressTaken => "address-taken",
        Origin::Rewritten => "rewritten",
        Origin::Hint => "hint",
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
        CopyKind::Rewrite(c) => format!("rewrite{c}"),
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

fn function_json(f: &Function) -> Value {
    let bytes: usize = f.instructions.values().map(|n| usize::from(*n)).sum();
    json!({
        "entry": loc_json(f.entry),
        "origin": origin_name(f.origin),
        "instruction_count": f.instructions.len(),
        "bytes": bytes,
        "returns": f.returns,
        "edges": f.edges.iter().map(|e| json!({
            "site": loc_json(e.site), "target": loc_json(e.target), "kind": edge_name(e.kind),
        })).collect::<Vec<_>>(),
        "instructions": f.instructions.iter()
            .map(|(l, n)| json!([l.addr, if l.thumb { "T" } else { "A" }, l.copy, n]))
            .collect::<Vec<_>>(),
    })
}

fn report_json(report: &Report) -> Value {
    json!({
        "irq_handlers": report.irq_handlers.iter().map(|l| loc_json(*l)).collect::<Vec<_>>(),
        "copies": report.copies.iter().enumerate().map(|(i, c)| json!({
            "index": i, "site": loc_json(c.site), "kind": copy_name(c.kind),
            "src": c.src, "dst": c.dst, "len": c.len(),
        })).collect::<Vec<_>>(),
        "swis": report.swis.iter().map(|s| json!({"site": loc_json(s.site), "number": s.number})).collect::<Vec<_>>(),
        "data": report.data.iter().map(|(a, n)| json!([a, n])).collect::<Vec<_>>(),
        "conflicts": report.conflicts.iter().map(|l| loc_json(*l)).collect::<Vec<_>>(),
        "unresolved": report.unresolved.iter().map(|u| json!({
            "at": loc_json(u.at), "kind": u.kind.name(), "detail": u.detail,
        })).collect::<Vec<_>>(),
        "outside": report.outside.iter().map(|l| loc_json(*l)).collect::<Vec<_>>(),
        "functions": report.functions.values().map(function_json).collect::<Vec<_>>(),
    })
}

//! `relift-gbrom functions`: the functions of a ROM, per bank.

use std::collections::BTreeMap;
use std::io::Write;
use std::path::PathBuf;

use clap::Args;
use relift_gb_analyze::{
    CallKind, Function, Loc, MAX_KEYS, Origin, RamClass, Report, TableBound, analyze_with,
};
use serde_json::{Value, json};

use crate::{BoxError, input, print_json};

/// Arguments of `relift-gbrom functions`.
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
    /// With `--json`, list each function's instructions as
    /// `[bank, address, length]`.
    #[arg(long)]
    pub instructions: bool,
    /// Exit with code 1 when a transfer is unresolved.
    #[arg(long)]
    pub strict: bool,
    /// A learned file of a built program (`BBB:AAAA` lines, bank and
    /// address in hex): its ROM locations are roots too.
    #[arg(long)]
    pub seen: Option<PathBuf>,
}

/// The `BBB:AAAA` lines of a learned file.
fn seen_locations(text: &str) -> Vec<Loc> {
    text.lines()
        .filter_map(|l| {
            let (b, a) = l.trim().split_once(':')?;
            Some(Loc {
                bank: u16::from_str_radix(b, 16).ok()?,
                addr: u16::from_str_radix(a, 16).ok()?,
            })
        })
        .collect()
}

/// Run the subcommand.
///
/// # Errors
///
/// The file cannot be read or analyzed, or `--strict` found an unresolved
/// transfer.
pub fn run(args: &FunctionsArgs) -> Result<(), BoxError> {
    let rom = input(&args.rom)?;
    let seen = match &args.seen {
        Some(p) => seen_locations(&std::fs::read_to_string(p)?),
        None => Vec::new(),
    };
    let report = analyze_with(&rom, &seen).map_err(|e| format!("{}: {e}", args.rom.display()))?;
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

fn bound_name(b: TableBound) -> &'static str {
    match b {
        TableBound::Index => "index",
        TableBound::Run => "run",
    }
}

fn origin_name(o: Origin) -> &'static str {
    match o {
        Origin::Root => "root",
        Origin::Seen => "seen",
        Origin::Call(CallKind::Call) => "call",
        Origin::Call(CallKind::Rst) => "rst",
        Origin::Call(CallKind::Trampoline) => "trampoline",
        Origin::Call(CallKind::Indirect) => "indirect",
    }
}

fn tramp_text(f: &Function) -> Option<String> {
    let t = f.trampoline?;
    let bank = t.bank_reg.map_or(String::new(), |r| format!("bank in {r}"));
    let target = t
        .target_regs
        .map_or(String::new(), |(h, l)| format!("target in {h}{l}"));
    Some([bank, target].join(" ").trim().to_owned())
}

fn by_bank(report: &Report) -> BTreeMap<u16, Vec<&Function>> {
    let mut m: BTreeMap<u16, Vec<&Function>> = BTreeMap::new();
    for f in report.functions.values() {
        m.entry(f.entry.bank).or_default().push(f);
    }
    m
}

fn loc_json(l: Loc) -> Value {
    json!({"bank": l.bank, "addr": l.addr, "text": l.to_string()})
}

fn to_json(report: &Report, summary: bool, instructions: bool) -> Value {
    let banks: Vec<Value> = report
        .coverage
        .iter()
        .map(|(b, c)| json!({"bank": b, "functions": c.functions, "bytes": c.bytes}))
        .collect();
    let mut kinds: BTreeMap<&str, usize> = BTreeMap::new();
    for u in &report.unresolved {
        *kinds.entry(u.kind.name()).or_default() += 1;
    }
    let functions: Vec<Value> = if summary {
        Vec::new()
    } else {
        report
            .functions
            .values()
            .map(|f| {
                let mut v = json!({
                    "entry": loc_json(f.entry),
                    "origin": origin_name(f.origin),
                    "bytes": f.bytes,
                    "switches_bank": f.switches_bank,
                    "trampoline": tramp_text(f),
                });
                if instructions {
                    v["instructions"] = f
                        .body
                        .iter()
                        .map(|(l, n)| json!([l.bank, l.addr, n]))
                        .collect();
                }
                v
            })
            .collect()
    };
    json!({
        "mapper": report.mapper,
        "rom_banks": report.banks,
        "function_count": report.functions.len(),
        "banks": banks,
        "functions": functions,
        "unresolved_by_kind": kinds,
        "unreached_pointers": report.unreached_pointers.iter().map(|t| json!({
            "site": loc_json(t.site), "value": t.value, "target": t.target.map(loc_json), "stored_at": t.stored_at,
        })).collect::<Vec<_>>(),
        "tables": report.tables.iter().map(|t| json!({
            "site": loc_json(t.site), "table": loc_json(t.table), "step": t.step,
            "entries": t.entries, "bound": bound_name(t.bound),
        })).collect::<Vec<_>>(),
        "ram_pointers": report.ram_pointers.iter().map(|p| json!({
            "site": loc_json(p.site), "cell": p.cell, "targets": p.targets, "partial": p.partial,
        })).collect::<Vec<_>>(),
        "dynamic_bank_writes": report.dynamic_writes.iter().map(|l| loc_json(*l)).collect::<Vec<_>>(),
        "lift_banks": report.lift_banks,
        "unresolved": report.unresolved.iter().map(|u| json!({
            "at": loc_json(u.at), "kind": u.kind.name(), "detail": u.detail,
        })).collect::<Vec<_>>(),
        "ram_code": report.ram_code.iter().map(|r| json!({
            "function": loc_json(r.function),
            "site": loc_json(r.transfer.site),
            "target": r.transfer.target,
            "copied_by": r.copied_by.iter().map(|(f, c)| json!({
                "function": loc_json(*f),
                "loop": loc_json(c.at),
                "sources": c.sources,
                "dests": c.dests,
                "counts": c.counts,
            })).collect::<Vec<_>>(),
        })).collect::<Vec<_>>(),
        "ram_ops": {
            "entries": report.ram_ops.entries,
            "wide": report.ram_ops.wide,
            "max_keys": MAX_KEYS,
            "ops": report.ram_ops.ops.iter().map(|o| json!({
                "addr": o.addr, "key": o.key, "class": o.class.name(),
            })).collect::<Vec<_>>(),
        },
    })
}

fn print_text(report: &Report, summary: bool) -> Result<(), BoxError> {
    let mut out = std::io::stdout().lock();
    writeln!(
        out,
        "mapper {}, {} banks, {} functions",
        report.mapper,
        report.banks,
        report.functions.len()
    )?;
    let groups = by_bank(report);
    for (bank, c) in &report.coverage {
        if c.functions == 0 && c.bytes == 0 {
            continue;
        }
        writeln!(
            out,
            "bank {bank:02X}: {} functions, {} of 16384 bytes",
            c.functions, c.bytes
        )?;
        if summary {
            continue;
        }
        for f in groups.get(bank).into_iter().flatten() {
            let extra = tramp_text(f).map_or(String::new(), |t| format!(" [{t}]"));
            let sw = if f.switches_bank {
                " switches-bank"
            } else {
                ""
            };
            writeln!(
                out,
                "  {} {:5} bytes {}{sw}{extra}",
                f.entry,
                f.bytes,
                origin_name(f.origin)
            )?;
        }
    }
    let mut kinds: BTreeMap<&str, usize> = BTreeMap::new();
    for u in &report.unresolved {
        *kinds.entry(u.kind.name()).or_default() += 1;
    }
    writeln!(
        out,
        "banks to lift: {}",
        report
            .lift_banks
            .iter()
            .map(|b| format!("{b:02X}"))
            .collect::<Vec<_>>()
            .join(" ")
    )?;
    writeln!(out, "unresolved transfers: {}", report.unresolved.len())?;
    for (k, n) in &kinds {
        writeln!(out, "  {k}: {n}")?;
    }
    if !summary {
        for u in &report.unresolved {
            writeln!(out, "  {} {} {}", u.at, u.kind.name(), u.detail)?;
        }
    }
    writeln!(
        out,
        "bank register writes of an unknown value: {}",
        report.dynamic_writes.len()
    )?;
    if !summary {
        for l in &report.dynamic_writes {
            writeln!(out, "  {l}")?;
        }
    }
    writeln!(
        out,
        "constants that point into ROM outside the code found: {}",
        report.unreached_pointers.len()
    )?;
    if !summary {
        for t in &report.unreached_pointers {
            let to = t
                .target
                .map_or_else(|| "bank unknown".to_owned(), |l| l.to_string());
            let how = t
                .stored_at
                .map_or_else(String::new, |a| format!(", stored at ${a:04X}"));
            writeln!(out, "  {} -> ${:04X} ({to}{how})", t.site, t.value)?;
        }
    }
    writeln!(out, "jump tables followed: {}", report.tables.len())?;
    if !summary {
        for t in &report.tables {
            writeln!(
                out,
                "  {} table {} step {}, {} entries, bounded by {}",
                t.site,
                t.table,
                t.step,
                t.entries,
                bound_name(t.bound)
            )?;
        }
    }
    writeln!(
        out,
        "jumps through pointers in RAM followed: {}",
        report.ram_pointers.len()
    )?;
    if !summary {
        for p in &report.ram_pointers {
            let partial = if p.partial { ", partial" } else { "" };
            writeln!(
                out,
                "  {} through ${:04X}: {} targets{partial}",
                p.site, p.cell, p.targets
            )?;
        }
    }
    writeln!(out, "code run from RAM: {}", report.ram_code.len())?;
    for r in &report.ram_code {
        writeln!(
            out,
            "  {} in {} -> ${:04X}",
            r.transfer.site, r.function, r.transfer.target
        )?;
        for (f, c) in &r.copied_by {
            writeln!(
                out,
                "    possible copy loop {} in {f}: from {} to {}",
                c.at,
                addrs(&c.sources),
                addrs(&c.dests)
            )?;
        }
    }
    let ops = &report.ram_ops;
    let count = |c: RamClass| ops.ops.iter().filter(|o| o.class == c).count();
    writeln!(
        out,
        "instructions predicted in RAM: {} ({} copied, {} generated) from {} entries, {} addresses over {MAX_KEYS} keys",
        ops.ops.len(),
        count(RamClass::Copied),
        count(RamClass::Generated),
        ops.entries.len(),
        ops.wide.len()
    )?;
    if !summary {
        for o in &ops.ops {
            writeln!(out, "  ${:04X} {:03x} {}", o.addr, o.key, o.class.name())?;
        }
    }
    Ok(())
}

fn addrs(v: &[u16]) -> String {
    v.iter()
        .map(|a| format!("${a:04X}"))
        .collect::<Vec<_>>()
        .join(",")
}

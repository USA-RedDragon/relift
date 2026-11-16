//! `relift-check --oracle sst-z80|zex|ares-z80 ...`

use std::path::PathBuf;
use std::process::ExitCode;

use super::ares;
use super::mutate::Mutation;
use super::{sst, zex};

pub const USAGE: &str = "\
usage: relift-check --oracle sst-z80 run DIR [PREFIX] [-j N] [--mutate [flip-const|drop-set]]
       relift-check --oracle zex FILE.com
       relift-check --oracle ares-z80 rand N SEED [--irq] > IN.jsonl
       relift-check --oracle ares-z80 compare IN.jsonl OUT.jsonl
       relift-check --oracle ares-z80 run [N [SEED]] [--bin FILE]
DIR holds the SingleStepTests/z80 v1 files. ares-z80 run feeds random states to the ares
oracle (--bin, RELIFT_ARES_Z80 or ~/.cache/relift/ares-oracle/bin/z80-step, built by
check/oracle/ares/build-step.sh), instructions and then interrupt entries (N is states per
template, 10 N per entry; default 100 and seed 31). --mutate breaks every lifted block (the constant a register is last set to, or the
last register write) and succeeds when at least 95% of the files catch it.";

fn sst_run(args: &[String]) -> Result<bool, String> {
    let mut positional = Vec::new();
    let mut jobs = std::thread::available_parallelism().map_or(1, usize::from);
    let mut mutation = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "-j" => {
                jobs = args
                    .get(i + 1)
                    .and_then(|v| v.parse().ok())
                    .ok_or("-j needs a number")?;
                i += 1;
            }
            "--mutate" => {
                mutation = Some(match args.get(i + 1).and_then(|v| Mutation::parse(v)) {
                    Some(m) => {
                        i += 1;
                        m
                    }
                    None => Mutation::FlipConst,
                });
            }
            a if a.starts_with('-') => return Err(format!("unknown option {a}")),
            a => positional.push(a),
        }
        i += 1;
    }
    let Some(dir) = positional.first() else {
        return Err("run needs DIR".into());
    };
    sst::run(dir, positional.get(1).copied(), jobs, mutation)
}

fn ares_run(args: &[String]) -> Result<bool, String> {
    let mut nums = Vec::new();
    let mut bin = ares::binary("RELIFT_ARES_Z80", "z80-step");
    let mut i = 0;
    while i < args.len() {
        if args[i] == "--bin" {
            bin = PathBuf::from(args.get(i + 1).ok_or("--bin needs a file")?);
            i += 1;
        } else {
            nums.push(
                args[i]
                    .parse::<u64>()
                    .map_err(|_| format!("bad number {}", args[i]))?,
            );
        }
        i += 1;
    }
    if !bin.exists() {
        return Err(format!(
            "{} is missing; build it with check/oracle/ares/build-step.sh",
            bin.display()
        ));
    }
    let n = nums.first().copied().unwrap_or(100) as usize;
    ares::check(n, nums.get(1).copied().unwrap_or(31), &bin)
}

fn run(oracle: &str, args: &[String]) -> Result<bool, String> {
    match (oracle, args.split_first()) {
        ("sst-z80", Some((sub, rest))) if sub == "run" => sst_run(rest),
        ("zex", Some((file, []))) => zex::run(file),
        ("ares-z80", Some((sub, rest))) => match (sub.as_str(), rest) {
            ("rand", [n, seed, rest @ ..]) => {
                let n = n.parse().map_err(|_| "N is not a number")?;
                let seed = seed.parse().map_err(|_| "SEED is not a number")?;
                let lines = match rest {
                    [] => ares::states(n, seed),
                    [f] if f == "--irq" => ares::irq_states(n, seed),
                    _ => return Err("rand takes N SEED [--irq]".into()),
                };
                println!("{}", lines.join("\n"));
                Ok(true)
            }
            ("compare", [input, output]) => {
                let ins = std::fs::read_to_string(input).map_err(|e| format!("{input}: {e}"))?;
                let outs = std::fs::read_to_string(output).map_err(|e| format!("{output}: {e}"))?;
                let rep = ares::compare(&ins, &outs);
                rep.print(&format!("{input} against {output}"));
                Ok(rep.unexplained == 0)
            }
            ("run", rest) => ares_run(rest),
            _ => Err(format!("unknown command {sub}")),
        },
        _ => Err("bad arguments".into()),
    }
}

/// Handles `--oracle sst-z80|zex|ares-z80 ...`; `None` when the arguments
/// are for another oracle.
pub fn dispatch(args: &[String]) -> Option<ExitCode> {
    let i = args.iter().position(|a| a == "--oracle")?;
    let oracle = args.get(i + 1).map(String::as_str)?;
    if !matches!(oracle, "sst-z80" | "zex" | "ares-z80") {
        return None;
    }
    let rest: Vec<String> = args
        .iter()
        .enumerate()
        .filter(|(j, _)| *j != i && *j != i + 1)
        .map(|(_, a)| a.clone())
        .collect();
    Some(match run(oracle, &rest) {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("{e}\n{USAGE}");
            ExitCode::from(2)
        }
    })
}

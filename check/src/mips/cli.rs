//! `relift-check --oracle ares ...`: the VR4300 lifter against ares.

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::process::ExitCode;

use relift_cpu_mips::decode;
use relift_cpu_mips::lift::has_template;

use super::cases::{self, Case, Rng};
use super::compare::{SLOW, compare};
use super::cslow;
use super::fprule::slow_check;
use super::oracle;
use super::run::{self, Out};
use crate::mutate;

pub const USAGE: &str = "\
usage: relift-check --oracle ares lift [-n N] [--seed S] [--mutate] [--fp-corpus [--fp-modes]] [--bin FILE]
                    [--rom FILE] [--work DIR] [--c-slow-path FILE] [KIND...]
Runs each kind of VR4300 instruction (see `lift --list`) on N random machine states, or with
--fp-corpus the FP special-value corpus, lifted by relift-cpu-mips on the IR interpreter and on
the ares oracle (n64-oracle, built by check/oracle/ares/build.sh; --bin, RELIFT_ARES_ORACLE or
~/.cache/relift/n64-ares/bin/n64-oracle). --mutate breaks the first live operation of each
lifted block and expects the comparison to catch it. --rom runs the oracle's boot code from
a ROM first; without it the oracle starts at once. --work keeps the oracle's input and output.
--fp-modes runs the corpus under the directed rounding modes and the enable bits instead.
--c-slow-path also runs every COP1 case through the runtime's C FPU slow path (FILE is
mips_fpu_cases from the runtime Makefile) and compares it with ares on all of them.
       relift-check --oracle ares lift --list    the kinds, and the encodings without one";

struct Opts {
    n: usize,
    seed: u64,
    mutate: bool,
    corpus: bool,
    modes: bool,
    list: bool,
    bin: Option<PathBuf>,
    rom: Option<PathBuf>,
    work: Option<PathBuf>,
    c_slow: Option<PathBuf>,
    kinds: Vec<String>,
}

fn parse(args: &[String]) -> Result<Opts, String> {
    let mut o = Opts {
        n: 25,
        seed: 0,
        mutate: false,
        corpus: false,
        modes: false,
        list: false,
        bin: None,
        rom: None,
        work: None,
        c_slow: None,
        kinds: Vec::new(),
    };
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        let flag = a.starts_with('-');
        let v = || args.get(i + 1).ok_or(format!("{a} needs a value"));
        match a {
            "--mutate" => o.mutate = true,
            "--fp-corpus" => o.corpus = true,
            "--fp-modes" => o.modes = true,
            "--list" => o.list = true,
            "-n" | "--seed" => {
                let x: u64 = v()?.parse().map_err(|_| format!("bad value for {a}"))?;
                if a == "-n" {
                    o.n = x as usize;
                } else {
                    o.seed = x;
                }
                i += 1;
            }
            "--bin" | "--rom" | "--work" | "--c-slow-path" => {
                let p = PathBuf::from(v()?);
                match a {
                    "--bin" => o.bin = Some(p),
                    "--rom" => o.rom = Some(p),
                    "--work" => o.work = Some(p),
                    _ => o.c_slow = Some(p),
                }
                i += 1;
            }
            _ if flag => return Err(format!("unknown option {a}")),
            _ => o.kinds.push(a.to_string()),
        }
        i += 1;
    }
    Ok(o)
}

/// A divergence from ares kept on purpose: the VR4300 manual raises an address
/// error for SC and SCD at a misaligned address even when LLbit is clear; ares
/// skips the store and the check. The lifter follows the manual until a
/// hardware test settles it.
fn known(kind: &str, ir: &Out, or: &Out) -> bool {
    matches!(kind, "SC" | "SCD") && ir.exc == Some(5) && or.exc.is_none()
}

#[derive(Default)]
struct Tally {
    pass: u32,
    fail: u32,
    known: u32,
    slow: u32,
    msgs: Vec<String>,
}

fn kind_of(c: &Case) -> &str {
    c.name.rsplit_once('-').map_or(&c.name, |p| p.0)
}

fn dispatch_run(o: &Opts) -> Result<bool, String> {
    let all = cases::cases();
    if o.list {
        for (name, _) in &all {
            println!("{name}");
        }
        let have: Vec<&str> = all.iter().map(|(n, _)| *n).collect();
        let missing: Vec<&str> = decode::forms()
            .iter()
            .filter(|(e, ..)| has_template(*e))
            .map(|(e, ..)| e.name())
            .filter(|n| {
                !have
                    .iter()
                    .any(|h| h == n || h.starts_with(&format!("{n}.")))
            })
            .collect();
        println!(
            "encodings with a template and no kind of that name: {}",
            missing.join(" ")
        );
        return Ok(true);
    }
    let mut rng = Rng(0x9E37_79B9_7F4A_7C15 ^ o.seed);
    let mut cs: Vec<Case> = Vec::new();
    if o.corpus {
        cs = if o.modes {
            cases::mode_cases()
        } else {
            cases::special_cases()
        };
        cs.retain(|c| o.kinds.is_empty() || o.kinds.iter().any(|k| k == kind_of(c)));
    } else {
        for (name, g) in &all {
            if !o.kinds.is_empty() && !o.kinds.iter().any(|k| k == name) {
                continue;
            }
            for k in 0..o.n {
                let mut c = cases::random_case(&mut rng, name);
                let w = g(&mut rng, &mut c);
                c.words.insert(0, w);
                c.name = format!("{name}-{k:03}");
                cs.push(c);
            }
        }
    }
    if cs.is_empty() {
        return Err("no case selected".into());
    }
    let bin = oracle::find(o.bin.clone())?;
    let work = o.work.clone().unwrap_or_else(|| {
        std::env::temp_dir().join(format!("relift-mips-check-{}", std::process::id()))
    });
    let ors = oracle::run(&bin, o.rom.as_deref(), &work, &cs);
    if o.work.is_none() {
        let _ = std::fs::remove_dir_all(&work);
    }
    let ors = ors?;

    let mut per: BTreeMap<String, Tally> = BTreeMap::new();
    let mut slow_kinds: BTreeMap<String, u32> = BTreeMap::new();
    let mut slow_exc: BTreeMap<String, u32> = BTreeMap::new();
    let mut rules: BTreeMap<String, (u32, u32, u32, Vec<String>)> = BTreeMap::new();
    let mut caught: BTreeMap<String, (u32, u32)> = BTreeMap::new();
    let mut fp_cases: Vec<(&Case, &Out, bool)> = Vec::new();
    for (c, or) in cs.iter().zip(&ors) {
        let kind = kind_of(c).to_string();
        let t = per.entry(kind.clone()).or_default();
        let Some(or) = or else {
            t.fail += 1;
            t.msgs.push(format!("{}: no oracle output", c.name));
            continue;
        };
        let lifted = run::lift(c).and_then(|mut l| {
            let m = o.mutate.then(|| mutate::mutate(&mut l.block));
            run::run_ir(c, &l.block).map(|out| (l, m, out))
        });
        let (l, m, ir) = match lifted {
            Ok(x) => x,
            Err(e) => {
                t.fail += 1;
                t.msgs.push(format!("{}: lift or run: {e}", c.name));
                continue;
            }
        };
        let d = compare(c, &ir, or);
        if cslow::is_fp(c) {
            fp_cases.push((c, or, d.len() == 1 && d[0] == SLOW));
        }
        if o.mutate {
            let e = caught.entry(kind).or_default();
            if d.is_empty() {
                e.0 += 1;
            } else {
                e.1 += 1;
                if t.msgs.is_empty() {
                    t.msgs.push(format!("mutation: {}", m.unwrap_or_default()));
                }
            }
            continue;
        }
        if d.len() == 1 && d[0] == SLOW {
            t.slow += 1;
            *slow_kinds.entry(kind.clone()).or_default() += 1;
            *slow_exc
                .entry(or.exc.map_or("none".into(), |x| x.to_string()))
                .or_default() += 1;
            if o.corpus {
                let r = rules.entry(kind).or_default();
                match slow_check(c, or) {
                    None => r.0 += 1,
                    Some(m) if m.starts_with("skip") => r.2 += 1,
                    Some(m) => {
                        r.1 += 1;
                        if r.3.len() < 4 {
                            let w = c.words[0];
                            r.3.push(format!(
                                "{} w={w:08x} x={:#x} y={:#x} fcr={:08x}: {m}",
                                c.name,
                                c.fpr[(w >> 11 & 31) as usize],
                                c.fpr[(w >> 16 & 31) as usize],
                                c.fcr31
                            ));
                        }
                    }
                }
            }
        } else if d.is_empty() {
            t.pass += 1;
        } else if known(&kind, &ir, or) {
            t.known += 1;
        } else {
            t.fail += 1;
            let words: Vec<String> = c.words.iter().map(|w| format!("{w:08x}")).collect();
            t.msgs.push(format!(
                "{} [{}] {}: {}",
                c.name,
                l.encs.join("+"),
                words.join(","),
                d.join("; ")
            ));
        }
    }

    if o.mutate {
        let mut missed = 0;
        for (k, (same, diff)) in &caught {
            let verdict = if *diff > 0 { "CAUGHT" } else { "MISSED" };
            println!("{verdict:6} {k:16} {diff}/{} cases differ", same + diff);
            if *diff == 0 {
                missed += 1;
            }
        }
        println!(
            "{}/{} kinds caught, {missed} missed",
            caught.len() - missed,
            caught.len()
        );
        return Ok(missed == 0);
    }
    let (mut pass, mut fail, mut kn, mut slow) = (0, 0, 0, 0);
    for (k, t) in &per {
        println!("{k:16} pass {:3} fail {:3}", t.pass, t.fail);
        for m in t.msgs.iter().take(3) {
            println!("    {m}");
        }
        pass += t.pass;
        fail += t.fail;
        kn += t.known;
        slow += t.slow;
    }
    if o.corpus {
        let (mut a, mut d, mut s) = (0, 0, 0);
        for (k, (ra, rd, rs, msgs)) in &rules {
            println!("RULE {k:10} slow-path cases: rule==ares {ra:4} differ {rd:4} skipped {rs:4}");
            for m in msgs {
                println!("    {m}");
            }
            a += ra;
            d += rd;
            s += rs;
        }
        println!("RULE TOTAL: rule==ares {a}, differ {d}, skipped {s}");
    }
    println!(
        "slow path (FP guard to the runtime, not compared): {slow} {slow_kinds:?}; oracle exceptions there: {slow_exc:?}"
    );
    let mut c_ok = true;
    if let Some(bin) = &o.c_slow {
        for slow_only in [true, false] {
            let sel: Vec<(&Case, &Out)> = fp_cases
                .iter()
                .filter(|(.., s)| *s || !slow_only)
                .map(|(c, or, _)| (*c, *or))
                .collect();
            let t = cslow::check(bin, &sel)?;
            let what = if slow_only {
                "slow-path cases"
            } else {
                "all COP1 cases"
            };
            println!(
                "C SLOW PATH {what}: {} match ares, {} differ",
                t.cases - t.differ,
                t.differ
            );
            for m in &t.msgs {
                println!("    {m}");
            }
            c_ok &= t.differ == 0;
        }
    }
    let clean = per.values().filter(|t| t.fail == 0 && t.pass > 0).count();
    println!(
        "TOTAL: {} kinds ({clean} all-pass), {pass} cases pass, {fail} fail, {kn} known divergence, {slow} slow path",
        per.len()
    );
    Ok(fail == 0 && c_ok)
}

/// Handles the command line when it selects the ares oracle. Returns `None`
/// for any other oracle.
pub fn dispatch(args: &[String]) -> Option<ExitCode> {
    let i = args.iter().position(|a| a == "--oracle")?;
    if args.get(i + 1).map(String::as_str) != Some("ares") {
        return None;
    }
    let rest: Vec<String> = args
        .iter()
        .enumerate()
        .filter(|(j, _)| *j != i && *j != i + 1)
        .map(|(_, a)| a.clone())
        .collect();
    let result = match rest.split_first() {
        Some((sub, tail)) if sub == "lift" => parse(tail).and_then(|o| dispatch_run(&o)),
        _ => Err("expected `lift`".into()),
    };
    Some(match result {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("{e}\n{USAGE}");
            ExitCode::from(2)
        }
    })
}

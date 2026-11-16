//! The reference interpreter runner. A separate workspace, so nothing in the
//! main one links it; `relift check` runs it as a subprocess.
//!
//! It lifts A64 instruction words with `relift-cpu-arm64`, runs them in the
//! `relift-ir` interpreter and on an oracle (Unicorn, or the native harness
//! on real hardware or QEMU, see `oracle.rs`) on generated states, and
//! compares every general and vector register, SP, NZCV, the next PC, the
//! data window and, with `--fpsr`, FPSR.
//!
//! ```text
//! relift-check insn <hex>[,<hex>...] [options]
//! relift-check forms <name>[,<name>...]|all [options]
//! relift-check --oracle rustyboi step|rom ...   (Game Boy SM83, see gb/cli.rs)
//! relift-check --oracle nba rom ...             (GBA, see gba/cli.rs)
//! relift-check --oracle ares lift ...           (VR4300, see mips/cli.rs)
//! relift-check --oracle sst-z80|zex|ares-z80 ... (Z80, see z80/cli.rs)
//! options: [-n N] [--seed S] [--mutate] [--hand] [--dce] [--fpcr HEX] [--fpsr]
//!          [--oracle unicorn|qemu|qemu-max|m2|cmd:<argv>] [--features SPEC]
//!          [--slow cmd:<argv>] [--all-slow] [--fp-flags] [--undef]
//! ```
//!
//! `insn` runs a fixed word sequence. `forms` runs each named encoding (or
//! every encoding that has a lifter) on N states, each with a fresh random
//! word of that encoding. `--mutate` breaks the lifted IR of the last word
//! (see `mutate.rs`); then a form passes when the run catches it. `--hand`
//! lifts with the hand-written lifter instead of the generated templates, and
//! `all` then means every encoding that has one. `--dce` runs every lifted
//! block through `relift_ir::dce` first. `--fpcr` runs with that FPCR: a
//! block that uses floating point must then leave through its FPCR guard
//! for the runtime's slow path (counted, and compared only with `--slow`),
//! and every other block must still match. `--oracle` picks
//! the machine and with it the CPU feature set (`Features::parse` syntax),
//! which `--features` overrides; words decode and lift for that CPU, and a
//! form the CPU lacks is skipped. `--undef` checks those forms instead:
//! their words must lift to an undefined-instruction trap and fault with
//! SIGILL on the oracle. `--slow` names the
//! slow-path engine (`check/oracle/slow.c`, the runtime's `rl_fp_slow`): a
//! block that leaves through its FPCR guard runs there instead and is
//! compared with the oracle like any other; `--all-slow` sends every
//! guarded block there, also with FPCR zero. `--fp-flags` lifts as for a
//! program that produces FPSR flags: blocks whose flags are built in IR
//! (QC, compares, min/max) are compared in the interpreter, and blocks with
//! host FP operations or a flags guard go to the slow engine.

mod emu;
mod gb;
mod gba;
mod mips;
mod mutate;
mod oracle;
mod states;
mod z80;

use std::process::ExitCode;

use relift_cpu_arm64::decode::{Encoding, Insn};
use relift_cpu_arm64::lift::sail::{SLOW_FLAGS, SLOW_FPCR};
use relift_cpu_arm64::lift::{lifter, supported};

use emu::{Guest, compare, interpret, lift_all};
use oracle::{Case, Oracle};
use states::{Rng, decode};

struct Opts {
    n: u64,
    seed: u64,
    mutate: bool,
    fpcr: u64,
}

#[derive(Default)]
struct Report {
    states: u64,
    matched: u64,
    slow: u64,
    both_faulted: u64,
    unplaced: u64,
    mutation: Option<String>,
    first: Option<String>,
}

/// One case and its lifted blocks, or why it could not be lifted.
struct Prepared {
    case: Case,
    blocks: Result<Vec<relift_ir::Block>, String>,
}

fn prepare(words: Vec<u32>, g: Guest, opts: &Opts, rep: &mut Report) -> Prepared {
    let mut blocks = lift_all(&words);
    if opts.mutate
        && let Ok(bs) = &mut blocks
        && let Some(last) = bs.last_mut()
    {
        let m = mutate::mutate(last);
        rep.mutation.get_or_insert(m);
    }
    Prepared {
        case: Case { words, input: g },
        blocks,
    }
}

/// The engine that runs blocks which leave for the FPCR slow path.
struct Slow {
    engine: Box<dyn Oracle>,
    /// Route every guarded block there, also with FPCR zero.
    always: bool,
}

/// Runs the prepared cases on the interpreter (or, for a block that leaves
/// for the FPCR slow path, on the slow engine) and the oracle, and counts.
fn run_all(
    oracle: &mut dyn Oracle,
    slow: &mut Option<Slow>,
    ps: &[Prepared],
    rep: &mut Report,
) -> Result<(), String> {
    let mut todo = Vec::new();
    let mut irs = Vec::new();
    let mut to_slow = Vec::new();
    for p in ps {
        rep.states += 1;
        let blocks = match &p.blocks {
            Ok(b) => b,
            Err(e) => {
                rep.first.get_or_insert(format!("lift failed: {e}"));
                continue;
            }
        };
        let ir = interpret(blocks, &p.case.words, &p.case.input);
        // The FP guards; an alignment guard (`SLOW_ALIGN`) is a fault and
        // leaves no block for the FP slow path.
        let guarded = blocks.iter().any(|b| {
            b.insts
                .iter()
                .any(|i| matches!(i.op, relift_ir::Op::Guard(_, SLOW_FPCR | SLOW_FLAGS)))
        });
        // In a flag-producing program, blocks with host FP operations get
        // their flags from the host, which the interpreter does not model:
        // they run on the slow engine, as do blocks that left by a guard.
        let fp_ops = blocks.iter().any(|b| {
            b.insts
                .iter()
                .any(|i| matches!(i.op, relift_ir::Op::Fp(..)))
        });
        let flags_mode = emu::FP_FLAGS.load(std::sync::atomic::Ordering::Relaxed);
        if guarded
            && let Some(sl) = slow
            && (p.case.input.fpcr != 0
                || sl.always
                || (flags_mode && (fp_ops || matches!(ir, emu::Outcome::Slow(_)))))
        {
            if p.case.input.fpcr != 0 && !matches!(ir, emu::Outcome::Slow(_)) {
                rep.first.get_or_insert(format!(
                    "[{:08x}] the FPCR guard did not leave the block",
                    p.case.words.last().copied().unwrap_or(0)
                ));
                continue;
            }
            rep.slow += 1;
            to_slow.push(todo.len());
            todo.push(p.case.clone());
            irs.push(ir);
            continue;
        }
        if p.case.input.fpcr != 0 && guarded {
            if matches!(ir, emu::Outcome::Slow(_)) {
                rep.slow += 1;
                rep.matched += 1;
            } else {
                rep.first.get_or_insert(format!(
                    "[{:08x}] the FPCR guard did not leave the block",
                    p.case.words.last().copied().unwrap_or(0)
                ));
            }
            continue;
        }
        todo.push(p.case.clone());
        irs.push(ir);
    }
    if let Some(sl) = slow
        && !to_slow.is_empty()
    {
        let cases: Vec<Case> = to_slow.iter().map(|i| todo[*i].clone()).collect();
        for (i, o) in to_slow.iter().zip(sl.engine.run(&cases)?) {
            irs[*i] = o;
        }
    }
    let ors = oracle.run(&todo)?;
    for ((c, ir), or) in todo.iter().zip(&irs).zip(&ors) {
        if matches!((ir, or), (emu::Outcome::Fault(_), emu::Outcome::Fault(_))) {
            rep.both_faulted += 1;
        }
        let mut d = compare(ir, or);
        if emu::UNDEF.load(std::sync::atomic::Ordering::Relaxed)
            && d.is_empty()
            && !matches!(or, emu::Outcome::Fault(s) if s.starts_with("signal 4 ") || s.contains("INSN_INVALID") || s.contains("EXCEPTION"))
        {
            d.push(format!(
                "expected an undefined-instruction fault, oracle gave {or:?}"
            ));
        }
        if d.is_empty() {
            rep.matched += 1;
        } else if rep.first.is_none() {
            let words: Vec<String> = c.words.iter().map(|w| format!("{w:08x}")).collect();
            rep.first = Some(format!("[{}] {}", words.join(","), d.join("; ")));
        }
    }
    Ok(())
}

fn check_form(
    oracle: &mut dyn Oracle,
    slow: &mut Option<Slow>,
    enc: Encoding,
    opts: &Opts,
) -> Result<Report, String> {
    let mut r = Rng::new(enc.name(), opts.seed);
    let mut rep = Report::default();
    let mut ps = Vec::new();
    let mut tries = 0;
    while (ps.len() as u64) < opts.n && tries < opts.n * 10 {
        tries += 1;
        let Some(word) = states::word_of(enc, &mut r) else {
            rep.first = Some(NO_WORD.to_string());
            break;
        };
        let insn = decode(word).expect("word_of decodes");
        let Some(mut g) = states::state(&insn, &mut r) else {
            rep.unplaced += 1;
            continue;
        };
        g.fpcr = opts.fpcr;
        let mut words = states::prelude(&insn, &mut g, &mut r);
        words.push(word);
        ps.push(prepare(words, g, opts, &mut rep));
    }
    run_all(oracle, slow, &ps, &mut rep)?;
    Ok(rep)
}

fn check_words(
    oracle: &mut dyn Oracle,
    slow: &mut Option<Slow>,
    words: &[u32],
    opts: &Opts,
) -> Result<Report, String> {
    let last = *words.last().ok_or("no words")?;
    let insn: Insn = decode(last).ok_or(format!("{last:08x} does not decode"))?;
    let mut r = Rng::new(&format!("{words:?}"), opts.seed);
    let mut rep = Report::default();
    let mut ps = Vec::new();
    while (ps.len() as u64) < opts.n && rep.unplaced < opts.n * 10 {
        let Some(mut g) = states::state(&insn, &mut r) else {
            rep.unplaced += 1;
            continue;
        };
        g.fpcr = opts.fpcr;
        ps.push(prepare(words.to_vec(), g, opts, &mut rep));
    }
    run_all(oracle, slow, &ps, &mut rep)?;
    Ok(rep)
}

/// Whether no word of the form lifts (a feature the CPU lacks, or a form
/// with only some words liftable).
fn skipped(rep: &Report) -> bool {
    rep.states == 0 && rep.first.as_deref() == Some(NO_WORD)
}

const NO_WORD: &str = "no liftable word found";

fn print(name: &str, rep: &Report, opts: &Opts) -> bool {
    if skipped(rep) {
        println!("SKIP   {name:28} {NO_WORD}");
        return true;
    }
    let ok = if opts.mutate {
        rep.matched < rep.states
    } else {
        rep.states > 0 && rep.matched == rep.states
    };
    let verdict = match (opts.mutate, ok) {
        (false, true) => "PASS",
        (false, false) => "FAIL",
        (true, true) => "CAUGHT",
        (true, false) => "MISSED",
    };
    println!(
        "{verdict:6} {name:28} {}/{} matched, {} both faulted, {} unplaced{}{}{}",
        rep.matched,
        rep.states,
        rep.both_faulted,
        rep.unplaced,
        if rep.slow > 0 {
            format!(", {} to the slow path", rep.slow)
        } else {
            String::new()
        },
        rep.mutation
            .as_ref()
            .map(|m| format!(", mutation: {m}"))
            .unwrap_or_default(),
        rep.first
            .as_ref()
            .map(|f| format!(", first: {f}"))
            .unwrap_or_default(),
    );
    ok
}

fn usage() -> ExitCode {
    eprintln!(
        "usage: relift-check insn <hex>[,<hex>...] [options]\n       relift-check forms <name>[,<name>...]|all [options]\noptions: [-n N] [--seed S] [--mutate] [--hand] [--dce] [--fpcr HEX] [--fpsr]\n         [--oracle unicorn|qemu|qemu-max|m2|cmd:<argv>] [--features SPEC]\n         [--slow cmd:<argv>] [--all-slow] [--fp-flags] [--undef]\n       relift-check --oracle rustyboi step|lift|rom ...  (Game Boy)\n       relift-check --oracle nba rom ...  (GBA)\n       relift-check --oracle ares lift ...  (VR4300)\n       relift-check --oracle sst-z80|zex|ares-z80 ...  (Z80)"
    );
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Some(code) = gb::cli::dispatch(&args) {
        return code;
    }
    if let Some(code) = gba::cli::dispatch(&args) {
        return code;
    }
    if let Some(code) = mips::cli::dispatch(&args) {
        return code;
    }
    if let Some(code) = z80::cli::dispatch(&args) {
        return code;
    }
    if args.len() < 2 {
        return usage();
    }
    let mut opts = Opts {
        n: 10_000,
        seed: 1,
        mutate: false,
        fpcr: 0,
    };
    let mut oracle_spec = "unicorn".to_string();
    let mut features: Option<String> = None;
    let mut slow_spec: Option<String> = None;
    let mut all_slow = false;
    let mut i = 2;
    while i < args.len() {
        match args[i].as_str() {
            "--fpcr" => {
                let Some(v) = args
                    .get(i + 1)
                    .and_then(|v| u64::from_str_radix(v.trim_start_matches("0x"), 16).ok())
                else {
                    return usage();
                };
                opts.fpcr = v;
                i += 1;
            }
            "-n" | "--seed" => {
                let Some(v) = args.get(i + 1).and_then(|v| v.parse().ok()) else {
                    return usage();
                };
                if args[i] == "-n" {
                    opts.n = v;
                } else {
                    opts.seed = v;
                }
                i += 1;
            }
            "--oracle" | "--features" | "--slow" => {
                let Some(v) = args.get(i + 1) else {
                    return usage();
                };
                match args[i].as_str() {
                    "--oracle" => oracle_spec = v.clone(),
                    "--features" => features = Some(v.clone()),
                    _ => slow_spec = Some(v.clone()),
                }
                i += 1;
            }
            "--all-slow" => all_slow = true,
            "--fp-flags" => emu::FP_FLAGS.store(true, std::sync::atomic::Ordering::Relaxed),
            "--fpsr" => emu::FPSR.store(true, std::sync::atomic::Ordering::Relaxed),
            "--mutate" => opts.mutate = true,
            "--hand" => emu::HAND.store(true, std::sync::atomic::Ordering::Relaxed),
            "--undef" => emu::UNDEF.store(true, std::sync::atomic::Ordering::Relaxed),
            "--dce" => emu::DCE.store(true, std::sync::atomic::Ordering::Relaxed),
            _ => return usage(),
        }
        i += 1;
    }
    let (mut oracle, default_features) = match oracle::open(&oracle_spec) {
        Ok(o) => o,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::FAILURE;
        }
    };
    let features = features.unwrap_or(default_features);
    let _ = emu::FEATURES.set(features);
    let mut slow = match slow_spec.as_deref().map(oracle::open) {
        None => None,
        Some(Ok((engine, _))) => Some(Slow {
            engine,
            always: all_slow,
        }),
        Some(Err(e)) => {
            eprintln!("--slow: {e}");
            return ExitCode::FAILURE;
        }
    };
    let oracle = oracle.as_mut();
    let mut all_ok = true;
    match args[0].as_str() {
        "insn" => {
            let words: Result<Vec<u32>, _> = args[1]
                .split(',')
                .map(|h| u32::from_str_radix(h.trim_start_matches("0x"), 16))
                .collect();
            let Ok(words) = words else {
                return usage();
            };
            match check_words(oracle, &mut slow, &words, &opts) {
                Ok(rep) => all_ok &= print(&args[1], &rep, &opts),
                Err(e) => {
                    eprintln!("{e}");
                    return ExitCode::FAILURE;
                }
            }
        }
        "forms" => {
            let hand = emu::HAND.load(std::sync::atomic::Ordering::Relaxed);
            let encs: Vec<Encoding> = if args[1] == "all" && hand {
                Encoding::all().filter(|e| lifter(*e).is_some()).collect()
            } else if args[1] == "all" {
                supported().collect()
            } else {
                let mut v = Vec::new();
                for name in args[1].split(',') {
                    match Encoding::all().find(|e| e.name() == name) {
                        Some(e) if hand && lifter(e).is_none() => {
                            eprintln!("{name} has no hand-written lifter");
                            return ExitCode::FAILURE;
                        }
                        Some(e) => v.push(e),
                        None => {
                            eprintln!("no encoding named {name}");
                            return ExitCode::FAILURE;
                        }
                    }
                }
                v
            };
            let (mut pass, mut total, mut skip) = (0, 0, 0);
            for enc in encs {
                let rep = match check_form(oracle, &mut slow, enc, &opts) {
                    Ok(r) => r,
                    Err(e) => {
                        eprintln!("{e}");
                        return ExitCode::FAILURE;
                    }
                };
                if skipped(&rep) {
                    skip += 1;
                    print(enc.name(), &rep, &opts);
                    continue;
                }
                total += 1;
                if print(enc.name(), &rep, &opts) {
                    pass += 1;
                } else {
                    all_ok = false;
                }
            }
            let what = if opts.mutate { "caught" } else { "passed" };
            println!(
                "{pass}/{total} forms {what} against {}, {skip} skipped",
                oracle.name()
            );
        }
        _ => return usage(),
    }
    if all_ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

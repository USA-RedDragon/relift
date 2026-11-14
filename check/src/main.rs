//! The reference interpreter runner. A separate workspace, so nothing in the
//! main one links it; `relift check` runs it as a subprocess.
//!
//! It lifts A64 instruction words with `relift-cpu-a64`, runs them in the
//! `relift-ir` interpreter and in Unicorn on generated states, and compares
//! every general and vector register, SP, NZCV, the next PC and the data
//! window.
//!
//! ```text
//! relift-check insn <hex>[,<hex>...] [-n N] [--seed S] [--mutate] [--hand] [--dce] [--fpcr HEX]
//! relift-check forms <name>[,<name>...]|all [-n N] [--seed S] [--mutate] [--hand] [--dce] [--fpcr HEX]
//! relift-check --oracle rustyboi step|rom ...   (Game Boy SM83, see gb/cli.rs)
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
//! for the runtime's slow path (counted, not compared, since Unicorn has no
//! slow path), and every other block must still match.

mod emu;
mod gb;
mod mutate;
mod states;

use std::process::ExitCode;

use relift_cpu_a64::decode::{Encoding, Insn, decode};
use relift_cpu_a64::lift::{lifter, supported};

use emu::{Emu, compare, interpret, lift_all};
use states::Rng;

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

fn run_one(emu: &mut Emu, words: &[u32], g: &emu::Guest, opts: &Opts, rep: &mut Report) {
    let mut blocks = match lift_all(words) {
        Ok(b) => b,
        Err(e) => {
            rep.states += 1;
            rep.first.get_or_insert(format!("lift failed: {e}"));
            return;
        }
    };
    if opts.mutate
        && let Some(last) = blocks.last_mut()
    {
        let m = mutate::mutate(last);
        rep.mutation.get_or_insert(m);
    }
    let ir = interpret(&blocks, words, g);
    rep.states += 1;
    let guarded = blocks.iter().any(|b| {
        b.insts
            .iter()
            .any(|i| matches!(i.op, relift_ir::Op::Guard(..)))
    });
    if g.fpcr != 0 && guarded {
        if matches!(ir, emu::Outcome::Slow(_)) {
            rep.slow += 1;
            rep.matched += 1;
        } else {
            rep.first.get_or_insert(format!(
                "[{:08x}] the FPCR guard did not leave the block",
                words.last().copied().unwrap_or(0)
            ));
        }
        return;
    }
    let uc = match emu.run(words, g) {
        Ok(o) => o,
        Err(e) => emu::Outcome::Fault(format!("unicorn: {e:?}")),
    };
    if matches!((&ir, &uc), (emu::Outcome::Fault(_), emu::Outcome::Fault(_))) {
        rep.both_faulted += 1;
    }
    let d = compare(&ir, &uc);
    if d.is_empty() {
        rep.matched += 1;
    } else if rep.first.is_none() {
        let words: Vec<String> = words.iter().map(|w| format!("{w:08x}")).collect();
        rep.first = Some(format!("[{}] {}", words.join(","), d.join("; ")));
    }
}

fn check_form(emu: &mut Emu, enc: Encoding, opts: &Opts) -> Report {
    let mut r = Rng::new(enc.name(), opts.seed);
    let mut rep = Report::default();
    let mut tries = 0;
    while rep.states < opts.n && tries < opts.n * 10 {
        tries += 1;
        let Some(word) = states::word_of(enc, &mut r) else {
            rep.first = Some("no liftable word found".to_string());
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
        run_one(emu, &words, &g, opts, &mut rep);
    }
    rep
}

fn check_words(emu: &mut Emu, words: &[u32], opts: &Opts) -> Result<Report, String> {
    let last = *words.last().ok_or("no words")?;
    let insn: Insn = decode(last).ok_or(format!("{last:08x} does not decode"))?;
    let mut r = Rng::new(&format!("{words:?}"), opts.seed);
    let mut rep = Report::default();
    while rep.states < opts.n && rep.unplaced < opts.n * 10 {
        let Some(mut g) = states::state(&insn, &mut r) else {
            rep.unplaced += 1;
            continue;
        };
        g.fpcr = opts.fpcr;
        run_one(emu, words, &g, opts, &mut rep);
    }
    Ok(rep)
}

fn print(name: &str, rep: &Report, opts: &Opts) -> bool {
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
        "usage: relift-check insn <hex>[,<hex>...] [-n N] [--seed S] [--mutate] [--hand] [--dce] [--fpcr HEX]\n       relift-check forms <name>[,<name>...]|all [-n N] [--seed S] [--mutate] [--hand] [--dce] [--fpcr HEX]"
    );
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Some(code) = gb::cli::dispatch(&args) {
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
            "--mutate" => opts.mutate = true,
            "--hand" => emu::HAND.store(true, std::sync::atomic::Ordering::Relaxed),
            "--dce" => emu::DCE.store(true, std::sync::atomic::Ordering::Relaxed),
            _ => return usage(),
        }
        i += 1;
    }
    let mut emu = match Emu::new() {
        Ok(e) => e,
        Err(e) => {
            eprintln!("unicorn: {e:?}");
            return ExitCode::FAILURE;
        }
    };
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
            match check_words(&mut emu, &words, &opts) {
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
            let (mut pass, mut total) = (0, 0);
            for enc in encs {
                let rep = check_form(&mut emu, enc, &opts);
                total += 1;
                if print(enc.name(), &rep, &opts) {
                    pass += 1;
                } else {
                    all_ok = false;
                }
            }
            let what = if opts.mutate { "caught" } else { "passed" };
            println!("{pass}/{total} forms {what}");
        }
        _ => return usage(),
    }
    if all_ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

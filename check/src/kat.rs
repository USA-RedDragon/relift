//! `relift-check kat`: records the A64 known-answer suite.
//!
//! ```text
//! relift-check kat record DIR [-n N] [--classes a,b] [--oracle thor|cmd:<argv>] [--src NAME]
//! relift-check kat list
//! relift-check kat cross DIR
//! ```
//!
//! `record` generates cases for every encoding that lifts and that the
//! oracle's CPU has (the Thor's, `--oracle thor`: its harness is run over
//! `adb shell`), runs each on the machine twice in two processes, and writes
//! `DIR/<class>.txt` in the format of `cpus/arm64/tests/kat/format.rs`. Bits
//! that change from one process to the next (the pointer authentication code
//! field: the kernel draws the keys per process) are masked; a case that
//! differs anywhere else is dropped and counted. The answers always come
//! from the hardware: Unicorn, QEMU and the like are refused, and `cross`
//! only reports where Unicorn disagrees with a recording.
//!
//! `DIR/skipped.txt` names the encodings the CPU lacks and the features
//! they need.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::Path;
use std::process::ExitCode;

use relift_cpu_arm64::decode::{Encoding, Features, Insn, decode_for};
use relift_cpu_arm64::lift::supported;

use crate::emu::{self, CODE, Guest, Outcome};
use crate::oracle::{self, Case as Run, Oracle as _};
use crate::states::{self, Rng};

#[path = "../../cpus/arm64/tests/kat/format.rs"]
mod format;

use format::{Case, Result as Answer, State};

const _: () = assert!(emu::DATA == format::DATA && emu::DATA_SIZE == format::DATA_SIZE);

/// Where the Thor's harness is on the device.
const THOR_HARNESS: &str = "/data/local/tmp/a64kat/harness-linux";

/// The class (file) of an encoding.
pub fn class(enc: Encoding) -> &'static str {
    let n = enc.name();
    let m = enc.mnemonic();
    let pac = [
        "PAC", "AUT", "XPAC", "BRAA", "BRAB", "BLRAA", "BLRAB", "RETAA", "RETAB", "LDRAA", "LDRAB",
    ];
    if pac.iter().any(|p| m.starts_with(p)) {
        return "pac";
    }
    if n.ends_with("_memop") || n.contains("_comswap") || m.starts_with("CAS") || m == "SWP" {
        return "atomic";
    }
    if m.starts_with("LDAPR")
        || m.starts_with("LDAPUR")
        || m.starts_with("STLUR")
        || n.contains("_ldapstl")
    {
        return "ldapr";
    }
    match enc.group() {
        "ldst" => "ldst",
        "dpimm" | "dpreg" => "alu",
        "control" | "branch_exception_system" | "branch" => "branch",
        g if g.contains("simd") || g.contains("fp") => "simd-fp",
        _ => "other",
    }
}

/// The registers a word names, as (x or v index) sets.
fn referenced(words: &[u32], f: &Features, hints: &mut bool) -> ([bool; 32], [bool; 32]) {
    let (mut x, mut v) = ([false; 32], [false; 32]);
    for w in words {
        let Some(insn) = decode_for(*w, f) else {
            continue;
        };
        if insn.encoding.name().ends_with("_hints") {
            *hints = true;
        }
        for fld in insn.encoding.fields() {
            let nm = fld.name;
            let is_reg = matches!(
                nm,
                "Rd" | "Rn"
                    | "Rm"
                    | "Ra"
                    | "Rt"
                    | "Rt2"
                    | "Rs"
                    | "Rt3"
                    | "Rt4"
                    | "Rdn"
                    | "Rda"
                    | "Rv"
            );
            if !is_reg {
                continue;
            }
            let r = fld.extract(*w) as usize;
            let n = insn.encoding.name();
            let simd = class(insn.encoding) == "simd-fp";
            let vec_mem = n.contains("asisd")
                || n.split('_').nth(1).is_some_and(|k| {
                    matches!(k, "B" | "BL" | "H" | "S" | "D" | "Q") && n.contains("ldst")
                });
            let is_data = matches!(nm, "Rt" | "Rt2" | "Rt3" | "Rt4");
            if simd || (vec_mem && is_data) {
                let span = if n.contains("asisd") || n.contains("TBL") || n.contains("TBX") {
                    4
                } else {
                    1
                };
                for k in 0..span {
                    v[(r + k) % 32] = true;
                }
            }
            if simd || !(vec_mem && is_data) {
                x[r % 32] = true;
                if insn.encoding.mnemonic().starts_with("CASP") {
                    x[(r + 1) % 32] = true;
                }
            }
        }
    }
    (x, v)
}

/// A guest state with every register the words do not name set to its
/// canary, as the format has it.
fn sparse(g: &mut Guest, words: &[u32], f: &Features) {
    let mut hints = false;
    let (x, v) = referenced(words, f, &mut hints);
    for (i, used) in x.iter().enumerate().take(31) {
        if !used && !(hints && matches!(i, 16 | 17 | 30)) {
            g.x[i] = format::canary_x(i);
        }
    }
    for (i, used) in v.iter().enumerate().take(32) {
        if !used {
            g.v[i] = format::canary_v(i);
        }
    }
}

fn state_of(g: &Guest) -> State {
    let nzcv = g
        .nzcv
        .iter()
        .enumerate()
        .fold(0u8, |a, (i, b)| a | (u8::from(*b) << (3 - i)));
    State {
        x: g.x,
        sp: g.sp,
        v: g.v,
        nzcv,
        fpcr: g.fpcr as u32,
        fpsr: g.fpsr,
        mem: g.mem.clone(),
        pc: g.pc.wrapping_sub(CODE),
    }
}

/// The signal of a faulting outcome.
fn signal(msg: &str) -> u32 {
    msg.strip_prefix("signal ")
        .and_then(|r| r.split(' ').next())
        .and_then(|s| s.parse().ok())
        .unwrap_or(0)
}

/// Bits of a register that vary with the keys.
const PAC_FIELD: u64 = 0x007f_ff80_0000_0000;
const PAC_HI32: u64 = 0xffff_ffff_0000_0000;

/// The answer the two runs agree on, with the varying bits masked, or why
/// the case cannot be recorded.
fn merge(a: &Outcome, b: &Outcome) -> Result<(Answer, BTreeMap<String, u128>), String> {
    let (ga, gb) = match (a, b) {
        (Outcome::Fault(x), Outcome::Fault(y)) => {
            let (sa, sb) = (signal(x), signal(y));
            return if sa == sb {
                Ok((Answer::Fault(sa), BTreeMap::new()))
            } else {
                Err(format!("faults differ: {x} / {y}"))
            };
        }
        (Outcome::Done(x), Outcome::Done(y)) => (x, y),
        _ => return Err("one run faulted".to_string()),
    };
    let (sa, sb) = (state_of(ga), state_of(gb));
    let mut mask = BTreeMap::new();
    for i in 0..31 {
        let d = sa.x[i] ^ sb.x[i];
        if d == 0 {
            continue;
        }
        let m = if d & !PAC_FIELD == 0 {
            PAC_FIELD
        } else if d & !PAC_HI32 == 0 {
            PAC_HI32
        } else {
            return Err(format!(
                "x{i} differs between runs: {:#x} / {:#x}",
                sa.x[i], sb.x[i]
            ));
        };
        mask.insert(format!("x{i}"), u128::from(m));
    }
    if sa.sp != sb.sp
        || sa.v != sb.v
        || sa.nzcv != sb.nzcv
        || sa.fpsr != sb.fpsr
        || sa.mem != sb.mem
        || sa.pc != sb.pc
    {
        return Err("vector, flag or memory state differs between runs".to_string());
    }
    Ok((Answer::Done(Box::new(sa)), mask))
}

/// The FPCR modes cases run in: the rounding modes, flush to zero and default
/// NaN. FEAT_AFP (AH, FIZ, NEP) is absent from the Thor.
const FPCRS: [u32; 8] = [
    0,
    0,
    1 << 22,
    2 << 22,
    3 << 22,
    1 << 24,
    1 << 25,
    (1 << 24) | (1 << 25) | (2 << 22),
];

struct Opts {
    n: usize,
    classes: Option<Vec<String>>,
    oracle: String,
    src: String,
}

fn open(o: &Opts) -> Result<Box<dyn oracle::Oracle>, String> {
    let spec = if o.oracle == "thor" {
        let h = std::env::var("RELIFT_KAT_HARNESS").unwrap_or_else(|_| THOR_HARNESS.to_string());
        format!("cmd:adb shell {h}")
    } else {
        o.oracle.clone()
    };
    if !spec.starts_with("cmd:") {
        return Err("answers come from hardware: use --oracle thor or cmd:<argv>".to_string());
    }
    oracle::open(&spec).map(|(b, _)| b)
}

fn record(dir: &Path, o: &Opts) -> Result<(), String> {
    let features = Features::parse(format::THOR_FEATURES)?;
    let _ = emu::FEATURES.set(format::THOR_FEATURES.to_string());
    let mut by_class: BTreeMap<&'static str, Vec<Encoding>> = BTreeMap::new();
    let mut skipped = String::new();
    for enc in supported() {
        if enc.is_sve_or_sme() {
            let _ = writeln!(skipped, "{}: SVE/SME (the Thor has neither)", enc.name());
            continue;
        }
        by_class.entry(class(enc)).or_default().push(enc);
    }
    std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    let mut total = 0;
    let mut dropped = 0;
    for (cls, encs) in &by_class {
        if let Some(only) = &o.classes
            && !only.iter().any(|c| c == cls)
        {
            continue;
        }
        let per = if matches!(*cls, "pac" | "atomic" | "ldapr") {
            o.n * 3
        } else {
            o.n
        };
        let mut runs: Vec<Run> = Vec::new();
        let mut names = Vec::new();
        for enc in encs {
            let mut r = Rng::new(enc.name(), 0x004b_4154);
            let mut got = 0;
            let mut tries = 0;
            while got < per && tries < per * 10 {
                tries += 1;
                let Some(w) = states::word_of(*enc, &mut r) else {
                    let need: Vec<&str> = enc.features().iter().map(|f| f.name()).collect();
                    let _ = writeln!(
                        skipped,
                        "{}: no word on the Thor (needs {})",
                        enc.name(),
                        need.join(", ")
                    );
                    break;
                };
                let insn: Insn = decode_for(w, &features).expect("word_of decodes");
                let Some(mut g) = states::state(&insn, &mut r) else {
                    continue;
                };
                let fp = matches!(*cls, "simd-fp");
                g.fpcr = if fp { u64::from(r.pick(&FPCRS)) } else { 0 };
                let mut words = states::prelude(&insn, &mut g, &mut r);
                words.push(w);
                sparse(&mut g, &words, &features);
                runs.push(Run { words, input: g });
                names.push(enc.name());
                got += 1;
            }
        }
        let mut out_a = open(o)?;
        let a = out_a.run(&runs)?;
        drop(out_a);
        let mut out_b = open(o)?;
        let b = out_b.run(&runs)?;
        drop(out_b);
        let mut text = String::new();
        let mut kept = 0;
        for (((run, x), y), name) in runs.iter().zip(&a).zip(&b).zip(&names) {
            match merge(x, y) {
                Ok((out, mask)) => {
                    let input = state_of(&run.input);
                    let case = Case {
                        src: o.src.clone(),
                        words: run.words.clone(),
                        input,
                        seed: run.input.mem_seed.expect("seeded window"),
                        out,
                        mask,
                    };
                    text.push_str(&format::format(&case));
                    text.push('\n');
                    kept += 1;
                }
                Err(e) => {
                    dropped += 1;
                    eprintln!("dropped {name} {:08x?}: {e}", run.words);
                }
            }
        }
        relift_cache::write_if_changed(&dir.join(format!("{cls}.txt")), text.as_bytes())
            .map_err(|e| e.to_string())?;
        println!("{cls}: {} encodings, {kept} cases", encs.len());
        total += kept;
    }
    relift_cache::write_if_changed(&dir.join("skipped.txt"), skipped.as_bytes())
        .map_err(|e| e.to_string())?;
    println!("{total} cases recorded, {dropped} dropped");
    Ok(())
}

/// Runs every recorded case on Unicorn and reports the ones that disagree
/// with the hardware's answer.
fn cross(dir: &Path) -> Result<(), String> {
    let mut u = oracle::Unicorn::new().map_err(|e| format!("unicorn: {e:?}"))?;
    let mut paths: Vec<_> = std::fs::read_dir(dir)
        .map_err(|e| e.to_string())?
        .map(|e| e.expect("entry").path())
        .filter(|p| p.extension().is_some_and(|x| x == "txt") && !p.ends_with("skipped.txt"))
        .collect();
    paths.sort();
    let (mut n, mut bad) = (0, 0);
    for p in paths {
        let text = std::fs::read_to_string(&p).map_err(|e| e.to_string())?;
        for l in text.lines() {
            let c = format::parse(l);
            let g = Guest {
                x: c.input.x,
                sp: c.input.sp,
                nzcv: [3, 2, 1, 0].map(|i| c.input.nzcv >> i & 1 == 1),
                v: c.input.v,
                fpcr: u64::from(c.input.fpcr),
                fpsr: c.input.fpsr,
                pc: 0,
                mem: c.input.mem.clone(),
                mem_seed: Some(c.seed),
            };
            let r = u.run(&[Run {
                words: c.words.clone(),
                input: g,
            }])?;
            n += 1;
            let got = match &r[0] {
                Outcome::Done(g) => Answer::Done(Box::new(state_of(g))),
                Outcome::Fault(m) => Answer::Fault(signal(m.as_str())),
                Outcome::Slow(_) => continue,
            };
            let d = format::diff(&got, &c, false);
            if !d.is_empty() {
                bad += 1;
                println!(
                    "unicorn differs: {} {}",
                    c.words
                        .iter()
                        .map(|w| format!("{w:08x}"))
                        .collect::<Vec<_>>()
                        .join(","),
                    d.join("; ")
                );
            }
        }
    }
    println!("{n} cases, {bad} where Unicorn differs from the hardware");
    Ok(())
}

/// Handles `relift-check kat ...`.
pub fn dispatch(args: &[String]) -> Option<ExitCode> {
    if args.first().map(String::as_str) != Some("kat") {
        return None;
    }
    let usage = || {
        eprintln!(
            "usage: relift-check kat record DIR [-n N] [--classes a,b] [--oracle thor|cmd:<argv>] [--src NAME]\n       relift-check kat list\n       relift-check kat cross DIR"
        );
        ExitCode::from(2)
    };
    let mut o = Opts {
        n: 8,
        classes: None,
        oracle: "thor".to_string(),
        src: "thor".to_string(),
    };
    let mut pos = Vec::new();
    let mut i = 2;
    while i < args.len() {
        match args[i].as_str() {
            "-n" => {
                i += 1;
                o.n = args.get(i)?.parse().ok()?;
            }
            "--classes" => {
                i += 1;
                o.classes = Some(args.get(i)?.split(',').map(String::from).collect());
            }
            "--oracle" => {
                i += 1;
                o.oracle = args.get(i)?.clone();
            }
            "--src" => {
                i += 1;
                o.src = args.get(i)?.clone();
            }
            a => pos.push(a.to_string()),
        }
        i += 1;
    }
    let r = match (args.get(1).map(String::as_str), pos.first()) {
        (Some("record"), Some(d)) => record(Path::new(d), &o),
        (Some("cross"), Some(d)) => cross(Path::new(d)),
        (Some("list"), _) => {
            let mut c: BTreeMap<&str, usize> = BTreeMap::new();
            for e in supported() {
                *c.entry(class(e)).or_default() += 1;
            }
            println!("{c:?}");
            Ok(())
        }
        _ => return Some(usage()),
    };
    Some(match r {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    })
}

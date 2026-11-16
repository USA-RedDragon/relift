//! The runtime's C FPU slow path against ares: the COP1 cases go through
//! `mips_fpu_cases` (runtime/cpus/mips/tools/fpu_cases.c, built by the
//! runtime Makefile), one line per case, and its exception, FCR31 and FPU
//! registers are compared with the oracle's.

use std::io::Write;
use std::path::Path;
use std::process::Stdio;

use super::cases::Case;
use super::run::Out;

/// Whether `c` is one COP1 arithmetic, conversion or compare instruction.
pub fn is_fp(c: &Case) -> bool {
    c.words.len() == 1 && c.words[0] >> 26 == 0x11 && (c.words[0] >> 21 & 31) >= 0x10
}

/// The slow path's outcome of one case.
struct Got {
    exc: bool,
    fcr31: u32,
    fpr: [u64; 32],
}

fn run_bin(bin: &Path, cases: &[&Case]) -> Result<Vec<Got>, String> {
    let mut input = String::new();
    for c in cases {
        input.push_str(&format!("{:x} {:x} {:x}", c.words[0], c.status, c.fcr31));
        for f in &c.fpr {
            input.push_str(&format!(" {f:x}"));
        }
        input.push('\n');
    }
    let mut child = relift_proc::command(bin)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .map_err(|e| format!("{}: {e}", bin.display()))?;
    let mut stdin = child.stdin.take().ok_or("no stdin")?;
    let writer = std::thread::spawn(move || stdin.write_all(input.as_bytes()));
    let out = child
        .wait_with_output()
        .map_err(|e| format!("{}: {e}", bin.display()))?;
    writer
        .join()
        .map_err(|_| "writer panicked".to_string())?
        .map_err(|e| format!("{}: {e}", bin.display()))?;
    if !out.status.success() {
        return Err(format!("{} failed: {}", bin.display(), out.status));
    }
    let text = String::from_utf8_lossy(&out.stdout);
    let mut got = Vec::new();
    for line in text.lines() {
        let v: Vec<u64> = line
            .split_whitespace()
            .map(|t| u64::from_str_radix(t, 16).map_err(|e| format!("{line}: {e}")))
            .collect::<Result<_, _>>()?;
        if v.len() != 34 {
            return Err(format!("bad line from {}: {line}", bin.display()));
        }
        let mut fpr = [0u64; 32];
        fpr.copy_from_slice(&v[2..]);
        got.push(Got {
            exc: v[0] == 15,
            fcr31: v[1] as u32,
            fpr,
        });
    }
    if got.len() != cases.len() {
        return Err(format!(
            "{} answered {} of {} cases",
            bin.display(),
            got.len(),
            cases.len()
        ));
    }
    Ok(got)
}

/// The tally of one run.
#[derive(Default)]
pub struct Tally {
    /// Cases compared.
    pub cases: u32,
    /// Cases that differ.
    pub differ: u32,
    /// The first differences.
    pub msgs: Vec<String>,
}

fn differences(c: &Case, g: &Got, or: &Out) -> Vec<String> {
    let mut d = Vec::new();
    let oexc = or.exc == Some(15);
    if or.exc.is_some() && !oexc {
        d.push(format!("oracle exception {:?}", or.exc));
    }
    if g.exc != oexc {
        d.push(format!("FPE: c {} oracle {oexc}", g.exc));
    }
    if g.fcr31 != or.fcr31 {
        d.push(format!("fcr31: c {:08x} oracle {:08x}", g.fcr31, or.fcr31));
    }
    for i in 0..32 {
        if g.fpr[i] != or.fpr[i] {
            d.push(format!(
                "f{i}: c {:#x} oracle {:#x} (was {:#x})",
                g.fpr[i], or.fpr[i], c.fpr[i]
            ));
        }
    }
    d
}

/// Runs the COP1 cases of `cases` (with their oracle outcomes) through the
/// C slow path at `bin`.
pub fn check(bin: &Path, cases: &[(&Case, &Out)]) -> Result<Tally, String> {
    let list: Vec<&Case> = cases.iter().map(|(c, _)| *c).collect();
    let got = run_bin(bin, &list)?;
    let mut t = Tally::default();
    for ((c, or), g) in cases.iter().zip(&got) {
        t.cases += 1;
        let d = differences(c, g, or);
        if !d.is_empty() {
            t.differ += 1;
            if t.msgs.len() < 8 {
                t.msgs.push(format!(
                    "{} w={:08x} fcr={:08x}: {}",
                    c.name,
                    c.words[0],
                    c.fcr31,
                    d.join("; ")
                ));
            }
        }
    }
    Ok(t)
}

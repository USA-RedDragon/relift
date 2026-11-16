//! The comparison of the IR run with the oracle.

use super::cases::Case;
use super::run::Out;

/// The "difference" of a block that left for the FPU slow path.
pub const SLOW: &str = "SLOWPATH";

/// The differences between the IR run and the oracle; a block that left for
/// the FPU slow path is not compared and gives [`SLOW`].
pub fn compare(c: &Case, ir: &Out, or: &Out) -> Vec<String> {
    let mut d = Vec::new();
    if ir.slow {
        d.push(SLOW.into());
        return d;
    }
    match (ir.exc, or.exc) {
        (Some(a), Some(b)) => {
            if a != b {
                d.push(format!("exception {a} vs oracle {b}"));
            } else if ir.delay != or.delay {
                d.push(format!(
                    "exception in delay slot: ir {}, oracle {}",
                    ir.delay, or.delay
                ));
            }
            return d;
        }
        (Some(a), None) => {
            d.push(format!("IR exception {a}, oracle none"));
            return d;
        }
        (None, Some(b)) => {
            d.push(format!("oracle exception {b}, IR none"));
            return d;
        }
        _ => {}
    }
    for i in 1..32 {
        if ir.gpr[i] != or.gpr[i] {
            d.push(format!(
                "r{i}: ir {:#x} oracle {:#x} (was {:#x})",
                ir.gpr[i], or.gpr[i], c.gpr[i]
            ));
        }
    }
    if ir.hi != or.hi {
        d.push(format!("hi: ir {:#x} oracle {:#x}", ir.hi, or.hi));
    }
    if ir.lo != or.lo {
        d.push(format!("lo: ir {:#x} oracle {:#x}", ir.lo, or.lo));
    }
    for i in 0..32 {
        if ir.fpr[i] != or.fpr[i] {
            d.push(format!(
                "f{i}: ir {:#x} oracle {:#x} (was {:#x})",
                ir.fpr[i], or.fpr[i], c.fpr[i]
            ));
        }
    }
    if ir.fcr31 != or.fcr31 {
        d.push(format!("fcr31: ir {:#x} oracle {:#x}", ir.fcr31, or.fcr31));
    }
    for (k, v) in &ir.cop0 {
        let o = or.cop0.get(k).copied().unwrap_or(0);
        if *v != o {
            d.push(format!("cop0.{k}: ir {v:#x} oracle {o:#x}"));
        }
    }
    if ir.data != or.data {
        d.push("memory differs".into());
    }
    if ir.pc != or.pc {
        d.push(format!("pc: ir {:#x} oracle {:#x}", ir.pc, or.pc));
    }
    d
}

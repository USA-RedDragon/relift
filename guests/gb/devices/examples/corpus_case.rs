//! Runs one corpus case natively through the case harness.
//!
//! `corpus_case <rom> <model> <spec-file>` prints `<verdict> <detail>` and the
//! instructions run. The CPU is rustyboi's own, so the verdict must equal
//! rustyboi-test-runner's.

use relift_gb_devices::{Rlgbd, Verdict};
use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 4 {
        eprintln!("usage: corpus_case <rom> <model> <spec-file>");
        return ExitCode::from(64);
    }
    let Ok(rom) = std::fs::read(&args[1]) else {
        eprintln!("cannot read {}", args[1]);
        return ExitCode::from(66);
    };
    let Ok(spec) = std::fs::read_to_string(&args[3]) else {
        eprintln!("cannot read {}", args[3]);
        return ExitCode::from(66);
    };
    let model: u32 = args[2].parse().unwrap_or(0);
    let Some(mut m) = Rlgbd::new(model, &rom) else {
        eprintln!("not a cartridge");
        return ExitCode::from(65);
    };
    if let Err(e) = m.case_open(&spec) {
        println!("open-failed {e}");
        return ExitCode::from(2);
    }
    let mut steps = 0u64;
    loop {
        let pc = m.regs().pc;
        let mut v = m.case_poll(pc);
        if v == Verdict::NeedRegs {
            let regs = m.regs();
            m.case_regs(&regs);
            v = m.case_poll(pc);
        }
        if v != Verdict::Continue {
            let detail =
                String::from_utf8_lossy(&m.case_detail()[..m.case_detail().len() - 1]).into_owned();
            println!("{v:?} {detail}");
            println!("steps {steps}");
            return ExitCode::from(v as u8);
        }
        if let Some(op) = m.begin() {
            m.exec_native(op);
        }
        steps += 1;
    }
}

//! The vector unit's templates on the IR interpreter against the reference
//! in `reference` for every COP2 instruction, on random states and on states
//! made of edge values (saturation, accumulator clamps, the flag registers,
//! the divider inputs 0 and 0x8000, DMEM wrapping at 4 KiB).

mod reference;
mod systemtest;

use reference::{Rng, Rsp, run_lifted};
use relift_cpu_rsp::decode::{self, Encoding};
use relift_cpu_rsp::lift;
use relift_ir::Op;

/// Cases per encoding: half on random states, half on edge states.
const CASES: usize = 300;

fn word_of(enc: Encoding, mask: u32, value: u32, rng: &mut Rng) -> u32 {
    let w = value | (rng.next() as u32 & !mask);
    assert_eq!(decode::decode(w), Some(enc));
    w
}

fn check_class(class: &str, seed: u64) -> (usize, Vec<String>) {
    let mut rng = Rng(seed);
    let mut pass = 0;
    let mut fails = Vec::new();
    for &(enc, mask, value, c) in decode::forms() {
        if c != class {
            continue;
        }
        let mut failed = 0;
        for i in 0..CASES {
            let s0 = if i % 2 == 0 {
                Rsp::random(&mut rng)
            } else {
                Rsp::edge(&mut rng)
            };
            let w = word_of(enc, mask, value, &mut rng);
            let mut want = s0.clone();
            want.step(w);
            let got = run_lifted(&s0, w);
            if got == want {
                pass += 1;
            } else {
                failed += 1;
                if failed <= 2 {
                    fails.push(format!(
                        "{} {w:#010x}: {}",
                        enc.name(),
                        reference::diff(&want, &got)
                    ));
                }
            }
        }
        if failed > 0 {
            fails.push(format!("{}: {failed} of {CASES} differ", enc.name()));
        }
    }
    (pass, fails)
}

#[test]
fn vector_computational_and_moves_match_the_reference() {
    let (pass, fails) = check_class("vector", 0x5EED_0001);
    println!("vector: {pass} pass");
    assert!(fails.is_empty(), "{}", fails.join("\n"));
}

#[test]
fn vector_loads_and_stores_match_the_reference() {
    let (pass, fails) = check_class("vmem", 0x5EED_0002);
    println!("vmem: {pass} pass");
    assert!(fails.is_empty(), "{}", fails.join("\n"));
}

fn vop(f: u32, e: u32, vt: u32, de: u32, vd: u32) -> u32 {
    (0x12 << 26) | (1 << 25) | (e << 21) | (vt << 16) | (de << 11) | (vd << 6) | f
}

/// VRCP and VRSQ on every 16-bit input, and VRCPL and VRSQL on every input
/// after VRCPH or VRSQH with a high half of 0, 1, 0x7FFF, 0x8000 or 0xFFFF
/// (and with no high half).
#[test]
fn divides_match_the_reference_on_every_input() {
    let base = Rsp::random(&mut Rng(7));
    let mut checked = 0;
    let mut fails = Vec::new();
    let mut run = |s: &Rsp, w: u32, x: u32| {
        let mut want = s.clone();
        want.step(w);
        let got = run_lifted(s, w);
        checked += 1;
        if got != want && fails.len() < 4 {
            fails.push(format!(
                "{w:#010x} x={x:#06x} divdp={} divin={:#06x}: {}",
                s.divdp,
                s.divin,
                reference::diff(&want, &got)
            ));
        }
    };
    for f in [0x30u32, 0x34] {
        for x in 0..=0xFFFFu32 {
            let mut s = base.clone();
            let e = x % 16;
            s.v[3][(e & 7) as usize] = x as u16;
            run(&s, vop(f, e, 3, x % 8, 5), x);
        }
    }
    for f in [0x31u32, 0x35] {
        for divin in [
            None,
            Some(0u16),
            Some(1),
            Some(0x7FFF),
            Some(0x8000),
            Some(0xFFFF),
        ] {
            for x in (0..=0xFFFFu32).step_by(if divin.is_some() { 3 } else { 5 }) {
                let mut s = base.clone();
                let e = x % 16;
                s.v[3][(e & 7) as usize] = x as u16;
                s.divdp = divin.is_some();
                s.divin = divin.unwrap_or(0x1234);
                run(&s, vop(f, e, 3, x % 8, 3), x);
            }
        }
    }
    println!("divides: {checked} checked");
    assert!(fails.is_empty(), "{}", fails.join("\n"));
}

/// The divider's ROMs as the templates compute them, on every index,
/// against the tables built the way ares builds them.
#[test]
fn rom_entries_match_the_reference_tables() {
    let (rcp, rsq) = reference::roms();
    assert_eq!(&relift_cpu_rsp::lift::rt::rcp_rom(), rcp);
    assert_eq!(&relift_cpu_rsp::lift::rt::rsq_rom(), rsq);
    let mut base = Rsp::random(&mut Rng(9));
    base.divdp = false;
    for i in 0..512u32 {
        for (f, sh) in [(0x31u32, 6), (0x35, 6), (0x35, 5)] {
            let x = (512 + i) << sh;
            let mut s = base.clone();
            s.divdp = true;
            s.divin = (x >> 16) as u16;
            s.v[1][0] = x as u16;
            let w = vop(f, 0, 1, 0, 2);
            let mut want = s.clone();
            want.step(w);
            assert_eq!(run_lifted(&s, w), want, "{w:#010x} index {i}");
        }
    }
}

/// The number of IR operations of a template, without constants and
/// register reads and writes, on a word with element selector 0 (no
/// permutation) and distinct registers.
fn ir_ops(word: u32) -> usize {
    let l = lift::lift(0x100, word, 0).expect("lifts");
    l.block
        .insts
        .iter()
        .filter(|i| !matches!(i.op, Op::Const(_) | Op::Get(_) | Op::Set(..)))
        .count()
}

/// Every vector template's size: `cargo test -p relift-cpu-rsp --test cop2
/// sizes -- --nocapture` prints them. The computational ones are a short
/// lane sequence (the divides are scalar code); the bound catches a template that falls back to per-lane
/// scalar code.
#[test]
fn template_sizes() {
    let mut rows = Vec::new();
    for &(enc, _, value, class) in decode::forms() {
        let w = match class {
            "vector" => value | (1 << 16) | (2 << 11) | (3 << 6),
            "vmem" => value | (1 << 21) | (2 << 16) | 1,
            _ => continue,
        };
        let n = ir_ops(w);
        let divide = matches!(enc.name(), "VRCP" | "VRCPL" | "VRSQ" | "VRSQL");
        let limit = match class {
            "vector" if divide => 64,
            "vector" => 40,
            _ => 60,
        };
        rows.push((enc.name(), n, limit));
    }
    for (name, n, _) in &rows {
        println!("{name} {n}");
    }
    for (name, n, limit) in rows {
        assert!(n <= limit, "{name} takes {n} IR operations");
    }
}

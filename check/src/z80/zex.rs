//! `--oracle zex FILE.com`: Frank Cringle's ZEXDOC or ZEXALL exerciser run
//! on the lifted instructions with a CP/M stub. The .com is loaded at 0x100,
//! `HALT` is at 0 and a `RET` at the BDOS entry 5; BDOS functions 2 (print
//! E) and 9 (print the string at DE up to `$`) are handled in the run loop.
//! The exerciser is GPL-2: it is test data only, fetched by the caller and
//! never part of relift.

use std::io::Write as _;

use relift_cpu_z80::regs;

use super::cpu::Cpu;

/// What a run gave.
pub struct Summary {
    pub instructions: u64,
    pub clock: u64,
    pub blocks: usize,
    pub ok: usize,
    pub error: usize,
}

/// Runs `com`, writing the exerciser's output to stdout and progress to
/// stderr.
pub fn run_com(com: &[u8], progress: bool) -> Result<Summary, String> {
    if com.len() > 0xF000 - 0x100 {
        return Err("the .com does not fit".into());
    }
    let mut cpu = Cpu::new();
    cpu.use_cache = true;
    {
        let mut b = cpu.bus.borrow_mut();
        b.logging = false;
        b.mem[0x100..0x100 + com.len()].copy_from_slice(com);
        b.mem[0] = 0x76;
        b.mem[5] = 0xC9;
        b.mem[6] = 0x00;
        b.mem[7] = 0xF0;
    }
    cpu.pc = 0x100;
    cpu.set(regs::SP, 0xF000);
    let start = std::time::Instant::now();
    let mut n: u64 = 0;
    let mut text = String::new();
    let out = std::io::stdout();
    while cpu.pc != 0 {
        if cpu.pc == 5 {
            let mut o = out.lock();
            let b = cpu.bus.borrow();
            let mut emit = |ch: char| {
                text.push(ch);
                let _ = write!(o, "{ch}");
            };
            match cpu.r(regs::C) {
                2 => emit(cpu.r(regs::E) as u8 as char),
                9 => {
                    let mut a = (cpu.r(regs::D) << 8 | cpu.r(regs::E)) as u16;
                    while b.mem[usize::from(a)] != b'$' {
                        emit(b.mem[usize::from(a)] as char);
                        a = a.wrapping_add(1);
                    }
                }
                _ => {}
            }
            let _ = o.flush();
            let sp = cpu.r(regs::SP) as u16;
            let lo = u16::from(b.mem[usize::from(sp)]);
            let hi = u16::from(b.mem[usize::from(sp.wrapping_add(1))]);
            drop(b);
            cpu.set(regs::SP, u64::from(sp.wrapping_add(2)));
            cpu.pc = hi << 8 | lo;
            continue;
        }
        let at = cpu.pc;
        cpu.step().map_err(|e| format!("at {at:#06x}: {e}"))?;
        n += 1;
        if progress && n.is_multiple_of(1 << 27) {
            let el = start.elapsed().as_secs_f64();
            eprintln!(
                "[zex] {n} instructions, {el:.0} s, {:.1} M/s",
                n as f64 / el / 1e6
            );
        }
    }
    let clock = cpu.bus.borrow().clock;
    Ok(Summary {
        instructions: n,
        clock,
        blocks: cpu.distinct_blocks(),
        ok: text.matches(" OK").count(),
        error: text.matches("ERROR").count(),
    })
}

/// Runs the .com at `path` and prints the group counts. Gives whether
/// every group passed.
pub fn run(path: &str) -> Result<bool, String> {
    let com = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
    let start = std::time::Instant::now();
    let s = run_com(&com, true)?;
    println!(
        "\n[zex] {path}: {} instructions, {} T-states, {:.0} s, {} distinct lifted blocks, groups OK {}, ERROR {}",
        s.instructions,
        s.clock,
        start.elapsed().as_secs_f64(),
        s.blocks,
        s.ok,
        s.error
    );
    Ok(s.error == 0 && s.ok > 0)
}

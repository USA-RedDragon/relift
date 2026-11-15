//! Lifts and runs one instruction per input line on the IR interpreter,
//! for checking the templates against a reference model.
//!
//! Input lines are hex words separated by spaces: the Thumb bit, the
//! instruction (a Thumb BL pair as the first halfword in the low half and
//! the second in the high half), 1 for a BL pair, the instruction address,
//! the scratch base, the pool base, the 51 register-file slots in
//! `regs::regs()` order, then the 64 scratch bytes and the 192 pool bytes
//! as one hex string each. Scratch is read-write, the pool read-only, and
//! any other access faults.
//!
//! Output lines: `ok NEXT SLOT... SCRATCH` (hex, the condition-failed exit
//! included as an instruction with no effect), or `err WHY`.

use std::io::{BufRead, Write};

use relift_cpu_arm32::lift::{self, COND_FAIL};
use relift_cpu_arm32::regs;
use relift_ir::interp::{self, Fault, GuestMemory, NoIntrinsics, State};
use relift_ir::{Flow, simplify};

struct Mem {
    scratch: (u64, Vec<u8>),
    pool: (u64, Vec<u8>),
}

impl Mem {
    fn find(&mut self, addr: u64, n: usize, write: bool) -> Result<&mut [u8], Fault> {
        let fault = Fault {
            addr,
            size: n as u32,
            write,
        };
        let regions: [(&mut (u64, Vec<u8>), bool); 2] =
            [(&mut self.scratch, true), (&mut self.pool, false)];
        for (r, writable) in regions {
            if write && !writable {
                continue;
            }
            let off = match addr.checked_sub(r.0) {
                Some(o) => o as usize,
                None => continue,
            };
            if off + n <= r.1.len() {
                return Ok(&mut r.1[off..off + n]);
            }
        }
        Err(fault)
    }
}

impl GuestMemory for Mem {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault> {
        let s = self.find(addr, buf.len(), false)?;
        buf.copy_from_slice(s);
        Ok(())
    }

    fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), Fault> {
        let s = self.find(addr, data.len(), true)?;
        s.copy_from_slice(data);
        Ok(())
    }
}

fn bytes(hex: &str) -> Result<Vec<u8>, String> {
    (0..hex.len() / 2)
        .map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).map_err(|e| e.to_string()))
        .collect()
}

fn word(t: &str) -> Result<u32, String> {
    u32::from_str_radix(t, 16).map_err(|e| format!("{t}: {e}"))
}

/// Runs the instruction at `pc` once; gives the next PC.
fn step(word: u32, pc: u32, thumb: bool, st: &mut State, mem: &mut Mem) -> Result<u32, String> {
    let l = lift::lift(word, pc, thumb).map_err(|e| e.to_string())?;
    let block = simplify(&l.block);
    let out = interp::run(&block, st, mem, &mut NoIntrinsics)
        .map_err(|e| format!("{}: {e}", l.enc.name()))?;
    Ok(match out.flow {
        Flow::Trap(COND_FAIL) => pc.wrapping_add(if thumb { 2 } else { 4 }),
        Flow::Trap(c) => return Err(format!("{}: trap {c:#x}", l.enc.name())),
        _ => out.next as u32,
    })
}

fn line(l: &str) -> Result<String, String> {
    let t: Vec<&str> = l.split_whitespace().collect();
    let slots = regs::regs().len();
    if t.len() != 6 + slots + 2 {
        return Err(format!("{} fields, expected {}", t.len(), 8 + slots));
    }
    let thumb = word(t[0])? != 0;
    let insn = word(t[1])?;
    let pair = word(t[2])? != 0;
    let pc = word(t[3])?;
    let mut mem = Mem {
        scratch: (u64::from(word(t[4])?), bytes(t[6 + slots])?),
        pool: (u64::from(word(t[5])?), bytes(t[7 + slots])?),
    };
    let mut st = State::new(slots);
    for (i, v) in t[6..6 + slots].iter().enumerate() {
        st.regs[i] = u128::from(word(v)?);
    }
    let next = if pair {
        step(insn & 0xffff, pc, true, &mut st, &mut mem)?;
        step(insn >> 16, pc.wrapping_add(2), true, &mut st, &mut mem)?
    } else {
        step(insn, pc, thumb, &mut st, &mut mem)?
    };
    let mut out = format!("ok {next:x}");
    for v in &st.regs {
        out.push_str(&format!(" {v:x}"));
    }
    out.push(' ');
    for b in &mem.scratch.1 {
        out.push_str(&format!("{b:02x}"));
    }
    Ok(out)
}

fn main() {
    let stdin = std::io::stdin();
    let mut out = std::io::BufWriter::new(std::io::stdout());
    for l in stdin.lock().lines() {
        let l = l.expect("reads stdin");
        let r = match line(&l) {
            Ok(s) => s,
            Err(e) => format!("err {e}"),
        };
        writeln!(out, "{r}").expect("writes stdout");
    }
}

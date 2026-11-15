//! The GBA oracle: NanoBoyAdvance run as a separate process
//! (`check/oracle/nba`, GPL-3.0-or-later, never linked into relift), driven
//! over the line protocol in `check/oracle/nba/README.md`.

pub mod cli;
pub mod game;
#[cfg(test)]
mod tests;

use std::collections::BTreeMap;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, ChildStdout, Stdio};

/// An oracle failure.
#[derive(Debug)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

/// How the oracle process starts.
#[derive(Clone, Debug, Default)]
pub struct Options {
    pub rom: PathBuf,
    pub bios: Option<PathBuf>,
    pub skip_bios: bool,
    pub save: Option<String>,
    pub save_file: Option<PathBuf>,
}

/// The registers after a command, by the names of the protocol's `REGS`
/// line (`r0`..`r15`, `cpsr`, `spsr_irq`, `r13_svc`, ...), with the address
/// of the next instruction, the halt state and the cycle count.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Regs {
    pub named: BTreeMap<String, u32>,
    pub pc: u32,
    pub halted: bool,
    pub cycles: u64,
}

/// One line of a `STEP` trace.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    /// An instruction ran; the registers are those after it.
    Insn {
        cycles: u64,
        pc: u32,
        opcode: u32,
        regs: [u32; 16],
        cpsr: u32,
    },
    /// An interrupt was taken before the next instruction at `pc`.
    Irq { cycles: u64, pc: u32 },
    /// The CPU was halted until `cycles`.
    Halt { cycles: u64 },
}

/// A frame or audio digest: how many there were and the FNV-1a 64 hash.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Digest {
    pub count: u64,
    pub hash: u64,
}

/// The cycle count and frame count after a run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Clock {
    pub cycles: u64,
    pub frames: u64,
}

/// The oracle binary: `RELIFT_NBA_ORACLE`, else
/// `~/.cache/relift/gba-oracle/bin/nba-oracle` when it exists.
pub fn binary() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("RELIFT_NBA_ORACLE") {
        return Some(PathBuf::from(p));
    }
    let home = std::env::var_os("HOME")?;
    let p = Path::new(&home).join(".cache/relift/gba-oracle/bin/nba-oracle");
    p.exists().then_some(p)
}

fn hex(s: &str) -> Result<u64, Error> {
    u64::from_str_radix(s, 16).map_err(|_| Error(format!("bad hex {s}")))
}

fn dec(s: &str) -> Result<u64, Error> {
    s.parse().map_err(|_| Error(format!("bad number {s}")))
}

fn field<'a>(line: &'a str, key: &str) -> Result<&'a str, Error> {
    line.split_whitespace()
        .find_map(|w| w.strip_prefix(key).and_then(|r| r.strip_prefix('=')))
        .ok_or_else(|| Error(format!("no {key}= in {line}")))
}

fn bytes(s: &str) -> Result<Vec<u8>, Error> {
    if !s.len().is_multiple_of(2) {
        return Err(Error("odd hex length".into()));
    }
    (0..s.len() / 2)
        .map(|i| {
            u8::from_str_radix(&s[2 * i..2 * i + 2], 16)
                .map_err(|_| Error(format!("bad hex byte at {i}")))
        })
        .collect()
}

pub fn parse_regs(line: &str) -> Result<Regs, Error> {
    let rest = line
        .strip_prefix("REGS ")
        .ok_or_else(|| Error(format!("expected REGS, got {line}")))?;
    let mut r = Regs::default();
    for w in rest.split_whitespace() {
        let (k, v) = w
            .split_once('=')
            .ok_or_else(|| Error(format!("bad field {w}")))?;
        match k {
            "pc" => r.pc = hex(v)? as u32,
            "halt" => r.halted = v == "1",
            "cyc" => r.cycles = dec(v)?,
            _ => {
                r.named.insert(k.to_string(), hex(v)? as u32);
            }
        }
    }
    Ok(r)
}

pub fn parse_event(line: &str) -> Result<Event, Error> {
    let w: Vec<&str> = line.split_whitespace().collect();
    match w.first().copied() {
        Some("T") if w.len() == 21 => {
            let mut regs = [0u32; 16];
            for (i, r) in regs.iter_mut().enumerate() {
                *r = hex(w[4 + i])? as u32;
            }
            Ok(Event::Insn {
                cycles: dec(w[1])?,
                pc: hex(w[2])? as u32,
                opcode: hex(w[3])? as u32,
                regs,
                cpsr: hex(w[20])? as u32,
            })
        }
        Some("I") if w.len() == 3 => Ok(Event::Irq {
            cycles: dec(w[1])?,
            pc: hex(w[2])? as u32,
        }),
        Some("H") if w.len() == 2 => Ok(Event::Halt { cycles: dec(w[1])? }),
        _ => Err(Error(format!("bad trace line {line}"))),
    }
}

fn parse_clock(line: &str) -> Result<Clock, Error> {
    Ok(Clock {
        cycles: dec(field(line, "cyc")?)?,
        frames: dec(field(line, "frames")?)?,
    })
}

fn parse_digest(line: &str) -> Result<Digest, Error> {
    let count = match field(line, "count") {
        Ok(v) => v,
        Err(_) => field(line, "samples")?,
    };
    Ok(Digest {
        count: dec(count)?,
        hash: hex(field(line, "hash")?)?,
    })
}

/// A running oracle process.
pub struct Nba {
    child: Child,
    input: ChildStdin,
    output: BufReader<ChildStdout>,
}

impl Nba {
    pub fn spawn(bin: &Path, o: &Options) -> Result<Self, Error> {
        let mut c = relift_proc::command(bin);
        c.arg("--rom").arg(&o.rom);
        if let Some(b) = &o.bios {
            c.arg("--bios").arg(b);
        }
        if o.skip_bios {
            c.arg("--skip-bios");
        }
        if let Some(s) = &o.save {
            c.arg("--save").arg(s);
        }
        if let Some(f) = &o.save_file {
            c.arg("--save-file").arg(f);
        }
        let mut child = c
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()
            .map_err(|e| Error(format!("{}: {e}", bin.display())))?;
        let input = child.stdin.take().ok_or(Error("no stdin".into()))?;
        let output = BufReader::new(child.stdout.take().ok_or(Error("no stdout".into()))?);
        let mut n = Nba {
            child,
            input,
            output,
        };
        let hello = n.line()?;
        if !hello.starts_with("READY ") {
            return Err(Error(format!("oracle did not start: {hello}")));
        }
        Ok(n)
    }

    fn line(&mut self) -> Result<String, Error> {
        let mut s = String::new();
        let n = self
            .output
            .read_line(&mut s)
            .map_err(|e| Error(e.to_string()))?;
        if n == 0 {
            return Err(Error("oracle closed its output".into()));
        }
        let s = s.trim_end().to_string();
        match s.strip_prefix("ERR ") {
            Some(e) => Err(Error(e.to_string())),
            None => Ok(s),
        }
    }

    fn send(&mut self, cmd: &str) -> Result<(), Error> {
        writeln!(self.input, "{cmd}")
            .and_then(|_| self.input.flush())
            .map_err(|e| Error(e.to_string()))
    }

    pub fn command(&mut self, cmd: &str) -> Result<String, Error> {
        self.send(cmd)?;
        self.line()
    }

    fn ok(&mut self, cmd: &str) -> Result<String, Error> {
        let r = self.command(cmd)?;
        if r == "OK" || r.starts_with("OK ") {
            Ok(r)
        } else {
            Err(Error(format!("{cmd}: unexpected {r}")))
        }
    }

    fn data(&mut self, cmd: &str, tag: &str) -> Result<Vec<u8>, Error> {
        let r = self.command(cmd)?;
        let rest = r
            .strip_prefix(tag)
            .and_then(|r| r.strip_prefix(' '))
            .ok_or_else(|| Error(format!("{cmd}: unexpected {r}")))?;
        bytes(rest)
    }

    pub fn run_cycles(&mut self, n: u64) -> Result<Clock, Error> {
        parse_clock(&self.ok(&format!("RUN cycles {n}"))?)
    }

    pub fn run_frames(&mut self, n: u64) -> Result<Clock, Error> {
        parse_clock(&self.ok(&format!("RUN frames {n}"))?)
    }

    /// Runs until the next instruction is at `pc`, at most `limit` cycles.
    pub fn run_until(&mut self, pc: u32, limit: u64) -> Result<Clock, Error> {
        parse_clock(&self.ok(&format!("RUN until {limit} {pc:x}"))?)
    }

    /// Runs `n` instructions (a halt counts as one) and returns the trace.
    pub fn step(&mut self, n: u64) -> Result<Vec<Event>, Error> {
        self.send(&format!("STEP {n}"))?;
        let mut out = Vec::new();
        loop {
            let l = self.line()?;
            if l.starts_with("OK") {
                return Ok(out);
            }
            out.push(parse_event(&l)?);
        }
    }

    /// Holds the keys in `mask` (KEYINPUT bit order, 1 = pressed).
    pub fn keys(&mut self, mask: u16) -> Result<(), Error> {
        self.ok(&format!("KEYS {mask:x}")).map(|_| ())
    }

    pub fn regs(&mut self) -> Result<Regs, Error> {
        parse_regs(&self.command("REGS")?)
    }

    pub fn set(&mut self, regs: &[(&str, u32)]) -> Result<(), Error> {
        let mut cmd = "SET".to_string();
        for (k, v) in regs {
            cmd += &format!(" {k}={v:x}");
        }
        self.ok(&cmd).map(|_| ())
    }

    /// Moves execution to `pc` in ARM or Thumb state and refills the pipeline.
    pub fn jump(&mut self, pc: u32, thumb: bool) -> Result<(), Error> {
        self.ok(&format!("JUMP {pc:x} {}", u8::from(thumb)))
            .map(|_| ())
    }

    /// Reads memory without side effects or bus timing.
    pub fn mem(&mut self, addr: u32, len: u32) -> Result<Vec<u8>, Error> {
        self.data(&format!("MEM {addr:x} {len:x}"), "MEM")
    }

    pub fn poke(&mut self, addr: u32, data: &[u8]) -> Result<(), Error> {
        let h: String = data.iter().map(|b| format!("{b:02x}")).collect();
        self.ok(&format!("POKE {addr:x} {h}")).map(|_| ())
    }

    /// The I/O registers 0x04000000..0x04000400 as the CPU reads them.
    pub fn io(&mut self) -> Result<Vec<u8>, Error> {
        self.data("IO", "IO")
    }

    /// The last frame as BGR555 pixels, row by row.
    pub fn pixels(&mut self) -> Result<Vec<u16>, Error> {
        let b = self.data("PIXELS", "PIXELS")?;
        Ok(b.chunks(2)
            .map(|c| u16::from_le_bytes([c[0], c[1]]))
            .collect())
    }

    pub fn frame(&mut self) -> Result<Digest, Error> {
        parse_digest(&self.command("FRAME")?)
    }

    pub fn audio(&mut self) -> Result<Digest, Error> {
        parse_digest(&self.command("AUDIO")?)
    }

    pub fn save(&mut self, path: &Path) -> Result<(), Error> {
        self.ok(&format!("SAVE {}", path.display())).map(|_| ())
    }
}

impl Drop for Nba {
    fn drop(&mut self) {
        if self.send("QUIT").is_ok() {
            let _ = self.line();
        }
        let _ = self.child.wait();
    }
}

//! The machines a lifted block is compared against: Unicorn in this
//! process, or the native harness (`check/oracle/harness.c`) on a real
//! AArch64 CPU or under QEMU user mode, reached through a child process
//! (`qemu-aarch64 ...`, `ssh host ...`) that reads requests on stdin and
//! writes results on stdout.
//!
//! The protocol is little-endian. A request is the magic `RLOQ`, the word
//! count, the words, the 800-byte state (`x0`-`x30`, SP, PC, NZCV in bits
//! 31 to 28, FPCR, FPSR, padding, `v0`-`v31`) and the data window: kind 0
//! and a 64-bit seed for [`fill_window`], or kind 1 and the window's bytes.
//! A result is the magic `RLOR`, a status (0 ran, 1 faulted, 2 refused, 3
//! no slow path), the
//! signal number, padding, the fault address, the state after the run and
//! the bytes of the window that changed, as (offset, length, bytes) ranges.

use std::io::{BufReader, BufWriter, Read, Write};
use std::process::{Child, ChildStdin, ChildStdout, Stdio};
use std::sync::mpsc;
use std::thread;

use unicorn_engine::unicorn_const::uc_error;

use crate::emu::{DATA_SIZE, Emu, Guest, Outcome};

/// One run: the words, from `CODE`, on a state.
#[derive(Clone, Debug)]
pub struct Case {
    /// The instruction words.
    pub words: Vec<u32>,
    /// The input state.
    pub input: Guest,
}

/// A machine that runs cases.
pub trait Oracle {
    /// The name in reports.
    fn name(&self) -> &str;
    /// The outcome of every case, in order.
    fn run(&mut self, cases: &[Case]) -> Result<Vec<Outcome>, String>;
}

/// The data window's contents for `seed`: splitmix64, eight bytes a step.
/// `check/oracle/harness.c` computes the same bytes.
pub fn fill_window(seed: u64) -> Vec<u8> {
    let mut s = seed;
    let mut out = Vec::with_capacity(DATA_SIZE);
    while out.len() < DATA_SIZE {
        s = s.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = s;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^= z >> 31;
        out.extend_from_slice(&z.to_le_bytes());
    }
    out
}

/// Unicorn, in this process.
pub struct Unicorn(Emu);

impl Unicorn {
    /// A fresh Unicorn oracle.
    pub fn new() -> Result<Unicorn, uc_error> {
        Ok(Unicorn(Emu::new()?))
    }
}

impl Oracle for Unicorn {
    fn name(&self) -> &str {
        "unicorn"
    }

    fn run(&mut self, cases: &[Case]) -> Result<Vec<Outcome>, String> {
        Ok(cases
            .iter()
            .map(|c| match self.0.run(&c.words, &c.input) {
                Ok(o) => o,
                Err(e) => Outcome::Fault(format!("unicorn: {e:?}")),
            })
            .collect())
    }
}

/// The native harness behind a child process.
pub struct Native {
    name: String,
    child: Child,
    tx: Option<mpsc::Sender<Vec<u8>>>,
    writer: Option<thread::JoinHandle<std::io::Result<()>>>,
    out: BufReader<ChildStdout>,
}

const REQ_MAGIC: u32 = 0x514f_4c52;
const RES_MAGIC: u32 = 0x524f_4c52;
/// The most words one request may carry.
pub const MAX_WORDS: usize = 64;

fn put_state(b: &mut Vec<u8>, g: &Guest) {
    for x in g.x {
        b.extend_from_slice(&x.to_le_bytes());
    }
    b.extend_from_slice(&g.sp.to_le_bytes());
    b.extend_from_slice(&0u64.to_le_bytes());
    let nzcv = g
        .nzcv
        .iter()
        .enumerate()
        .fold(0u32, |acc, (i, f)| acc | (u32::from(*f) << (31 - i)));
    b.extend_from_slice(&nzcv.to_le_bytes());
    b.extend_from_slice(&(g.fpcr as u32).to_le_bytes());
    b.extend_from_slice(&g.fpsr.to_le_bytes());
    b.extend_from_slice(&[0; 12]);
    for v in g.v {
        b.extend_from_slice(&v.to_le_bytes());
    }
}

fn request(c: &Case) -> Vec<u8> {
    let mut b = Vec::with_capacity(900);
    b.extend_from_slice(&REQ_MAGIC.to_le_bytes());
    b.extend_from_slice(&(c.words.len() as u32).to_le_bytes());
    for w in &c.words {
        b.extend_from_slice(&w.to_le_bytes());
    }
    put_state(&mut b, &c.input);
    match c.input.mem_seed {
        Some(s) => {
            b.extend_from_slice(&0u32.to_le_bytes());
            b.extend_from_slice(&s.to_le_bytes());
        }
        None => {
            b.extend_from_slice(&1u32.to_le_bytes());
            b.extend_from_slice(&c.input.mem);
        }
    }
    b
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().expect("4 bytes"))
}

fn u64_at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().expect("8 bytes"))
}

impl Native {
    /// Starts `argv` as the harness.
    pub fn spawn(name: &str, argv: &[String]) -> Result<Native, String> {
        let (prog, args) = argv.split_first().ok_or("empty oracle command")?;
        let mut child = relift_proc::command(prog)
            .args(args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()
            .map_err(|e| format!("{prog}: {e}"))?;
        let stdin: ChildStdin = child.stdin.take().ok_or("no stdin")?;
        let out = BufReader::with_capacity(1 << 20, child.stdout.take().ok_or("no stdout")?);
        let (tx, rx) = mpsc::channel::<Vec<u8>>();
        let writer = thread::spawn(move || -> std::io::Result<()> {
            let mut w = BufWriter::with_capacity(1 << 20, stdin);
            while let Ok(first) = rx.recv() {
                w.write_all(&first)?;
                while let Ok(more) = rx.try_recv() {
                    w.write_all(&more)?;
                }
                w.flush()?;
            }
            Ok(())
        });
        Ok(Native {
            name: name.to_string(),
            child,
            tx: Some(tx),
            writer: Some(writer),
            out,
        })
    }

    fn read_result(&mut self, input: &Guest) -> Result<Outcome, String> {
        let mut h = [0u8; 24];
        self.out
            .read_exact(&mut h)
            .map_err(|e| format!("{}: {e}", self.name))?;
        if u32_at(&h, 0) != RES_MAGIC {
            return Err(format!("{}: bad result header", self.name));
        }
        let (status, sig, addr) = (u32_at(&h, 4), u32_at(&h, 8), u64_at(&h, 16));
        let mut s = [0u8; 800];
        self.out
            .read_exact(&mut s)
            .map_err(|e| format!("{}: {e}", self.name))?;
        let mut n = [0u8; 4];
        self.out
            .read_exact(&mut n)
            .map_err(|e| format!("{}: {e}", self.name))?;
        let mut mem = match input.mem_seed {
            Some(seed) => fill_window(seed),
            None => input.mem.clone(),
        };
        for _ in 0..u32::from_le_bytes(n) {
            let mut r = [0u8; 8];
            self.out
                .read_exact(&mut r)
                .map_err(|e| format!("{}: {e}", self.name))?;
            let (off, len) = (u32_at(&r, 0) as usize, u32_at(&r, 4) as usize);
            if off + len > DATA_SIZE {
                return Err(format!("{}: bad window range", self.name));
            }
            self.out
                .read_exact(&mut mem[off..off + len])
                .map_err(|e| format!("{}: {e}", self.name))?;
        }
        match status {
            0 => {}
            2 => return Ok(Outcome::Fault("refused (a system call)".to_string())),
            3 => {
                return Ok(Outcome::Fault(
                    "no slow path for the instruction".to_string(),
                ));
            }
            _ => {
                return Ok(Outcome::Fault(format!(
                    "signal {sig} at pc {:#x}, address {addr:#x}",
                    u64_at(&s, 256)
                )));
            }
        }
        let mut g = Guest {
            sp: u64_at(&s, 248),
            pc: u64_at(&s, 256),
            fpcr: u64::from(u32_at(&s, 268)),
            fpsr: u32_at(&s, 272),
            mem,
            mem_seed: None,
            ..input.clone()
        };
        for (i, x) in g.x.iter_mut().enumerate() {
            *x = u64_at(&s, 8 * i);
        }
        let nzcv = u32_at(&s, 264);
        for (i, f) in g.nzcv.iter_mut().enumerate() {
            *f = (nzcv >> (31 - i)) & 1 == 1;
        }
        for (i, v) in g.v.iter_mut().enumerate() {
            let o = 288 + 16 * i;
            *v = u128::from_le_bytes(s[o..o + 16].try_into().expect("16 bytes"));
        }
        Ok(Outcome::Done(Box::new(g)))
    }
}

impl Oracle for Native {
    fn name(&self) -> &str {
        &self.name
    }

    fn run(&mut self, cases: &[Case]) -> Result<Vec<Outcome>, String> {
        let tx = self.tx.as_ref().ok_or("the harness is closed")?;
        for c in cases {
            if c.words.is_empty() || c.words.len() > MAX_WORDS {
                return Err(format!("{} words in one case", c.words.len()));
            }
            tx.send(request(c))
                .map_err(|_| format!("{}: the harness stopped", self.name))?;
        }
        cases.iter().map(|c| self.read_result(&c.input)).collect()
    }
}

impl Drop for Native {
    fn drop(&mut self) {
        self.tx = None;
        if let Some(w) = self.writer.take() {
            let _ = w.join();
        }
        let _ = self.child.wait();
    }
}

/// The Apple M2's features, as its `hw.optional.arm` sysctl lists them.
pub const M2_FEATURES: &[&str] = &[
    "FP", "AdvSIMD", "CRC32", "FlagM", "FlagM2", "FHM", "DotProd", "SHA3", "RDM", "LSE", "SHA256",
    "SHA512", "SHA1", "AES", "PMULL", "SB", "FRINTTS", "PACIMP", "LRCPC", "LRCPC2", "FCMA",
    "JSCVT", "PAuth", "PAuth2", "FPAC", "DPB", "DPB2", "BF16", "I8MM", "ECV", "LSE2", "CSV2",
    "CSV3", "DIT", "FP16", "SSBS", "BTI",
];

/// The oracle `spec` names and the CPU features to lift for:
///
/// - `unicorn`: Unicorn's max CPU, every feature;
/// - `qemu`: the harness under `qemu-aarch64 -cpu cortex-a57`, the Switch's
///   CPU; `qemu-max` under `-cpu max` without SVE and SME, every feature.
///   The harness path is `$RELIFT_CHECK_HARNESS_LINUX`;
/// - `m2`: the harness on an Apple M2 over ssh (macOS zeroes x18, so
///   states keep it zero and checks ignore it): host
///   `$RELIFT_CHECK_SSH` (default `mac`), path
///   `$RELIFT_CHECK_HARNESS_REMOTE` on it;
/// - `cmd:<argv>`: any command, split on spaces, with every feature.
pub fn open(spec: &str) -> Result<(Box<dyn Oracle>, String), String> {
    let env = |k: &str| std::env::var(k).map_err(|_| format!("--oracle {spec} needs ${k}"));
    let qemu = |cpu: &str| -> Result<Vec<String>, String> {
        Ok(vec![
            "qemu-aarch64".into(),
            "-cpu".into(),
            cpu.into(),
            env("RELIFT_CHECK_HARNESS_LINUX")?,
        ])
    };
    match spec {
        "unicorn" => Ok((
            Box::new(Unicorn::new().map_err(|e| format!("unicorn: {e:?}"))?),
            "all".to_string(),
        )),
        "qemu" => Ok((
            Box::new(Native::spawn("qemu", &qemu("cortex-a57")?)?),
            "cortex-a57".to_string(),
        )),
        "qemu-max" => Ok((
            Box::new(Native::spawn("qemu-max", &qemu("max,sve=off,sme=off")?)?),
            "all".to_string(),
        )),
        "m2" => {
            let host = std::env::var("RELIFT_CHECK_SSH").unwrap_or_else(|_| "mac".into());
            let argv = vec![
                "ssh".into(),
                "-o".into(),
                "BatchMode=yes".into(),
                host,
                env("RELIFT_CHECK_HARNESS_REMOTE")?,
            ];
            crate::emu::X18_ZERO.store(true, std::sync::atomic::Ordering::Relaxed);
            Ok((Box::new(Native::spawn("m2", &argv)?), M2_FEATURES.join(",")))
        }
        _ => match spec.strip_prefix("cmd:") {
            Some(cmd) => {
                let argv: Vec<String> = cmd.split(' ').map(String::from).collect();
                Ok((Box::new(Native::spawn("cmd", &argv)?), "all".to_string()))
            }
            None => Err(format!("unknown oracle {spec}")),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_fill_matches_the_harness() {
        // The first eight bytes of splitmix64 from seed 0.
        let w = fill_window(0);
        assert_eq!(w.len(), DATA_SIZE);
        assert_eq!(u64_at(&w, 0), 0xe220_a839_7b1d_cdaf);
    }

    #[test]
    fn requests_carry_the_state_layout() {
        let g = Guest {
            x: [7; 31],
            sp: 0x10,
            nzcv: [true, false, false, true],
            v: [3; 32],
            fpcr: 0x0040_0000,
            fpsr: 0x9f,
            pc: 0,
            mem: fill_window(5),
            mem_seed: Some(5),
        };
        let b = request(&Case {
            words: vec![0x8b01_0000],
            input: g,
        });
        assert_eq!(b.len(), 8 + 4 + 800 + 4 + 8);
        let s = &b[12..];
        assert_eq!(u64_at(s, 248), 0x10);
        assert_eq!(u32_at(s, 264), 0x9000_0000);
        assert_eq!(u32_at(s, 268), 0x0040_0000);
        assert_eq!(u32_at(s, 272), 0x9f);
        assert_eq!(s[288], 3);
    }
}

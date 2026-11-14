//! The two machines a check runs: Unicorn and the `relift-ir` interpreter.

use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};

use relift_cpu_a64::lift::sail::SLOW_FPCR;
use relift_cpu_a64::lift::{self, LiftError};
use relift_cpu_a64::regs;
use relift_ir::interp::{self, Fault, FlatMemory, GuestMemory, NoIntrinsics, State};
use relift_ir::{Block, Flow};
use unicorn_engine::RegisterARM64 as R;
use unicorn_engine::Unicorn;
use unicorn_engine::unicorn_const::{Arch, Mode, Prot, uc_error};

/// Where the instruction words go. Above 4 GiB, and above 0x70_0000_0000,
/// since macOS refuses fixed mappings below both; `check/oracle/harness.c`
/// uses the same layout.
pub const CODE: u64 = 0x100_1000_0000;
/// The mapped code region around [`CODE`], which covers 19-bit branches.
pub const CODE_MAP: u64 = CODE - 0x10_0000;
/// The size of the code region.
pub const CODE_SIZE: u64 = 0x20_0000;
/// The data window.
pub const DATA: u64 = 0x100_2000_0000;
/// The data window's size.
pub const DATA_SIZE: usize = 0x4000;

/// The guest state a check compares.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Guest {
    /// `x0` to `x30`.
    pub x: [u64; 31],
    /// The stack pointer.
    pub sp: u64,
    /// N, Z, C, V.
    pub nzcv: [bool; 4],
    /// `v0` to `v31`.
    pub v: [u128; 32],
    /// FPCR; the lifted FP blocks run only while it is zero.
    pub fpcr: u64,
    /// FPSR: the cumulative exception flags and QC.
    pub fpsr: u32,
    /// The program counter after the run.
    pub pc: u64,
    /// The data window.
    pub mem: Vec<u8>,
    /// The seed `mem` was filled from (`oracle::fill_window`), when it was;
    /// the native oracle then gets the seed instead of the bytes.
    pub mem_seed: Option<u64>,
}

/// How a run ended.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// Ran to the end.
    Done(Box<Guest>),
    /// Faulted, with a description.
    Fault(String),
    /// Left a block through a guard for the runtime's slow path (IR only).
    Slow(u32),
}

const XREGS: [R; 31] = [
    R::X0,
    R::X1,
    R::X2,
    R::X3,
    R::X4,
    R::X5,
    R::X6,
    R::X7,
    R::X8,
    R::X9,
    R::X10,
    R::X11,
    R::X12,
    R::X13,
    R::X14,
    R::X15,
    R::X16,
    R::X17,
    R::X18,
    R::X19,
    R::X20,
    R::X21,
    R::X22,
    R::X23,
    R::X24,
    R::X25,
    R::X26,
    R::X27,
    R::X28,
    R::X29,
    R::X30,
];

/// Unicorn with the code and data regions mapped.
pub struct Emu {
    uc: Unicorn<'static, ()>,
    loaded: Vec<u32>,
}

impl Emu {
    /// A fresh emulator.
    pub fn new() -> Result<Emu, uc_error> {
        let mut uc = Unicorn::new(Arch::ARM64, Mode::ARM)?;
        uc.ctl_set_cpu_model(CPU_ARM64_MAX)?;
        uc.mem_map(CODE_MAP, CODE_SIZE, Prot::READ | Prot::EXEC)?;
        uc.mem_map(DATA, DATA_SIZE as u64, Prot::READ | Prot::WRITE)?;
        uc.reg_write(R::CPACR_EL1, 3 << 20)?;
        Ok(Emu {
            uc,
            loaded: Vec::new(),
        })
    }

    fn load(&mut self, words: &[u32]) -> Result<(), uc_error> {
        if self.loaded == words {
            return Ok(());
        }
        let old = vec![0u8; 4 * self.loaded.len()];
        self.uc.mem_write(CODE, &old)?;
        let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        self.uc.mem_write(CODE, &bytes)?;
        self.uc
            .ctl_remove_cache(CODE, CODE + 4 * words.len().max(1) as u64)?;
        self.loaded = words.to_vec();
        Ok(())
    }

    /// Runs `words` from [`CODE`] on `input`.
    ///
    /// A fetch fault at the program counter after the last instruction ran
    /// is the end of the run, not a fault: Unicorn fetches the next
    /// instruction before it notices the count is reached.
    pub fn run(&mut self, words: &[u32], input: &Guest) -> Result<Outcome, uc_error> {
        self.load(words)?;
        let uc = &mut self.uc;
        for (r, v) in XREGS.iter().zip(input.x) {
            uc.reg_write(*r, v)?;
        }
        uc.reg_write(R::SP, input.sp)?;
        let n = input
            .nzcv
            .iter()
            .enumerate()
            .fold(0u64, |acc, (i, f)| acc | (u64::from(*f) << (31 - i)));
        uc.reg_write(R::NZCV, n)?;
        uc.reg_write(R::FPCR, input.fpcr)?;
        uc.reg_write(R::FPSR, u64::from(input.fpsr))?;
        for (i, v) in input.v.iter().enumerate() {
            uc.reg_write_long(qreg(i), &v.to_le_bytes())?;
        }
        uc.mem_write(DATA, &input.mem)?;
        let r = uc.emu_start(CODE, u64::MAX, 0, words.len());
        match r {
            Ok(()) => {}
            Err(uc_error::FETCH_UNMAPPED) => {}
            Err(e) => return Ok(Outcome::Fault(format!("{e:?}"))),
        }
        let mut out = Guest {
            x: [0; 31],
            sp: uc.reg_read(R::SP)?,
            nzcv: [false; 4],
            v: [0; 32],
            fpcr: input.fpcr,
            fpsr: uc.reg_read(R::FPSR)? as u32,
            pc: uc.reg_read(R::PC)?,
            mem: vec![0; DATA_SIZE],
            mem_seed: None,
        };
        for (i, r) in XREGS.iter().enumerate() {
            out.x[i] = uc.reg_read(*r)?;
        }
        for (i, v) in out.v.iter_mut().enumerate() {
            let b = uc.reg_read_long(qreg(i))?;
            let mut a = [0u8; 16];
            a.copy_from_slice(&b[..16]);
            *v = u128::from_le_bytes(a);
        }
        let f = uc.reg_read(R::NZCV)?;
        for i in 0..4 {
            out.nzcv[i] = (f >> (31 - i)) & 1 == 1;
        }
        uc.mem_read(DATA, &mut out.mem)?;
        Ok(Outcome::Done(Box::new(out)))
    }
}

/// `UC_CPU_ARM64_MAX`: every feature Unicorn's QEMU implements, so forms
/// beyond Armv8.0 run instead of raising an exception.
const CPU_ARM64_MAX: i32 = 3;

fn qreg(i: usize) -> i32 {
    R::Q0 as i32 + i as i32
}

/// Whether words lift with the hand-written lifter only.
pub static HAND: AtomicBool = AtomicBool::new(false);

/// Whether lifted blocks go through dead-value elimination.
pub static DCE: AtomicBool = AtomicBool::new(false);

/// Whether a check compares FPSR.
pub static FPSR: AtomicBool = AtomicBool::new(false);

/// Whether the oracle's kernel zeroes x18 on every return to user mode
/// (macOS, where x18 is the platform register): states then start with
/// x18 zero and checks ignore it.
pub static X18_ZERO: AtomicBool = AtomicBool::new(false);

/// The oracle's CPU features, in `Features::parse` syntax (`all` for
/// every feature). Not yet applied: the lifter on this branch lifts every
/// encoding for every CPU.
pub static FEATURES: OnceLock<String> = OnceLock::new();

/// Lifts `w` at `pc` as the frontend does, or with the hand-written lifter
/// under [`HAND`], then removes dead values under [`DCE`].
pub fn lift(w: u32, pc: u64) -> Result<Block, LiftError> {
    let b = if HAND.load(Ordering::Relaxed) {
        lift::lift_hand(w, pc)?
    } else {
        lift::lift(w, pc)?
    };
    if DCE.load(Ordering::Relaxed) {
        return Ok(relift_ir::dce(&b));
    }
    Ok(b)
}

/// Lifts every word at its address from [`CODE`].
pub fn lift_all(words: &[u32]) -> Result<Vec<Block>, String> {
    words
        .iter()
        .enumerate()
        .map(|(i, w)| lift(*w, CODE + 4 * i as u64).map_err(|e| e.to_string()))
        .collect()
}

/// The interpreter's view of what Unicorn maps: the data window, and the
/// read-only code region holding the words at [`CODE`] and zeros elsewhere.
struct Memory {
    data: FlatMemory,
    code: Vec<u8>,
}

fn code_image(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

impl Memory {
    fn in_code(addr: u64, n: usize) -> bool {
        addr >= CODE_MAP
            && addr
                .checked_add(n as u64)
                .is_some_and(|e| e <= CODE_MAP + CODE_SIZE)
    }
}

impl GuestMemory for Memory {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault> {
        if !Memory::in_code(addr, buf.len()) {
            return self.data.read(addr, buf);
        }
        for (i, b) in buf.iter_mut().enumerate() {
            let a = addr + i as u64;
            *b = a
                .checked_sub(CODE)
                .and_then(|o| self.code.get(o as usize).copied())
                .unwrap_or(0);
        }
        Ok(())
    }

    fn write(&mut self, addr: u64, data: &[u8]) -> Result<(), Fault> {
        if Memory::in_code(addr, data.len()) {
            return Err(Fault {
                addr,
                size: data.len() as u32,
                write: true,
            });
        }
        self.data.write(addr, data)
    }
}

/// Runs lifted blocks in the interpreter on `input`.
pub fn interpret(blocks: &[Block], words: &[u32], input: &Guest) -> Outcome {
    let file = regs::regs();
    let mut st = State::new(file.len());
    for (i, v) in input.x.iter().enumerate() {
        st.regs[i] = u128::from(*v);
    }
    st.regs[regs::SP.index()] = u128::from(input.sp);
    for (i, v) in input.v.iter().enumerate() {
        st.regs[regs::vreg(i as u32).index()] = *v;
    }
    st.regs[regs::FPCR.index()] = u128::from(input.fpcr);
    st.regs[regs::FPSR.index()] = u128::from(input.fpsr);
    for (i, r) in [regs::N, regs::Z, regs::C, regs::V].iter().enumerate() {
        st.regs[r.index()] = u128::from(input.nzcv[i]);
    }
    let mut mem = Memory {
        data: FlatMemory {
            base: DATA,
            bytes: input.mem.clone(),
        },
        code: code_image(words),
    };
    let mut pc = CODE;
    for b in blocks {
        if b.pc != pc {
            return Outcome::Fault(format!("control left the sequence at {pc:#x}"));
        }
        match interp::run(b, &mut st, &mut mem, &mut NoIntrinsics) {
            Ok(o) => {
                if let Flow::Trap(code) = o.flow
                    && code == SLOW_FPCR
                {
                    return Outcome::Slow(code);
                }
                pc = o.next;
            }
            Err(e) => return Outcome::Fault(e.to_string()),
        }
    }
    let mut out = Guest {
        x: [0; 31],
        sp: st.regs[regs::SP.index()] as u64,
        nzcv: [false; 4],
        v: [0; 32],
        fpcr: input.fpcr,
        fpsr: st.regs[regs::FPSR.index()] as u32,
        pc,
        mem: mem.data.bytes,
        mem_seed: None,
    };
    for (i, x) in out.x.iter_mut().enumerate() {
        *x = st.regs[i] as u64;
    }
    for (i, r) in [regs::N, regs::Z, regs::C, regs::V].iter().enumerate() {
        out.nzcv[i] = st.regs[r.index()] == 1;
    }
    for (i, v) in out.v.iter_mut().enumerate() {
        *v = st.regs[regs::vreg(i as u32).index()];
    }
    Outcome::Done(Box::new(out))
}

/// The differences between two results, empty when they agree. Two faults
/// agree.
pub fn compare(ir: &Outcome, uc: &Outcome) -> Vec<String> {
    match (ir, uc) {
        (Outcome::Fault(e), _) if e.starts_with("no slow path") => vec![e.clone()],
        (Outcome::Fault(_), Outcome::Fault(_)) => Vec::new(),
        (Outcome::Slow(_), _) => vec!["ir left for the slow path".to_string()],
        (_, Outcome::Slow(_)) => vec!["the oracle has no slow path".to_string()],
        (Outcome::Fault(e), Outcome::Done(_)) => vec![format!("ir faulted: {e}")],
        (Outcome::Done(_), Outcome::Fault(e)) => vec![format!("oracle faulted: {e}")],
        (Outcome::Done(a), Outcome::Done(b)) => {
            let mut d = Vec::new();
            let skip18 = X18_ZERO.load(Ordering::Relaxed);
            for i in 0..31 {
                if a.x[i] != b.x[i] && !(i == 18 && skip18) {
                    d.push(format!("x{i} ir={:#x} or={:#x}", a.x[i], b.x[i]));
                }
            }
            for i in 0..32 {
                if a.v[i] != b.v[i] {
                    d.push(format!("v{i} ir={:#x} or={:#x}", a.v[i], b.v[i]));
                }
            }
            if a.sp != b.sp {
                d.push(format!("sp ir={:#x} or={:#x}", a.sp, b.sp));
            }
            for (i, f) in ["n", "z", "c", "v"].iter().enumerate() {
                if a.nzcv[i] != b.nzcv[i] {
                    d.push(format!("{f} ir={} or={}", a.nzcv[i], b.nzcv[i]));
                }
            }
            if FPSR.load(Ordering::Relaxed) && a.fpsr != b.fpsr {
                d.push(format!("fpsr ir={:#x} or={:#x}", a.fpsr, b.fpsr));
            }
            if a.pc != b.pc {
                d.push(format!("pc ir={:#x} or={:#x}", a.pc, b.pc));
            }
            if let Some(off) = (0..DATA_SIZE).find(|i| a.mem[*i] != b.mem[*i]) {
                d.push(format!(
                    "mem+{off:#x} ir={:02x} or={:02x}",
                    a.mem[off], b.mem[off]
                ));
            }
            d
        }
    }
}

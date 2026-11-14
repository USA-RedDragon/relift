//! The two machines a check runs: Unicorn and the `relift-ir` interpreter.

use relift_cpu_a64::lift::lift;
use relift_cpu_a64::regs;
use relift_ir::Block;
use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
use unicorn_engine::RegisterARM64 as R;
use unicorn_engine::Unicorn;
use unicorn_engine::unicorn_const::{Arch, Mode, Prot, uc_error};

/// Where the instruction words go.
pub const CODE: u64 = 0x1000_0000;
/// The mapped code region around [`CODE`], which covers 19-bit branches.
pub const CODE_MAP: u64 = CODE - 0x10_0000;
/// The size of the code region.
pub const CODE_SIZE: u64 = 0x20_0000;
/// The data window.
pub const DATA: u64 = 0x2000_0000;
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
    /// The program counter after the run.
    pub pc: u64,
    /// The data window.
    pub mem: Vec<u8>,
}

/// How a run ended.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// Ran to the end.
    Done(Box<Guest>),
    /// Faulted, with a description.
    Fault(String),
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
        uc.mem_map(CODE_MAP, CODE_SIZE, Prot::ALL)?;
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
            pc: uc.reg_read(R::PC)?,
            mem: vec![0; DATA_SIZE],
        };
        for (i, r) in XREGS.iter().enumerate() {
            out.x[i] = uc.reg_read(*r)?;
        }
        let f = uc.reg_read(R::NZCV)?;
        for i in 0..4 {
            out.nzcv[i] = (f >> (31 - i)) & 1 == 1;
        }
        uc.mem_read(DATA, &mut out.mem)?;
        Ok(Outcome::Done(Box::new(out)))
    }
}

/// Lifts every word at its address from [`CODE`].
pub fn lift_all(words: &[u32]) -> Result<Vec<Block>, String> {
    words
        .iter()
        .enumerate()
        .map(|(i, w)| lift(*w, CODE + 4 * i as u64).map_err(|e| e.to_string()))
        .collect()
}

/// Runs lifted blocks in the interpreter on `input`.
pub fn interpret(blocks: &[Block], input: &Guest) -> Outcome {
    let file = regs::regs();
    let mut st = State::new(file.len());
    for (i, v) in input.x.iter().enumerate() {
        st.regs[i] = u128::from(*v);
    }
    st.regs[regs::SP.index()] = u128::from(input.sp);
    for (i, r) in [regs::N, regs::Z, regs::C, regs::V].iter().enumerate() {
        st.regs[r.index()] = u128::from(input.nzcv[i]);
    }
    let mut mem = FlatMemory {
        base: DATA,
        bytes: input.mem.clone(),
    };
    let mut pc = CODE;
    for b in blocks {
        if b.pc != pc {
            return Outcome::Fault(format!("control left the sequence at {pc:#x}"));
        }
        match interp::run(b, &mut st, &mut mem, &mut NoIntrinsics) {
            Ok(o) => pc = o.next,
            Err(e) => return Outcome::Fault(e.to_string()),
        }
    }
    let mut out = Guest {
        x: [0; 31],
        sp: st.regs[regs::SP.index()] as u64,
        nzcv: [false; 4],
        pc,
        mem: mem.bytes,
    };
    for (i, x) in out.x.iter_mut().enumerate() {
        *x = st.regs[i] as u64;
    }
    for (i, r) in [regs::N, regs::Z, regs::C, regs::V].iter().enumerate() {
        out.nzcv[i] = st.regs[r.index()] == 1;
    }
    Outcome::Done(Box::new(out))
}

/// The differences between two results, empty when they agree. Two faults
/// agree.
pub fn compare(ir: &Outcome, uc: &Outcome) -> Vec<String> {
    match (ir, uc) {
        (Outcome::Fault(_), Outcome::Fault(_)) => Vec::new(),
        (Outcome::Fault(e), Outcome::Done(_)) => vec![format!("ir faulted: {e}")],
        (Outcome::Done(_), Outcome::Fault(e)) => vec![format!("unicorn faulted: {e}")],
        (Outcome::Done(a), Outcome::Done(b)) => {
            let mut d = Vec::new();
            for i in 0..31 {
                if a.x[i] != b.x[i] {
                    d.push(format!("x{i} ir={:#x} uc={:#x}", a.x[i], b.x[i]));
                }
            }
            if a.sp != b.sp {
                d.push(format!("sp ir={:#x} uc={:#x}", a.sp, b.sp));
            }
            for (i, f) in ["n", "z", "c", "v"].iter().enumerate() {
                if a.nzcv[i] != b.nzcv[i] {
                    d.push(format!("{f} ir={} uc={}", a.nzcv[i], b.nzcv[i]));
                }
            }
            if a.pc != b.pc {
                d.push(format!("pc ir={:#x} uc={:#x}", a.pc, b.pc));
            }
            if let Some(off) = (0..DATA_SIZE).find(|i| a.mem[*i] != b.mem[*i]) {
                d.push(format!(
                    "mem+{off:#x} ir={:02x} uc={:02x}",
                    a.mem[off], b.mem[off]
                ));
            }
            d
        }
    }
}

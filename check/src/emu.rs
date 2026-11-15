//! The two machines a check runs: Unicorn and the `relift-ir` interpreter.

use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};

use relift_cpu_arm64::decode::{Features, decode_for};
use relift_cpu_arm64::lift::sail::{SLOW_FLAGS, SLOW_FPCR};
use relift_cpu_arm64::lift::system;
use relift_cpu_arm64::lift::{self, LiftError};
use relift_cpu_arm64::regs::{self, CHECK_PAC_KEYS, PacKey};
use relift_ir::interp::{self, Fault, FlatMemory, GuestMemory, Intrinsics, State};
use relift_ir::{Block, Flow};
use unicorn_engine::RegisterARM64 as R;
use unicorn_engine::RegisterARM64CP;
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
/// Where pointer authentication runs see the data window a second time:
/// [`DATA`] has bit 40 set, outside the 39-bit address range the keys are
/// checked in, so an authenticated pointer can only reach this alias.
pub const PAUTH_DATA: u64 = 0x20_2000_0000;

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
    el1: Option<bool>,
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
            el1: None,
        })
    }

    /// Runs at EL1 with the check keys while `pauth` is true, and at the
    /// exception level Unicorn starts in (EL3) otherwise. The first switch
    /// to EL1 enables the keys and the 39-bit address layout of the Switch
    /// kernel's EL0 view; the EL1&0 regime has the same upper and lower
    /// ranges as EL0.
    fn set_el(&mut self, pauth: bool) -> Result<(), uc_error> {
        match self.el1 {
            Some(cur) if cur == pauth => return Ok(()),
            None if !pauth => return Ok(()),
            _ => {}
        }
        if pauth && self.el1.is_none() {
            self.setup_pauth()?;
        }
        let mode = if pauth { EL1H } else { EL3H };
        self.uc.reg_write(R::PSTATE, mode)?;
        if pauth {
            self.uc
                .mem_map(PAUTH_DATA, DATA_SIZE as u64, Prot::READ | Prot::WRITE)?;
        } else if self.el1 == Some(true) {
            self.uc.mem_unmap(PAUTH_DATA, DATA_SIZE as u64)?;
        }
        self.el1 = Some(pauth);
        Ok(())
    }

    fn setup_pauth(&mut self) -> Result<(), uc_error> {
        const KEY_REGS: [(u32, u32); 5] = [(1, 0), (1, 2), (2, 0), (2, 2), (3, 0)];
        for (k, (crm, op2)) in KEY_REGS.iter().enumerate() {
            for (hi, v) in [(1, CHECK_PAC_KEYS[2 * k]), (0, CHECK_PAC_KEYS[2 * k + 1])] {
                self.coproc(3, 0, 2, *crm, op2 + hi, Some(v))?;
            }
        }
        let sctlr = self.coproc(3, 0, 1, 0, 0, None)?;
        self.coproc(3, 0, 1, 0, 0, Some(sctlr | SCTLR_PAUTH))?;
        self.coproc(3, 0, 2, 0, 2, Some(25 | 25 << 16))?;
        let hcr = self.coproc(3, 4, 1, 1, 0, None)?;
        self.coproc(3, 4, 1, 1, 0, Some(hcr | 3 << 40 | 1 << 31))?;
        let scr = self.coproc(3, 6, 1, 1, 0, None)?;
        self.coproc(3, 6, 1, 1, 0, Some(scr | 3 << 16 | 1 << 10 | 1))?;
        Ok(())
    }

    fn coproc(
        &mut self,
        op0: u32,
        op1: u32,
        crn: u32,
        crm: u32,
        op2: u32,
        write: Option<u64>,
    ) -> Result<u64, uc_error> {
        let mut reg = RegisterARM64CP::new()
            .op0(op0)
            .op1(op1)
            .crn(crn)
            .crm(crm)
            .op2(op2);
        match write {
            Some(v) => {
                reg.val = v;
                self.uc.reg_write_arm64_coproc(&reg)?;
            }
            None => self.uc.reg_read_arm64_coproc(&mut reg)?,
        }
        Ok(reg.val)
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
        let pauth = words.iter().any(|w| is_pauth(*w));
        self.set_el(pauth)?;
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
        if pauth {
            uc.mem_write(PAUTH_DATA, &input.mem)?;
        }
        let r = uc.emu_start(CODE, u64::MAX, 0, words.len());
        match r {
            Ok(()) => {}
            Err(uc_error::FETCH_UNMAPPED) => {}
            Err(uc_error::EXCEPTION) if pauth && uc.reg_read(R::PC)? >> 48 != 0 => {}
            Err(uc_error::EXCEPTION) if pauth && uc.reg_read(R::PC)? >> 48 != 0 => {}
            Err(e) => return Ok(Outcome::Fault(format!("{e:?}"))),
        }
        let mut out = Guest {
            x: [0; 31],
            sp: uc.reg_read(R::SP)?,
            nzcv: [false; 4],
            v: [0; 32],
            fpcr: uc.reg_read(R::FPCR)?,
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

/// PSTATE of EL1 with SP_EL1, and of EL3 with SP_EL3.
const EL1H: u64 = 0x5;
const EL3H: u64 = 0xd;

/// SCTLR_EL1's EnIA, EnIB, EnDA and EnDB.
const SCTLR_PAUTH: u64 = 1 << 31 | 1 << 30 | 1 << 27 | 1 << 13;

/// Whether `w` is a pointer authentication instruction.
pub fn is_pauth(w: u32) -> bool {
    const PREFIXES: [&str; 11] = [
        "PAC", "AUT", "XPAC", "BRAA", "BRAB", "BLRAA", "BLRAB", "RETAA", "RETAB", "LDRAA", "LDRAB",
    ];
    decode_for(w, &Features::ALL).is_some_and(|i| {
        let n = i.encoding.name();
        PREFIXES.iter().any(|p| n.starts_with(p))
    })
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

/// Whether lifted blocks run as in a program that produces FPSR flags
/// (`regs::FP_FLAGS` 1): blocks whose flags the host gives stay in the
/// interpreter, the others leave for the slow path.
pub static FP_FLAGS: AtomicBool = AtomicBool::new(false);

/// The A64 intrinsics the interpreter can run: the FPSR flag ones (the
/// host's sticky flags modeled as `host`) and those of
/// [`system::InterpIntrinsics`] (event hints, cache maintenance).
struct Arm64Intrinsics {
    host: u32,
}

impl Intrinsics for Arm64Intrinsics {
    fn call(&mut self, name: &'static str, args: &[u128]) -> Result<u128, interp::Error> {
        match name {
            system::FP_RAISE => {
                self.host |= args[0] as u32;
                Ok(0)
            }
            system::MRS_FPSR => Ok(args[0] | u128::from(self.host)),
            system::MSR_FPSR => {
                self.host = 0;
                Ok(0)
            }
            _ => system::InterpIntrinsics.call(name, args),
        }
    }
}

/// Whether a check compares FPSR.
pub static FPSR: AtomicBool = AtomicBool::new(false);

/// Whether the oracle's kernel zeroes x18 on every return to user mode
/// (macOS, where x18 is the platform register): states then start with
/// x18 zero and checks ignore it.
pub static X18_ZERO: AtomicBool = AtomicBool::new(false);

/// The oracle's CPU features, in `Features::parse` syntax (`all` for
/// every feature): words are decoded and lifted for that CPU.
pub static FEATURES: OnceLock<String> = OnceLock::new();

/// The parsed [`FEATURES`], every feature when unset.
pub fn features() -> Features {
    static F: OnceLock<Features> = OnceLock::new();
    *F.get_or_init(|| match FEATURES.get() {
        Some(s) => Features::parse(s).unwrap_or_else(|e| panic!("oracle features: {e}")),
        None => Features::ALL,
    })
}

/// Whether the check is of words the oracle's CPU does not implement: they
/// must lift to an undefined-instruction trap ([`UNDEF_TRAP`]) and fault
/// with SIGILL on the oracle.
pub static UNDEF: AtomicBool = AtomicBool::new(false);

/// The trap code of a block that stands for an undefined instruction.
pub const UNDEF_TRAP: u32 = 0x5544_4546;

/// Whether `e` means the word is undefined on the CPU.
pub fn is_undefined(e: &LiftError) -> bool {
    matches!(e, LiftError::Unallocated(_) | LiftError::Undefined(_))
}

/// Lifts `w` at `pc` as the frontend does, or with the hand-written lifter
/// under [`HAND`], then removes dead values under [`DCE`]. Under [`UNDEF`],
/// an undefined word becomes a block that traps with [`UNDEF_TRAP`], as the
/// emitter's undefined-instruction call does.
pub fn lift(w: u32, pc: u64) -> Result<Block, LiftError> {
    if UNDEF.load(Ordering::Relaxed) {
        match lift_plain(w, pc) {
            Err(e) if is_undefined(&e) => {
                let b = relift_ir::Builder::new(regs::regs(), relift_ir::Width::W64, pc);
                return Ok(b.finish(
                    4,
                    relift_ir::Exit {
                        next: relift_ir::Next::Const(pc),
                        flow: Flow::Trap(UNDEF_TRAP),
                    },
                ));
            }
            r => return r,
        }
    }
    lift_plain(w, pc)
}

pub fn lift_plain(w: u32, pc: u64) -> Result<Block, LiftError> {
    let b = if HAND.load(Ordering::Relaxed) {
        lift::lift_hand(w, pc)?
    } else {
        lift::lift(w, pc, &features())?
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
    alias: bool,
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

impl Memory {
    fn unalias(&self, addr: u64, n: usize) -> u64 {
        let end = addr.wrapping_add(n as u64);
        if self.alias && addr >= PAUTH_DATA && end <= PAUTH_DATA + DATA_SIZE as u64 {
            addr - PAUTH_DATA + DATA
        } else {
            addr
        }
    }
}

impl GuestMemory for Memory {
    fn read(&mut self, addr: u64, buf: &mut [u8]) -> Result<(), Fault> {
        let addr = self.unalias(addr, buf.len());
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
        let addr = self.unalias(addr, data.len());
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
    st.regs[regs::FP_FLAGS.index()] = u128::from(FP_FLAGS.load(Ordering::Relaxed));
    for (k, key) in PacKey::ALL.iter().enumerate() {
        st.regs[key.reg(true).index()] = u128::from(CHECK_PAC_KEYS[2 * k]);
        st.regs[key.reg(false).index()] = u128::from(CHECK_PAC_KEYS[2 * k + 1]);
    }
    let mut intr = Arm64Intrinsics { host: 0 };
    for (i, r) in [regs::N, regs::Z, regs::C, regs::V].iter().enumerate() {
        st.regs[r.index()] = u128::from(input.nzcv[i]);
    }
    let mut mem = Memory {
        data: FlatMemory {
            base: DATA,
            bytes: input.mem.clone(),
        },
        code: code_image(words),
        alias: words.iter().any(|w| is_pauth(*w)),
    };
    let mut pc = CODE;
    for b in blocks {
        if b.pc != pc {
            return Outcome::Fault(format!("control left the sequence at {pc:#x}"));
        }
        match interp::run(b, &mut st, &mut mem, &mut intr) {
            Ok(o) => {
                if let Flow::Trap(code) = o.flow
                    && (code == SLOW_FPCR || code == SLOW_FLAGS)
                {
                    return Outcome::Slow(code);
                }
                if let Flow::Trap(relift_cpu_arm64::lift::sail::SLOW_ALIGN) = o.flow {
                    return Outcome::Fault("alignment fault".to_string());
                }
                if let Flow::Trap(UNDEF_TRAP) = o.flow {
                    return Outcome::Fault("undefined instruction".to_string());
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
        fpcr: st.regs[regs::FPCR.index()] as u64,
        // As MRS FPSR would read it: the slot and the host's flags.
        fpsr: st.regs[regs::FPSR.index()] as u32 | intr.host,
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
            // FPCR after MSR: the mode bits every Armv8.0 CPU implements;
            // trap enables and other bits are IMPLEMENTATION DEFINED.
            if FPSR.load(Ordering::Relaxed) && (a.fpcr ^ b.fpcr) & 0x07c0_0000 != 0 {
                d.push(format!("fpcr ir={:#x} or={:#x}", a.fpcr, b.fpcr));
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

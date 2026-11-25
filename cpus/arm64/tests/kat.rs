//! The lifted A64 means what the Thor's CPU does: every case of the
//! known-answer files in `cpus/arm64/tests/kat` (recorded from the hardware
//! by `relift-check kat record`, see `check/README.md`) is lifted for the
//! Thor's feature set and run in the `relift-ir` interpreter, and the
//! registers, flags, memory and next address must equal the hardware's.
//!
//! Cases the interpreter cannot run are counted and left to the emitted-C
//! replay (`relift/tests/emit_a64_kat.rs`): a block that leaves through its
//! FPCR guard for the runtime's slow path, and one with host floating point,
//! whose FPSR flags the host gives. FPSR is compared for the others. The
//! one named gap is FPAC: the Thor faults on a failed authentication and
//! the lifter, which refuses a CPU with FPAC, lifts the poisoned pointer.
//! The other is top-byte ignore: Linux runs EL0 with it on and the
//! templates exist for the Switch's layout without it, so the pointer
//! authentication code field and the failure code sit in other bits. The
//! pointer authentication cases are compared on the address bits (38 to 0
//! and 55) and on whether the result is canonical.

use std::path::Path;

use relift_cpu_arm64::cpu::{Cpu, Va};
use relift_cpu_arm64::decode::{Features, decode_for};
use relift_cpu_arm64::lift::sail::{SLOW_FLAGS, SLOW_FPCR, SLOW_FPCR_FZ};
use relift_cpu_arm64::lift::{lift, system};
use relift_cpu_arm64::regs::{self, CHECK_PAC_KEYS, PacKey};
use relift_ir::interp::{self, Fault, FlatMemory, GuestMemory, Intrinsics, State};
use relift_ir::{Block, Flow, Op};

#[path = "kat/format.rs"]
mod format;

use format::{CODE, Case, DATA, DATA_SIZE, Result as Answer};

/// The mapped code region around [`CODE`].
const CODE_MAP: u64 = CODE - 0x10_0000;
const CODE_SIZE: u64 = 0x20_0000;

/// The code region as the hardware has it, read-only: the words and zeros.
struct Memory {
    data: FlatMemory,
    code: Vec<u8>,
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
            *b = (addr + i as u64)
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

/// The intrinsics the interpreter runs: the FPSR flag ones, with the host's
/// sticky flags modeled as `host`, and those of [`system::InterpIntrinsics`].
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

/// How a replay of a case ended.
enum Run {
    Result(Answer),
    /// Left a block for the slow path or has host floating point.
    NotHere,
}

fn replay(c: &Case, cpu: &Cpu) -> Run {
    let mut blocks: Vec<Block> = Vec::new();
    for (i, w) in c.words.iter().enumerate() {
        match lift(*w, CODE + 4 * i as u64, cpu) {
            Ok(b) => blocks.push(b),
            Err(e) => panic!("{w:08x} does not lift: {e}"),
        }
    }
    let file = regs::regs();
    let mut st = State::new(file.len());
    for (i, v) in c.input.x.iter().enumerate() {
        st.regs[i] = u128::from(*v);
    }
    st.regs[regs::SP.index()] = u128::from(c.input.sp);
    for (i, v) in c.input.v.iter().enumerate() {
        st.regs[regs::vreg(i as u32).index()] = *v;
    }
    st.regs[regs::FPCR.index()] = u128::from(c.input.fpcr);
    st.regs[regs::FPSR.index()] = u128::from(c.input.fpsr);
    st.regs[regs::FP_FLAGS.index()] = 1;
    for (k, key) in PacKey::ALL.iter().enumerate() {
        st.regs[key.reg(true).index()] = u128::from(CHECK_PAC_KEYS[2 * k]);
        st.regs[key.reg(false).index()] = u128::from(CHECK_PAC_KEYS[2 * k + 1]);
    }
    for (i, r) in [regs::N, regs::Z, regs::C, regs::V].iter().enumerate() {
        st.regs[r.index()] = u128::from(c.input.nzcv >> (3 - i) & 1);
    }
    let mut intr = Arm64Intrinsics { host: 0 };
    let mut mem = Memory {
        data: FlatMemory {
            base: DATA,
            bytes: c.input.mem.clone(),
        },
        code: c.words.iter().flat_map(|w| w.to_le_bytes()).collect(),
    };
    let fp_ops = blocks
        .iter()
        .any(|b| b.insts.iter().any(|i| matches!(i.op, Op::Fp(..))));
    let mut pc = CODE;
    for b in &blocks {
        if b.pc != pc {
            // Control left the sequence: the hardware ends there too.
            break;
        }
        match interp::run(b, &mut st, &mut mem, &mut intr) {
            Ok(o) => {
                if let Flow::Trap(code) = o.flow {
                    if code == SLOW_FPCR || code == SLOW_FPCR_FZ || code == SLOW_FLAGS {
                        return Run::NotHere;
                    }
                    return Run::Result(Answer::Fault(0));
                }
                pc = o.next;
            }
            Err(_) => return Run::Result(Answer::Fault(0)),
        }
    }
    if fp_ops {
        return Run::NotHere;
    }
    let mut s = format::State::base();
    for (i, x) in s.x.iter_mut().enumerate() {
        *x = st.regs[i] as u64;
    }
    s.sp = st.regs[regs::SP.index()] as u64;
    for (i, v) in s.v.iter_mut().enumerate() {
        *v = st.regs[regs::vreg(i as u32).index()];
    }
    s.nzcv = [regs::N, regs::Z, regs::C, regs::V]
        .iter()
        .enumerate()
        .fold(0, |a, (i, r)| {
            a | ((st.regs[r.index()] as u8 & 1) << (3 - i))
        });
    s.fpcr = st.regs[regs::FPCR.index()] as u32;
    s.fpsr = st.regs[regs::FPSR.index()] as u32 | intr.host;
    s.pc = pc.wrapping_sub(CODE);
    s.mem = mem.data.bytes;
    debug_assert_eq!(s.mem.len(), DATA_SIZE);
    Run::Result(Answer::Done(Box::new(s)))
}

/// Whether the last word authenticates a pointer: the Thor faults on a
/// failed authentication (FPAC), the lifter poisons the pointer.
fn authenticates(c: &Case, f: &Features) -> bool {
    let w = *c.words.last().expect("a word");
    decode_for(w, f).is_some_and(|i| {
        let m = i.encoding.mnemonic();
        [
            "AUT", "BRAA", "BRAB", "BLRAA", "BLRAB", "RETAA", "RETAB", "LDRAA", "LDRAB",
        ]
        .iter()
        .any(|p| m.starts_with(p))
    })
}

/// Whether the case is of a pointer authentication form.
fn is_pac(c: &Case, f: &Features) -> bool {
    let w = *c.words.last().expect("a word");
    decode_for(w, f).is_some_and(|i| {
        let m = i.encoding.mnemonic();
        ["PAC", "AUT", "XPAC", "BRA", "BLRA", "RETA", "LDRA"]
            .iter()
            .any(|p| m.starts_with(p))
    })
}

#[test]
fn lifted_a64_matches_the_thor_on_every_known_answer() {
    let features = Features::parse(format::THOR_FEATURES).expect("the Thor's features");
    let cpu = Cpu::new(features, Va::BITS39);
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/kat");
    let mut files: Vec<_> = std::fs::read_dir(&dir)
        .expect("the kat directory")
        .map(|e| e.expect("entry").path())
        .filter(|p| p.extension().is_some_and(|x| x == "txt") && !p.ends_with("skipped.txt"))
        .collect();
    files.sort();
    assert!(!files.is_empty());
    let (mut ran, mut elsewhere, mut gap) = (0usize, 0usize, 0usize);
    let mut bad: Vec<String> = Vec::new();
    for p in files {
        let text = std::fs::read_to_string(&p).expect("readable");
        for l in text.lines() {
            let c = format::parse(l);
            assert_eq!(c.src, "thor", "answers come from the Thor");
            let r = match replay(&c, &cpu) {
                Run::NotHere => {
                    elsewhere += 1;
                    continue;
                }
                Run::Result(r) => r,
            };
            if matches!(c.out, Answer::Fault(4))
                && matches!(r, Answer::Done(_))
                && authenticates(&c, &features)
            {
                gap += 1;
                continue;
            }
            ran += 1;
            let d = if is_pac(&c, &features) {
                format::pac_diff(&r, &c)
            } else {
                format::diff(&r, &c, true)
            };
            if !d.is_empty() {
                let w: Vec<String> = c.words.iter().map(|w| format!("{w:08x}")).collect();
                bad.push(format!("{}: {}", w.join(","), d.join("; ")));
            }
        }
    }
    eprintln!(
        "{ran} cases replayed, {elsewhere} left to the emitted-C replay, {gap} FPAC faults (named gap), {} differ",
        bad.len()
    );
    assert!(
        bad.is_empty(),
        "{} cases differ from the Thor, first: {}",
        bad.len(),
        bad.iter().take(12).cloned().collect::<Vec<_>>().join("\n")
    );
}

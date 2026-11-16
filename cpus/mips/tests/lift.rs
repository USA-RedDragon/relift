//! Lifted VR4300 instructions on the IR interpreter: known answers per
//! group, delay slots, likely branches, the FPU fast path and its guards.

use relift_cpu_mips::decode::{self, Encoding};
use relift_cpu_mips::lift::{self, EXC_GUARD, Exc, FP_SLOW, IN_DELAY, LiftError};
use relift_cpu_mips::regs;
use relift_ir::interp::{self, Error, FlatMemory, Intrinsics, State};
use relift_ir::{Flow, Reg};

/// A kseg0 address; physical 0x1000.
const PC: u64 = 0xFFFF_FFFF_8000_1000;
/// A kseg0 data address; physical 0x2000.
const DATA: u64 = 0xFFFF_FFFF_8000_2000;
/// Status: CU0, CU1, FR = 1, kernel mode.
const STATUS: u64 = 0x3400_0000;

fn r_type(rs: u32, rt: u32, rd: u32, sa: u32, funct: u32) -> u32 {
    (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct
}

fn i_type(op: u32, rs: u32, rt: u32, imm: u16) -> u32 {
    (op << 26) | (rs << 21) | (rt << 16) | u32::from(imm)
}

fn cop1(fmt: u32, ft: u32, fs: u32, fd: u32, funct: u32) -> u32 {
    (0x11 << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct
}

/// The test's stand-in for the runtime's FPU and CP0 intrinsics: FCR31
/// keeps its flags and clears the cause bits (the inputs here are exact),
/// and a compare sets the condition bit.
struct Runtime;

impl Intrinsics for Runtime {
    fn call(&mut self, name: &'static str, a: &[u128]) -> Result<u128, Error> {
        match name {
            "mips_fcr31" => Ok(a[2] & !(0x3F << 12)),
            "mips_fcmp" => {
                let (cond, fmt, fcr) = (a[0], a[1], a[2]);
                let (lt, eq, un) = if fmt == 0x10 {
                    let (p, q) = (f32::from_bits(a[3] as u32), f32::from_bits(a[4] as u32));
                    (p < q, p == q, p.is_nan() || q.is_nan())
                } else {
                    let (p, q) = (f64::from_bits(a[3] as u64), f64::from_bits(a[4] as u64));
                    (p < q, p == q, p.is_nan() || q.is_nan())
                };
                let c = (cond & 4 != 0 && lt) || (cond & 2 != 0 && eq) || (cond & 1 != 0 && un);
                Ok((fcr & !(1 << 23)) | (u128::from(c) << 23))
            }
            "mips_random" => Ok(31),
            "mips_tlb" | "mips_cache" => Ok(0),
            _ => Err(Error::UnknownIntrinsic(name)),
        }
    }
}

struct Cpu {
    st: State,
    mem: FlatMemory,
}

/// How a run ended.
#[derive(Debug, PartialEq, Eq)]
enum End {
    Next(u64),
    Trap(u32),
}

impl Cpu {
    fn new() -> Cpu {
        let mut c = Cpu {
            st: State::new(regs::regs().len()),
            mem: FlatMemory {
                base: 0,
                bytes: vec![0; 0x4000],
            },
        };
        c.put(regs::CP0_STATUS, STATUS);
        c
    }

    fn get(&self, r: Reg) -> u64 {
        self.st.regs[r.index()] as u64
    }

    fn put(&mut self, r: Reg, v: u64) {
        self.st.regs[r.index()] = u128::from(v);
    }

    fn gpr(&self, n: u8) -> u64 {
        self.get(regs::gpr(n))
    }

    fn set_gpr(&mut self, n: u8, v: u64) {
        self.put(regs::gpr(n), v);
    }

    fn run2(&mut self, word: u32, slot: u32) -> End {
        let l = lift::lift(PC, word, slot).expect("lifts");
        l.block.verify().expect("verifies");
        let out = interp::run(&l.block, &mut self.st, &mut self.mem, &mut Runtime).expect("runs");
        match out.flow {
            Flow::Trap(code) => End::Trap(code),
            _ => End::Next(out.next),
        }
    }

    fn run(&mut self, word: u32) -> End {
        self.run2(word, 0)
    }
}

fn exc(e: Exc) -> End {
    End::Trap(EXC_GUARD + e.code())
}

#[test]
fn decodes_known_words() {
    for (w, e) in [
        (0x2442_0001, Encoding::Addiu),
        (0x0000_0000, Encoding::Sll),
        (0x8c82_0000, Encoding::Lw),
        (0x8882_0000, Encoding::Lwl),
        (0xfc82_0000, Encoding::Sd),
        (0x1000_0001, Encoding::Beq),
        (0x5000_0001, Encoding::Beql),
        (0x0411_0001, Encoding::Bgezal),
        (0x0000_000c, Encoding::Syscall),
        (0x4200_0018, Encoding::Eret),
        (0x4080_6000, Encoding::Mtc0),
        (0x4502_0001, Encoding::Bc1fl),
        (0x4600_0000, Encoding::Cop1S),
        (0x4620_0000, Encoding::Cop1D),
        (0x4680_0020, Encoding::Cop1W),
        (0xc400_0000, Encoding::Lwc1),
    ] {
        assert_eq!(decode::decode(w), Some(e), "{w:#010x}");
    }
    assert_eq!(decode::decode(0x7000_0002), None, "MUL is not VR4300");
    assert_eq!(Encoding::Cop1S.name(), "COP1.S");
}

#[test]
fn every_encoding_has_a_template_that_verifies() {
    assert_eq!(decode::forms().len(), 137);
    for &(enc, _, value, _) in decode::forms() {
        assert!(lift::has_template(enc), "{}", enc.name());
        let l = lift::lift(PC, value, 0).unwrap_or_else(|e| panic!("{}: {e}", enc.name()));
        l.block
            .verify()
            .unwrap_or_else(|e| panic!("{}: {e:?}", enc.name()));
    }
}

#[test]
fn alu() {
    let mut c = Cpu::new();
    c.set_gpr(1, 0x7FFF_FFFF);
    c.set_gpr(2, 1);
    assert_eq!(c.run(r_type(1, 2, 3, 0, 0x21)), End::Next(PC + 4));
    assert_eq!(c.gpr(3), 0xFFFF_FFFF_8000_0000, "ADDU sign-extends");
    assert_eq!(
        c.run(r_type(1, 2, 3, 0, 0x20)),
        exc(Exc::Ov),
        "ADD overflows"
    );
    assert_eq!(c.run(r_type(1, 2, 3, 0, 0x2d)), End::Next(PC + 4));
    assert_eq!(c.gpr(3), 0x8000_0000, "DADDU");
    c.run(i_type(0x09, 0, 4, 0xFFFF));
    assert_eq!(c.gpr(4), u64::MAX, "ADDIU -1");
    c.run(r_type(4, 2, 5, 0, 0x2a));
    assert_eq!(c.gpr(5), 1, "SLT -1 < 1");
    c.run(r_type(4, 2, 5, 0, 0x2b));
    assert_eq!(c.gpr(5), 0, "SLTU");
    c.run(i_type(0x0f, 0, 6, 0x8001));
    assert_eq!(c.gpr(6), 0xFFFF_FFFF_8001_0000, "LUI");
    c.run(r_type(0, 2, 7, 0, 0x27));
    assert_eq!(c.gpr(7), !1, "NOR");
    c.run(r_type(1, 2, 0, 0, 0x21));
    assert_eq!(c.gpr(0), 0, "r0 stays 0");
}

#[test]
fn shifts() {
    let mut c = Cpu::new();
    c.set_gpr(1, 0x0000_0001_8000_0000);
    c.run(r_type(0, 1, 2, 4, 0x00));
    assert_eq!(c.gpr(2), 0, "SLL keeps the low word");
    c.run(r_type(0, 1, 2, 4, 0x03));
    assert_eq!(
        c.gpr(2),
        0x0000_0000_1800_0000,
        "SRA shifts the 64-bit value"
    );
    c.set_gpr(3, 36);
    c.run(r_type(3, 1, 2, 0, 0x07));
    assert_eq!(c.gpr(2), 0x1800_0000, "SRAV uses rs[4:0]");
    c.set_gpr(1, 0x8000_0000_0000_0000);
    c.run(r_type(0, 1, 2, 4, 0x3f));
    assert_eq!(c.gpr(2), 0xFFFF_FFFF_F800_0000, "DSRA32");
}

#[test]
fn multiply_and_divide() {
    let mut c = Cpu::new();
    c.set_gpr(1, 0xFFFF_FFFF_FFFF_FFFE);
    c.set_gpr(2, 3);
    c.run(r_type(1, 2, 0, 0, 0x18));
    assert_eq!(
        (c.get(regs::HI), c.get(regs::LO)),
        (u64::MAX, 0xFFFF_FFFF_FFFF_FFFA),
        "MULT -2 * 3"
    );
    c.run(r_type(1, 2, 0, 0, 0x19));
    assert_eq!(
        (c.get(regs::HI), c.get(regs::LO)),
        (2, 0xFFFF_FFFF_FFFF_FFFA),
        "MULTU 0xFFFFFFFE * 3"
    );
    c.set_gpr(1, 7);
    c.set_gpr(2, 2);
    c.run(r_type(1, 2, 0, 0, 0x1a));
    assert_eq!((c.get(regs::HI), c.get(regs::LO)), (1, 3), "DIV 7 / 2");
    c.set_gpr(2, 0);
    c.run(r_type(1, 2, 0, 0, 0x1a));
    assert_eq!(
        (c.get(regs::HI), c.get(regs::LO)),
        (7, u64::MAX),
        "DIV 7 / 0"
    );
    c.set_gpr(1, 0xFFFF_FFFF_FFFF_FFF9);
    c.run(r_type(1, 2, 0, 0, 0x1a));
    assert_eq!(
        (c.get(regs::HI), c.get(regs::LO)),
        (0xFFFF_FFFF_FFFF_FFF9, 1),
        "DIV -7 / 0"
    );
    c.set_gpr(1, 0x0000_0001_8000_0000);
    c.run(r_type(1, 2, 0, 0, 0x1b));
    assert_eq!(
        (c.get(regs::HI), c.get(regs::LO)),
        (0xFFFF_FFFF_8000_0000, u64::MAX),
        "DIVU by 0: HI is the sign-extended dividend word"
    );
    c.run(r_type(1, 2, 0, 0, 0x1f));
    assert_eq!(
        (c.get(regs::HI), c.get(regs::LO)),
        (0x0000_0001_8000_0000, u64::MAX),
        "DDIVU by 0"
    );
    c.set_gpr(2, 0x10);
    c.run(r_type(1, 2, 0, 0, 0x1e));
    assert_eq!((c.get(regs::HI), c.get(regs::LO)), (0, 0x1800_0000), "DDIV");
}

#[test]
fn loads_and_stores() {
    let mut c = Cpu::new();
    c.mem.bytes[0x2000..0x2008].copy_from_slice(&[0x81, 0x82, 0x83, 0x84, 0x05, 0x06, 0x07, 0x08]);
    c.set_gpr(1, DATA);
    c.run(i_type(0x23, 1, 2, 0));
    assert_eq!(
        c.gpr(2),
        0xFFFF_FFFF_8182_8384,
        "LW sign-extends, big-endian"
    );
    c.run(i_type(0x24, 1, 2, 1));
    assert_eq!(c.gpr(2), 0x82, "LBU");
    c.run(i_type(0x21, 1, 2, 2));
    assert_eq!(c.gpr(2), 0xFFFF_FFFF_FFFF_8384, "LH");
    c.set_gpr(3, 0x1111_1111_2222_2222);
    c.run(i_type(0x22, 1, 3, 1));
    assert_eq!(c.gpr(3), 0xFFFF_FFFF_8283_8422, "LWL at offset 1");
    c.set_gpr(3, 0x1111_1111_2222_2222);
    c.run(i_type(0x26, 1, 3, 5));
    assert_eq!(
        c.gpr(3),
        0x1111_1111_2222_0506,
        "LWR at offset 5 keeps bits 63..32"
    );
    c.set_gpr(4, 0xAABB_CCDD);
    c.run(i_type(0x2b, 1, 4, 8));
    assert_eq!(
        &c.mem.bytes[0x2008..0x200c],
        &[0xAA, 0xBB, 0xCC, 0xDD],
        "SW"
    );
    c.run(i_type(0x2a, 1, 4, 0x11));
    assert_eq!(&c.mem.bytes[0x2010..0x2014], &[0, 0xAA, 0xBB, 0xCC], "SWL");
    assert_eq!(
        c.run(i_type(0x23, 1, 2, 2)),
        exc(Exc::AdEL),
        "misaligned LW"
    );
    c.set_gpr(1, 0x2000);
    assert_eq!(
        c.run(i_type(0x23, 1, 2, 0)),
        exc(Exc::TLBL),
        "kuseg goes to the TLB"
    );
}

#[test]
fn branch_runs_its_delay_slot() {
    let mut c = Cpu::new();
    c.set_gpr(1, 5);
    c.set_gpr(2, 5);
    let addiu_r2 = i_type(0x09, 2, 2, 1);
    assert_eq!(c.run2(i_type(0x04, 1, 2, 3), addiu_r2), End::Next(PC + 16));
    assert_eq!(c.gpr(2), 6, "the slot ran after the compare");
    assert_eq!(c.run2(i_type(0x04, 1, 2, 3), addiu_r2), End::Next(PC + 8));
    assert_eq!(c.gpr(2), 7, "not taken: the slot still runs");
    assert_eq!(c.run2(0x0c00_0800, 0), End::Next(DATA), "JAL");
    assert_eq!(c.gpr(31), PC + 8, "JAL links past the slot");
    c.set_gpr(5, DATA);
    let addiu_r5 = i_type(0x09, 5, 5, 4);
    assert_eq!(c.run2(r_type(5, 0, 0, 0, 0x08), addiu_r5), End::Next(DATA));
    assert_eq!(c.gpr(5), DATA + 4, "JR reads its target before the slot");
}

#[test]
fn likely_branch_annuls_its_slot_when_not_taken() {
    let mut c = Cpu::new();
    c.set_gpr(1, 1);
    let addiu_r3 = i_type(0x09, 3, 3, 1);
    assert_eq!(c.run2(i_type(0x14, 1, 0, 3), addiu_r3), End::Next(PC + 8));
    assert_eq!(c.gpr(3), 0, "annulled");
    c.set_gpr(1, 0);
    assert_eq!(c.run2(i_type(0x14, 1, 0, 3), addiu_r3), End::Next(PC + 16));
    assert_eq!(c.gpr(3), 1, "taken: the slot runs");
    let sw = i_type(0x2b, 4, 4, 0);
    c.set_gpr(1, 1);
    c.set_gpr(4, DATA);
    c.run2(i_type(0x14, 1, 0, 3), sw);
    assert_eq!(&c.mem.bytes[0x2000..0x2004], &[0; 4], "an annulled store");
}

#[test]
fn delay_slot_exceptions_and_refusals() {
    let mut c = Cpu::new();
    c.set_gpr(1, 0x7FFF_FFFF);
    let add_ov = r_type(1, 1, 2, 0, 0x20);
    assert_eq!(
        c.run2(i_type(0x04, 0, 0, 3), add_ov),
        End::Trap(EXC_GUARD + Exc::Ov.code() + IN_DELAY)
    );
    assert_eq!(
        lift::lift(PC, i_type(0x04, 0, 0, 3), i_type(0x05, 0, 0, 3)).err(),
        Some(LiftError::BranchInDelaySlot(Encoding::Bne))
    );
    let l = lift::lift(PC, 0x0000_000c, 0).expect("lifts");
    assert_eq!(l.exception, Some(Exc::Sys));
    assert_eq!(c.run(0x0000_000c), exc(Exc::Sys), "SYSCALL");
}

#[test]
fn cop1_fast_path_and_guards() {
    let mut c = Cpu::new();
    c.put(regs::fpr(1), u64::from(1.5f32.to_bits()));
    c.put(regs::fpr(2), u64::from(2.25f32.to_bits()));
    assert_eq!(c.run(cop1(0x10, 2, 1, 3, 0)), End::Next(PC + 4));
    assert_eq!(f32::from_bits(c.get(regs::fpr(3)) as u32), 3.75, "ADD.S");
    c.put(regs::fpr(4), 1.5f64.to_bits());
    c.put(regs::fpr(5), (-0.25f64).to_bits());
    c.run(cop1(0x11, 5, 4, 6, 2));
    assert_eq!(f64::from_bits(c.get(regs::fpr(6))), -0.375, "MUL.D");
    c.run(cop1(0x11, 5, 4, 0, 0x3c));
    assert_eq!(c.get(regs::FCR31) >> 23 & 1, 0, "C.LT.D 1.5 < -0.25");
    c.put(regs::fpr(2), 1);
    assert_eq!(
        c.run(cop1(0x10, 2, 1, 3, 0)),
        End::Trap(FP_SLOW),
        "a denormal input takes the slow path"
    );
    c.put(regs::FCR31, 1);
    c.put(regs::fpr(2), u64::from(2.25f32.to_bits()));
    assert_eq!(
        c.run(cop1(0x10, 2, 1, 3, 0)),
        End::Trap(FP_SLOW),
        "rounding toward zero takes the slow path"
    );
    c.put(regs::CP0_STATUS, STATUS & !0x2000_0000);
    assert_eq!(c.run(cop1(0x10, 2, 1, 3, 0)), exc(Exc::CpU), "CU1 clear");
}

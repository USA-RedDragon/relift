//! The R3000A profile stub on the IR interpreter: its MIPS I decode table,
//! the 32-bit register file and the `int` class templates.

use relift_cpu_mips::r3000a::decode::{self, Encoding};
use relift_cpu_mips::r3000a::lift::{self, EXC_GUARD, Exc, LiftError};
use relift_cpu_mips::r3000a::regs;
use relift_ir::interp::{self, Error, FlatMemory, Intrinsics, State};
use relift_ir::{Flow, Width};

const PC: u32 = 0x8001_0000;

fn r_type(rs: u32, rt: u32, rd: u32, sa: u32, funct: u32) -> u32 {
    (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct
}

fn i_type(op: u32, rs: u32, rt: u32, imm: u16) -> u32 {
    (op << 26) | (rs << 21) | (rt << 16) | u32::from(imm)
}

struct NoIntrinsics;

impl Intrinsics for NoIntrinsics {
    fn call(&mut self, name: &'static str, _a: &[u128]) -> Result<u128, Error> {
        Err(Error::UnknownIntrinsic(name))
    }
}

/// Runs `word` from the general registers `init`; gives the registers
/// after it, or the guard code it left with.
fn run(word: u32, init: &[(u8, u32)]) -> Result<State, u32> {
    let l = lift::lift(PC, word, 0).expect("lifts");
    l.block.verify().expect("verifies");
    let mut st = State::new(regs::regs().len());
    for &(n, v) in init {
        st.regs[regs::gpr(n).index()] = u128::from(v);
    }
    let mut mem = FlatMemory {
        base: 0,
        bytes: Vec::new(),
    };
    let out = interp::run(&l.block, &mut st, &mut mem, &mut NoIntrinsics).expect("runs");
    match out.flow {
        Flow::Trap(code) => Err(code),
        _ => {
            assert_eq!(out.next, u64::from(PC) + 4);
            Ok(st)
        }
    }
}

fn gpr(st: &State, n: u8) -> u128 {
    st.regs[regs::gpr(n).index()]
}

#[test]
fn register_file_is_32_bit() {
    let f = regs::regs();
    assert_eq!(f.len(), 34);
    for i in 0..32 {
        assert_eq!(f.width(regs::gpr(i)), Width::W32);
    }
    assert_eq!(f.width(regs::HI), Width::W32);
    assert_eq!(f.width(regs::LO), Width::W32);
}

#[test]
fn decodes_mips_i_only() {
    for (w, e) in [
        (0x2442_0001, Encoding::Addiu),
        (0x0000_0000, Encoding::Sll),
        (0x8c82_0000, Encoding::Lw),
        (0x8882_0000, Encoding::Lwl),
        (0x1000_0001, Encoding::Beq),
        (0x0411_0001, Encoding::Bgezal),
        (0x0000_000c, Encoding::Syscall),
        (0x4080_6000, Encoding::Mtc0),
    ] {
        assert_eq!(decode::decode(w), Some(e), "{w:#010x}");
    }
    for (w, what) in [
        (0x6442_0001, "DADDIU"),
        (0xfc82_0000, "SD"),
        (0xdc82_0000, "LD"),
        (0xc082_0000, "LL"),
        (0xe082_0000, "SC"),
        (0x5000_0001, "BEQL"),
        (0x0000_0030, "TGE"),
        (0x4200_0018, "ERET"),
        (0x4200_0002, "TLBWI"),
        (0x0000_000f, "SYNC"),
        (0xbc82_0000, "CACHE"),
        (0x4600_0000, "COP1.S"),
        (0x7000_0002, "MUL"),
    ] {
        assert_eq!(decode::decode(w), None, "{what} is not MIPS I");
    }
}

#[test]
fn only_int_has_templates() {
    for (e, _, _, class) in decode::forms() {
        assert_eq!(lift::has_template(*e), *class == "int", "{}", e.name());
    }
    let lw = 0x8c82_0000;
    assert!(matches!(
        lift::lift(PC, lw, 0),
        Err(LiftError::Unsupported(Encoding::Lw))
    ));
}

#[test]
fn int_wraps_at_32_bits() {
    let addu = r_type(1, 2, 3, 0, 0x21);
    let st = run(addu, &[(1, 0x7FFF_FFFF), (2, 1)]).expect("no trap");
    assert_eq!(gpr(&st, 3), 0x8000_0000);
    let add = r_type(1, 2, 3, 0, 0x20);
    assert_eq!(
        run(add, &[(1, 0x7FFF_FFFF), (2, 1)]).err(),
        Some(EXC_GUARD + Exc::Ov.code())
    );
    let addiu = i_type(0x09, 1, 2, 0xFFFF);
    let st = run(addiu, &[(1, 0)]).expect("no trap");
    assert_eq!(gpr(&st, 2), 0xFFFF_FFFF);
    let sra = r_type(0, 1, 2, 4, 0x03);
    let st = run(sra, &[(1, 0x8000_0000)]).expect("no trap");
    assert_eq!(gpr(&st, 2), 0xF800_0000);
    let sltu = r_type(1, 2, 3, 0, 0x2B);
    let st = run(sltu, &[(1, 1), (2, 0x8000_0000)]).expect("no trap");
    assert_eq!(gpr(&st, 3), 1);
}

#[test]
fn mult_and_div_fill_32_bit_hi_lo() {
    let mult = r_type(1, 2, 0, 0, 0x18);
    let st = run(mult, &[(1, 0xFFFF_FFFF), (2, 3)]).expect("no trap");
    assert_eq!(st.regs[regs::HI.index()], 0xFFFF_FFFF);
    assert_eq!(st.regs[regs::LO.index()], 0xFFFF_FFFD);
    let div = r_type(1, 2, 0, 0, 0x1A);
    let st = run(div, &[(1, 0xFFFF_FFF9), (2, 0)]).expect("no trap");
    assert_eq!(st.regs[regs::LO.index()], 1, "negative / 0");
    assert_eq!(st.regs[regs::HI.index()], 0xFFFF_FFF9);
    let st = run(div, &[(1, 0x8000_0000), (2, 0xFFFF_FFFF)]).expect("no trap");
    assert_eq!(st.regs[regs::LO.index()], 0x8000_0000);
    assert_eq!(st.regs[regs::HI.index()], 0);
}

//! Lifted scalar RSP instructions on the IR interpreter: decoding, the
//! 32-bit register file without traps, DMEM wrapping, 12-bit branch
//! targets and links, delay slots, and the SP effects.

use relift_cpu_rsp::decode::{self, Encoding};
use relift_cpu_rsp::lift::{self, LiftError, Sys};
use relift_cpu_rsp::regs;
use relift_ir::interp::{self, Error, FlatMemory, Intrinsics, State};

/// The runtime's SP intrinsics: COP0 register `n` reads `0x100 + n`, and
/// the writes and BREAKs are recorded.
#[derive(Default)]
struct Sp {
    writes: Vec<(u128, u128, u128)>,
    breaks: Vec<u128>,
}

impl Intrinsics for Sp {
    fn call(&mut self, name: &'static str, a: &[u128]) -> Result<u128, Error> {
        match name {
            "rsp_mfc0" => Ok(0x100 + a[0]),
            "rsp_mtc0" => {
                self.writes.push((a[0], a[1], a[2]));
                Ok(0)
            }
            "rsp_break" => {
                self.breaks.push(a[0]);
                Ok(0)
            }
            _ => Err(Error::UnknownIntrinsic(name)),
        }
    }
}

struct Cpu {
    st: State,
    mem: FlatMemory,
    sp: Sp,
}

impl Cpu {
    fn new() -> Cpu {
        Cpu {
            st: State::new(regs::regs().len()),
            mem: FlatMemory {
                base: 0,
                bytes: vec![0; 0x1000],
            },
            sp: Sp::default(),
        }
    }

    fn gpr(&self, n: u8) -> u32 {
        self.st.regs[regs::gpr(n).index()] as u32
    }

    fn set_gpr(&mut self, n: u8, v: u32) {
        self.st.regs[regs::gpr(n).index()] = u128::from(v);
    }

    /// Runs `word` at `pc` with `slot` after it; the next PC.
    fn run2(&mut self, pc: u64, word: u32, slot: u32) -> u64 {
        let l = lift::lift(pc, word, slot).expect("lifts");
        interp::run(&l.block, &mut self.st, &mut self.mem, &mut self.sp)
            .expect("runs")
            .next
    }

    fn run(&mut self, word: u32) -> u64 {
        self.run2(0x100, word, 0)
    }
}

fn r_type(rs: u32, rt: u32, rd: u32, sa: u32, funct: u32) -> u32 {
    (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct
}

fn i_type(op: u32, rs: u32, rt: u32, imm: u16) -> u32 {
    (op << 26) | (rs << 21) | (rt << 16) | u32::from(imm)
}

#[test]
fn decodes_known_words() {
    for (w, e) in [
        (0x2442_0001, Some(Encoding::Addiu)),
        (0x0000_000D, Some(Encoding::Break)),
        (0x4A01_1100, Some(Encoding::Vmulf)),
        (0x4A00_0037, Some(Encoding::Vnop)),
        (0x4800_1000, Some(Encoding::Mfc2)),
        (0x48C2_0800, Some(Encoding::Ctc2)),
        (0xC801_2000, Some(Encoding::Lqv)),
        (0xE801_5800, Some(Encoding::Stv)),
        (0x4080_2000, Some(Encoding::Mtc0)),
        // LWV, MULT, a 64-bit add, a likely branch: not on the RSP.
        (0xC801_5000, None),
        (0x0022_0018, None),
        (0x0022_182C, None),
        (0x5022_0004, None),
    ] {
        assert_eq!(decode::decode(w), e, "{w:#010x}");
    }
    for &(enc, _, _, _) in decode::forms() {
        assert!(lift::has_template(enc), "{} has no template", enc.name());
    }
}

#[test]
fn arithmetic_wraps_without_traps() {
    let mut c = Cpu::new();
    c.set_gpr(1, 0x7FFF_FFFF);
    c.set_gpr(2, 1);
    c.run(r_type(1, 2, 3, 0, 0x20));
    assert_eq!(c.gpr(3), 0x8000_0000);
    c.run(i_type(0x08, 1, 4, 1));
    assert_eq!(c.gpr(4), 0x8000_0000);
    c.run(r_type(3, 2, 5, 0, 0x22));
    assert_eq!(c.gpr(5), 0x7FFF_FFFF);
    c.run(i_type(0x0F, 0, 6, 0x8000));
    assert_eq!(c.gpr(6), 0x8000_0000);
    c.run(r_type(0, 6, 7, 4, 0x03));
    assert_eq!(c.gpr(7), 0xF800_0000);
    c.run(r_type(1, 2, 0, 0, 0x21));
    assert_eq!(c.gpr(0), 0);
}

#[test]
fn scalar_memory_wraps_at_4_kib() {
    let mut c = Cpu::new();
    c.mem.bytes[0xFFE] = 0x12;
    c.mem.bytes[0xFFF] = 0x34;
    c.mem.bytes[0x000] = 0x56;
    c.mem.bytes[0x001] = 0x78;
    c.set_gpr(1, 0x1FFE);
    c.run(i_type(0x23, 1, 2, 0));
    assert_eq!(c.gpr(2), 0x1234_5678);
    c.run(i_type(0x21, 1, 3, 1));
    assert_eq!(c.gpr(3), 0x3456);
    c.set_gpr(4, 0xAABB_CCDD);
    c.run(i_type(0x2B, 1, 4, 0xFFFF));
    assert_eq!(&c.mem.bytes[0xFFD..], &[0xAA, 0xBB, 0xCC]);
    assert_eq!(c.mem.bytes[0], 0xDD);
}

#[test]
fn branches_are_12_bit_and_link_masked() {
    let mut c = Cpu::new();
    let nop = 0;
    assert_eq!(c.run2(0xFFC, 0x0C00_0040, nop), 0x100);
    assert_eq!(c.gpr(31), 0x004);
    c.set_gpr(1, 0x1234);
    assert_eq!(c.run2(0x100, r_type(1, 0, 0, 0, 0x08), nop), 0x234);
    c.set_gpr(2, 5);
    c.set_gpr(3, 5);
    assert_eq!(c.run2(0x200, i_type(0x04, 2, 3, 0xFFFF), nop), 0x200);
    c.set_gpr(3, 6);
    assert_eq!(c.run2(0x200, i_type(0x04, 2, 3, 0xFFFF), nop), 0x208);
}

#[test]
fn the_delay_slot_runs_with_the_branch() {
    let mut c = Cpu::new();
    c.set_gpr(1, 7);
    let next = c.run2(0x100, 0x0800_0010, i_type(0x09, 1, 2, 1));
    assert_eq!(next, 0x040);
    assert_eq!(c.gpr(2), 8);
    let jr = r_type(1, 0, 0, 0, 0x08);
    assert!(matches!(
        lift::lift(0x100, 0x0800_0010, jr),
        Err(LiftError::BranchInDelaySlot(Encoding::Jr))
    ));
}

#[test]
fn sp_registers_and_break_go_to_the_runtime() {
    let mut c = Cpu::new();
    c.run(0x4005_2000);
    assert_eq!(c.gpr(5), 0x104);
    c.set_gpr(6, 0xDEAD_BEEF);
    let l = lift::lift(0x100, 0x4086_3800, 0).expect("lifts");
    assert_eq!(l.sys, vec![Sys::Cop0Write(7)]);
    c.run(0x4086_3800);
    assert_eq!(c.sp.writes, vec![(7, 0xDEAD_BEEF, 1)]);
    let l = lift::lift(0x100, 0x0000_000D, 0).expect("lifts");
    assert_eq!(l.sys, vec![Sys::Break]);
    c.run(0x0000_000D);
    assert_eq!(c.sp.breaks, vec![1]);
}

#[test]
fn a_word_that_is_no_instruction_does_nothing() {
    let mut c = Cpu::new();
    c.set_gpr(1, 3);
    let before = c.st.regs.clone();
    assert_eq!(c.run(0x0022_0018), 0x104);
    assert_eq!(c.st.regs, before);
}

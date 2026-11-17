//! Lifted instructions on the IR interpreter: the mode and PSR rules the
//! hardware probe measured on an AGB-001, and the banked registers.

use relift_cpu_arm32::lift::{self, COND_FAIL};
use relift_cpu_arm32::regs::{self, Bank, Mode};
use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
use relift_ir::{Flow, Reg};

const PC: u32 = 0x0300_0100;

struct Cpu {
    st: State,
    mem: FlatMemory,
}

impl Cpu {
    fn new(mode: Mode) -> Cpu {
        let mut st = State::new(regs::regs().len());
        st.regs[regs::M.index()] = u128::from(mode.bits());
        Cpu {
            st,
            mem: FlatMemory {
                base: 0x0200_0000,
                bytes: vec![0; 0x100],
            },
        }
    }

    fn get(&self, r: Reg) -> u32 {
        self.st.regs[r.index()] as u32
    }

    fn put(&mut self, r: Reg, v: u32) {
        self.st.regs[r.index()] = u128::from(v);
    }

    fn spsr(&self, m: Mode) -> u32 {
        self.get(m.spsr().expect("mode has an SPSR"))
    }

    fn cpsr(&self) -> u32 {
        let b = |r: Reg, s: u32| self.get(r) << s;
        b(regs::N, 31)
            | b(regs::Z, 30)
            | b(regs::C, 29)
            | b(regs::V, 28)
            | self.get(regs::PSR_RSVD)
            | b(regs::I, 7)
            | b(regs::F, 6)
            | b(regs::T, 5)
            | self.get(regs::M)
    }

    fn run(&mut self, word: u32, thumb: bool) -> u32 {
        let l = lift::lift(word, PC, thumb).expect("lifts");
        let out =
            interp::run(&l.block, &mut self.st, &mut self.mem, &mut NoIntrinsics).expect("runs");
        match out.flow {
            Flow::Trap(COND_FAIL) => PC + if thumb { 2 } else { 4 },
            _ => out.next as u32,
        }
    }
}

#[test]
fn msr_spsr_forces_mode_bit_4() {
    let mut c = Cpu::new(Mode::Irq);
    let next = c.run(0xe361_f000, false);
    assert_eq!(next, PC + 4);
    assert_eq!(c.spsr(Mode::Irq) & 0xff, 0x10);
    let mut c = Cpu::new(Mode::Svc);
    c.put(regs::r(3), 0xffff_ffef);
    c.run(0xe169_f003, false);
    assert_eq!(c.spsr(Mode::Svc), 0xf000_00ff);
}

#[test]
fn test_ops_with_rd_15_restore_the_cpsr_or_change_nothing() {
    let mut c = Cpu::new(Mode::Irq);
    c.put(Mode::Irq.spsr().expect("irq"), 0x6000_001f);
    c.put(regs::r(13), 0x1313);
    c.put(Bank::Usr.slot(13).expect("usr r13"), 0x5555);
    let next = c.run(0xe151_f002, false);
    assert_eq!(next, PC + 4);
    assert_eq!(c.cpsr(), 0x6000_001f);
    assert_eq!(c.get(regs::r(13)), 0x5555);
    assert_eq!(c.get(Bank::Irq.slot(13).expect("irq r13")), 0x1313);
    for m in [Mode::Sys, Mode::Usr] {
        let mut c = Cpu::new(m);
        c.put(regs::N, 1);
        c.put(regs::r(1), 5);
        c.put(regs::r(2), 5);
        c.run(0xe151_f002, false);
        assert_eq!(c.cpsr(), 0x8000_0000 | u32::from(m.bits()));
    }
}

#[test]
fn thumb_cmp_with_rd_15_keeps_the_thumb_bit() {
    let mut c = Cpu::new(Mode::Svc);
    c.put(regs::T, 1);
    c.put(Mode::Svc.spsr().expect("svc"), 0x9000_0012);
    let next = c.run(0x4587, true);
    assert_eq!(next, PC + 2);
    assert_eq!(c.cpsr(), 0x9000_0032);
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::T, 1);
    c.put(regs::V, 1);
    c.run(0x4587, true);
    assert_eq!(c.cpsr(), 0x1000_003f);
}

#[test]
fn msr_cpsr_swaps_the_banks() {
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::r(13), 0xaaaa);
    c.put(regs::r(14), 0xbbbb);
    c.put(regs::r(8), 0x8888);
    c.put(Bank::Irq.slot(13).expect("irq r13"), 0x1313);
    c.put(Bank::Irq.slot(14).expect("irq r14"), 0x1414);
    c.run(0xe321_f0d2, false);
    assert_eq!(c.get(regs::M), 0x12);
    assert_eq!((c.get(regs::I), c.get(regs::F)), (1, 1));
    assert_eq!(c.get(regs::r(13)), 0x1313);
    assert_eq!(c.get(regs::r(14)), 0x1414);
    assert_eq!(c.get(regs::r(8)), 0x8888);
    assert_eq!(c.get(Bank::Usr.slot(13).expect("usr r13")), 0xaaaa);
    assert_eq!(c.get(Bank::Usr.slot(14).expect("usr r14")), 0xbbbb);
    let mut c = Cpu::new(Mode::Usr);
    c.run(0xe321_f0d2, false);
    assert_eq!(c.get(regs::M), 0x10);
}

#[test]
fn stm_with_the_s_bit_stores_the_user_bank() {
    let mut c = Cpu::new(Mode::Fiq);
    c.put(regs::r(0), 0x0200_0010);
    c.put(regs::r(8), 0xf8);
    c.put(Bank::Usr.slot(8).expect("usr r8"), 0x88);
    c.put(Bank::Usr.slot(13).expect("usr r13"), 0xdd);
    c.run(0xe8c0_2100, false);
    assert_eq!(&c.mem.bytes[0x10..0x18], &[0x88, 0, 0, 0, 0xdd, 0, 0, 0]);
}

#[test]
fn ldm_with_the_s_bit_and_writeback_loads_the_user_bank() {
    let mut c = Cpu::new(Mode::Irq);
    c.put(regs::r(1), 0x0200_0010);
    c.put(regs::r(13), 0x1313);
    for (i, b) in c.mem.bytes[0x14..0x18].iter_mut().enumerate() {
        *b = [0xdd, 0xd0, 0, 0][i];
    }
    c.run(0xe9f1_2000, false);
    assert_eq!(c.get(Bank::Usr.slot(13).expect("usr r13")), 0xd0dd);
    assert_eq!(c.get(regs::r(13)), 0x1313);
    assert_eq!(c.get(regs::r(1)), 0x0200_0014);
}

#[test]
fn swi_enters_supervisor_mode() {
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::C, 1);
    c.put(regs::r(13), 0x7f00);
    c.put(Bank::Svc.slot(13).expect("svc r13"), 0x7fe0);
    let next = c.run(0xef00_0005, false);
    assert_eq!(next, 0x08);
    assert_eq!(c.get(regs::M), 0x13);
    assert_eq!(c.get(regs::I), 1);
    assert_eq!(c.get(regs::r(14)), PC + 4);
    assert_eq!(c.get(regs::r(13)), 0x7fe0);
    assert_eq!(c.spsr(Mode::Svc), 0x2000_001f);
    let mut c = Cpu::new(Mode::Usr);
    c.put(regs::T, 1);
    let next = c.run(0xdf05, true);
    assert_eq!(next, 0x08);
    assert_eq!(c.get(regs::T), 0);
    assert_eq!(c.get(regs::r(14)), PC + 2);
    assert_eq!(c.spsr(Mode::Svc), 0x30);
}

#[test]
fn a_failed_condition_has_no_effect() {
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::r(1), 7);
    let next = c.run(0x0281_1001, false);
    assert_eq!(next, PC + 4);
    assert_eq!(c.get(regs::r(1)), 7);
}

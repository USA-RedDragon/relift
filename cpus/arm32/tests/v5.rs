//! The ARMv5TE templates that qemu-arm user mode cannot check (CP15
//! transfers, BKPT, and the ARM946E-S rules taken from GBATEK).

use relift_cpu_arm32::decode::Profile;
use relift_cpu_arm32::lift::{self, COND_FAIL};
use relift_cpu_arm32::regs::{self, Bank, Mode};
use relift_ir::interp::{self, Error, FlatMemory, Intrinsics, State};
use relift_ir::{Flow, Op, Reg};

const PC: u32 = 0x0200_0100;

#[derive(Default)]
struct Cp {
    calls: Vec<(&'static str, Vec<u128>)>,
    read: u128,
}

impl Intrinsics for Cp {
    fn call(&mut self, name: &'static str, args: &[u128]) -> Result<u128, Error> {
        self.calls.push((name, args.to_vec()));
        Ok(if name == "arm.cp_read" { self.read } else { 0 })
    }
}

struct Cpu {
    st: State,
    mem: FlatMemory,
    cp: Cp,
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
            cp: Cp::default(),
        }
    }
    fn get(&self, r: Reg) -> u32 {
        self.st.regs[r.index()] as u32
    }
    fn put(&mut self, r: Reg, v: u32) {
        self.st.regs[r.index()] = u128::from(v);
    }
    fn word(&mut self, a: u32, v: u32) {
        let o = (a - 0x0200_0000) as usize;
        self.mem.bytes[o..o + 4].copy_from_slice(&v.to_le_bytes());
    }
    fn mem32(&self, a: u32) -> u32 {
        let o = (a - 0x0200_0000) as usize;
        u32::from_le_bytes(self.mem.bytes[o..o + 4].try_into().unwrap())
    }
    /// Runs one instruction; gives the next PC and whether the block syncs.
    fn run(&mut self, word: u32, thumb: bool) -> (u32, bool) {
        let l = lift::lift_for(word, PC, thumb, Profile::V5te).expect("lifts");
        let sync = l.block.insts.iter().any(|i| matches!(i.op, Op::Sync));
        let out = interp::run(&l.block, &mut self.st, &mut self.mem, &mut self.cp).expect("runs");
        let next = match out.flow {
            Flow::Trap(COND_FAIL) => PC + if thumb { 2 } else { 4 },
            _ => out.next as u32,
        };
        (next, sync)
    }
}

const fn key(opc1: u32, crn: u32, crm: u32, opc2: u32) -> u128 {
    (1 << 14 | opc1 << 11 | crn << 7 | crm << 3 | opc2) as u128
}

#[test]
fn mcr_p15_calls_the_hook_with_the_register_key_and_syncs() {
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::r(0), 0x0080_000a);
    // MCR p15, 0, r0, c9, c1, 0 (DTCM region)
    let (next, sync) = c.run(0xee09_0f11, false);
    assert_eq!(next, PC + 4);
    assert!(sync);
    assert_eq!(
        c.cp.calls,
        vec![("arm.cp_write", vec![key(0, 9, 1, 0), 0x0080_000a, 1])]
    );
    // MCR p15, 0, r0, c7, c0, 4 (wait for interrupt)
    let mut c = Cpu::new(Mode::Svc);
    c.run(0xee07_0f90, false);
    assert_eq!(c.cp.calls[0].1[0], key(0, 7, 0, 4));
    // a failed condition calls nothing
    let mut c = Cpu::new(Mode::Svc);
    c.run(0x0e07_0f90, false);
    assert!(c.cp.calls.is_empty());
}

#[test]
fn mrc_p15_reads_the_hook_and_r15_takes_the_flags() {
    let mut c = Cpu::new(Mode::Svc);
    c.cp.read = 0x0005_2078;
    // MRC p15, 0, r1, c1, c0, 0 (control register)
    c.run(0xee11_1f10, false);
    assert_eq!(c.get(regs::r(1)), 0x0005_2078);
    assert_eq!(c.cp.calls, vec![("arm.cp_read", vec![key(0, 1, 0, 0)])]);
    // MRC p15, 0, r15, c1, c0, 0: N Z C V from bits 31 to 28
    let mut c = Cpu::new(Mode::Svc);
    c.cp.read = 0xa000_0000;
    let (next, _) = c.run(0xee11_ff10, false);
    assert_eq!(next, PC + 4);
    assert_eq!(
        (
            c.get(regs::N),
            c.get(regs::Z),
            c.get(regs::C),
            c.get(regs::V)
        ),
        (1, 0, 1, 0)
    );
}

#[test]
fn bkpt_is_a_prefetch_abort() {
    for (word, thumb) in [(0xe120_0070u32, false), (0xbe00, true)] {
        let mut c = Cpu::new(Mode::Sys);
        c.put(regs::T, u32::from(thumb));
        c.put(regs::C, 1);
        let (next, _) = c.run(word, thumb);
        assert_eq!(next, 0x0c);
        assert_eq!(c.get(regs::M), 0x17);
        assert_eq!(c.get(regs::T), 0);
        assert_eq!(c.get(regs::I), 1);
        assert_eq!(c.get(regs::r(14)), PC + 4);
        let spsr = c.get(Mode::Abt.spsr().unwrap());
        assert_eq!(spsr, 0x2000_001f | if thumb { 0x20 } else { 0 });
    }
}

#[test]
fn ldm_writeback_and_empty_list_follow_the_v5_rules() {
    // LDMIA r0!, {r0, r1}: r0 not last -> writeback wins over the load
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::r(0), 0x0200_0010);
    c.word(0x0200_0010, 0x1111);
    c.word(0x0200_0014, 0x2222);
    c.run(0xe8b0_0003, false);
    assert_eq!(c.get(regs::r(0)), 0x0200_0018);
    assert_eq!(c.get(regs::r(1)), 0x2222);
    // LDMIA r1!, {r0, r1}: r1 last -> loaded value, no writeback
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::r(1), 0x0200_0010);
    c.word(0x0200_0010, 0x1111);
    c.word(0x0200_0014, 0x2222);
    c.run(0xe8b1_0003, false);
    assert_eq!(c.get(regs::r(1)), 0x2222);
    // LDMIA r1!, {r1}: only register -> writeback
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::r(1), 0x0200_0010);
    c.word(0x0200_0010, 0x1111);
    c.run(0xe8b1_0002, false);
    assert_eq!(c.get(regs::r(1)), 0x0200_0014);
    // LDMIA r2!, {}: nothing loaded, base + 0x40, no jump
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::r(2), 0x0200_0010);
    let (next, _) = c.run(0xe8b2_0000, false);
    assert_eq!(next, PC + 4);
    assert_eq!(c.get(regs::r(2)), 0x0200_0050);
}

#[test]
fn stm_with_the_base_in_the_list_stores_the_old_base() {
    // STMIA r1!, {r0, r1}: ARMv4 stored the new base, ARMv5 the old one
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::r(0), 0xaaaa);
    c.put(regs::r(1), 0x0200_0010);
    c.run(0xe8a1_0003, false);
    assert_eq!(c.mem32(0x0200_0014), 0x0200_0010);
    assert_eq!(c.get(regs::r(1)), 0x0200_0018);
}

#[test]
fn odd_halfword_loads_are_forced_aligned() {
    let mut c = Cpu::new(Mode::Sys);
    c.word(0x0200_0010, 0x8899_aabb);
    c.put(regs::r(1), 0x0200_0011);
    c.run(0xe1d1_00b0, false); // LDRH r0, [r1]
    assert_eq!(c.get(regs::r(0)), 0xaabb);
    c.run(0xe1d1_00f0, false); // LDRSH r0, [r1]
    assert_eq!(c.get(regs::r(0)), 0xffff_aabb);
}

#[test]
fn unaligned_ldr_rotates_and_ldr_pc_interworks() {
    let mut c = Cpu::new(Mode::Sys);
    c.word(0x0200_0010, 0x1122_3344);
    c.put(regs::r(1), 0x0200_0011);
    c.run(0xe591_0000, false); // LDR r0, [r1]
    assert_eq!(c.get(regs::r(0)), 0x4411_2233);
    // MOV pc, r0 with bit 0 set: no interworking on ARMv5, word aligned
    let mut c = Cpu::new(Mode::Sys);
    c.put(regs::r(0), 0x0200_0041);
    let (next, _) = c.run(0xe1a0_f000, false);
    assert_eq!((next, c.get(regs::T)), (0x0200_0040, 0));
}

#[test]
fn qadd_keeps_q_and_msr_spsr_writes_it() {
    let mut c = Cpu::new(Mode::Svc);
    c.put(regs::PSR_RSVD, 0x0800_0000);
    c.put(regs::r(1), 1);
    c.put(regs::r(2), 2);
    c.run(0xe102_0051, false); // QADD r0, r1, r2: no overflow, Q stays 1
    assert_eq!(c.get(regs::r(0)), 3);
    assert_eq!(c.get(regs::PSR_RSVD) >> 27 & 1, 1);
    c.put(regs::r(3), 0x0800_0000);
    c.run(0xe168_f003, false); // MSR SPSR_f, r3
    assert_eq!(c.get(Mode::Svc.spsr().unwrap()), 0x0800_0000);
}

/// Console results of the ldm2 probe (ARM946E-S, IRQ mode): the writeback
/// of `LDMIA Rn!,{..}^` lands after the user bank's loads, in the current
/// mode's register, so a base in the list ends with the written-back address.
#[test]
fn ldm_with_the_s_bit_and_writeback_writes_the_base_back_last() {
    let mut c = Cpu::new(Mode::Irq);
    for i in 0..15 {
        c.word(0x0200_0010 + 4 * i, 0xc000_0000 | i);
    }
    c.put(regs::r(1), 0x0200_0010);
    c.put(regs::r(13), 0xb000_000d);
    c.run(0xe8f1_7fff, false);
    assert_eq!(c.get(regs::r(1)), 0x0200_004c);
    assert_eq!(c.get(regs::r(0)), 0xc000_0000);
    assert_eq!(c.get(regs::r(13)), 0xb000_000d);
    assert_eq!(c.get(Bank::Usr.slot(13).expect("usr r13")), 0xc000_000d);
    let mut c = Cpu::new(Mode::Irq);
    for i in 0..15 {
        c.word(0x0200_0010 + 4 * i, 0xc000_0000 | i);
    }
    c.put(regs::r(13), 0x0200_0010);
    c.run(0xe8fd_7fff, false);
    assert_eq!(c.get(regs::r(13)), 0x0200_004c);
    assert_eq!(c.get(Bank::Usr.slot(13).expect("usr r13")), 0xc000_000d);
}

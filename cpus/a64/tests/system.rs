//! Tests of the hand-written system lifters, run in the `relift-ir`
//! interpreter where they have no intrinsic.

use relift_cpu_a64::decode::{Feature, Features};
use relift_cpu_a64::lift::{lift, system};
use relift_cpu_a64::regs::{self, C, N, TPIDR_EL0, TPIDRRO_EL0, V, Z};
use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
use relift_ir::{Flow, Next, Op};

const PC: u64 = 0x4000;

fn run(word: u32, s: &mut State) -> u64 {
    let b = lift(word, PC, &Features::ALL).expect("lifts");
    let mut m = FlatMemory {
        base: PC,
        bytes: (0..=255u8).cycle().take(0x1000).collect(),
    };
    interp::run(&b, s, &mut m, &mut NoIntrinsics)
        .expect("runs")
        .next
}

fn mrs(sysreg: u32, rt: u32) -> u32 {
    0xd530_0000 | (sysreg << 5) | rt
}

fn msr(sysreg: u32, rt: u32) -> u32 {
    0xd510_0000 | (sysreg << 5) | rt
}

/// The 15-bit `o0:op1:CRn:CRm:op2` field of a system register.
const fn sysreg(op0: u32, op1: u32, crn: u32, crm: u32, op2: u32) -> u32 {
    ((op0 & 1) << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2
}

const TPIDRRO_ENC: u32 = sysreg(3, 3, 13, 0, 3);
const TPIDR_ENC: u32 = sysreg(3, 3, 13, 0, 2);
const NZCV_ENC: u32 = sysreg(3, 3, 4, 2, 0);

#[test]
fn svc_is_a_trap_that_resumes_after_it() {
    let b = lift(0xd400_0001 | (0x27 << 5), PC, &Features::ALL).expect("lifts");
    assert_eq!(b.exit.flow, Flow::Trap(0x27));
    assert_eq!(b.exit.next, Next::Const(PC + 4));
}

#[test]
fn mrs_reads_the_thread_pointers() {
    let mut s = State::new(regs::regs().len());
    s.regs[TPIDRRO_EL0.index()] = 0x1234_5000;
    s.regs[TPIDR_EL0.index()] = 0x99;
    run(mrs(TPIDRRO_ENC, 3), &mut s);
    assert_eq!(s.regs[regs::x(3).index()], 0x1234_5000);
    run(mrs(TPIDR_ENC, 4), &mut s);
    assert_eq!(s.regs[regs::x(4).index()], 0x99);
}

#[test]
fn msr_tpidrro_is_undefined() {
    assert!(lift(msr(TPIDRRO_ENC, 0), PC, &Features::ALL).is_err());
}

#[test]
fn nzcv_round_trips_through_x() {
    let mut s = State::new(regs::regs().len());
    s.regs[regs::x(1).index()] = 0xa000_0000;
    run(msr(NZCV_ENC, 1), &mut s);
    assert_eq!([N, Z, C, V].map(|r| s.regs[r.index()]), [1, 0, 1, 0]);
    run(mrs(NZCV_ENC, 2), &mut s);
    assert_eq!(s.regs[regs::x(2).index()], 0xa000_0000);
}

#[test]
fn hints_without_the_feature_are_nops() {
    let no_pac = Features::ALL.without(Feature::PAuth);
    for w in [0xd503_245f, 0xd503_251f, 0xd503_20ff] {
        let b = lift(w, PC, &no_pac).expect("lifts");
        assert!(b.insts.is_empty(), "{w:#x}");
        assert_eq!(b.exit.next, Next::Const(PC + 4));
    }
    assert!(lift(0xd503_20ff, PC, &Features::ALL).is_err());
}

#[test]
fn counter_reads_are_intrinsics() {
    let b = lift(mrs(sysreg(3, 3, 14, 0, 1), 0), PC, &Features::ALL).expect("lifts");
    assert!(
        b.insts
            .iter()
            .any(|i| i.op == Op::Intrinsic(system::CNTPCT, Vec::new()))
    );
}

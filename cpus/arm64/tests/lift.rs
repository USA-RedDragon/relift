//! Lifter tests on words built from field values, run in the `relift-ir`
//! interpreter. The differential check against Unicorn lives in `check/`.

use relift_cpu_arm64::cpu::{Cpu, Va};
use relift_cpu_arm64::decode::{Features, decode};
use relift_cpu_arm64::lift::{LiftError, lift, lift_hand, lifter, sail, supported};
use relift_cpu_arm64::regs::{self, C, N, SP, V, Z};
use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
use relift_ir::{Flow, Next};

const ALL: Cpu = Cpu::new(Features::ALL, Va::BITS39);

const PC: u64 = 0x4000;

fn state() -> State {
    State::new(regs::regs().len())
}

fn mem() -> FlatMemory {
    FlatMemory {
        base: 0x1_0000,
        bytes: (0..=255u8).cycle().take(0x1000).collect(),
    }
}

fn step(word: u32, s: &mut State, m: &mut FlatMemory) -> u64 {
    let b = lift(word, PC, &ALL).expect("lifts");
    interp::run(&b, s, m, &mut NoIntrinsics).expect("runs").next
}

fn x(s: &State, n: u32) -> u64 {
    s.regs[regs::x(n).index()] as u64
}

fn setx(s: &mut State, n: u32, v: u64) {
    s.regs[regs::x(n).index()] = u128::from(v);
}

fn flags(s: &State) -> [u128; 4] {
    [N, Z, C, V].map(|r| s.regs[r.index()])
}

#[test]
fn subs_immediate_sets_all_four_flags() {
    let subs = |sf: u32, imm: u32, rn: u32, rd: u32| {
        (sf << 31) | 0x7100_0000 | (imm << 10) | (rn << 5) | rd
    };
    let mut s = state();
    setx(&mut s, 1, 5);
    step(subs(1, 5, 1, 0), &mut s, &mut mem());
    assert_eq!(x(&s, 0), 0);
    assert_eq!(flags(&s), [0, 1, 1, 0]);
    setx(&mut s, 1, 0x8000_0000);
    step(subs(0, 1, 1, 0), &mut s, &mut mem());
    assert_eq!(x(&s, 0), 0x7fff_ffff);
    assert_eq!(flags(&s), [0, 0, 1, 1]);
    setx(&mut s, 1, 0);
    step(subs(1, 1, 1, 31), &mut s, &mut mem());
    assert_eq!(flags(&s), [1, 0, 0, 0]);
}

#[test]
fn add_immediate_reads_and_writes_sp() {
    let mut s = state();
    s.regs[SP.index()] = 0x1000;
    step(
        0x9100_0000 | (16 << 10) | (31 << 5) | 31,
        &mut s,
        &mut mem(),
    );
    assert_eq!(s.regs[SP.index()], 0x1010);
}

#[test]
fn wide_moves_and_logical_immediates() {
    let mut s = state();
    step(
        0xd280_0000 | (0x1234 << 5) | (1 << 21) | 3,
        &mut s,
        &mut mem(),
    );
    assert_eq!(x(&s, 3), 0x1234_0000);
    step(0xf280_0000 | (0xbeef << 5) | 3, &mut s, &mut mem());
    assert_eq!(x(&s, 3), 0x1234_beef);
    step(0x1280_0000 | 4, &mut s, &mut mem());
    assert_eq!(x(&s, 4), 0xffff_ffff);
    setx(&mut s, 1, u64::MAX);
    step(0x9240_0000 | (7 << 10) | (1 << 5) | 2, &mut s, &mut mem());
    assert_eq!(x(&s, 2), 0xff);
}

#[test]
fn adrp_and_branches_compute_targets() {
    let mut s = state();
    let adrp = 0x9000_0000 | (1 << 29) | (2 << 5) | 7;
    step(adrp, &mut s, &mut mem());
    assert_eq!(x(&s, 7), (PC & !0xfff) + (9 << 12));

    let bl = lift(0x9400_0000 | 0x3ff_ffff, PC, &ALL).expect("lifts");
    assert_eq!(bl.exit.next, Next::Const(PC - 4));
    assert_eq!(bl.exit.flow, Flow::Call { ret: PC + 4 });
    // `bl .+8` steps over an inline literal: a jump that sets x30.
    let lit = lift(0x9400_0002, PC, &ALL).expect("lifts");
    assert_eq!(lit.exit.next, Next::Const(PC + 8));
    assert_eq!(lit.exit.flow, Flow::Jump);
    let mut s = state();
    let mut m = mem();
    assert_eq!(step(0x9400_0002, &mut s, &mut m), PC + 8);
    assert_eq!(x(&s, 30), PC + 4);

    let beq = 0x5400_0000 | (4 << 5);
    s.regs[Z.index()] = 1;
    assert_eq!(step(beq, &mut s, &mut mem()), PC + 16);
    s.regs[Z.index()] = 0;
    assert_eq!(step(beq, &mut s, &mut mem()), PC + 4);

    let tbnz = 0x3700_0000 | (1 << 31) | (1 << 19) | (2 << 5) | 9;
    setx(&mut s, 9, 1 << 33);
    assert_eq!(step(tbnz, &mut s, &mut mem()), PC + 8);

    let ret = lift(0xd65f_03c0, PC, &ALL).expect("lifts");
    assert_eq!(ret.exit.flow, Flow::Return);
}

#[test]
fn loads_stores_and_pairs_use_the_window() {
    let mut s = state();
    let mut m = mem();
    setx(&mut s, 1, 0x1_0000);
    step(0xf940_0000 | (1 << 10) | (1 << 5), &mut s, &mut m);
    assert_eq!(x(&s, 0), 0x0f0e_0d0c_0b0a_0908);
    step(0xb980_0000 | (1 << 5) | 2, &mut s, &mut m);
    assert_eq!(x(&s, 2), 0x0302_0100);
    setx(&mut s, 3, 0xaabb);
    let stp_pre = 0xa980_0000 | (0x7e << 15) | (4 << 10) | (1 << 5) | 3;
    setx(&mut s, 1, 0x1_0100);
    step(stp_pre, &mut s, &mut m);
    assert_eq!(x(&s, 1), 0x1_00f0);
    assert_eq!(&m.bytes[0xf0..0xf2], &[0xbb, 0xaa]);
}

#[test]
fn exclusive_pairs_report_status() {
    let mut s = state();
    let mut m = mem();
    setx(&mut s, 1, 0x1_0008);
    setx(&mut s, 2, 0x55);
    step(0xc85f_fc00 | (1 << 5), &mut s, &mut m);
    step(0xc800_fc00 | (3 << 16) | (1 << 5) | 2, &mut s, &mut m);
    assert_eq!(x(&s, 3), 0);
    assert_eq!(m.bytes[8], 0x55);
    step(0xc800_fc00 | (3 << 16) | (1 << 5) | 2, &mut s, &mut m);
    assert_eq!(x(&s, 3), 1);
}

/// LDXP X3, X4, [X1] reads both registers in one 16-byte exclusive access
/// that STXP can complete, and a base that is not 16-byte aligned leaves
/// for the alignment fault before any register changes. The checker only
/// places aligned bases for exclusives, so the fault is tested here.
#[test]
fn exclusive_pair_of_64_bit_registers() {
    let ldxp = 0xc87f_0000 | (4 << 10) | (1 << 5) | 3;
    let stxp = 0xc820_0000 | (5 << 16) | (4 << 10) | (1 << 5) | 3;
    let mut s = state();
    let mut m = mem();
    setx(&mut s, 1, 0x1_0010);
    step(ldxp, &mut s, &mut m);
    assert_eq!(x(&s, 3), 0x1716_1514_1312_1110);
    assert_eq!(x(&s, 4), 0x1f1e_1d1c_1b1a_1918);
    step(stxp, &mut s, &mut m);
    assert_eq!(x(&s, 5), 0);
    setx(&mut s, 1, 0x1_0008);
    setx(&mut s, 3, 7);
    let b = lift(ldxp, PC, &ALL).expect("lifts");
    let o = interp::run(&b, &mut s, &mut m, &mut NoIntrinsics).expect("runs");
    assert_eq!(o.flow, Flow::Trap(sail::SLOW_ALIGN));
    assert_eq!(x(&s, 3), 7);
}

/// Runs `word` lifted for `cpu` with x1 = `addr`, x2 = 1; returns the
/// block's flow and x3.
fn at(word: u32, cpu: &Cpu, addr: u64) -> (Flow, u64) {
    let mut s = state();
    let mut m = mem();
    setx(&mut s, 1, addr);
    setx(&mut s, 2, 1);
    setx(&mut s, 3, 7);
    let b = lift(word, PC, cpu).expect("lifts");
    let o = interp::run(&b, &mut s, &mut m, &mut NoIntrinsics).expect("runs");
    (o.flow, x(&s, 3))
}

#[test]
fn the_cortex_a78c_runs_unaligned_atomics_inside_a_16_byte_granule() {
    let a78c = Cpu::new(Features::CORTEX_A78C, Va::BITS39);
    let a57 = Cpu::new(Features::CORTEX_A57, Va::BITS39);
    let ldadd = 0xb822_0023;
    let ldar = 0x88df_fc23;
    let ldxr = 0x885f_7c23;
    let fault = Flow::Trap(sail::SLOW_ALIGN);
    let (flow, old) = at(ldadd, &a78c, 0x1_0121);
    assert_ne!(flow, fault);
    assert_eq!(old, 0x2423_2221);
    assert_ne!(at(ldadd, &a78c, 0x1_012c).0, fault);
    assert_eq!(at(ldadd, &a78c, 0x1_012d).0, fault);
    let (flow, v) = at(ldar, &a78c, 0x1_0125);
    assert_ne!(flow, fault);
    assert_eq!(v, 0x2827_2625);
    assert_eq!(at(ldar, &a78c, 0x1_013e).0, fault);
    assert_eq!(at(ldxr, &a78c, 0x1_0121).0, fault);
    let (flow, v) = at(ldar, &a57, 0x1_0124);
    assert_ne!(flow, fault);
    assert_eq!(v, 0x2726_2524);
    assert_eq!(at(ldar, &a57, 0x1_0125).0, fault);
}

#[test]
fn unpredictable_and_unallocated_words_are_refused() {
    let ldr_post_same = 0xf840_0400 | (1 << 5) | 1;
    assert!(matches!(
        lift(ldr_post_same, PC, &ALL),
        Err(LiftError::Unpredictable(_))
    ));
    let unallocated = 0x0200_0000;
    assert!(relift_cpu_arm64::decode::decode(unallocated).is_none());
    assert!(matches!(
        lift(unallocated, PC, &ALL),
        Err(LiftError::Unallocated(_))
    ));
    assert!(supported().count() > 150);
}

/// A small deterministic generator for the agreement test.
struct Lcg(u64);

impl Lcg {
    fn next(&mut self) -> u64 {
        self.0 = self
            .0
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        self.0 ^ (self.0 >> 29)
    }
}

/// Every encoding with both a hand-written lifter and a generated template
/// computes the same state in the interpreter, from random words and states
/// whose base registers point into the memory window.
#[test]
fn generated_templates_agree_with_the_hand_lifter() {
    let mut r = Lcg(7);
    let encs: Vec<_> = supported()
        .filter(|e| lifter(*e).is_some() && sail::has_template(*e))
        .collect();
    assert!(
        encs.len() >= 10,
        "control: only {} encodings have both lifters",
        encs.len()
    );
    for enc in encs {
        let mut tried = 0;
        for _ in 0..400 {
            let w = enc.value() | (r.next() as u32 & !enc.mask());
            if decode(w).map(|i| i.encoding) != Some(enc) {
                continue;
            }
            let (Ok(h), Ok(g)) = (lift_hand(w, PC), lift(w, PC, &ALL)) else {
                continue;
            };
            let mut s = state();
            for i in 0..31 {
                setx(&mut s, i, 0x1_0000 + (r.next() & 0x7f8));
            }
            s.regs[SP.index()] = 0x1_0800;
            for f in [N, Z, C, V] {
                s.regs[f.index()] = u128::from(r.next() & 1);
            }
            let (mut s1, mut s2) = (s.clone(), s);
            let (mut m1, mut m2) = (mem(), mem());
            let o1 = interp::run(&h, &mut s1, &mut m1, &mut NoIntrinsics);
            let o2 = interp::run(&g, &mut s2, &mut m2, &mut NoIntrinsics);
            match (o1, o2) {
                (Ok(a), Ok(b)) => {
                    assert_eq!(a.next, b.next, "{} {w:#010x}: next pc", enc.name());
                    assert_eq!(s1.regs, s2.regs, "{} {w:#010x}: registers", enc.name());
                    assert_eq!(m1.bytes, m2.bytes, "{} {w:#010x}: memory", enc.name());
                }
                (Err(_), Err(_)) => {}
                (a, b) => panic!("{} {w:#010x}: {a:?} vs {b:?}", enc.name()),
            }
            tried += 1;
        }
        assert!(tried > 50, "control: {} ran only {tried} words", enc.name());
    }
}

/// FCVTZS H12, H1, #15 (scalar, half precision, fixed point) writes the
/// 16-bit result zero-extended to the whole vector register. Unicorn 2.1.5
/// sign-extends it to 32 bits (0xffffffde); the Apple M2 and QEMU 11.1.2
/// give 0xffde, as here.
#[test]
fn fcvtzs_half_to_fixed_zero_extends_unlike_unicorn() {
    let word = 0x5f11_fc2c;
    let n = (word >> 5) & 31;
    let d = word & 31;
    let mut s = state();
    s.regs[regs::vreg(n).index()] = 0xdead_beef_0000_9440;
    s.regs[regs::vreg(d).index()] = u128::MAX;
    step(word, &mut s, &mut mem());
    assert_eq!(s.regs[regs::vreg(d).index()], 0xffde);
}

/// The ordering of the first load in the IR of `word`.
fn load_order(word: u32) -> String {
    let text = lift(word, PC, &ALL).expect("lifts").to_string();
    let line = text
        .lines()
        .find(|l| l.contains("load"))
        .unwrap_or_else(|| panic!("no load in {text}"));
    line.to_string()
}

#[test]
fn rcpc_loads_keep_their_weaker_ordering_apart_from_ldar() {
    let ldar = 0xc8df_fc20; // LDAR X0, [X1]
    let ldapr = 0xf8bf_c020; // LDAPR X0, [X1]
    let ldapur = 0x1900_0000 | (3 << 30) | (1 << 22) | (1 << 5); // LDAPUR X0, [X1]
    assert!(load_order(ldar).contains("acquire"), "{}", load_order(ldar));
    assert!(!load_order(ldar).contains("acquirepc"));
    assert!(
        load_order(ldapr).contains("acquirepc"),
        "{}",
        load_order(ldapr)
    );
    assert!(
        load_order(ldapur).contains("acquirepc"),
        "{}",
        load_order(ldapur)
    );
}

/// LDAPRB, LDAPRH and LDAPR W and X (FEAT_LRCPC), and LDAPURB, LDAPURH and
/// LDAPUR W and X (FEAT_LRCPC2), of `X1` into register 0.
const RCPC_LOADS: [(&str, u32); 8] = [
    ("LDAPRB W0, [X1]", 0x38bf_c020),
    ("LDAPRH W0, [X1]", 0x78bf_c020),
    ("LDAPR W0, [X1]", 0xb8bf_c020),
    ("LDAPR X0, [X1]", 0xf8bf_c020),
    ("LDAPURB W0, [X1]", 0x1940_0020),
    ("LDAPURH W0, [X1]", 0x5940_0020),
    ("LDAPUR W0, [X1]", 0x9940_0020),
    ("LDAPUR X0, [X1]", 0xd940_0020),
];

#[test]
fn every_size_of_ldapr_and_ldapur_lifts_on_the_a78c_and_is_undefined_on_the_a57() {
    let a78c = Cpu::new(Features::CORTEX_A78C, Va::BITS39);
    let a57 = Cpu::new(Features::CORTEX_A57, Va::BITS39);
    for (name, word) in RCPC_LOADS {
        let text = lift(word, PC, &a78c)
            .unwrap_or_else(|e| panic!("{name}: {e:?}"))
            .to_string();
        assert!(
            text.lines()
                .any(|l| l.contains("load") && l.contains("acquirepc")),
            "{name}: {text}"
        );
        assert!(
            matches!(lift(word, PC, &a57), Err(LiftError::Unallocated(_))),
            "{name} on the A57"
        );
    }
}

/// The value `MRS X0, <reg>` leaves in X0 on `cpu`, for (op1, CRn, CRm, op2).
fn mrs_value(cpu: &Cpu, (op1, crn, crm, op2): (u32, u32, u32, u32)) -> Result<u64, LiftError> {
    let word = 0xd530_0000 | 1 << 19 | op1 << 16 | crn << 12 | crm << 8 | op2 << 5;
    let b = lift(word, PC, cpu)?;
    let mut s = state();
    let mut m = mem();
    interp::run(&b, &mut s, &mut m, &mut NoIntrinsics).expect("runs");
    Ok(x(&s, 0))
}

#[test]
fn an_mrs_of_an_id_register_reads_the_features_where_the_os_answers_it() {
    use relift_cpu_arm64::idregs::{ID_AA64ISAR1_EL1, user_id_reg};
    let f = Features::CORTEX_A78C;
    let linux = Cpu::new(f, Va::BITS39).with_user_ids(0x410f_d4b2);
    let (_, op1, crn, crm, op2) = ID_AA64ISAR1_EL1;
    let isar1 = mrs_value(&linux, (op1, crn, crm, op2)).expect("lifts");
    assert_eq!(Some(isar1), user_id_reg(&f, 0x410f_d4b2, ID_AA64ISAR1_EL1));
    assert_eq!(isar1 >> 20 & 0xf, 2, "LRCPC2");
    assert_eq!(
        mrs_value(&linux, (0, 0, 0, 0)).expect("MIDR lifts"),
        0x410f_d4b2
    );
    let plain = Cpu::new(f, Va::BITS39);
    assert!(
        mrs_value(&plain, (op1, crn, crm, op2)).is_err(),
        "no OS answers it"
    );
}

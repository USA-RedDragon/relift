//! Pointer authentication lifted for the Cortex-A78C and run in the
//! `relift-ir` interpreter: a signed pointer authenticates back to itself and
//! strips back to itself, a wrong modifier leaves a pointer that is not
//! canonical, and the address bits and bit 55 never change, for pointers
//! with bit 55 set and clear. Exact PAC values are checked against the
//! sail-arm model by `relift-check --pac-states` (`check/README.md`).

use relift_cpu_arm64::cpu::{Cpu, Va};
use relift_cpu_arm64::decode::Features;
use relift_cpu_arm64::lift::{LiftError, lift};
use relift_cpu_arm64::regs::{self, CHECK_PAC_KEYS, PacKey, SP};
use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};

const PC: u64 = 0x4000;

const A78C: Cpu = Cpu::new(Features::CORTEX_A78C, Va::BITS39);

const PACIA: u32 = 0xdac1_0000 | (2 << 5) | 1;
const AUTIA: u32 = 0xdac1_1000 | (2 << 5) | 1;
const XPACI: u32 = 0xdac1_43e0 | 1;
const PACIASP: u32 = 0xd503_233f;
const AUTIASP: u32 = 0xd503_23bf;
const XPACLRI: u32 = 0xd503_20ff;

/// The address bits of the 39-bit layout.
const LOW: u64 = (1 << 39) - 1;

const POINTERS: [u64; 4] = [
    0x0000_0012_3456_7890,
    0x0000_007f_ffff_fffc,
    0xffff_ff80_0000_1000,
    0xffff_ffd2_3456_7890,
];

fn state() -> State {
    let mut s = State::new(regs::regs().len());
    for (k, key) in PacKey::ALL.into_iter().enumerate() {
        s.regs[key.reg(true).index()] = u128::from(CHECK_PAC_KEYS[2 * k]);
        s.regs[key.reg(false).index()] = u128::from(CHECK_PAC_KEYS[2 * k + 1]);
    }
    s
}

fn run(word: u32, s: &mut State) {
    let b = lift(word, PC, &A78C).expect("lifts");
    let mut m = FlatMemory {
        base: PC,
        bytes: vec![0; 16],
    };
    interp::run(&b, s, &mut m, &mut NoIntrinsics).expect("runs");
}

fn x(s: &State, n: u32) -> u64 {
    s.regs[regs::x(n).index()] as u64
}

/// Whether bits 63 to 39 of `p` are all copies of bit 55.
fn canonical(p: u64) -> bool {
    let top = p >> 39;
    let fill = if p >> 55 & 1 == 1 { (1 << 25) - 1 } else { 0 };
    top == fill
}

/// Runs `sign`, `auth` and `strip` on `reg` (signed with `modifier`) and
/// checks the round trips; `set` puts a value in the register and the
/// modifier in its place.
fn round_trip(sign: u32, auth: u32, strip: u32, reg: u32, set: &dyn Fn(&mut State, u64, u64)) {
    for p in POINTERS {
        for modifier in [0, 0x7fff_ffff_f000, u64::MAX] {
            let mut s = state();
            set(&mut s, p, modifier);
            run(sign, &mut s);
            let signed = x(&s, reg);
            assert_ne!(signed, p, "{sign:#x} left {p:#x} unsigned");
            assert_eq!(signed & LOW, p & LOW, "{sign:#x} {p:#x}");
            assert_eq!(signed >> 55 & 1, p >> 55 & 1, "{sign:#x} {p:#x}");

            let mut t = state();
            set(&mut t, signed, modifier);
            run(auth, &mut t);
            assert_eq!(x(&t, reg), p, "{auth:#x} of {signed:#x}");

            let mut u = state();
            set(&mut u, signed, modifier);
            run(strip, &mut u);
            assert_eq!(x(&u, reg), p, "{strip:#x} of {signed:#x}");

            let mut w = state();
            set(&mut w, signed, modifier ^ 0x10);
            run(auth, &mut w);
            let bad = x(&w, reg);
            assert_eq!(bad & LOW, p & LOW, "{auth:#x} wrong modifier {signed:#x}");
            assert!(
                !canonical(bad),
                "{auth:#x} accepted a wrong modifier: {bad:#x}"
            );
        }
    }
}

#[test]
fn pacia_autia_and_xpaci_round_trip_and_a_wrong_modifier_fails() {
    round_trip(PACIA, AUTIA, XPACI, 1, &|s, p, m| {
        s.regs[regs::x(1).index()] = u128::from(p);
        s.regs[regs::x(2).index()] = u128::from(m);
    });
}

#[test]
fn paciasp_autiasp_and_xpaclri_round_trip_and_a_wrong_sp_fails() {
    round_trip(PACIASP, AUTIASP, XPACLRI, 30, &|s, p, m| {
        s.regs[regs::x(30).index()] = u128::from(p);
        s.regs[SP.index()] = u128::from(m);
    });
}

#[test]
fn a_tampered_pac_fails_authentication() {
    for p in POINTERS {
        let mut s = state();
        s.regs[regs::x(1).index()] = u128::from(p);
        s.regs[regs::x(2).index()] = 0x1234;
        run(PACIA, &mut s);
        let signed = x(&s, 1);
        for bit in [39, 54, 56, 63] {
            let mut t = state();
            t.regs[regs::x(1).index()] = u128::from(signed ^ (1 << bit));
            t.regs[regs::x(2).index()] = 0x1234;
            run(AUTIA, &mut t);
            assert!(!canonical(x(&t, 1)), "{p:#x} with bit {bit} flipped");
        }
    }
}

#[test]
fn top_byte_ignore_is_refused_for_every_form() {
    let tbi = Cpu::new(
        Features::CORTEX_A78C,
        Va {
            bits: 39,
            tbi: true,
        },
    );
    for w in [PACIA, AUTIA, XPACI, PACIASP, AUTIASP, XPACLRI] {
        assert!(
            matches!(lift(w, PC, &tbi), Err(LiftError::Layout { .. })),
            "{w:#x}"
        );
    }
}

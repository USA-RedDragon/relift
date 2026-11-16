use std::collections::{BTreeMap, BTreeSet};
use std::sync::Arc;

use super::{Code, Host, msr_fpcr, prove};
use crate::cpu::{Cpu, Va};
use crate::decode::Features;
use crate::lift::lift_for_emit;

const RET: u32 = 0xd65f03c0;
const MOV_X29_SP: u32 = 0x910003fd;
const ADD_X0_SP_8: u32 = 0x910023e0;
const MOV_X1_SP: u32 = 0x910003e1;
const BLR_X8: u32 = 0xd63f0100;
const PUSH_FRAME: u32 = 0xa9be7bfd;
const POP_FRAME: u32 = 0xa8c27bfd;
const MOV_X0_M1: u32 = 0x92800000;
const MOV_W8_RP: u32 = 0x52a00808;
const STR_W8_SP: u32 = 0xb90003e8;
const STR_W8_SP_8: u32 = 0xb9000be8;
const NOP: u32 = 0xd503201f;
const ADD_X9_X0_X10: u32 = 0x8b0a0009;
const ADD_X9_X1_X10: u32 = 0x8b0a0029;
const STR_W8_X9: u32 = 0xb9000128;
const PUSH_X0_X1: u32 = 0xa9bf07e0;
const POP_X0_X1: u32 = 0xa8c107e0;
const MRS_X0_FPCR: u32 = 0xd53b4400;
const MSR_FPCR_X0: u32 = 0xd51b4400;

const FESETROUND: [u32; 6] = [
    0xd53b4401, 0x12077021, 0x2a000021, 0xd51b4401, 0x52800000, RET,
];
const FEGETENV: [u32; 5] = [0xd53b4401, 0xd53b4422, 0x29000801, 0x52800000, RET];
const FESETENV: [u32; 9] = [
    0xd2800001, 0xd2800002, 0xb100041f, 0x54000040, 0x29400801, 0xd51b4401, 0xd51b4422, 0x52800000,
    RET,
];

fn movz_w0(imm: u32) -> u32 {
    0x5280_0000 | (imm << 5)
}

fn movz_w0_hi(imm: u32) -> u32 {
    0x52a0_0000 | (imm << 5)
}

fn bl(from: u64, to: u64) -> u32 {
    let off = (to.wrapping_sub(from) as i64 >> 2) as u32 & 0x03ff_ffff;
    0x9400_0000 | off
}

#[derive(Default)]
struct Prog {
    words: BTreeMap<u64, u32>,
    entries: BTreeSet<u64>,
    pads: Vec<(u64, u64, u64)>,
    callers: BTreeMap<u64, Vec<u64>>,
    escaped: BTreeSet<u64>,
}

impl Prog {
    fn func(&mut self, at: u64, words: &[u32]) {
        self.entries.insert(at);
        for (k, &w) in words.iter().enumerate() {
            self.words.insert(at + 4 * k as u64, w);
        }
    }

    fn calls(&mut self, caller: u64, callee: u64) {
        self.callers.entry(callee).or_default().push(caller);
    }

    fn sites(&self) -> BTreeMap<u64, BTreeSet<u64>> {
        let mut out: BTreeMap<u64, BTreeSet<u64>> = BTreeMap::new();
        for (&pc, &w) in &self.words {
            if msr_fpcr(w) {
                let f = self.entries.range(..=pc).next_back().copied();
                out.entry(pc).or_default().extend(f);
            }
        }
        out
    }

    fn constant(&self) -> bool {
        let block = |pc: u64| {
            let w = *self.words.get(&pc)?;
            lift_for_emit(w, pc, &Cpu::new(Features::ALL, Va::BITS39))
                .ok()
                .map(Arc::new)
        };
        let slot = |_: u64| None;
        let table = |_: u64| None;
        let is_entry = |a: u64| self.entries.contains(&a);
        let pads = |a: u64| {
            self.pads
                .iter()
                .filter(|p| a >= p.0 && a < p.0 + p.1)
                .map(|p| p.2)
                .collect()
        };
        let host = |_: u64| Host::Guest;
        let code = Code {
            block: &block,
            slot: &slot,
            table: &table,
            is_entry: &is_entry,
            pads: &pads,
            host: &host,
        };
        let callers = |f: u64| {
            if self.escaped.contains(&f) {
                None
            } else {
                Some(self.callers.get(&f).cloned().unwrap_or_default())
            }
        };
        prove(&code, &callers, &self.sites()).constant
    }
}

const SWITCH_STACK: u32 = 0x910002df;
const KEEP_SP_X19: u32 = 0x910003f3;
const SP_FROM_X19: u32 = 0x9100027f;

const SETROUND: u64 = 0x1000;
const GETENV: u64 = 0x1100;
const SETENV: u64 = 0x1200;
const SAVE: u64 = 0x1300;
const RESTORE: u64 = 0x1400;
const CALLER: u64 = 0x2000;

fn setround_caller(mode: u32) -> Prog {
    let mut p = Prog::default();
    p.func(SETROUND, &FESETROUND);
    let w = if mode >> 16 != 0 {
        movz_w0_hi(mode >> 16)
    } else {
        movz_w0(mode)
    };
    p.func(
        CALLER,
        &[PUSH_FRAME, w, bl(CALLER + 8, SETROUND), POP_FRAME, RET],
    );
    p.calls(CALLER, SETROUND);
    p.escaped.insert(CALLER);
    p
}

#[test]
fn round_to_nearest_is_the_default() {
    assert!(setround_caller(0).constant());
}

#[test]
fn another_rounding_mode_is_not() {
    assert!(!setround_caller(0x40_0000).constant());
}

#[test]
fn a_write_nothing_calls_never_runs() {
    let mut p = setround_caller(0x40_0000);
    p.escaped.clear();
    assert!(p.constant());
}

#[test]
fn a_writer_whose_address_escapes_is_not() {
    let mut p = setround_caller(0);
    p.escaped.insert(SETROUND);
    assert!(!p.constant());
}

#[test]
fn a_write_in_no_function_is_not() {
    let mut p = setround_caller(0);
    p.words.insert(0x10, MSR_FPCR_X0);
    assert!(!p.constant());
}

/// A program with `fegetenv`, `fesetenv`, a scope guard that saves the
/// environment and sets the default (`save`), one that restores it
/// (`restore`), and a caller whose body between the two is `body`.
fn guarded(body: &[u32]) -> Prog {
    let mut p = Prog::default();
    p.func(GETENV, &FEGETENV);
    p.func(SETENV, &FESETENV);
    p.func(
        SAVE,
        &[
            PUSH_FRAME,
            MOV_X29_SP,
            bl(SAVE + 8, GETENV),
            MOV_X0_M1,
            bl(SAVE + 16, SETENV),
            POP_FRAME,
            RET,
        ],
    );
    p.func(
        RESTORE,
        &[
            PUSH_FRAME,
            MOV_X29_SP,
            bl(RESTORE + 8, SETENV),
            POP_FRAME,
            RET,
        ],
    );
    let mut c = vec![PUSH_FRAME, MOV_X29_SP, ADD_X0_SP_8];
    c.push(bl(CALLER + 4 * c.len() as u64, SAVE));
    c.extend_from_slice(body);
    c.push(ADD_X0_SP_8);
    c.push(bl(CALLER + 4 * c.len() as u64, RESTORE));
    c.extend_from_slice(&[POP_FRAME, RET]);
    p.func(CALLER, &c);
    p.calls(SAVE, GETENV);
    p.calls(SAVE, SETENV);
    p.calls(RESTORE, SETENV);
    p.calls(CALLER, SAVE);
    p.calls(CALLER, RESTORE);
    p.escaped.insert(CALLER);
    p
}

#[test]
fn a_restore_of_a_saved_environment_keeps_the_default() {
    assert!(guarded(&[BLR_X8]).constant());
}

#[test]
fn a_save_deeper_than_the_inline_depth_is_followed() {
    let mut p = guarded(&[BLR_X8]);
    let mut at = SAVE;
    for k in 0..4u64 {
        let w = 0x3000 + 0x100 * k;
        p.func(w, &[PUSH_FRAME, MOV_X29_SP, bl(w + 8, at), POP_FRAME, RET]);
        p.calls(w, at);
        at = w;
    }
    p.words.insert(CALLER + 12, bl(CALLER + 12, at));
    p.calls(CALLER, at);
    assert!(p.constant());
}

#[test]
fn a_saved_environment_whose_address_escapes_is_not_trusted() {
    assert!(!guarded(&[ADD_X0_SP_8, BLR_X8]).constant());
}

#[test]
fn another_object_escaping_does_not_reach_the_saved_environment() {
    assert!(guarded(&[MOV_X1_SP, BLR_X8]).constant());
}

#[test]
fn an_environment_overwritten_before_the_restore_is_not() {
    assert!(!guarded(&[MOV_W8_RP, STR_W8_SP_8]).constant());
}

#[test]
fn a_store_at_an_unknown_offset_from_the_environment_may_reach_it() {
    assert!(!guarded(&[MOV_W8_RP, ADD_X0_SP_8, ADD_X9_X0_X10, STR_W8_X9]).constant());
}

#[test]
fn a_store_at_an_unknown_offset_from_another_object_does_not_reach_it() {
    assert!(guarded(&[MOV_W8_RP, MOV_X1_SP, ADD_X9_X1_X10, STR_W8_X9]).constant());
}

#[test]
fn a_restore_without_a_save_is_not() {
    let mut p = guarded(&[BLR_X8]);
    p.words.insert(CALLER + 12, NOP);
    assert!(!p.constant());
}

#[test]
fn a_landing_pad_restores_what_the_guarded_call_left() {
    let mut p = guarded(&[BLR_X8]);
    let pad = CALLER + 0x100;
    p.func(pad, &[ADD_X0_SP_8, bl(pad + 4, RESTORE), POP_FRAME, RET]);
    p.entries.remove(&pad);
    p.pads.push((CALLER + 16, 4, pad));
    p.calls(CALLER, RESTORE);
    assert!(p.constant());
}

#[test]
fn a_landing_pad_starts_only_from_the_calls_it_serves() {
    let mut p = guarded(&[BLR_X8]);
    p.words.insert(CALLER + 4, BLR_X8);
    let pad = CALLER + 0x100;
    p.func(pad, &[ADD_X0_SP_8, bl(pad + 4, RESTORE), POP_FRAME, RET]);
    p.entries.remove(&pad);
    p.pads.push((CALLER + 16, 4, pad));
    p.calls(CALLER, RESTORE);
    assert!(p.constant());
}

#[test]
fn a_landing_pad_reached_before_the_save_is_not() {
    let mut p = guarded(&[BLR_X8]);
    p.words.insert(CALLER + 4, BLR_X8);
    let pad = CALLER + 0x100;
    p.func(pad, &[ADD_X0_SP_8, bl(pad + 4, RESTORE), POP_FRAME, RET]);
    p.entries.remove(&pad);
    p.pads.push((CALLER + 4, 16, pad));
    p.calls(CALLER, RESTORE);
    assert!(!p.constant());
}

#[test]
fn a_callback_wrapper_restores_what_it_saved() {
    let mut p = Prog::default();
    p.func(
        CALLER,
        &[
            PUSH_FRAME,
            MRS_X0_FPCR,
            PUSH_X0_X1,
            BLR_X8,
            POP_X0_X1,
            MSR_FPCR_X0,
            POP_FRAME,
            RET,
        ],
    );
    p.escaped.insert(CALLER);
    assert!(p.constant());
}

#[test]
fn a_callback_wrapper_that_switches_stacks_restores_what_it_saved() {
    let mut p = Prog::default();
    p.func(
        CALLER,
        &[
            KEEP_SP_X19,
            SWITCH_STACK,
            MRS_X0_FPCR,
            PUSH_X0_X1,
            BLR_X8,
            POP_X0_X1,
            MSR_FPCR_X0,
            SP_FROM_X19,
            RET,
        ],
    );
    p.escaped.insert(CALLER);
    assert!(p.constant());
}

#[test]
fn a_wrapper_that_restores_a_value_it_did_not_save_is_not() {
    let mut p = Prog::default();
    p.func(
        CALLER,
        &[
            PUSH_FRAME,
            STR_W8_SP,
            BLR_X8,
            POP_X0_X1,
            MSR_FPCR_X0,
            POP_FRAME,
            RET,
        ],
    );
    p.escaped.insert(CALLER);
    assert!(!p.constant());
}

#[test]
fn the_writes_are_the_msr_fpcr_words() {
    assert!(msr_fpcr(MSR_FPCR_X0));
    assert!(msr_fpcr(0xd51b4401));
    assert!(!msr_fpcr(0xd51b4422));
    assert!(!msr_fpcr(MRS_X0_FPCR));
}

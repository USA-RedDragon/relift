use std::collections::{BTreeMap, BTreeSet};

use relift_guest::toy::{Toy, branch, word};
use relift_guest::{Frontend, Home, LiftFailure};
use relift_ir::{Block, FpOp, Reg, RegFile, Width};

use super::{Keeps, Shape, program, solve};
use crate::func::Emitted1;
use crate::reuse::{self, Targets};
use crate::{Function, Insn, Options, Target, emit};

/// The toy frontend with `r1` and the stack pointer `r2` callee-saved.
struct Saved;

/// Bit 0: `r1`; bit 1: `r2`.
const R1: Keeps = 1;
const SP: Keeps = 2;

impl Frontend for Saved {
    fn granule(&self) -> u64 {
        Toy.granule()
    }
    fn regs(&self) -> RegFile {
        Toy.regs()
    }
    fn stack_pointer(&self) -> Option<Reg> {
        Toy.stack_pointer()
    }
    fn pad_resume(&self) -> Option<&'static str> {
        Toy.pad_resume()
    }
    fn preserved(&self) -> Vec<Reg> {
        Toy.preserved()
    }
    fn callee_saved(&self) -> Vec<Reg> {
        vec![Reg(1), Reg(2)]
    }
    fn lift(&self, bytes: &[u8], pc: u64) -> Result<Block, LiftFailure> {
        Toy.lift(bytes, pc)
    }
    fn abi_headers(&self) -> &'static [&'static str] {
        Toy.abi_headers()
    }
    fn home(&self, reg: Reg) -> Home {
        Toy.home(reg)
    }
    fn intrinsic(&self, name: &str, args: &[String]) -> Option<String> {
        Toy.intrinsic(name, args)
    }
    fn trap(&self, code: u32) -> String {
        Toy.trap(code)
    }
    fn undefined(&self, pc: u64, insn: u64) -> String {
        Toy.undefined(pc, insn)
    }
    fn monitor(&self) -> &'static str {
        Toy.monitor()
    }
    fn fp(
        &self,
        op: FpOp,
        from: Width,
        to: Width,
        args: &[String],
        reg: &dyn Fn(Reg) -> String,
    ) -> Option<String> {
        Toy.fp(op, from, to, args, reg)
    }
    fn fp_state(&self) -> Vec<Reg> {
        Toy.fp_state()
    }
    fn guard_state(&self, code: u32) -> Option<Reg> {
        Toy.guard_state(code)
    }
    fn guard_slow(&self, code: u32, pc: u64, insn: u64) -> Option<String> {
        Toy.guard_slow(code, pc, insn)
    }
}

const NOP: [u8; 4] = [0, 0, 0, 0];
const RET: [u8; 4] = [3, 0, 0, 0];

fn addi(r: u8, imm: i16) -> [u8; 4] {
    word(0x09, r, imm as u16)
}

/// `ld ra, [rb]` and `st ra, [rb]`.
fn ld(a: u8, b: u8) -> [u8; 4] {
    word(0x0a, a, u16::from(b))
}

fn st(a: u8, b: u8) -> [u8; 4] {
    word(0x0b, a, u16::from(b))
}

/// Lifted toy code: each function is its words at its own entry.
struct Code(Vec<(u64, Vec<(u64, Block)>)>);

impl Code {
    fn new(fns: &[(u64, &[[u8; 4]])]) -> Code {
        Code(
            fns.iter()
                .map(|&(entry, words)| {
                    let blocks = words
                        .iter()
                        .enumerate()
                        .map(|(i, w)| {
                            let pc = entry + 4 * i as u64;
                            (pc, Saved.lift(w, pc).expect("lifts"))
                        })
                        .collect();
                    (entry, blocks)
                })
                .collect(),
        )
    }

    fn functions(&self) -> Vec<Function<'_>> {
        self.0
            .iter()
            .map(|(entry, blocks)| Function {
                entry: *entry,
                name: format!("F_{entry:x}"),
                guest_name: None,
                span: 4 * blocks.len() as u64,
                insts: blocks
                    .iter()
                    .map(|(pc, b)| (*pc, Insn::Lifted { block: b, raw: 0 }))
                    .collect(),
                tables: BTreeMap::new(),
                pads: BTreeSet::new(),
                guesses: BTreeMap::new(),
            })
            .collect()
    }
}

fn targets(fns: &[Function<'_>], overrides: &[u64]) -> BTreeMap<u64, Target> {
    let mut t: BTreeMap<u64, Target> = fns
        .iter()
        .map(|f| {
            let target = Target {
                name: f.name.clone(),
                lifted: true,
                resume_point: false,
                declare: true,
                keeps: 0,
            };
            (f.entry, target)
        })
        .collect();
    for &o in overrides {
        t.insert(
            o,
            Target {
                name: format!("H_{o:x}"),
                lifted: false,
                resume_point: false,
                declare: true,
                keeps: 0,
            },
        );
    }
    t
}

fn keeps_of(fns: &[Function<'_>], overrides: &[u64]) -> BTreeMap<u64, Keeps> {
    let t = targets(fns, overrides);
    let progs = fns
        .iter()
        .filter(|f| t[&f.entry].lifted)
        .map(|f| (f.entry, program(&Saved, f, &t, false)))
        .collect();
    solve(&Shape::of(&Saved), &progs)
}

#[test]
fn a_leaf_that_does_not_write_them_keeps_both() {
    let code = Code::new(&[(0x100, &[word(0x04, 0, 7), word(0x05, 0, 1), RET])]);
    assert_eq!(keeps_of(&code.functions(), &[])[&0x100], R1 | SP);
}

#[test]
fn a_frame_popped_by_arithmetic_keeps_sp_and_a_register_restored_from_memory_is_lost() {
    let code = Code::new(&[(
        0x100,
        &[
            addi(2, -16),
            st(1, 2),
            word(0x04, 1, 9),
            ld(1, 2),
            addi(2, 16),
            RET,
        ],
    )]);
    assert_eq!(keeps_of(&code.functions(), &[])[&0x100], SP);
}

#[test]
fn a_write_on_one_path_loses_the_register() {
    let code = Code::new(&[(0x100, &[branch(0x07, 0, 2), word(0x04, 1, 1), RET])]);
    assert_eq!(keeps_of(&code.functions(), &[])[&0x100], SP);
}

#[test]
fn calls_keep_what_the_callee_keeps() {
    let code = Code::new(&[
        // 0x100: calls 0x200, which writes r1, and 0x300, which writes r0.
        (0x100, &[branch(0x02, 0, 0x40), branch(0x02, 0, 0x7f), RET]),
        (0x200, &[word(0x04, 1, 1), RET]),
        (0x300, &[word(0x04, 0, 1), RET]),
    ]);
    let k = keeps_of(&code.functions(), &[]);
    assert_eq!(k[&0x200], SP);
    assert_eq!(k[&0x300], R1 | SP);
    assert_eq!(k[&0x100], SP);
}

#[test]
fn a_tail_call_keeps_only_what_its_target_keeps() {
    let code = Code::new(&[
        (0x100, &[branch(0x01, 0, 0x40)]),
        (0x200, &[word(0x04, 1, 1), RET]),
        (0x300, &[branch(0x01, 0, 0x40)]),
        (0x400, &[RET]),
    ]);
    let k = keeps_of(&code.functions(), &[]);
    assert_eq!(k[&0x100], SP);
    assert_eq!(k[&0x300], R1 | SP);
}

#[test]
fn indirect_calls_traps_overrides_and_unknown_targets_keep_nothing() {
    let code = Code::new(&[
        (0x100, &[word(0x0c, 0, 0), RET]),
        (0x200, &[word(0x08, 0, 1), RET]),
        (0x300, &[branch(0x02, 0, 0x40), RET]),
        (0x400, &[RET]),
        (0x500, &[branch(0x01, 0, 0x80)]),
        (0x600, &[word(0x06, 0, 0)]),
    ]);
    let fns = code.functions();
    let k = keeps_of(&fns, &[0x400]);
    assert_eq!(k[&0x100], 0);
    assert_eq!(k[&0x200], 0);
    assert_eq!(k[&0x300], 0, "a call of an override");
    assert!(!k.contains_key(&0x400));
    assert_eq!(k[&0x500], 0, "a tail call outside the module");
    assert_eq!(k[&0x600], 0, "a computed jump");
}

#[test]
fn recursion_keeps_what_every_path_keeps() {
    let code = Code::new(&[
        // 0x100: if r0 != 0 { sp -= 16; call 0x100; sp += 16 }
        (
            0x100,
            &[
                branch(0x07, 0, 5),
                addi(2, -16),
                branch(0x02, 0, -2),
                addi(2, 16),
                NOP,
                RET,
            ],
        ),
        // 0x200 and 0x300 call each other; 0x300 writes r1.
        (0x200, &[branch(0x07, 0, 2), branch(0x02, 0, 0x3f), RET]),
        (0x300, &[word(0x04, 1, 1), branch(0x02, 0, -0x41), RET]),
    ]);
    let k = keeps_of(&code.functions(), &[]);
    assert_eq!(k[&0x100], R1 | SP);
    assert_eq!(k[&0x200], SP);
    assert_eq!(k[&0x300], SP);
}

#[test]
fn a_landing_pad_resumes_with_unknown_registers() {
    let code = Code::new(&[(0x100, &[branch(0x02, 0, 0x40), RET, RET]), (0x200, &[RET])]);
    let mut fns = code.functions();
    assert_eq!(keeps_of(&fns, &[])[&0x100], R1 | SP);
    fns[0].pads.insert(0x108);
    assert_eq!(keeps_of(&fns, &[])[&0x100], 0);
}

#[test]
fn an_instruction_that_does_not_lift_never_returns() {
    let failure = LiftFailure {
        insn: 0xff,
        message: "undefined".into(),
    };
    let code = Code::new(&[(0x100, &[branch(0x07, 0, 2), RET, RET])]);
    let mut fns = code.functions();
    fns[0].insts.insert(0x104, Insn::Failed(&failure));
    assert_eq!(keeps_of(&fns, &[])[&0x100], R1 | SP);
}

#[test]
fn a_caller_keeps_the_registers_the_callee_keeps_in_its_locals() {
    let code = Code::new(&[
        // 0x100: r0 = r1 + sp; call 0x200; r0 += r1; r0 += sp; call 0x300;
        // r0 += r1; ret.
        (
            0x100,
            &[
                word(0x05, 0, 1),
                word(0x05, 0, 2),
                branch(0x02, 0, 0x3e),
                word(0x05, 0, 1),
                word(0x05, 0, 2),
                branch(0x02, 0, 0x7b),
                word(0x05, 0, 1),
                RET,
            ],
        ),
        (0x200, &[RET]),
        (0x300, &[word(0x04, 1, 1), RET]),
    ]);
    let out = emit(&Saved, &code.functions(), &[], &Options::default());
    let s = &out.files[1].text;
    let after = |pc: u64| {
        let from = s
            .find(&format!("rc = F_{pc:x}(c, mem);"))
            .unwrap_or_else(|| panic!("{s}"));
        &s[from..from + s[from..].find("\n}\n").expect("end")]
    };
    assert!(!after(0x200).contains("r1 = rl_e_ld64(&c->x[1])"), "{s}");
    assert!(!after(0x200).contains("r2 = rl_e_ld64(&c->x[2])"), "{s}");
    assert!(after(0x300).contains("r1 = rl_e_ld64(&c->x[1]);"), "{s}");
    assert!(!after(0x300).contains("r2 = rl_e_ld64(&c->x[2])"), "{s}");
}

#[test]
fn a_register_kept_across_a_call_is_loaded_before_it() {
    // r1 is first read after the call, so it must be in its local before.
    let code = Code::new(&[
        (0x100, &[branch(0x02, 0, 0x40), word(0x05, 0, 1), RET]),
        (0x200, &[RET]),
    ]);
    let out = emit(&Saved, &code.functions(), &[], &Options::default());
    let s = &out.files[1].text;
    assert!(s.contains("uint64_t r1 = rl_e_ld64(&c->x[1]);"), "{s}");
    let from = s.find("rc = F_200(c, mem);").expect("call");
    assert!(!s[from..].contains("r1 = rl_e_ld64(&c->x[1])"), "{s}");
}

#[test]
fn a_changed_callee_proof_changes_the_reuse_key_of_its_callers() {
    let code = Code::new(&[(0x100, &[RET])]);
    let fns = code.functions();
    let before = targets(&fns, &[]);
    let mut after = before.clone();
    after.get_mut(&0x100).expect("target").keeps = SP;
    let seen = Targets::new(&before);
    let _ = seen.get(&0x100);
    let e = Emitted1 {
        pieces: Vec::new(),
        callees: BTreeSet::new(),
        undefined: 0,
        flags: "RL_FN_MAY_UNWIND",
        inner: Vec::new(),
    };
    let bytes = reuse::output(&e, &seen).expect("bytes");
    assert!(reuse::reuse(&bytes, &before).is_some());
    assert!(reuse::reuse(&bytes, &after).is_none());
}

#[test]
fn a_call_another_emit_takes_reloads_what_the_callee_of_this_emit_keeps() {
    // 0x200 keeps r1 here, but `Options::calls` sends the call to a function
    // of another emit (the cold copy's call of the hot function).
    let code = Code::new(&[
        (0x100, &[branch(0x02, 0, 0x40), word(0x05, 0, 1), RET]),
        (0x200, &[RET]),
    ]);
    let opts = Options {
        calls: vec![crate::Override {
            entry: 0x200,
            host_fn: "H_200".into(),
            may_unwind: true,
            resume_point: false,
            declare: true,
        }],
        ..Options::default()
    };
    let out = emit(&Saved, &code.functions(), &[], &opts);
    let s = &out.files[1].text;
    let from = s
        .find("rc = H_200(c, mem);")
        .unwrap_or_else(|| panic!("{s}"));
    assert!(s[from..].contains("r1 = rl_e_ld64(&c->x[1])"), "{s}");
}

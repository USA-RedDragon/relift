//! What the emitter cannot express is an emit error and reports itself at
//! run time; the intrinsic carriers; the fallback without tail calls.

use std::collections::BTreeMap;

use relift_emit_wasm::{Exits, Function, Insn, Intrinsic, Options, emit, intrinsics_of};
use relift_guest::LiftFailure;
use relift_guest::toy::Toy;
use relift_ir::{Block, Builder, Exit, Flow, FpOp, Next, Reg, Width};

use crate::ops::{file, step_options};
use crate::support::{CTX, FIRST_SLOT, Rig, engine, put_reg};

const PC: u64 = 0x4000;

fn one(insn: Insn<'_>) -> Function<'_> {
    Function {
        entry: PC,
        bank: None,
        name: "f".into(),
        insts: [(PC, insn)].into_iter().collect(),
        tables: BTreeMap::new(),
    }
}

fn lifted(b: &Block, raw: u64) -> Function<'_> {
    one(Insn::Lifted { block: b, raw })
}

/// Runs `f` once; gives the return code and the undefined reports.
fn run(f: &Function<'_>, opts: &Options) -> (i32, Vec<(u64, u64)>, Vec<u8>) {
    let out = emit(std::slice::from_ref(f), opts);
    let mut rig = Rig::new(&engine(opts.tail_calls), FIRST_SLOT + 1, file(), &[]);
    let inst = rig.load(&out).expect("the module loads");
    let func = rig.func(&inst, "f");
    for (i, b) in rig.guest()[0x8000..0x8100].iter_mut().enumerate() {
        *b = i as u8 | 1;
    }
    put_reg(rig.mem(), CTX, Reg(0), 0x8047);
    let rc = rig.call(&func, CTX).expect("no trap");
    let mem = rig.guest()[0x8000..0x8100].to_vec();
    (rc, rig.store.data().undefined.clone(), mem)
}

fn with(name: &'static str, k: Intrinsic) -> Options {
    let mut o = step_options();
    o.intrinsics = [(name.to_string(), k)].into_iter().collect();
    o
}

fn intrinsic_block(name: &'static str, width: Option<Width>) -> Block {
    let mut b = Builder::new(file(), Width::W64, PC);
    let a = b.get(Reg(0));
    let v = b.intrinsic(name, &[a], width);
    if width.is_some() {
        b.set(Reg(1), v);
    }
    b.jump(4, PC + 4)
}

#[test]
fn an_unknown_intrinsic_is_an_error_and_undefined_at_run_time() {
    let b = intrinsic_block("t.unknown", Some(Width::W64));
    let f = lifted(&b, 0xd503_201f);
    let out = emit(std::slice::from_ref(&f), &step_options());
    assert_eq!(out.errors.len(), 1);
    assert_eq!(out.errors[0].pc, PC);
    assert!(out.errors[0].message.contains("t.unknown"));
    assert_eq!(out.undefined, 1);
    let (rc, undefined, _) = run(&f, &step_options());
    assert_eq!(rc, 1);
    assert_eq!(undefined, vec![(PC, 0xd503_201f)]);
}

#[test]
fn a_frontend_without_a_carrier_leaves_the_intrinsic_out() {
    let b = intrinsic_block("t.unknown", None);
    let f = lifted(&b, 0);
    assert!(intrinsics_of(&Toy, std::slice::from_ref(&f)).is_empty());
}

#[test]
fn a_failed_lift_reports_itself_without_an_emit_error() {
    let e = LiftFailure {
        insn: 0xdead_beef,
        message: "no".into(),
    };
    let f = one(Insn::Failed(&e));
    let out = emit(std::slice::from_ref(&f), &step_options());
    assert!(out.errors.is_empty());
    assert_eq!(out.undefined, 1);
    let (rc, undefined, _) = run(&f, &step_options());
    assert_eq!(rc, 1);
    assert_eq!(undefined, vec![(PC, 0xdead_beef)]);
}

#[test]
fn shapes_wasm_has_no_form_for_are_errors() {
    let mut cases: Vec<(&str, Block)> = Vec::new();
    let mut b = Builder::new(file(), Width::W64, PC);
    let a = b.get(Reg(0));
    let h = b.trunc(a, Width::W32);
    let v = b.fp(FpOp::Add, &[h, h], Width::W32);
    let z = b.zext(v, Width::W64);
    b.set(Reg(1), z);
    let mut half = b.jump(4, PC + 4);
    for x in [h, v] {
        half.insts[x.index()].width = Some(Width::W16);
    }
    cases.push(("16-bit to 16-bit floating-point", half));
    let mut b = Builder::new(file(), Width::W128, PC);
    let q = b.get(Reg(8));
    cases.push((
        "address wider than 64 bits",
        b.finish(
            4,
            Exit {
                next: Next::Dynamic(q),
                flow: Flow::Jump,
            },
        ),
    ));
    for (what, b) in &cases {
        let f = lifted(b, 7);
        let opts = Options {
            exits: Exits::Chain,
            ..step_options()
        };
        let out = emit(std::slice::from_ref(&f), &opts);
        assert_eq!(out.errors.len(), 1, "{what}");
        assert!(
            out.errors[0].message.contains(what),
            "{what}: {:?}",
            out.errors
        );
        let (rc, undefined, _) = run(&f, &opts);
        assert_eq!((rc, undefined), (1, vec![(PC, 7)]), "{what}");
    }
}

#[test]
fn carriers_that_do_not_fit_the_intrinsic_are_errors() {
    for (k, width, what) in [
        (
            Intrinsic::Nop,
            Some(Width::W64),
            "gives a value but is a no-op",
        ),
        (
            Intrinsic::Const(1),
            None,
            "gives no value but is a constant",
        ),
        (Intrinsic::ZeroBlock(12), None, "zeroes 12 bytes"),
    ] {
        let b = intrinsic_block("t.x", width);
        let out = emit(&[lifted(&b, 0)], &with("t.x", k));
        assert_eq!(out.errors.len(), 1, "{what}");
        assert!(out.errors[0].message.contains(what), "{:?}", out.errors);
    }
}

#[test]
fn zero_block_clears_the_aligned_block_only() {
    let b = intrinsic_block("t.zva", None);
    let f = lifted(&b, 0);
    let (rc, undefined, mem) = run(&f, &with("t.zva", Intrinsic::ZeroBlock(64)));
    assert_eq!((rc, undefined.len()), (0, 0));
    for (i, b) in mem.iter().enumerate() {
        let want = if (0x40..0x80).contains(&i) {
            0
        } else {
            i as u8 | 1
        };
        assert_eq!(*b, want, "byte {i:#x}");
    }
}

#[test]
fn constants_and_no_ops_need_no_import() {
    let b = intrinsic_block("t.c", Some(Width::W64));
    let out = emit(&[lifted(&b, 0)], &with("t.c", Intrinsic::Const(0x1234)));
    assert!(out.errors.is_empty());
    let module = wasmtime::Module::new(&engine(true), &out.bytes).expect("valid");
    assert!(
        module
            .imports()
            .all(|i| i.module() != relift_emit_wasm::INTRINSIC)
    );
}

#[test]
fn the_fallback_needs_no_tail_calls_and_tail_calls_need_them() {
    let mut b0 = Builder::new(file(), Width::W64, PC);
    let a = b0.get(Reg(0));
    let b0 = b0.finish(
        4,
        Exit {
            next: Next::Dynamic(a),
            flow: Flow::Jump,
        },
    );
    let b1 = Builder::new(file(), Width::W64, PC + 0x100).jump(4, PC);
    let fns = [
        lifted(&b0, 0),
        Function {
            entry: PC + 0x100,
            bank: None,
            name: "g".into(),
            insts: [(PC + 0x100, Insn::Lifted { block: &b1, raw: 0 })]
                .into_iter()
                .collect(),
            tables: BTreeMap::new(),
        },
    ];
    let opts = |tail_calls| Options {
        exits: Exits::Chain,
        tail_calls,
        ..step_options()
    };
    let plain = emit(&fns, &opts(false));
    let tail = emit(&fns, &opts(true));
    let no_tail = engine(false);
    wasmtime::Module::new(&no_tail, &plain.bytes)
        .expect("the fallback validates without tail calls");
    assert!(
        wasmtime::Module::new(&no_tail, &tail.bytes).is_err(),
        "control: the tail-call module needs the proposal"
    );
    wasmtime::Module::new(&engine(true), &tail.bytes).expect("valid with tail calls");
}

#[test]
fn a_banked_function_is_left_to_the_lookup() {
    let b = Builder::new(file(), Width::W64, PC).jump(4, PC + 4);
    let mut f = lifted(&b, 0);
    f.bank = Some(3);
    let out = emit(&[f], &step_options());
    assert_eq!(out.slots, vec![(PC, Some(3), FIRST_SLOT)]);
}

//! Interpreter semantics: every operation on boundary values, memory with
//! both byte orders, the exclusive monitor, exits, faults and verification.

use relift_ir::interp::{self, Error, FlatMemory, NoIntrinsics, State};
use relift_ir::{
    AtomicOp, BinOp, Block, Builder, CmpOp, Endian, Exit, Flow, FpOp, FpRound, Inst, Next, Op,
    Order, Reg, RegDecl, RegFile, UnOp, Value, Width,
};

static DECLS: [RegDecl; 4] = [
    RegDecl {
        name: "a",
        width: Width::W64,
    },
    RegDecl {
        name: "b",
        width: Width::W64,
    },
    RegDecl {
        name: "f",
        width: Width::W1,
    },
    RegDecl {
        name: "w",
        width: Width::W32,
    },
];

fn file() -> RegFile {
    RegFile::new("test", &DECLS)
}

fn mem() -> FlatMemory {
    FlatMemory {
        base: 0x1000,
        bytes: (0..64u8).collect(),
    }
}

fn run(block: &Block, state: &mut State, m: &mut FlatMemory) -> Result<interp::Outcome, Error> {
    interp::run(block, state, m, &mut NoIntrinsics)
}

/// Builds a block that computes `f(builder)` and stores it in slot `a`
/// (zero-extended), runs it, and returns slot `a`.
fn eval(f: impl FnOnce(&mut Builder) -> Value) -> u128 {
    let mut b = Builder::new(file(), Width::W64, 0);
    let v = f(&mut b);
    let v = b.zext(v, Width::W64);
    b.set(Reg(0), v);
    let block = b.jump(4, 4);
    let mut s = State::new(4);
    let mut m = mem();
    run(&block, &mut s, &mut m).expect("runs");
    s.regs[0]
}

fn bin(op: BinOp, w: Width, x: u128, y: u128) -> u128 {
    eval(|b| {
        let x = b.konst(w, x);
        let y = b.konst(w, y);
        b.bin(op, x, y)
    })
}

#[test]
fn integer_ops_wrap_at_their_width() {
    assert_eq!(bin(BinOp::Add, Width::W8, 0xff, 2), 1);
    assert_eq!(bin(BinOp::Sub, Width::W8, 0, 1), 0xff);
    assert_eq!(bin(BinOp::Mul, Width::W16, 0x100, 0x100), 0);
    assert_eq!(bin(BinOp::Mul, Width::W32, 0xffff_ffff, 0xffff_ffff), 1);
    assert_eq!(bin(BinOp::And, Width::W8, 0xf0, 0x3c), 0x30);
    assert_eq!(bin(BinOp::Or, Width::W8, 0xf0, 0x0c), 0xfc);
    assert_eq!(bin(BinOp::Xor, Width::W8, 0xff, 0x0f), 0xf0);
}

#[test]
fn division_by_zero_and_overflow_are_defined() {
    assert_eq!(bin(BinOp::UDiv, Width::W32, 7, 0), 0);
    assert_eq!(bin(BinOp::UDiv, Width::W32, 7, 2), 3);
    assert_eq!(
        bin(BinOp::SDiv, Width::W32, 0x8000_0000, 0xffff_ffff),
        0x8000_0000
    );
    assert_eq!(
        bin(BinOp::SDiv, Width::W32, (-7i32) as u32 as u128, 2),
        (-3i32) as u32 as u128
    );
    assert_eq!(bin(BinOp::SDiv, Width::W64, 5, 0), 0);
    assert_eq!(bin(BinOp::URem, Width::W8, 7, 0), 7);
    assert_eq!(bin(BinOp::URem, Width::W8, 7, 3), 1);
    assert_eq!(bin(BinOp::SRem, Width::W8, 0x80, 0xff), 0);
    assert_eq!(bin(BinOp::SRem, Width::W8, 0xf9, 2), 0xff);
    assert_eq!(bin(BinOp::SRem, Width::W8, 0xf9, 0), 0xf9);
}

#[test]
fn shifts_saturate_and_rotates_wrap() {
    assert_eq!(bin(BinOp::Shl, Width::W8, 1, 7), 0x80);
    assert_eq!(bin(BinOp::Shl, Width::W8, 1, 8), 0);
    assert_eq!(bin(BinOp::LShr, Width::W8, 0x80, 7), 1);
    assert_eq!(bin(BinOp::LShr, Width::W8, 0x80, 200), 0);
    assert_eq!(bin(BinOp::AShr, Width::W8, 0x80, 3), 0xf0);
    assert_eq!(bin(BinOp::AShr, Width::W8, 0x80, 200), 0xff);
    assert_eq!(bin(BinOp::AShr, Width::W8, 0x40, 200), 0);
    assert_eq!(bin(BinOp::RotR, Width::W8, 0x81, 1), 0xc0);
    assert_eq!(bin(BinOp::RotR, Width::W8, 0x81, 9), 0xc0);
    assert_eq!(bin(BinOp::RotR, Width::W32, 0x1234_5678, 0), 0x1234_5678);
    let amount_of_other_width = eval(|b| {
        let x = b.konst(Width::W64, 1);
        let k = b.konst(Width::W8, 63);
        b.bin(BinOp::Shl, x, k)
    });
    assert_eq!(amount_of_other_width, 1 << 63);
}

fn un(op: UnOp, w: Width, x: u128) -> u128 {
    eval(|b| {
        let x = b.konst(w, x);
        b.un(op, x)
    })
}

#[test]
fn bit_counting_ops() {
    assert_eq!(un(UnOp::Clz, Width::W32, 0), 32);
    assert_eq!(un(UnOp::Clz, Width::W32, 1), 31);
    assert_eq!(un(UnOp::Clz, Width::W64, 1 << 63), 0);
    assert_eq!(un(UnOp::Ctz, Width::W16, 0), 16);
    assert_eq!(un(UnOp::Ctz, Width::W16, 0x8), 3);
    assert_eq!(un(UnOp::Popcount, Width::W64, u64::MAX as u128), 64);
    assert_eq!(un(UnOp::BitReverse, Width::W8, 0x01), 0x80);
    assert_eq!(un(UnOp::BitReverse, Width::W32, 0x0000_00f1), 0x8f00_0000);
    assert_eq!(un(UnOp::ByteSwap, Width::W32, 0x1122_3344), 0x4433_2211);
    assert_eq!(un(UnOp::Not, Width::W8, 0x0f), 0xf0);
    assert_eq!(un(UnOp::Neg, Width::W8, 1), 0xff);
}

fn cmp(op: CmpOp, w: Width, x: u128, y: u128) -> u128 {
    eval(|b| {
        let x = b.konst(w, x);
        let y = b.konst(w, y);
        b.cmp(op, x, y)
    })
}

#[test]
fn comparisons_respect_signedness() {
    assert_eq!(cmp(CmpOp::Ult, Width::W8, 1, 0xff), 1);
    assert_eq!(cmp(CmpOp::Slt, Width::W8, 1, 0xff), 0);
    assert_eq!(cmp(CmpOp::Sle, Width::W8, 0x80, 0x7f), 1);
    assert_eq!(cmp(CmpOp::Ule, Width::W8, 3, 3), 1);
    assert_eq!(cmp(CmpOp::Eq, Width::W8, 3, 3), 1);
    assert_eq!(cmp(CmpOp::Ne, Width::W8, 3, 3), 0);
}

#[test]
fn extension_extract_concat_select() {
    let sx = eval(|b| {
        let x = b.konst(Width::W8, 0x80);
        b.sext(x, Width::W64)
    });
    assert_eq!(sx, 0xffff_ffff_ffff_ff80);
    let zx = eval(|b| {
        let x = b.konst(Width::W8, 0x80);
        b.zext(x, Width::W64)
    });
    assert_eq!(zx, 0x80);
    let ex = eval(|b| {
        let x = b.konst(Width::W32, 0x1234_5678);
        b.extract(x, 8, Width::W16)
    });
    assert_eq!(ex, 0x3456);
    let cat = eval(|b| {
        let h = b.konst(Width::W8, 0xab);
        let l = b.konst(Width::W16, 0xcdef);
        b.concat(h, l)
    });
    assert_eq!(cat, 0xab_cdef);
    let sel = eval(|b| {
        let c = b.konst(Width::W1, 0);
        let x = b.konst(Width::W8, 1);
        let y = b.konst(Width::W8, 2);
        b.select(c, x, y)
    });
    assert_eq!(sel, 2);
}

#[test]
fn add_with_carry_matches_a_reference_on_every_8_bit_input() {
    let mut b = Builder::new(file(), Width::W64, 0);
    let a = b.get(Reg(0));
    let x = b.trunc(a, Width::W8);
    let bb = b.get(Reg(1));
    let y = b.trunc(bb, Width::W8);
    let c = b.get(Reg(2));
    let (sum, carry, overflow) = b.add_with_carry(x, y, c);
    let s64 = b.zext(sum, Width::W64);
    b.set(Reg(0), s64);
    let c64 = b.zext(carry, Width::W64);
    b.set(Reg(1), c64);
    b.set(Reg(2), overflow);
    let block = b.jump(4, 4);
    for x in 0..256u128 {
        for y in 0..256u128 {
            for c in 0..2u128 {
                let mut s = State::new(4);
                s.regs[0] = x;
                s.regs[1] = y;
                s.regs[2] = c;
                run(&block, &mut s, &mut mem()).expect("runs");
                let full = x + y + c;
                let signed = (x as u8 as i8) as i32 + (y as u8 as i8) as i32 + c as i32;
                assert_eq!(s.regs[0], full & 0xff);
                assert_eq!(s.regs[1], full >> 8);
                assert_eq!(s.regs[2], u128::from(!(-128..=127).contains(&signed)));
            }
        }
    }
}

#[test]
fn memory_honors_byte_order() {
    let mut b = Builder::new(file(), Width::W64, 0);
    let addr = b.konst(Width::W64, 0x1000);
    let le = b.load(addr, Width::W32, Endian::Little, Order::Plain);
    let be = b.load(addr, Width::W32, Endian::Big, Order::Acquire);
    let le64 = b.zext(le, Width::W64);
    let be64 = b.zext(be, Width::W64);
    b.set(Reg(0), le64);
    b.set(Reg(1), be64);
    let at = b.konst(Width::W64, 0x1010);
    let v = b.konst(Width::W16, 0xaabb);
    b.store(at, v, Endian::Big, Order::Release);
    let block = b.jump(4, 4);
    let mut s = State::new(4);
    let mut m = mem();
    run(&block, &mut s, &mut m).expect("runs");
    assert_eq!(s.regs[0], 0x0302_0100);
    assert_eq!(s.regs[1], 0x0001_0203);
    assert_eq!(&m.bytes[0x10..0x12], &[0xaa, 0xbb]);
}

#[test]
fn atomics_update_memory_and_give_the_old_value() {
    let cases = [
        (
            AtomicOp::Add,
            0x8000_0001u128,
            0x0302_0100 + 0x8000_0001u128,
        ),
        (AtomicOp::Clear, 0x0000_0101, 0x0302_0000),
        (AtomicOp::Xor, 0xffff_ffff, !0x0302_0100u128 & 0xffff_ffff),
        (AtomicOp::Or, 0x8000_0000, 0x8302_0100),
        (AtomicOp::SMax, 0x8000_0000, 0x0302_0100),
        (AtomicOp::SMin, 0x8000_0000, 0x8000_0000),
        (AtomicOp::UMax, 0x8000_0000, 0x8000_0000),
        (AtomicOp::UMin, 0x8000_0000, 0x0302_0100),
        (AtomicOp::Swap, 0x1234_5678, 0x1234_5678),
    ];
    for (op, operand, want) in cases {
        let mut b = Builder::new(file(), Width::W64, 0);
        let addr = b.konst(Width::W64, 0x1000);
        let v = b.konst(Width::W32, operand);
        let old = b.atomic_rmw(op, addr, v, Endian::Little, Order::AcqRel);
        let old = b.zext(old, Width::W64);
        b.set(Reg(0), old);
        let block = b.jump(4, 4);
        block.verify().expect("verifies");
        let mut s = State::new(4);
        let mut m = mem();
        run(&block, &mut s, &mut m).expect("runs");
        assert_eq!(s.regs[0], 0x0302_0100, "{op:?}");
        let got = u32::from_le_bytes(m.bytes[..4].try_into().expect("4 bytes"));
        assert_eq!(u128::from(got), want, "{op:?}");
    }
    for (expected, stored) in [(0x0100u128, true), (0x0101, false)] {
        let mut b = Builder::new(file(), Width::W64, 0);
        let addr = b.konst(Width::W64, 0x1000);
        let e = b.konst(Width::W16, expected);
        let v = b.konst(Width::W16, 0xbeef);
        let old = b.compare_swap(addr, e, v, Endian::Little, Order::Plain);
        let old = b.zext(old, Width::W64);
        b.set(Reg(0), old);
        let block = b.jump(4, 4);
        let mut s = State::new(4);
        let mut m = mem();
        run(&block, &mut s, &mut m).expect("runs");
        assert_eq!(s.regs[0], 0x0100);
        let want: [u8; 2] = if stored { [0xef, 0xbe] } else { [0, 1] };
        assert_eq!(m.bytes[..2], want);
    }
}

#[test]
fn a_fault_discards_register_writes() {
    let mut b = Builder::new(file(), Width::W64, 0);
    let one = b.konst(Width::W64, 1);
    b.set(Reg(0), one);
    let addr = b.konst(Width::W64, 0x2000);
    let x = b.load(addr, Width::W64, Endian::Little, Order::Plain);
    b.set(Reg(1), x);
    let block = b.jump(4, 4);
    let mut s = State::new(4);
    let e = run(&block, &mut s, &mut mem()).expect_err("faults");
    assert!(matches!(e, Error::Fault(f) if f.addr == 0x2000 && !f.write));
    assert_eq!(s.regs, vec![0; 4]);
}

#[test]
fn gets_read_entry_values_after_a_set() {
    let mut b = Builder::new(file(), Width::W64, 0);
    let a = b.get(Reg(0));
    let one = b.konst(Width::W64, 1);
    let a1 = b.add(a, one);
    b.set(Reg(0), a1);
    let again = b.get(Reg(0));
    b.set(Reg(1), again);
    let block = b.jump(4, 4);
    let mut s = State::new(4);
    s.regs[0] = 41;
    run(&block, &mut s, &mut mem()).expect("runs");
    assert_eq!(s.regs[0], 42);
    assert_eq!(s.regs[1], 41);
}

fn excl_block(load: bool, store_at: u64, clear_first: bool) -> Block {
    let mut b = Builder::new(file(), Width::W64, 0);
    if clear_first {
        b.clear_exclusive();
    }
    if load {
        let a = b.konst(Width::W64, 0x1008);
        let x = b.load_exclusive(a, Width::W64, Endian::Little, Order::Acquire);
        b.set(Reg(0), x);
    }
    let a = b.konst(Width::W64, store_at.into());
    let v = b.konst(Width::W64, 0x1111);
    let ok = b.store_exclusive(a, v, Endian::Little, Order::Release);
    b.set(Reg(2), ok);
    b.jump(4, 4)
}

#[test]
fn the_exclusive_monitor_pairs_loads_and_stores() {
    let mut m = mem();
    let mut s = State::new(4);
    run(&excl_block(true, 0x1008, true), &mut s, &mut m).expect("runs");
    assert_eq!(s.regs[2], 1);
    assert_eq!(s.monitor, None);
    assert_eq!(&m.bytes[8..10], &[0x11, 0x11]);

    let mut m = mem();
    run(&excl_block(true, 0x1010, true), &mut s, &mut m).expect("runs");
    assert_eq!(s.regs[2], 0);
    assert_eq!(m.bytes[0x10], 0x10);

    let mut m = mem();
    run(&excl_block(false, 0x1008, true), &mut s, &mut m).expect("runs");
    assert_eq!(s.regs[2], 0);

    let mut m = mem();
    let mut b = Builder::new(file(), Width::W64, 0);
    let a = b.konst(Width::W64, 0x1008);
    b.load_exclusive(a, Width::W64, Endian::Little, Order::Plain);
    b.clear_exclusive();
    let block = b.jump(4, 4);
    run(&block, &mut s, &mut m).expect("runs");
    assert_eq!(s.monitor, None);
}

#[test]
fn exits_choose_the_next_pc() {
    let mut b = Builder::new(file(), Width::W64, 0x100);
    let f = b.get(Reg(2));
    let block = b.finish(
        4,
        Exit {
            next: Next::Select {
                cond: f,
                taken: 0x200,
                not_taken: 0x104,
            },
            flow: Flow::Jump,
        },
    );
    let mut s = State::new(4);
    assert_eq!(run(&block, &mut s, &mut mem()).expect("runs").next, 0x104);
    s.regs[2] = 1;
    assert_eq!(run(&block, &mut s, &mut mem()).expect("runs").next, 0x200);

    let mut b = Builder::new(file(), Width::W64, 0x100);
    let t = b.get(Reg(1));
    let block = b.finish(
        4,
        Exit {
            next: Next::Dynamic(t),
            flow: Flow::Return,
        },
    );
    s.regs[1] = 0xdead_beef;
    let out = run(&block, &mut s, &mut mem()).expect("runs");
    assert_eq!((out.next, out.flow), (0xdead_beef, Flow::Return));
}

#[test]
fn a_32_bit_guest_wraps_addresses() {
    let mut b = Builder::new(file(), Width::W32, 0);
    let w = b.get(Reg(3));
    let k = b.konst(Width::W32, 0x10);
    let addr = b.add(w, k);
    let x = b.load(addr, Width::W8, Endian::Big, Order::Plain);
    let x64 = b.zext(x, Width::W64);
    b.set(Reg(0), x64);
    let block = b.jump(4, 4);
    let mut s = State::new(4);
    s.regs[3] = 0xffff_fff0;
    let mut m = FlatMemory {
        base: 0,
        bytes: vec![7; 16],
    };
    run(&block, &mut s, &mut m).expect("runs");
    assert_eq!(s.regs[0], 7);
}

#[test]
fn floating_point_intrinsics_use_ieee_arithmetic() {
    let r = eval(|b| {
        let x = b.konst(Width::W64, 1.5f64.to_bits().into());
        let y = b.konst(Width::W64, 2.25f64.to_bits().into());
        b.fp(FpOp::Mul, &[x, y], Width::W64)
    });
    assert_eq!(f64::from_bits(r as u64), 3.375);
    let r = eval(|b| {
        let x = b.konst(Width::W32, 2.0f32.to_bits().into());
        b.fp(FpOp::Sqrt, &[x], Width::W32)
    });
    assert_eq!(f32::from_bits(r as u32), std::f32::consts::SQRT_2);
    let r = eval(|b| {
        let x = b.konst(Width::W32, 2.0f32.to_bits().into());
        b.fp(FpOp::Neg, &[x], Width::W32)
    });
    assert_eq!(f32::from_bits(r as u32), -2.0);
}

#[test]
fn floating_point_conversions_round_saturate_and_change_width() {
    let fp = |op: FpOp, v: u128, from: Width, to: Width| {
        eval(|b| {
            let x = b.konst(from, v);
            b.fp(op, &[x], to)
        })
    };
    let d = |x: f64| u128::from(x.to_bits());
    let (w32, w64) = (Width::W32, Width::W64);
    assert_eq!(
        fp(FpOp::ToSInt(FpRound::Zero), d(-2.7), w64, w32),
        0xffff_fffe
    );
    assert_eq!(
        fp(FpOp::ToSInt(FpRound::Floor), d(-2.5), w64, w64),
        (-3i64) as u64 as u128
    );
    assert_eq!(fp(FpOp::ToSInt(FpRound::TiesEven), d(2.5), w64, w64), 2);
    assert_eq!(fp(FpOp::ToSInt(FpRound::TiesAway), d(2.5), w64, w64), 3);
    assert_eq!(
        fp(FpOp::ToSInt(FpRound::Zero), d(1e300), w64, w32),
        0x7fff_ffff
    );
    assert_eq!(fp(FpOp::ToUInt(FpRound::Ceil), d(-0.5), w64, w64), 0);
    assert_eq!(fp(FpOp::ToSInt(FpRound::Zero), d(f64::NAN), w64, w64), 0);
    assert_eq!(fp(FpOp::FromSInt, 0xffff_ffff, w32, w64), d(-1.0));
    assert_eq!(
        fp(FpOp::FromUInt, 0xffff_ffff, w32, w64),
        d(4_294_967_295.0)
    );
    assert_eq!(
        fp(FpOp::Convert, d(1.0 + f64::EPSILON), w64, w32),
        u128::from(1.0f32.to_bits())
    );
    assert_eq!(
        fp(FpOp::RoundInt(FpRound::Ceil), d(-0.5), w64, w64),
        d(-0.0)
    );
}

#[test]
fn an_unknown_intrinsic_is_an_error() {
    let mut b = Builder::new(file(), Width::W64, 0);
    b.intrinsic("svc", &[], None);
    let block = b.jump(4, 4);
    let e = run(&block, &mut State::new(4), &mut mem()).expect_err("unknown");
    assert_eq!(e, Error::UnknownIntrinsic("svc"));
}

#[test]
fn verify_rejects_ill_typed_blocks() {
    let base = |insts: Vec<Inst>| Block {
        regs: file(),
        addr_width: Width::W64,
        pc: 0,
        len: 4,
        insts,
        exit: Exit {
            next: Next::Const(4),
            flow: Flow::Jump,
        },
        bank: None,
    };
    let c8 = Inst {
        op: Op::konst(1),
        width: Some(Width::W8),
    };
    let c16 = Inst {
        op: Op::konst(1),
        width: Some(Width::W16),
    };
    let add = Inst {
        op: Op::Bin(BinOp::Add, Value(0), Value(1)),
        width: Some(Width::W8),
    };
    assert!(
        base(vec![c8.clone(), c8.clone(), add.clone()])
            .verify()
            .is_ok()
    );
    assert!(base(vec![c8.clone(), c16.clone(), add]).verify().is_err());
    let forward = Inst {
        op: Op::Un(UnOp::Not, Value(1)),
        width: Some(Width::W8),
    };
    assert!(base(vec![forward, c8.clone()]).verify().is_err());
    let too_big = Inst {
        op: Op::konst(0x100),
        width: Some(Width::W8),
    };
    assert!(base(vec![too_big]).verify().is_err());
    let set_wrong = Inst {
        op: Op::Set(Reg(0), Value(0)),
        width: None,
    };
    assert!(base(vec![c8.clone(), set_wrong]).verify().is_err());
    let bad_load = Inst {
        op: Op::Load {
            addr: Value(0),
            endian: Endian::Little,
            order: Order::Plain,
        },
        width: Some(Width::W8),
    };
    assert!(base(vec![c8.clone(), bad_load]).verify().is_err());
    let mut bad_exit = base(vec![c8]);
    bad_exit.exit.next = Next::Dynamic(Value(0));
    assert!(bad_exit.verify().is_err());
}

#[test]
#[should_panic(expected = "ill-typed")]
fn the_builder_panics_on_a_width_mismatch() {
    let mut b = Builder::new(file(), Width::W64, 0);
    let x = b.konst(Width::W8, 1);
    let y = b.konst(Width::W16, 1);
    b.add(x, y);
}

#[test]
fn a_guard_leaves_for_the_slow_path_before_any_write() {
    let mut b = Builder::new(file(), Width::W64, 0x40);
    let x = b.get(Reg(0));
    let zero = b.konst(Width::W64, 0);
    let ne = b.cmp(CmpOp::Ne, x, zero);
    b.guard(ne, 0x51);
    let one = b.konst(Width::W64, 1);
    b.set(Reg(1), one);
    let block = b.jump(4, 0x44);
    let mut s = State::new(4);
    s.regs[0] = 7;
    let o = run(&block, &mut s, &mut mem()).expect("runs");
    assert_eq!((o.next, o.flow), (0x40, Flow::Trap(0x51)));
    assert_eq!(s.regs[1], 0);
    s.regs[0] = 0;
    let o = run(&block, &mut s, &mut mem()).expect("runs");
    assert_eq!(o.next, 0x44);
    assert_eq!(s.regs[1], 1);
}

#[test]
#[should_panic(expected = "a guard follows an operation with an effect")]
fn a_guard_after_an_effect_does_not_verify() {
    let mut b = Builder::new(file(), Width::W64, 0);
    let x = b.get(Reg(0));
    b.set(Reg(1), x);
    let c = b.konst(Width::W1, 1);
    b.guard(c, 1);
    let _ = b.jump(4, 4);
}

#[test]
fn binary16_to_integer_saturates_at_every_width() {
    let to = |op: FpOp, h: u128, bits: u32| interp::eval_fp(op, &[h], Width::W16, Width::new(bits));
    let s = FpOp::ToSInt(FpRound::Zero);
    let u = FpOp::ToUInt(FpRound::Zero);
    for bits in [16u32, 32, 64] {
        let m = Width::new(bits).mask();
        assert_eq!(to(s, 0x7c00, bits), m >> 1, "+inf to s{bits}");
        assert_eq!(to(s, 0xfc00, bits), (m >> 1) + 1, "-inf to s{bits}");
        assert_eq!(to(u, 0x7c00, bits), m, "+inf to u{bits}");
        assert_eq!(to(u, 0xfc00, bits), 0, "-inf to u{bits}");
        assert_eq!(to(s, 0x7e00, bits), 0, "NaN to s{bits}");
        assert_eq!(to(s, 0xbc00, bits), m, "-1 to s{bits}");
    }
    assert_eq!(to(s, 0x7bff, 16), 0x7fff);
    assert_eq!(to(u, 0x7bff, 16), 0xffe0);
    assert_eq!(to(s, 0x7bff, 64), 65504);
}

//! Text format round trips, parse errors and dead-value elimination.

use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
use relift_ir::{
    BinOp, Block, Builder, CmpOp, Endian, Exit, Flow, FpOp, FpRound, IoKind, Next, Order, Reg,
    RegDecl, RegFile, UnOp, Value, Width, dce, parse,
};

static DECLS: [RegDecl; 4] = [
    RegDecl {
        name: "x0",
        width: Width::W64,
    },
    RegDecl {
        name: "x1",
        width: Width::W64,
    },
    RegDecl {
        name: "z",
        width: Width::W1,
    },
    RegDecl {
        name: "w2",
        width: Width::W32,
    },
];

fn file() -> RegFile {
    RegFile::new("t", &DECLS)
}

fn files() -> [RegFile; 1] {
    [file()]
}

fn roundtrip(b: &Block) {
    let text = b.to_string();
    let back = parse(&text, &files()).unwrap_or_else(|e| panic!("{e}\n{text}"));
    assert_eq!(&back, b, "{text}");
    assert_eq!(back.to_string(), text);
}

fn everything(exit: Exit, addr: Width) -> Block {
    let mut b = Builder::new(file(), addr, 0x1000);
    let x0 = b.get(Reg(0));
    let x1 = b.get(Reg(1));
    let w2 = b.get(Reg(3));
    let ad = if addr == Width::W64 { x1 } else { w2 };
    let k = b.konst(Width::W64, 0x10);
    let g = b.cmp(CmpOp::Eq, x0, k);
    b.guard(g, 0x4650_4352);
    let wide = b.konst(Width::new(65), 1 << 64);
    let _ = wide;
    for op in [
        BinOp::Add,
        BinOp::Sub,
        BinOp::Mul,
        BinOp::UDiv,
        BinOp::SDiv,
        BinOp::URem,
        BinOp::SRem,
        BinOp::And,
        BinOp::Or,
        BinOp::Xor,
        BinOp::Shl,
        BinOp::LShr,
        BinOp::AShr,
        BinOp::RotR,
    ] {
        b.bin(op, x0, k);
    }
    for op in [
        UnOp::Not,
        UnOp::Neg,
        UnOp::Clz,
        UnOp::Ctz,
        UnOp::Popcount,
        UnOp::BitReverse,
        UnOp::ByteSwap,
    ] {
        b.un(op, x0);
    }
    let mut last = x0;
    for op in [
        CmpOp::Eq,
        CmpOp::Ne,
        CmpOp::Ult,
        CmpOp::Ule,
        CmpOp::Slt,
        CmpOp::Sle,
    ] {
        last = b.cmp(op, x0, x1);
    }
    let cond = b.cmp(CmpOp::Eq, x0, x1);
    let _ = last;
    b.select(cond, x0, x1);
    let z = b.zext(w2, Width::W64);
    let s = b.sext(w2, Width::W64);
    b.extract(x0, 4, Width::new(12));
    b.concat(x0, x1);
    b.concat(z, s);
    for (e, o) in [
        (Endian::Little, Order::Plain),
        (Endian::Big, Order::Acquire),
        (Endian::Little, Order::Release),
        (Endian::Big, Order::AcqRel),
        (Endian::Little, Order::SeqCst),
    ] {
        let l = b.load(ad, Width::W32, e, o);
        b.store(ad, l, e, o);
        let le = b.load_exclusive(ad, Width::W64, e, o);
        b.store_exclusive(ad, le, e, o);
        b.fence(o);
    }
    b.clear_exclusive();
    let f = b.trunc(x0, Width::W32);
    for op in [FpOp::Add, FpOp::Sub, FpOp::Mul, FpOp::Div] {
        b.fp(op, &[f, w2], Width::W32);
    }
    for op in [FpOp::Sqrt, FpOp::Neg, FpOp::Abs] {
        b.fp(op, &[f], Width::W32);
    }
    b.fp(FpOp::MulAdd, &[f, w2, f], Width::W32);
    for r in [
        FpRound::TiesEven,
        FpRound::TiesAway,
        FpRound::Floor,
        FpRound::Ceil,
        FpRound::Zero,
    ] {
        b.fp(FpOp::RoundInt(r), &[f], Width::W32);
        b.fp(FpOp::ToSInt(r), &[f], Width::W64);
        b.fp(FpOp::ToUInt(r), &[x0], Width::W32);
    }
    b.fp(FpOp::FromSInt, &[x0], Width::W32);
    b.fp(FpOp::FromUInt, &[f], Width::W64);
    b.fp(FpOp::Convert, &[f], Width::W64);
    b.intrinsic("sys.\"q\"\\x", &[x0, x1], Some(Width::W64));
    b.intrinsic("barrier", &[], None);
    b.intrinsic("one", &[x0], None);
    b.tick(4);
    b.tick_if(cond, 12);
    b.tick(u32::MAX);
    for (e, k) in [(Endian::Little, IoKind::Any), (Endian::Big, IoKind::Device)] {
        let l = b.io_load(ad, Width::W16, e, k);
        b.io_store(ad, l, e, k);
    }
    b.set(Reg(0), x1);
    b.set(Reg(2), cond);
    b.finish(4, exit)
}

#[test]
fn every_op_and_exit_round_trips() {
    for addr in [Width::W64, Width::W32] {
        let probe = everything(
            Exit {
                next: Next::Const(0),
                flow: Flow::Jump,
            },
            addr,
        );
        let dynamic = Value(
            probe
                .insts
                .iter()
                .position(|i| i.width == Some(addr))
                .unwrap() as u32,
        );
        let flows = [
            Flow::Jump,
            Flow::Call { ret: 0x1008 },
            Flow::Return,
            Flow::Trap(0xdead),
        ];
        for flow in flows {
            let cond = Value(
                probe
                    .insts
                    .iter()
                    .position(|i| i.width == Some(Width::W1))
                    .unwrap() as u32,
            );
            for next in [
                Next::Const(0x1004),
                Next::Select {
                    cond,
                    taken: 0x1040,
                    not_taken: 0x1004,
                },
                Next::Dynamic(dynamic),
            ] {
                roundtrip(&everything(Exit { next, flow }, addr));
            }
        }
    }
}

#[test]
fn printed_form_matches_the_documented_style() {
    let mut b = Builder::new(file(), Width::W64, 0x1000);
    let a = b.get(Reg(1));
    let k = b.konst(Width::W64, 0x10);
    let s = b.add(a, k);
    let l = b.load(s, Width::W64, Endian::Little, Order::Plain);
    b.store(s, l, Endian::Little, Order::Release);
    b.set(Reg(0), l);
    let block = b.finish(
        4,
        Exit {
            next: Next::Const(0x1004),
            flow: Flow::Jump,
        },
    );
    assert_eq!(
        block.to_string(),
        "block 0x1000 len 4 regs t\n  v0:i64 = get x1\n  v1:i64 = const 0x10\n  v2:i64 = add v0, v1\n  v3:i64 = load.le.plain v2\n  store.le.release v2, v3\n  set x0, v3\n  exit jump 0x1004\n"
    );
}

fn err(text: &str) -> (usize, usize, String) {
    let e = parse(text, &files()).expect_err("must not parse");
    assert_eq!(
        e.to_string(),
        format!("{}:{}: {}", e.line, e.col, e.message)
    );
    (e.line, e.col, e.message)
}

#[test]
fn parse_errors_carry_positions() {
    let head = "block 0x1000 len 4 regs t\n";
    let tail = "  exit jump 0x1004\n";

    let (l, c, m) = err(&format!("{head}  v0:i64 = frob x0\n{tail}"));
    assert_eq!((l, c), (2, 12));
    assert!(m.contains("unknown mnemonic"), "{m}");

    let (l, c, m) = err(&format!(
        "{head}  v0:i64 = get x0\n  v1:i64 = add v0, v2\n{tail}"
    ));
    assert_eq!((l, c), (3, 20));
    assert!(m.contains("not defined"), "{m}");

    let (l, c, m) = err(&format!("{head}  v0:i64 = get q9\n{tail}"));
    assert_eq!((l, c), (2, 16));
    assert!(m.contains("unknown register"), "{m}");

    let (l, c, m) = err(&format!(
        "{head}  v0:i64 = get x0\n  v1:i32 = add v0, v0\n{tail}"
    ));
    assert_eq!((l, c), (3, 12));
    assert!(m.contains("width mismatch"), "{m}");

    let (l, c, m) = err(&format!("{head}  v0:i64 = const 0xzz\n{tail}"));
    assert_eq!((l, c), (2, 18));
    assert!(m.contains("hex"), "{m}");

    let (l, c, _) = err(&format!("{head}  v1:i64 = const 0x1\n{tail}"));
    assert_eq!((l, c), (2, 3));

    let (l, c, m) = err("block 0x1000 len 4 regs nope\n");
    assert_eq!((l, c), (1, 25));
    assert!(m.contains("register file"), "{m}");

    let (l, _, m) = err(head);
    assert_eq!(l, 2);
    assert!(m.contains("exit"), "{m}");

    let (l, c, _) = err(&format!(
        "{head}  v0:i64 = get x0\n  exit jump select v0, 0x1, 0x2\n"
    ));
    assert_eq!((l, c), (3, 3));

    let (l, c, m) = err(&format!("{head}  tick 0x4\n{tail}"));
    assert_eq!((l, c), (2, 8));
    assert!(m.contains("cycle count"), "{m}");

    let (l, c, m) = err(&format!(
        "{head}  v0:i64 = get x1\n  v1:i8 = io.load.le.maybe v0\n{tail}"
    ));
    assert_eq!((l, c), (3, 22));
    assert!(m.contains("I/O kind"), "{m}");

    let (l, c, m) = err(&format!("{head}  v0:i64 = get x0\n  tick.if v0, 4\n{tail}"));
    assert_eq!((l, c), (3, 3));
    assert!(m.contains("width mismatch"), "{m}");
}

fn mem() -> FlatMemory {
    FlatMemory {
        base: 0x1000,
        bytes: (0..128u8).collect(),
    }
}

fn run(b: &Block, regs: [u128; 4]) -> (Result<interp::Outcome, interp::Error>, State, FlatMemory) {
    let mut s = State::new(4);
    s.regs = regs.to_vec();
    let mut m = mem();
    let r = interp::run(b, &mut s, &mut m, &mut NoIntrinsics);
    (r, s, m)
}

fn same(a: &Block, b: &Block) {
    for regs in [[0x1010, 0x1020, 0, 7], [5, 0x9000, 1, 3]] {
        let (ra, sa, ma) = run(a, regs);
        let (rb, sb, mb) = run(b, regs);
        assert_eq!(ra, rb);
        assert_eq!(sa, sb);
        assert_eq!(ma.bytes, mb.bytes);
    }
}

fn sample() -> Block {
    let mut b = Builder::new(file(), Width::W64, 0x1000);
    let x0 = b.get(Reg(0));
    let x1 = b.get(Reg(1));
    let dead1 = b.add(x0, x1);
    let dead2 = b.un(UnOp::Not, dead1);
    let _ = dead2;
    let k = b.konst(Width::W64, 8);
    let a = b.add(x0, k);
    let _unused_load = b.load(a, Width::W64, Endian::Little, Order::Plain);
    let used = b.sub(x1, k);
    b.store(a, used, Endian::Little, Order::Plain);
    let c = b.cmp(CmpOp::Ult, x0, x1);
    let _dead3 = b.select(c, x0, x1);
    b.set(Reg(1), used);
    b.finish(
        4,
        Exit {
            next: Next::Select {
                cond: c,
                taken: 0x2000,
                not_taken: 0x1004,
            },
            flow: Flow::Jump,
        },
    )
}

#[test]
fn dce_removes_dead_pure_ops_and_keeps_effects() {
    let b = sample();
    let d = dce(&b);
    d.verify().unwrap();
    assert_eq!(d.insts.len(), b.insts.len() - 3);
    assert_eq!(dce(&d), d);
    roundtrip(&d);
    same(&b, &d);
    let t = d.to_string();
    assert!(t.contains("load.le.plain"), "{t}");
    assert!(t.contains("store.le.plain"), "{t}");
    assert!(t.contains("set x1"), "{t}");
}

#[test]
fn dce_keeps_exclusives_fences_intrinsics_and_dynamic_targets() {
    let mut b = Builder::new(file(), Width::W64, 0x1000);
    let x0 = b.get(Reg(0));
    let x1 = b.get(Reg(1));
    let junk = b.bin(BinOp::Mul, x0, x1);
    let _ = junk;
    let le = b.load_exclusive(x1, Width::W64, Endian::Little, Order::Acquire);
    let _ok = b.store_exclusive(x1, le, Endian::Little, Order::Release);
    b.fence(Order::SeqCst);
    b.clear_exclusive();
    b.intrinsic("n", &[x0], None);
    let t = b.add(x0, x1);
    let block = b.finish(
        4,
        Exit {
            next: Next::Dynamic(t),
            flow: Flow::Return,
        },
    );
    let d = dce(&block);
    d.verify().unwrap();
    assert_eq!(d.insts.len(), block.insts.len() - 1);
    roundtrip(&d);
    same(&block, &d);
}

#[test]
fn dce_of_a_pure_block_leaves_only_what_the_exit_needs() {
    let mut b = Builder::new(file(), Width::W64, 0);
    let x0 = b.get(Reg(0));
    let x1 = b.get(Reg(1));
    b.add(x0, x1);
    let block = b.jump(4, 4);
    let d = dce(&block);
    assert!(d.insts.is_empty());
    same(&block, &d);
}

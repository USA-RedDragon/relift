//! Random blocks over every IR operation, run one step at a time.

use std::collections::BTreeMap;

use relift_emit_wasm::{Exits, Function, Insn, Intrinsic, Options, ctx, emit};
use relift_ir::interp::{self, Error, FlatMemory, Intrinsics, Monitor, State};
use std::collections::BTreeSet;

use relift_ir::{
    AtomicOp, BinOp, Block, Builder, CmpOp, CryptoOp, Endian, Exit, Flow, FpOp, FpRound, Next, Op,
    Order, Reg, RegDecl, RegFile, UnOp, Value, Width,
};

use crate::mutate::{mutate, mutate_newer};
use crate::support::{
    BIAS, Bytes, CTX, FIRST_SLOT, Rig, Rng, WINDOW, ctx_u64, diff_bytes, engine, get_reg,
    hash_intrinsic, put_reg, scale, seed,
};

const fn w(bits: u32) -> Width {
    Width::new(bits)
}

static DECLS: [RegDecl; 16] = [
    RegDecl {
        name: "x0",
        width: w(64),
    },
    RegDecl {
        name: "x1",
        width: w(64),
    },
    RegDecl {
        name: "x2",
        width: w(64),
    },
    RegDecl {
        name: "w3",
        width: w(32),
    },
    RegDecl {
        name: "w4",
        width: w(32),
    },
    RegDecl {
        name: "b5",
        width: w(8),
    },
    RegDecl {
        name: "h6",
        width: w(16),
    },
    RegDecl {
        name: "f7",
        width: w(1),
    },
    RegDecl {
        name: "q8",
        width: w(128),
    },
    RegDecl {
        name: "q9",
        width: w(128),
    },
    RegDecl {
        name: "o10",
        width: w(100),
    },
    RegDecl {
        name: "o11",
        width: w(13),
    },
    RegDecl {
        name: "o12",
        width: w(65),
    },
    RegDecl {
        name: "o13",
        width: w(47),
    },
    RegDecl {
        name: "o14",
        width: w(24),
    },
    RegDecl {
        name: "f15",
        width: w(1),
    },
];

pub fn file() -> RegFile {
    RegFile::new("ops", &DECLS)
}

const WIDTHS: [u32; 15] = [1, 2, 8, 13, 16, 24, 32, 47, 63, 64, 65, 96, 100, 127, 128];
const BYTE_WIDTHS: [u32; 9] = [8, 16, 24, 32, 56, 64, 72, 96, 128];
const DATA: u64 = 0x8000;
const PC: u64 = 0x4000;
const GUARD: u32 = 7;

const INTRINSICS: [&str; 4] = ["t.hash1", "t.hash2", "t.wide", "t.effect"];

pub fn intrinsics() -> BTreeMap<String, Intrinsic> {
    let mut m: BTreeMap<String, Intrinsic> = INTRINSICS
        .iter()
        .map(|n| ((*n).to_string(), Intrinsic::Import))
        .collect();
    m.insert(
        "t.const".into(),
        Intrinsic::Const(0x1234_5678_9abc_def0_0fed_cba9_8765_4321),
    );
    m.insert("t.nop".into(), Intrinsic::Nop);
    m
}

struct TestIntrinsics;

impl Intrinsics for TestIntrinsics {
    fn call(&mut self, name: &'static str, args: &[u128]) -> Result<u128, Error> {
        match name {
            "t.hash1" | "t.hash2" | "t.wide" => Ok(hash_intrinsic(args)),
            "t.const" => Ok(0x1234_5678_9abc_def0_0fed_cba9_8765_4321),
            "t.nop" | "t.effect" => Ok(0),
            _ => Err(Error::UnknownIntrinsic(name)),
        }
    }
}

struct Gen<'a> {
    b: Builder,
    r: &'a mut Rng,
    pool: Vec<Value>,
}

impl Gen<'_> {
    fn width(&self, v: Value) -> Width {
        self.b.width(v)
    }

    fn fit(&mut self, v: Value, to: Width) -> Value {
        let have = self.width(v);
        if have == to {
            v
        } else if have.bits() > to.bits() {
            let lo = self.r.below(u64::from(have.bits() - to.bits()) + 1) as u32;
            self.b.extract(v, lo, to)
        } else if self.r.chance(0.5) {
            self.b.zext(v, to)
        } else {
            self.b.sext(v, to)
        }
    }

    fn of(&mut self, to: Width) -> Value {
        if self.pool.is_empty() || self.r.chance(0.15) {
            let v = self.r.value(to);
            return self.b.konst(to, v);
        }
        let v = self.r.pick(&self.pool);
        self.fit(v, to)
    }

    fn keep(&mut self, v: Value) -> Value {
        self.pool.push(v);
        v
    }

    fn any_width(&mut self) -> Width {
        w(self.r.pick(&WIDTHS))
    }

    fn addr(&mut self) -> Value {
        let v = self.of(w(64));
        let m = self.b.konst(w(64), 0x3fef);
        let off = self.b.and(v, m);
        let base = self.b.konst(w(64), u128::from(DATA));
        self.b.add(off, base)
    }

    fn float(&mut self, fw: Width) -> Value {
        if self.r.chance(0.5) {
            return self.of(fw);
        }
        let specials32: [u128; 10] = [
            0,
            0x8000_0000,
            0x3f80_0000,
            0xbf80_0000,
            0x7f80_0000,
            0x7fc0_0000,
            0x3f00_0000,
            0x4b80_0000,
            0x0000_0001,
            0x4f00_0000,
        ];
        let specials64: [u128; 10] = [
            0,
            0x8000_0000_0000_0000,
            0x3ff0_0000_0000_0000,
            0xbff0_0000_0000_0000,
            0x7ff0_0000_0000_0000,
            0x7ff8_0000_0000_0000,
            0x3fe0_0000_0000_0000,
            0x4330_0000_0000_0000,
            0x0000_0000_0000_0001,
            0x43e0_0000_0000_0000,
        ];
        let v = if fw.bits() == 32 {
            self.r.pick(&specials32)
        } else {
            self.r.pick(&specials64)
        };
        let v = if self.r.chance(0.3) {
            v ^ (self.r.below(8) as u128)
        } else {
            v
        };
        self.b.konst(fw, v & fw.mask())
    }

    /// `v` with any NaN replaced by one quiet NaN, since a NaN result may
    /// be any NaN.
    fn canon(&mut self, v: Value, fw: Width) -> Value {
        let (abs, inf, qnan) = if fw.bits() == 16 {
            (0x7fffu128, 0x7c00u128, 0x7e00u128)
        } else if fw.bits() == 32 {
            (0x7fff_ffffu128, 0x7f80_0000u128, 0x7fc0_0000u128)
        } else {
            (
                0x7fff_ffff_ffff_ffffu128,
                0x7ff0_0000_0000_0000u128,
                0x7ff8_0000_0000_0000u128,
            )
        };
        let m = self.b.konst(fw, abs);
        let a = self.b.and(v, m);
        let i = self.b.konst(fw, inf);
        let nan = self.b.cmp(CmpOp::Ult, i, a);
        let q = self.b.konst(fw, qnan);
        self.b.select(nan, q, v)
    }

    /// A float of width `fw` biased to the edges of binary16: specials,
    /// subnormals, the largest finite values, the overflow threshold and
    /// halfway cases of the narrowing conversions.
    fn half_float(&mut self, fw: Width) -> Value {
        let specials16: [u128; 20] = [
            0x0000, 0x8000, 0x3c00, 0xbc00, 0x7c00, 0xfc00, 0x7e00, 0x7d01, 0xfe00, 0x0001, 0x8001,
            0x03ff, 0x0400, 0x7bff, 0xfbff, 0x3800, 0x3e00, 0x4100, 0x7800, 0x77ff,
        ];
        let specials32: [u128; 10] = [
            0x477f_f000,
            0x477f_efff,
            0x477f_f001,
            0x3300_0000,
            0x3300_0001,
            0x33c0_0000,
            0x3880_0000,
            0x387f_ffff,
            0xc77f_f000,
            0x4700_0000,
        ];
        let specials64: [u128; 10] = [
            0x40ef_fe00_0000_0000,
            0x40ef_fdff_ffff_ffff,
            0x40ef_fe00_0000_0001,
            0x3e60_0000_0000_0000,
            0x3e60_0000_0000_0001,
            0x3e78_0000_0000_0000,
            0x3f10_0000_0000_0000,
            0x3f0f_ffff_ffff_ffff,
            0xc0ef_fe00_0000_0000,
            0x40e0_0000_0000_0000,
        ];
        if self.r.chance(0.3) {
            return self.float(fw);
        }
        let v = match fw.bits() {
            16 => self.r.pick(&specials16),
            32 => self.r.pick(&specials32),
            _ => self.r.pick(&specials64),
        };
        let v = if self.r.chance(0.25) {
            v ^ (self.r.below(4) as u128)
        } else {
            v
        };
        self.b.konst(fw, v & fw.mask())
    }

    fn rounding(&mut self) -> FpRound {
        self.r.pick(&[
            FpRound::TiesEven,
            FpRound::TiesAway,
            FpRound::Floor,
            FpRound::Ceil,
            FpRound::Zero,
        ])
    }

    /// A floating-point operation where an operand or the result is
    /// binary16 (or a 16-bit integer).
    fn fp16(&mut self) {
        let h = w(16);
        let rnd = self.rounding();
        let op = self.r.pick(&[
            FpOp::Add,
            FpOp::Sub,
            FpOp::Mul,
            FpOp::Div,
            FpOp::Sqrt,
            FpOp::MulAdd,
            FpOp::Neg,
            FpOp::Abs,
            FpOp::RoundInt(rnd),
            FpOp::ToSInt(rnd),
            FpOp::ToUInt(rnd),
            FpOp::FromSInt,
            FpOp::FromUInt,
            FpOp::Convert,
        ]);
        let other = w(self.r.pick(&[16, 32, 64]));
        let v = match op {
            FpOp::ToSInt(_) | FpOp::ToUInt(_) => {
                let (from, to) = if self.r.chance(0.6) {
                    (h, other)
                } else {
                    (w(self.r.pick(&[32, 64])), h)
                };
                let a = self.half_float(from);
                let v = self.b.fp(op, &[a], to);
                self.keep(v)
            }
            FpOp::FromSInt | FpOp::FromUInt => {
                let (from, to) = if self.r.chance(0.6) {
                    (other, h)
                } else {
                    (h, w(self.r.pick(&[32, 64])))
                };
                let a = self.of(from);
                let v = self.b.fp(op, &[a], to);
                self.canon(v, to)
            }
            FpOp::Convert => {
                let wide = w(self.r.pick(&[32, 64]));
                let (from, to) = match self.r.below(5) {
                    0 | 1 => (h, wide),
                    2 | 3 => (wide, h),
                    _ => (h, h),
                };
                let a = self.half_float(from);
                let v = self.b.fp(op, &[a], to);
                self.canon(v, to)
            }
            _ => {
                let args: Vec<Value> = (0..op.arity()).map(|_| self.half_float(h)).collect();
                let v = self.b.fp(op, &args, h);
                self.canon(v, h)
            }
        };
        self.keep(v);
    }

    /// An atomic read-modify-write or compare-and-swap, sometimes on the
    /// address of an exclusive load before it, which it must disarm.
    fn atomic(&mut self) {
        let n = self.r.pick(&[1u32, 2, 3, 4, 8, 12, 16]);
        let ww = w(8 * n);
        let a = self.addr();
        let e = self.r.pick(&[Endian::Little, Endian::Big]);
        let o = self.r.pick(&[
            Order::Plain,
            Order::Acquire,
            Order::Release,
            Order::AcqRel,
            Order::SeqCst,
        ]);
        let exclusive = self.r.chance(0.25);
        if exclusive {
            let v = self.b.load_exclusive(a, ww, e, Order::Acquire);
            self.keep(v);
        }
        let v = if self.r.chance(0.6) {
            let op = self.r.pick(&[
                AtomicOp::Add,
                AtomicOp::Clear,
                AtomicOp::Xor,
                AtomicOp::Or,
                AtomicOp::SMax,
                AtomicOp::SMin,
                AtomicOp::UMax,
                AtomicOp::UMin,
                AtomicOp::Swap,
            ]);
            let x = self.of(ww);
            self.b.atomic_rmw(op, a, x, e, o)
        } else {
            let expected = if self.r.chance(0.5) {
                self.b.load(a, ww, e, Order::Plain)
            } else {
                self.of(ww)
            };
            let x = self.of(ww);
            self.b.compare_swap(a, expected, x, e, o)
        };
        self.keep(v);
        if exclusive {
            let x = self.of(ww);
            let ok = self.b.store_exclusive(a, x, e, Order::Release);
            self.keep(ok);
        }
    }

    fn crypto(&mut self) {
        let op = self.r.pick(&[
            CryptoOp::AesSubBytes,
            CryptoOp::AesInvSubBytes,
            CryptoOp::AesMixColumns,
            CryptoOp::AesInvMixColumns,
            CryptoOp::Sm4Sbox,
        ]);
        let x = self.of(op.width());
        let v = self.b.crypto(op, x);
        self.keep(v);
    }

    fn fp(&mut self) {
        let fw = w(self.r.pick(&[32, 64]));
        let other = w(self.r.pick(&[32, 64]));
        let rnd = self.r.pick(&[
            FpRound::TiesEven,
            FpRound::TiesAway,
            FpRound::Floor,
            FpRound::Ceil,
            FpRound::Zero,
        ]);
        let op = self.r.pick(&[
            FpOp::Add,
            FpOp::Sub,
            FpOp::Mul,
            FpOp::Div,
            FpOp::Sqrt,
            FpOp::MulAdd,
            FpOp::Neg,
            FpOp::Abs,
            FpOp::RoundInt(rnd),
            FpOp::ToSInt(rnd),
            FpOp::ToUInt(rnd),
            FpOp::FromSInt,
            FpOp::FromUInt,
            FpOp::Convert,
        ]);
        let v = match op {
            FpOp::ToSInt(_) | FpOp::ToUInt(_) => {
                let a = self.float(other);
                let v = self.b.fp(op, &[a], fw);
                self.keep(v)
            }
            FpOp::FromSInt | FpOp::FromUInt => {
                let a = self.of(other);
                let v = self.b.fp(op, &[a], fw);
                self.canon(v, fw)
            }
            FpOp::Convert => {
                let a = self.float(other);
                let v = self.b.fp(op, &[a], fw);
                self.canon(v, fw)
            }
            _ => {
                let args: Vec<Value> = (0..op.arity()).map(|_| self.float(fw)).collect();
                let v = self.b.fp(op, &args, fw);
                self.canon(v, fw)
            }
        };
        self.keep(v);
    }

    /// A step of the atomic, cipher and binary16 operations only.
    fn newer_step(&mut self) {
        match self.r.below(10) {
            0..=3 => self.atomic(),
            4..=5 => self.crypto(),
            _ => self.fp16(),
        }
    }

    fn step(&mut self) {
        let k = self.r.below(28);
        if k >= 24 {
            match k {
                24 | 25 => self.atomic(),
                26 => self.crypto(),
                _ => self.fp16(),
            }
            return;
        }
        match self.r.below(24) {
            0..=2 => {
                let op = self.r.pick(&[
                    UnOp::Not,
                    UnOp::Neg,
                    UnOp::Clz,
                    UnOp::Ctz,
                    UnOp::Popcount,
                    UnOp::BitReverse,
                    UnOp::ByteSwap,
                ]);
                let ww = if op == UnOp::ByteSwap {
                    w(self.r.pick(&BYTE_WIDTHS))
                } else {
                    self.any_width()
                };
                let x = self.of(ww);
                let v = self.b.un(op, x);
                self.keep(v);
            }
            3..=8 => {
                let op = self.r.pick(&[
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
                ]);
                let ww = self.any_width();
                let x = self.of(ww);
                let y = if matches!(op, BinOp::Shl | BinOp::LShr | BinOp::AShr | BinOp::RotR) {
                    let aw = self.any_width();
                    if self.r.chance(0.5) {
                        let k = self.r.below(u64::from(ww.bits()) + 3) as u128 & aw.mask();
                        self.b.konst(aw, k)
                    } else {
                        self.of(aw)
                    }
                } else {
                    self.of(ww)
                };
                let v = self.b.bin(op, x, y);
                self.keep(v);
            }
            9..=10 => {
                let op = self.r.pick(&[
                    CmpOp::Eq,
                    CmpOp::Ne,
                    CmpOp::Ult,
                    CmpOp::Ule,
                    CmpOp::Slt,
                    CmpOp::Sle,
                ]);
                let ww = self.any_width();
                let x = self.of(ww);
                let y = if self.r.chance(0.2) { x } else { self.of(ww) };
                let v = self.b.cmp(op, x, y);
                self.keep(v);
            }
            11 => {
                let c = self.of(w(1));
                let ww = self.any_width();
                let (x, y) = (self.of(ww), self.of(ww));
                let v = self.b.select(c, x, y);
                self.keep(v);
            }
            12 => {
                let hw = self.r.below(127) as u32 + 1;
                let lw = self.r.below(u64::from(128 - hw)) as u32 + 1;
                let (h, l) = (self.of(w(hw)), self.of(w(lw)));
                let v = self.b.concat(h, l);
                self.keep(v);
            }
            13..=14 => {
                let n = self.r.below(16) as u32 + 1;
                let a = self.addr();
                let e = self.r.pick(&[Endian::Little, Endian::Big]);
                let o = self.r.pick(&[Order::Plain, Order::Acquire, Order::SeqCst]);
                let v = self.b.load(a, w(8 * n), e, o);
                self.keep(v);
            }
            15..=16 => {
                let n = self.r.below(16) as u32 + 1;
                let a = self.addr();
                let e = self.r.pick(&[Endian::Little, Endian::Big]);
                let o = self.r.pick(&[Order::Plain, Order::Release, Order::SeqCst]);
                let v = self.of(w(8 * n));
                self.b.store(a, v, e, o);
            }
            17 => {
                let n = self.r.pick(&[1u32, 2, 4, 8, 16]);
                let a = self.addr();
                let e = self.r.pick(&[Endian::Little, Endian::Big]);
                let v = self.b.load_exclusive(a, w(8 * n), e, Order::Acquire);
                self.keep(v);
                if self.r.chance(0.2) {
                    self.b.clear_exclusive();
                }
                let (a2, n2) = match self.r.below(4) {
                    0 => (self.addr(), n),
                    1 => (a, self.r.pick(&[1u32, 2, 4, 8, 16])),
                    _ => (a, n),
                };
                let x = self.of(w(8 * n2));
                let ok = self.b.store_exclusive(a2, x, e, Order::Release);
                self.keep(ok);
            }
            18 => {
                if self.r.chance(0.5) {
                    self.b.clear_exclusive();
                } else {
                    let o = self
                        .r
                        .pick(&[Order::Acquire, Order::Release, Order::SeqCst]);
                    self.b.fence(o);
                }
            }
            19..=21 => self.fp(),
            _ => {
                let v = match self.r.below(6) {
                    0 => {
                        let a = self.of(w(64));
                        let rw = w(self.r.pick(&[64, 32, 13]));
                        self.b.intrinsic("t.hash1", &[a], Some(rw))
                    }
                    1 => {
                        let (a, c) = (self.of(w(64)), self.of(w(32)));
                        self.b.intrinsic("t.hash2", &[a, c], Some(w(64)))
                    }
                    2 => {
                        let a = self.of(w(128));
                        let rw = w(self.r.pick(&[128, 100]));
                        self.b.intrinsic("t.wide", &[a], Some(rw))
                    }
                    3 => {
                        let rw = self.any_width();
                        self.b.intrinsic("t.const", &[], Some(rw))
                    }
                    4 => {
                        self.b.intrinsic("t.nop", &[], None);
                        return;
                    }
                    _ => {
                        self.b.intrinsic("t.effect", &[], None);
                        return;
                    }
                };
                self.keep(v);
            }
        }
    }
}

/// A random block at `pc` over every operation.
pub fn block(r: &mut Rng, pc: u64) -> Block {
    block_of(r, pc, false)
}

/// A random block at `pc`: over every operation, or with `newer` over the
/// atomic, cipher and binary16 operations.
fn block_of(r: &mut Rng, pc: u64, newer: bool) -> Block {
    let regs = file();
    let mut g = Gen {
        b: Builder::new(regs, w(64), pc),
        r,
        pool: Vec::new(),
    };
    for i in 0..regs.len() {
        if g.r.chance(0.5) {
            let v = g.b.get(Reg(i as u16));
            g.keep(v);
        }
    }
    if g.r.chance(0.08) {
        let x = g.of(w(8));
        let k = g.b.konst(w(8), 0x80);
        let c = g.b.cmp(CmpOp::Ult, k, x);
        g.b.guard(c, GUARD);
    }
    let n = 2 + g.r.below(14);
    for _ in 0..n {
        if newer {
            g.newer_step();
        } else {
            g.step();
        }
    }
    for i in 0..regs.len() {
        if g.r.chance(0.6) {
            let reg = Reg(i as u16);
            let v = g.of(regs.width(reg));
            g.b.set(reg, v);
        }
    }
    let next = match g.r.below(3) {
        0 => Next::Const(g.r.next()),
        1 => {
            let cond = g.of(w(1));
            Next::Select {
                cond,
                taken: g.r.next(),
                not_taken: g.r.next(),
            }
        }
        _ => Next::Dynamic(g.of(w(64))),
    };
    g.b.finish(
        4,
        Exit {
            next,
            flow: Flow::Jump,
        },
    )
}

/// What one step left behind.
#[derive(Debug, PartialEq, Eq)]
pub struct Snap {
    pub slow: bool,
    pub pc: u64,
    pub regs: Vec<u128>,
    pub mem: Bytes,
    pub monitor: Option<Monitor>,
}

pub struct Input {
    pub regs: Vec<u128>,
    pub mem: Vec<u8>,
    pub monitor: Option<Monitor>,
}

pub fn input(r: &mut Rng) -> Input {
    let regs = file().decls().iter().map(|d| r.value(d.width)).collect();
    let mem = (0..WINDOW).map(|_| r.next() as u8).collect();
    let monitor = r.chance(0.3).then(|| Monitor {
        addr: DATA + (r.next() & 0x3fef),
        size: r.pick(&[1, 2, 4, 8, 16]),
    });
    Input { regs, mem, monitor }
}

fn interpret(b: &Block, i: &Input) -> Snap {
    try_interpret(b, i).expect("the block runs")
}

fn try_interpret(b: &Block, i: &Input) -> Option<Snap> {
    let mut st = State::new(file().len());
    st.regs.clone_from(&i.regs);
    st.monitor = i.monitor;
    let mut m = FlatMemory {
        base: 0,
        bytes: i.mem.clone(),
    };
    let o = interp::run(b, &mut st, &mut m, &mut TestIntrinsics).ok()?;
    let slow = matches!(o.flow, Flow::Trap(GUARD)) && o.next == b.pc;
    Some(Snap {
        slow,
        pc: if slow { 0 } else { o.next },
        regs: st.regs,
        mem: Bytes(m.bytes),
        monitor: st.monitor,
    })
}

fn run(rig: &mut Rig, f: &wasmtime::TypedFunc<(i32, i32), i32>, i: &Input) -> Snap {
    let regs = file();
    let m = rig.mem();
    m[CTX as usize..CTX as usize + ctx::size(&regs) as usize].fill(0);
    for (k, v) in i.regs.iter().enumerate() {
        put_reg(m, CTX, Reg(k as u16), *v);
    }
    if let Some(mon) = i.monitor {
        let c = CTX as usize;
        m[c + ctx::MONITOR_ADDR as usize..][..8].copy_from_slice(&mon.addr.to_le_bytes());
        m[c + ctx::MONITOR_SIZE as usize..][..4].copy_from_slice(&mon.size.to_le_bytes());
        m[c + ctx::MONITOR_ARMED as usize..][..4].copy_from_slice(&1u32.to_le_bytes());
    }
    rig.guest()[..WINDOW].copy_from_slice(&i.mem);
    rig.store.data_mut().slow.clear();
    let rc = rig.call(f, CTX).expect("no trap");
    let slow = !rig.store.data().slow.is_empty();
    assert_eq!(rc, i32::from(slow), "the return code follows the slow path");
    let m = rig.mem();
    let c = CTX as usize;
    let armed = u32::from_le_bytes(
        m[c + ctx::MONITOR_ARMED as usize..][..4]
            .try_into()
            .unwrap(),
    );
    let monitor = (armed == 1).then(|| Monitor {
        addr: ctx_u64(m, CTX, ctx::MONITOR_ADDR),
        size: u32::from_le_bytes(m[c + ctx::MONITOR_SIZE as usize..][..4].try_into().unwrap()),
    });
    Snap {
        slow,
        pc: if slow { 0 } else { ctx_u64(m, CTX, ctx::PC) },
        regs: regs
            .decls()
            .iter()
            .enumerate()
            .map(|(k, d)| get_reg(m, CTX, Reg(k as u16), d.width))
            .collect(),
        mem: Bytes(rig.guest()[..WINDOW].to_vec()),
        monitor,
    }
}

/// Emits `blocks` as one step-mode module, one function `f<i>` each.
pub fn module(blocks: &[Block], opts: &Options) -> relift_emit_wasm::Output {
    let fns: Vec<Function<'_>> = blocks
        .iter()
        .enumerate()
        .map(|(i, b)| Function {
            entry: b.pc,
            bank: None,
            name: format!("f{i}"),
            insts: [(b.pc, Insn::Lifted { block: b, raw: 0 })]
                .into_iter()
                .collect(),
            tables: BTreeMap::new(),
        })
        .collect();
    emit(&fns, opts)
}

pub fn step_options() -> Options {
    Options {
        exits: Exits::Step,
        table_base: FIRST_SLOT,
        memory_pages: crate::support::PAGES,
        intrinsics: intrinsics(),
        export: true,
        ..Options::default()
    }
}

#[test]
fn random_blocks_match_the_interpreter() {
    let blocks_n = scale(600, 20_000);
    let states = scale(6, 40);
    let batch = 50;
    let mut r = Rng::new("ops", seed());
    let engine = engine(true);
    let (mut matched, mut total, mut slow) = (0u64, 0u64, 0u64);
    let (mut caught, mut seen) = (0u64, 0u64);
    let mut first = None;
    let mut done = 0;
    while done < blocks_n {
        let k = batch.min(blocks_n - done) as usize;
        let blocks: Vec<Block> = (0..k).map(|i| block(&mut r, PC + 16 * i as u64)).collect();
        let mut broken = blocks.clone();
        for b in &mut broken {
            mutate(b);
        }
        let opts = step_options();
        let out = module(&blocks, &opts);
        assert!(out.errors.is_empty(), "emit errors: {:?}", out.errors);
        let bad = module(&broken, &opts);
        let mut rig = Rig::new(&engine, FIRST_SLOT + k as u32, file(), &INTRINSICS);
        let inst = rig.load(&out).expect("the module loads");
        let mut mrig = Rig::new(&engine, FIRST_SLOT + k as u32, file(), &INTRINSICS);
        let minst = mrig.load(&bad).expect("the mutated module loads");
        for (i, (b, m)) in blocks.iter().zip(&broken).enumerate() {
            let f = rig.func(&inst, &format!("f{i}"));
            let mf = mrig.func(&minst, &format!("f{i}"));
            for _ in 0..states {
                let inp = input(&mut r);
                let want = interpret(b, &inp);
                let got = run(&mut rig, &f, &inp);
                total += 1;
                slow += u64::from(want.slow);
                if want == got {
                    matched += 1;
                } else if first.is_none() {
                    first = Some(format!(
                        "{b}\nwant {want:x?}\ngot {got:x?}\nmemory {}",
                        diff_bytes(&want.mem, &got.mem)
                    ));
                }
                if try_interpret(m, &inp).is_some_and(|x| x != want) {
                    seen += 1;
                    if run(&mut mrig, &mf, &inp) != want {
                        caught += 1;
                    }
                }
            }
        }
        done += k as u64;
    }
    println!(
        "ops: {blocks_n} blocks, {total} states, {matched} matched, {slow} slow path; mutation caught {caught}/{seen}"
    );
    assert!(
        first.is_none(),
        "first difference:\n{}",
        first.unwrap_or_default()
    );
    assert!(slow > 0, "control: no state took the slow path");
    assert!(
        seen > 0 && caught == seen,
        "mutation control: caught {caught}/{seen}"
    );
    let _ = BIAS;
}

/// The names of the atomic, cipher and binary16 shapes `b` uses, for the
/// coverage check: each atomic update and compare-and-swap by width, each
/// cipher step, and each binary16 operation by operand and result width.
fn shapes(b: &Block, out: &mut BTreeSet<String>) {
    let wid = |v: Value| b.insts[v.index()].width.map_or(1, |w| w.bits());
    for inst in &b.insts {
        let n = inst.width.map_or(1, |w| w.bits());
        match &inst.op {
            Op::AtomicRmw { op, .. } => {
                out.insert(format!("{op:?} {n}"));
            }
            Op::CompareSwap { .. } => {
                out.insert(format!("cas {n}"));
            }
            Op::Crypto(c, _) => {
                out.insert(format!("{c:?}"));
            }
            Op::Fp(f, args) => {
                let aw = wid(args[0]);
                if aw == 16 || n == 16 {
                    out.insert(format!("{f:?} {aw}->{n}"));
                }
            }
            _ => {}
        }
    }
}

/// Every shape the binary16, atomic and cipher check must reach.
fn wanted_shapes() -> Vec<String> {
    let mut want = Vec::new();
    let atomics = [
        AtomicOp::Add,
        AtomicOp::Clear,
        AtomicOp::Xor,
        AtomicOp::Or,
        AtomicOp::SMax,
        AtomicOp::SMin,
        AtomicOp::UMax,
        AtomicOp::UMin,
        AtomicOp::Swap,
    ];
    for n in [8, 16, 32, 64, 128] {
        for op in atomics {
            want.push(format!("{op:?} {n}"));
        }
        want.push(format!("cas {n}"));
    }
    for c in [
        CryptoOp::AesSubBytes,
        CryptoOp::AesInvSubBytes,
        CryptoOp::AesMixColumns,
        CryptoOp::AesInvMixColumns,
        CryptoOp::Sm4Sbox,
    ] {
        want.push(format!("{c:?}"));
    }
    let rounds = [
        FpRound::TiesEven,
        FpRound::TiesAway,
        FpRound::Floor,
        FpRound::Ceil,
        FpRound::Zero,
    ];
    let mut same: Vec<FpOp> = vec![
        FpOp::Add,
        FpOp::Sub,
        FpOp::Mul,
        FpOp::Div,
        FpOp::Sqrt,
        FpOp::MulAdd,
        FpOp::Neg,
        FpOp::Abs,
        FpOp::Convert,
    ];
    same.extend(rounds.iter().map(|r| FpOp::RoundInt(*r)));
    for f in same {
        want.push(format!("{f:?} 16->16"));
    }
    for r in rounds {
        for f in [FpOp::ToSInt(r), FpOp::ToUInt(r)] {
            for n in [16, 32, 64] {
                want.push(format!("{f:?} 16->{n}"));
            }
            for a in [32, 64] {
                want.push(format!("{f:?} {a}->16"));
            }
        }
    }
    for f in [FpOp::FromSInt, FpOp::FromUInt] {
        for a in [16, 32, 64] {
            want.push(format!("{f:?} {a}->16"));
        }
        for n in [32, 64] {
            want.push(format!("{f:?} 16->{n}"));
        }
    }
    for n in [32, 64] {
        want.push(format!("Convert 16->{n}"));
        want.push(format!("Convert {n}->16"));
    }
    want
}

#[test]
fn atomic_cipher_and_binary16_blocks_match_the_interpreter() {
    let blocks_n = scale(1_500, 30_000);
    let states = scale(6, 40);
    let batch = 50;
    let mut r = Rng::new("newer", seed());
    let engine = engine(true);
    let (mut matched, mut total) = (0u64, 0u64);
    let (mut caught, mut seen) = (0u64, 0u64);
    let mut reached = BTreeSet::new();
    let mut first = None;
    let mut done = 0;
    while done < blocks_n {
        let k = batch.min(blocks_n - done) as usize;
        let blocks: Vec<Block> = (0..k)
            .map(|i| block_of(&mut r, PC + 16 * i as u64, true))
            .collect();
        for b in &blocks {
            shapes(b, &mut reached);
        }
        let mut broken = blocks.clone();
        for b in &mut broken {
            mutate_newer(b);
        }
        let opts = step_options();
        let out = module(&blocks, &opts);
        assert!(out.errors.is_empty(), "emit errors: {:?}", out.errors);
        let bad = module(&broken, &opts);
        let mut rig = Rig::new(&engine, FIRST_SLOT + k as u32, file(), &INTRINSICS);
        let inst = rig.load(&out).expect("the module loads");
        let mut mrig = Rig::new(&engine, FIRST_SLOT + k as u32, file(), &INTRINSICS);
        let minst = mrig.load(&bad).expect("the mutated module loads");
        for (i, (b, m)) in blocks.iter().zip(&broken).enumerate() {
            let f = rig.func(&inst, &format!("f{i}"));
            let mf = mrig.func(&minst, &format!("f{i}"));
            for _ in 0..states {
                let inp = input(&mut r);
                let want = interpret(b, &inp);
                let got = run(&mut rig, &f, &inp);
                total += 1;
                if want == got {
                    matched += 1;
                } else if first.is_none() {
                    first = Some(format!(
                        "{b}\nwant {want:x?}\ngot {got:x?}\nmemory {}",
                        diff_bytes(&want.mem, &got.mem)
                    ));
                }
                if try_interpret(m, &inp).is_some_and(|x| x != want) {
                    seen += 1;
                    if run(&mut mrig, &mf, &inp) != want {
                        caught += 1;
                    }
                }
            }
        }
        done += k as u64;
    }
    let missing: Vec<String> = wanted_shapes()
        .into_iter()
        .filter(|s| !reached.contains(s))
        .collect();
    println!(
        "atomic, cipher and binary16: {blocks_n} blocks, {} shapes, {total} states, {matched} matched; mutation caught {caught}/{seen}",
        reached.len()
    );
    assert!(
        first.is_none(),
        "first difference:\n{}",
        first.unwrap_or_default()
    );
    assert!(missing.is_empty(), "shapes never generated: {missing:?}");
    assert!(
        seen > 0 && caught == seen,
        "mutation control: caught {caught}/{seen}"
    );
}

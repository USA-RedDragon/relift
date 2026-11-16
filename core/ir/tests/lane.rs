//! Per-lane operations: every kind and lane width against a reference written
//! here, edge values, text and binary round trips, and verifier rejections.

use relift_ir::interp::{self, FlatMemory, NoIntrinsics, State};
use relift_ir::{BinOp, Reader};
use relift_ir::{
    Block, Builder, Exit, Flow, FpArgs, LaneKind, LaneOp, LaneWidth, Next, Op, Permute, Reg,
    RegDecl, RegFile, Value, Width, parse, read_block, write_block,
};

static DECLS: [RegDecl; 5] = [
    RegDecl {
        name: "a",
        width: Width::W128,
    },
    RegDecl {
        name: "b",
        width: Width::W128,
    },
    RegDecl {
        name: "c",
        width: Width::W128,
    },
    RegDecl {
        name: "s",
        width: Width::W64,
    },
    RegDecl {
        name: "o",
        width: Width::W128,
    },
];

fn file() -> RegFile {
    RegFile::new("t", &DECLS)
}

fn files() -> [RegFile; 1] {
    [file()]
}

fn run_block(block: &Block, regs: [u128; 4]) -> u128 {
    let mut s = State::new(5);
    s.regs[..4].copy_from_slice(&regs);
    let mut m = FlatMemory {
        base: 0,
        bytes: vec![0; 16],
    };
    interp::run(block, &mut s, &mut m, &mut NoIntrinsics).expect("runs");
    s.regs[4]
}

fn finish(mut b: Builder, v: Value) -> Block {
    b.set(Reg(4), v);
    b.jump(4, 4)
}

fn exec(op: LaneOp, args: [u128; 3]) -> u128 {
    let mut b = Builder::new(file(), Width::W64, 0);
    let regs: Vec<Value> = (0..op.kind.arity())
        .map(|i| {
            if op.operand_width(i) == Width::W128 {
                b.get(Reg(i as u16))
            } else {
                let g = b.get(Reg(3));
                b.trunc(g, op.lane.scalar())
            }
        })
        .collect();
    let r = b.lane(op, &regs);
    let r = if op.result_width() == Width::W128 {
        r
    } else {
        b.zext(r, Width::W128)
    };
    let block = finish(b, r);
    let mut inputs = [args[0], args[1], args[2], 0];
    for (i, a) in args.iter().enumerate().take(op.kind.arity()) {
        if op.operand_width(i) != Width::W128 {
            inputs[3] = *a;
        }
    }
    run_block(&block, inputs)
}

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u128 {
        let mut x = || {
            self.0 = self
                .0
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            u128::from(self.0 >> 11) << 64 | u128::from(self.0.rotate_left(29))
        };
        x() ^ (x() << 53)
    }
}

fn lane_vals(v: u128, bits: u32) -> Vec<i128> {
    (0..128 / bits)
        .map(|i| ((v >> (i * bits)) & ((1u128 << bits) - 1)) as i128)
        .collect()
}

fn signed(x: i128, bits: u32) -> i128 {
    if x >= 1 << (bits - 1) {
        x - (1 << bits)
    } else {
        x
    }
}

fn pack(vals: &[i128], bits: u32) -> u128 {
    let mask = (1u128 << bits) - 1;
    vals.iter().enumerate().fold(0u128, |acc, (i, &v)| {
        acc | (v as u128 & mask) << (i as u32 * bits)
    })
}

fn edge_vectors(bits: u32) -> Vec<u128> {
    let n = 128 / bits;
    let pools: [i128; 8] = [
        0,
        1,
        2,
        (1 << (bits - 1)) - 1,
        1 << (bits - 1),
        (1 << (bits - 1)) + 1,
        (1 << bits) - 1,
        (1 << bits) - 2,
    ];
    let mut out = vec![0, u128::MAX];
    for k in 0..pools.len() {
        out.push(pack(&vec![pools[k]; n as usize], bits));
        let rot: Vec<i128> = (0..n as usize).map(|i| pools[(i + k) % 8]).collect();
        out.push(pack(&rot, bits));
    }
    out
}

fn inputs(bits: u32, count: usize) -> Vec<(u128, u128, u128)> {
    let edges = edge_vectors(bits);
    let mut v = Vec::new();
    for (i, a) in edges.iter().enumerate() {
        for (j, b) in edges.iter().enumerate() {
            v.push((*a, *b, edges[(i + j) % edges.len()]));
        }
    }
    let mut rng = Rng(u64::from(bits));
    for _ in 0..count {
        v.push((rng.next(), rng.next(), rng.next()));
    }
    v
}

fn range(bits: u32, signed: bool) -> (i128, i128) {
    if signed {
        (-(1 << (bits - 1)), (1 << (bits - 1)) - 1)
    } else {
        (0, (1 << bits) - 1)
    }
}

fn reference(kind: LaneKind, bits: u32, imm: u32, a: u128, b: u128, c: u128) -> u128 {
    use LaneKind::*;
    let n = 128 / bits;
    let ua = lane_vals(a, bits);
    let ub = lane_vals(b, bits);
    let sa: Vec<i128> = ua.iter().map(|&x| signed(x, bits)).collect();
    let sb: Vec<i128> = ub.iter().map(|&x| signed(x, bits)).collect();
    let (slo, shi) = range(bits, true);
    let (_, uhi) = range(bits, false);
    let each = |f: &dyn Fn(usize) -> i128| -> u128 {
        pack(&(0..n as usize).map(f).collect::<Vec<_>>(), bits)
    };
    let mask = |c: bool| if c { -1 } else { 0 };
    let half = (n / 2) as usize;
    let wide = |off: usize, signed_ext: bool| -> u128 {
        let v: Vec<i128> = (0..half)
            .map(|i| if signed_ext { sa[off + i] } else { ua[off + i] })
            .collect();
        pack(&v, bits * 2)
    };
    let wide_mul = |off: usize, sa_: bool, sb_: bool| -> u128 {
        let v: Vec<i128> = (0..half)
            .map(|i| {
                let x = if sa_ { sa[off + i] } else { ua[off + i] };
                let y = if sb_ { sb[off + i] } else { ub[off + i] };
                x * y
            })
            .collect();
        pack(&v, bits * 2)
    };
    let narrow = |f: &dyn Fn(i128) -> i128| -> u128 {
        let d = bits / 2;
        let mut v: Vec<i128> = sa.iter().map(|&x| f(x)).collect();
        v.extend(sb.iter().map(|&x| f(x)));
        pack(&v, d)
    };
    let shamt = |i: usize| -> u32 {
        if matches!(kind, ShlImm | LShrImm | AShrImm) {
            imm
        } else {
            ub[i].min(255) as u32
        }
    };
    match kind {
        Add => each(&|i| ua[i] + ub[i]),
        Sub => each(&|i| ua[i] - ub[i]),
        AddSatS => each(&|i| (sa[i] + sb[i]).clamp(slo, shi)),
        SubSatS => each(&|i| (sa[i] - sb[i]).clamp(slo, shi)),
        AddSatU => each(&|i| (ua[i] + ub[i]).min(uhi)),
        SubSatU => each(&|i| (ua[i] - ub[i]).max(0)),
        MulLo => each(&|i| ua[i] * ub[i]),
        MulHiS => each(&|i| (sa[i] * sb[i]) >> bits),
        MulHiU => each(&|i| (ua[i] * ub[i]) >> bits),
        MulHiSU => each(&|i| (sa[i] * ub[i]) >> bits),
        And => a & b,
        Or => a | b,
        Xor => a ^ b,
        AndNot => a & !b,
        Not => !a,
        MinS => each(&|i| sa[i].min(sb[i])),
        MaxS => each(&|i| sa[i].max(sb[i])),
        MinU => each(&|i| ua[i].min(ub[i])),
        MaxU => each(&|i| ua[i].max(ub[i])),
        Abs => each(&|i| sa[i].abs()),
        Neg => each(&|i| -sa[i]),
        Eq => each(&|i| mask(ua[i] == ub[i])),
        Ne => each(&|i| mask(ua[i] != ub[i])),
        Slt => each(&|i| mask(sa[i] < sb[i])),
        Sle => each(&|i| mask(sa[i] <= sb[i])),
        Ult => each(&|i| mask(ua[i] < ub[i])),
        Ule => each(&|i| mask(ua[i] <= ub[i])),
        ShlImm | Shl => each(&|i| {
            if shamt(i) >= bits {
                0
            } else {
                ua[i] << shamt(i)
            }
        }),
        LShrImm | LShr => each(&|i| {
            if shamt(i) >= bits {
                0
            } else {
                ua[i] >> shamt(i)
            }
        }),
        AShrImm | AShr => each(&|i| {
            if shamt(i) >= bits {
                if sa[i] < 0 { -1 } else { 0 }
            } else {
                sa[i] >> shamt(i)
            }
        }),
        Select => (a & b) | (!a & c),
        WidenLoS => wide(0, true),
        WidenLoU => wide(0, false),
        WidenHiS => wide(half, true),
        WidenHiU => wide(half, false),
        NarrowTrunc => narrow(&|x| x),
        NarrowSS => {
            let (lo, hi) = range(bits / 2, true);
            narrow(&|x| x.clamp(lo, hi))
        }
        NarrowSU => {
            let (lo, hi) = range(bits / 2, false);
            narrow(&|x| x.clamp(lo, hi))
        }
        NarrowUU => {
            let (_, hi) = range(bits / 2, false);
            let d = bits / 2;
            let v: Vec<i128> = ua.iter().chain(ub.iter()).map(|&x| x.min(hi)).collect();
            pack(&v, d)
        }
        MulWideLoSS => wide_mul(0, true, true),
        MulWideLoUU => wide_mul(0, false, false),
        MulWideLoSU => wide_mul(0, true, false),
        MulWideHiSS => wide_mul(half, true, true),
        MulWideHiUU => wide_mul(half, false, false),
        MulWideHiSU => wide_mul(half, true, false),
        Broadcast => each(&|_| ua[imm as usize]),
        Extract => ua[imm as usize] as u128,
        Insert => {
            let mut v = ua.clone();
            v[imm as usize] = (b & ((1u128 << bits) - 1)) as i128;
            pack(&v, bits)
        }
        Splat => each(&|_| (a & ((1u128 << bits) - 1)) as i128),
    }
}

const WIDTHS: [LaneWidth; 4] = [
    LaneWidth::L8,
    LaneWidth::L16,
    LaneWidth::L32,
    LaneWidth::L64,
];

const KINDS: [LaneKind; 52] = {
    use LaneKind::*;
    [
        Add,
        Sub,
        AddSatS,
        AddSatU,
        SubSatS,
        SubSatU,
        MulLo,
        MulHiS,
        MulHiU,
        MulHiSU,
        And,
        Or,
        Xor,
        AndNot,
        Not,
        MinS,
        MinU,
        MaxS,
        MaxU,
        Abs,
        Neg,
        Eq,
        Ne,
        Slt,
        Sle,
        Ult,
        Ule,
        ShlImm,
        LShrImm,
        AShrImm,
        Shl,
        LShr,
        AShr,
        Select,
        WidenLoS,
        WidenLoU,
        WidenHiS,
        WidenHiU,
        NarrowTrunc,
        NarrowSS,
        NarrowUU,
        NarrowSU,
        MulWideLoSS,
        MulWideLoUU,
        MulWideLoSU,
        MulWideHiSS,
        MulWideHiUU,
        MulWideHiSU,
        Broadcast,
        Extract,
        Insert,
        Splat,
    ]
};

fn imms(kind: LaneKind, lane: LaneWidth) -> Vec<u8> {
    use LaneKind::*;
    match kind {
        ShlImm | LShrImm | AShrImm => {
            let w = lane.bits() as u8;
            vec![0, 1, w / 2, w - 1]
        }
        Broadcast | Extract | Insert => (0..lane.lanes() as u8).collect(),
        _ => vec![0],
    }
}

fn eq_hex(got: u128, want: u128, what: &str) {
    assert_eq!(got, want, "{what}: got {got:#034x}, want {want:#034x}");
}

#[test]
fn every_kind_and_width_matches_the_reference() {
    let mut checked = 0;
    for kind in KINDS {
        for lane in WIDTHS {
            if !kind.allows(lane) || (kind.bitwise() && lane != LaneWidth::L64) {
                continue;
            }
            let bits = lane.bits();
            for imm in imms(kind, lane) {
                let op = LaneOp::with_imm(kind, lane, imm);
                for (a, b, c) in inputs(bits, 40) {
                    let (a, b, c) = match kind {
                        LaneKind::Insert | LaneKind::Splat => {
                            let m = (1u128 << bits) - 1;
                            let (ia, ib) = if kind == LaneKind::Splat {
                                (a, 0)
                            } else {
                                (a, b)
                            };
                            let sc = if kind == LaneKind::Splat {
                                ia & m
                            } else {
                                ib & m
                            };
                            if kind == LaneKind::Splat {
                                (sc, 0, 0)
                            } else {
                                (ia, sc, 0)
                            }
                        }
                        _ => (a, b, c),
                    };
                    let want = reference(kind, bits, u32::from(imm), a, b, c);
                    let got = exec(op, [a, b, c]);
                    eq_hex(
                        got,
                        want,
                        &format!("{kind:?} L{bits} imm {imm} a={a:#x} b={b:#x} c={c:#x}"),
                    );
                    checked += 1;
                }
            }
        }
    }
    assert!(checked > 10_000, "{checked}");
}

fn v16(x: [u16; 8]) -> u128 {
    x.iter()
        .enumerate()
        .fold(0, |acc, (i, &v)| acc | u128::from(v) << (16 * i))
}

fn lane(kind: LaneKind, w: LaneWidth, args: &[u128]) -> u128 {
    let mut a = [0u128; 3];
    a[..args.len()].copy_from_slice(args);
    exec(LaneOp::new(kind, w), a)
}

#[test]
fn saturation_bounds_by_hand() {
    let l = LaneWidth::L16;
    let a = v16([0x7fff, 0x8000, 0xffff, 1, 0, 0x7fff, 0x8000, 0xffff]);
    let b = v16([1, 0xffff, 1, 0xffff, 0, 0x7fff, 0x8000, 0xffff]);
    assert_eq!(
        lane(LaneKind::AddSatS, l, &[a, b]),
        v16([0x7fff, 0x8000, 0, 0, 0, 0x7fff, 0x8000, 0xfffe])
    );
    assert_eq!(
        lane(LaneKind::AddSatU, l, &[a, b]),
        v16([0x8000, 0xffff, 0xffff, 0xffff, 0, 0xfffe, 0xffff, 0xffff])
    );
    assert_eq!(
        lane(LaneKind::SubSatS, l, &[a, b]),
        v16([0x7ffe, 0x8001, 0xfffe, 2, 0, 0, 0, 0])
    );
    assert_eq!(
        lane(LaneKind::SubSatU, l, &[a, b]),
        v16([0x7ffe, 0, 0xfffe, 0, 0, 0, 0, 0])
    );
}

#[test]
fn abs_and_neg_keep_the_minimum() {
    let a = v16([0x8000, 0xffff, 1, 0, 0x7fff, 0x8001, 2, 0xfffe]);
    assert_eq!(
        lane(LaneKind::Abs, LaneWidth::L16, &[a]),
        v16([0x8000, 1, 1, 0, 0x7fff, 0x7fff, 2, 2])
    );
    assert_eq!(
        lane(LaneKind::Neg, LaneWidth::L16, &[a]),
        v16([0x8000, 1, 0xffff, 0, 0x8001, 0x7fff, 0xfffe, 2])
    );
    assert_eq!(lane(LaneKind::Abs, LaneWidth::L8, &[0x80]), 0x80);
    assert_eq!(
        lane(LaneKind::Abs, LaneWidth::L32, &[0x8000_0000]),
        0x8000_0000
    );
}

#[test]
fn shifts_at_and_past_the_width() {
    let a = v16([0x8001; 8]);
    let amounts = v16([0, 1, 15, 16, 17, 255, 0x100, 0xffff]);
    assert_eq!(
        lane(LaneKind::Shl, LaneWidth::L16, &[a, amounts]),
        v16([0x8001, 2, 0x8000, 0, 0, 0, 0, 0])
    );
    assert_eq!(
        lane(LaneKind::LShr, LaneWidth::L16, &[a, amounts]),
        v16([0x8001, 0x4000, 1, 0, 0, 0, 0, 0])
    );
    assert_eq!(
        lane(LaneKind::AShr, LaneWidth::L16, &[a, amounts]),
        v16([
            0x8001, 0xc000, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff
        ])
    );
    let pos = v16([0x7001; 8]);
    assert_eq!(
        lane(LaneKind::AShr, LaneWidth::L16, &[pos, amounts]),
        v16([0x7001, 0x3800, 0, 0, 0, 0, 0, 0])
    );
    let imm = |k, i| exec(LaneOp::with_imm(k, LaneWidth::L16, i), [a, 0, 0]);
    assert_eq!(imm(LaneKind::ShlImm, 15), v16([0x8000; 8]));
    assert_eq!(imm(LaneKind::LShrImm, 15), v16([1; 8]));
    assert_eq!(imm(LaneKind::AShrImm, 15), v16([0xffff; 8]));
}

#[test]
fn narrowing_clamps() {
    let a = (0..4u32)
        .map(|i| [0x0001_0000u32, 0xffff_8000, 0x7fff_ffff, 0x0000_8000][i as usize])
        .enumerate()
        .fold(0u128, |acc, (i, v)| acc | u128::from(v) << (32 * i));
    let z = 0u128;
    assert_eq!(
        lane(LaneKind::NarrowSS, LaneWidth::L32, &[a, z]),
        v16([0x7fff, 0x8000, 0x7fff, 0x7fff, 0, 0, 0, 0])
    );
    assert_eq!(
        lane(LaneKind::NarrowSU, LaneWidth::L32, &[a, z]),
        v16([0xffff, 0, 0xffff, 0x8000, 0, 0, 0, 0])
    );
    assert_eq!(
        lane(LaneKind::NarrowUU, LaneWidth::L32, &[a, z]),
        v16([0xffff, 0xffff, 0xffff, 0x8000, 0, 0, 0, 0])
    );
    assert_eq!(
        lane(LaneKind::NarrowTrunc, LaneWidth::L32, &[a, z]),
        v16([0, 0x8000, 0xffff, 0x8000, 0, 0, 0, 0])
    );
    assert_eq!(
        lane(LaneKind::NarrowTrunc, LaneWidth::L32, &[z, a]),
        v16([0, 0, 0, 0, 0, 0x8000, 0xffff, 0x8000])
    );
}

#[test]
fn widening_and_wide_multiplies() {
    let a = v16([0x8000, 0x7fff, 0xffff, 1, 0x8000, 0x7fff, 0xffff, 2]);
    let wide = |x: [u32; 4]| {
        x.iter()
            .enumerate()
            .fold(0u128, |acc, (i, &v)| acc | u128::from(v) << (32 * i))
    };
    let l = LaneWidth::L16;
    assert_eq!(
        lane(LaneKind::WidenLoS, l, &[a]),
        wide([0xffff_8000, 0x7fff, 0xffff_ffff, 1])
    );
    assert_eq!(
        lane(LaneKind::WidenLoU, l, &[a]),
        wide([0x8000, 0x7fff, 0xffff, 1])
    );
    assert_eq!(
        lane(LaneKind::WidenHiS, l, &[a]),
        wide([0xffff_8000, 0x7fff, 0xffff_ffff, 2])
    );
    assert_eq!(
        lane(LaneKind::WidenHiU, l, &[a]),
        wide([0x8000, 0x7fff, 0xffff, 2])
    );
    assert_eq!(
        lane(LaneKind::MulWideLoSS, l, &[a, a]),
        wide([0x4000_0000, 0x3fff_0001, 1, 1])
    );
    assert_eq!(
        lane(LaneKind::MulWideLoUU, l, &[a, a]),
        wide([0x4000_0000, 0x3fff_0001, 0xfffe_0001, 1])
    );
    assert_eq!(
        lane(LaneKind::MulWideLoSU, l, &[a, a]),
        wide([0xc000_0000, 0x3fff_0001, 0xffff_0001, 1])
    );
    assert_eq!(
        lane(LaneKind::MulHiS, l, &[a, a]),
        v16([0x4000, 0x3fff, 0, 0, 0x4000, 0x3fff, 0, 0])
    );
    assert_eq!(
        lane(LaneKind::MulHiSU, l, &[a, a]),
        v16([0xc000, 0x3fff, 0xffff, 0, 0xc000, 0x3fff, 0xffff, 0])
    );
    assert_eq!(
        lane(LaneKind::MulLo, l, &[a, a]),
        v16([0, 1, 1, 1, 0, 1, 1, 4])
    );
}

#[test]
fn extract_insert_broadcast_splat() {
    let a = v16([10, 20, 30, 40, 50, 60, 70, 80]);
    let l = LaneWidth::L16;
    assert_eq!(
        exec(LaneOp::with_imm(LaneKind::Extract, l, 0), [a, 0, 0]),
        10
    );
    assert_eq!(
        exec(LaneOp::with_imm(LaneKind::Extract, l, 7), [a, 0, 0]),
        80
    );
    assert_eq!(
        exec(LaneOp::with_imm(LaneKind::Insert, l, 3), [a, 0x1_ffff, 0]),
        v16([10, 20, 30, 0xffff, 50, 60, 70, 80])
    );
    assert_eq!(
        exec(LaneOp::with_imm(LaneKind::Broadcast, l, 5), [a, 0, 0]),
        v16([60; 8])
    );
    assert_eq!(
        exec(LaneOp::new(LaneKind::Splat, l), [0xabcd, 0, 0]),
        v16([0xabcd; 8])
    );
    assert_eq!(
        exec(
            LaneOp::with_imm(LaneKind::Extract, LaneWidth::L8, 15),
            [a << 8, 0, 0]
        ),
        0x50
    );
}

#[test]
fn lane_zero_is_the_least_significant() {
    let a = 0x0f0e_0d0c_0b0a_0908_0706_0504_0302_0100u128;
    for i in 0..16u8 {
        assert_eq!(
            exec(
                LaneOp::with_imm(LaneKind::Extract, LaneWidth::L8, i),
                [a, 0, 0]
            ),
            u128::from(i)
        );
    }
    assert_eq!(
        exec(
            LaneOp::with_imm(LaneKind::Extract, LaneWidth::L32, 1),
            [a, 0, 0]
        ),
        0x0706_0504
    );
}

fn permute(pattern: [u8; 16], a: u128, b: Option<u128>) -> u128 {
    let mut bl = Builder::new(file(), Width::W64, 0);
    let x = bl.get(Reg(0));
    let r = match b {
        Some(_) => {
            let y = bl.get(Reg(1));
            bl.permute2(x, y, pattern)
        }
        None => bl.permute(x, pattern),
    };
    run_block(&finish(bl, r), [a, b.unwrap_or(0), 0, 0])
}

#[test]
fn permutes_including_out_of_range() {
    let a = 0x0f0e_0d0c_0b0a_0908_0706_0504_0302_0100u128;
    let b = 0x1f1e_1d1c_1b1a_1918_1716_1514_1312_1110u128;
    let id: [u8; 16] = std::array::from_fn(|i| i as u8);
    assert_eq!(permute(id, a, None), a);
    let rev: [u8; 16] = std::array::from_fn(|i| 15 - i as u8);
    assert_eq!(permute(rev, a, None), a.swap_bytes());
    let mut oob = id;
    oob[0] = 16;
    oob[1] = 0x80;
    oob[2] = 255;
    assert_eq!(permute(oob, a, None), a & !0xff_ffff);
    let two: [u8; 16] = std::array::from_fn(|i| {
        if i % 2 == 0 {
            i as u8 / 2
        } else {
            16 + i as u8 / 2
        }
    });
    assert_eq!(
        permute(two, a, Some(b)),
        u128::from_le_bytes([
            0, 0x10, 1, 0x11, 2, 0x12, 3, 0x13, 4, 0x14, 5, 0x15, 6, 0x16, 7, 0x17
        ])
    );
    let mut oob2 = id;
    oob2[5] = 32;
    assert_eq!(permute(oob2, a, Some(b)) >> 40 & 0xff, 0);
    assert_eq!(permute(id.map(|x| x + 16), a, Some(b)), b);
}

fn sample() -> Block {
    let mut b = Builder::new(file(), Width::W64, 0x1000);
    let x = b.get(Reg(0));
    let y = b.get(Reg(1));
    let z = b.get(Reg(2));
    let s = b.get(Reg(3));
    let mut acc = b.lane2(LaneKind::AddSatS, LaneWidth::L16, x, y);
    for kind in KINDS {
        for lane in WIDTHS {
            if !kind.allows(lane) || (kind.bitwise() && lane != LaneWidth::L64) {
                continue;
            }
            let imm = *imms(kind, lane).last().unwrap();
            let op = LaneOp::with_imm(kind, lane, imm);
            let args: Vec<Value> = (0..kind.arity())
                .map(|i| {
                    if op.operand_width(i) == Width::W128 {
                        [x, y, z][i]
                    } else {
                        b.trunc(s, lane.scalar())
                    }
                })
                .collect();
            let r = b.lane(op, &args);
            let r = if op.result_width() == Width::W128 {
                r
            } else {
                b.zext(r, Width::W128)
            };
            acc = b.bin(BinOp::Xor, acc, r);
        }
    }
    let p = b.permute(
        acc,
        [3, 2, 1, 0, 255, 16, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14],
    );
    let q = b.permute2(
        p,
        x,
        [0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23],
    );
    b.set(Reg(4), q);
    b.jump(4, 4)
}

#[test]
fn text_and_binary_round_trip() {
    let block = sample();
    let text = block.to_string();
    let back = parse(&text, &files()).unwrap_or_else(|e| panic!("{e}\n{text}"));
    assert_eq!(back, block);
    assert_eq!(back.to_string(), text);
    let mut bin = Vec::new();
    write_block(&mut bin, &block, &files());
    let mut r = Reader::new(&bin);
    assert_eq!(read_block(&mut r, &files()).unwrap(), block);
    assert!(r.is_empty());
    for cut in 0..bin.len() {
        assert!(read_block(&mut Reader::new(&bin[..cut]), &files()).is_err());
    }
    assert!(text.contains("lane.qadds.16 "), "{text}");
    assert!(text.contains("lane.and "), "{text}");
    assert!(text.contains("lane.perm2 "), "{text}");
}

#[test]
fn text_form_is_readable() {
    let text = "block 0x0 len 4 regs t\n  v0:i128 = get a\n  v1:i128 = get b\n  v2:i128 = lane.mulhs.16 v0, v1\n  v3:i16 = lane.extract.16 v2, 3\n  v4:i128 = lane.shli.32 v0, 5\n  v5:i128 = lane.perm v4, 0xf0e0d0c0b0a09080706050403020100\n  set o, v5\n  exit jump 0x4\n";
    let b = parse(text, &files()).unwrap();
    assert_eq!(b.to_string(), text);
    let out = run_block(&b, [3u128 << 100 | 7, 0, 0, 0]);
    assert_eq!(out, 3u128 << 105 | 0xe0);
}

#[test]
fn parse_rejects_bad_lane_text() {
    let head = "block 0x0 len 4 regs t\n  v0:i128 = get a\n  v1:i128 = get b\n";
    let tail = "  exit jump 0x4\n";
    for bad in [
        "  v2:i128 = lane.nonsense.16 v0, v1\n",
        "  v2:i128 = lane.add v0, v1\n",
        "  v2:i128 = lane.add.12 v0, v1\n",
        "  v2:i128 = lane.add.16.x v0, v1\n",
        "  v2:i128 = lane.add.16 v0\n",
        "  v2:i128 = lane.shli.16 v0\n",
        "  v2:i128 = lane.shli.16 v0, 16\n",
        "  v2:i128 = lane.bcast.16 v0, 8\n",
        "  v2:i128 = lane.and.16 v0, v1\n",
        "  v2:i128 = lane.addsatu.64 v0, v1\n",
        "  v2:i64 = lane.add.16 v0, v1\n",
        "  v2:i128 = lane.perm v0\n",
        "  v2:i128 = lane.perm2 v0, 0x1\n",
        "  v2:i64 = lane.perm v0, 0x1\n",
        "  v2:i128 = lane.add.16 v0, v3\n",
    ] {
        assert!(
            parse(&format!("{head}{bad}{tail}"), &files()).is_err(),
            "{bad}"
        );
    }
}

fn check(op: Op, w: Option<Width>) -> Result<(), String> {
    let block = Block {
        regs: file(),
        addr_width: Width::W64,
        pc: 0,
        len: 4,
        insts: {
            let mut v = vec![
                relift_ir::Inst {
                    op: Op::Get(Reg(0)),
                    width: Some(Width::W128),
                },
                relift_ir::Inst {
                    op: Op::Get(Reg(3)),
                    width: Some(Width::W64),
                },
            ];
            v.push(relift_ir::Inst { op, width: w });
            v
        },
        exit: Exit {
            flow: Flow::Jump,
            next: Next::Const(4),
        },
        bank: None,
    };
    block.verify().map_err(|e| e.to_string())
}

fn lane_op(kind: LaneKind, lane: LaneWidth, imm: u8, args: &[u32]) -> Op {
    let args: Vec<Value> = args.iter().map(|&i| Value(i)).collect();
    Op::Lane(LaneOp::with_imm(kind, lane, imm), FpArgs::new(&args))
}

#[test]
fn verifier_accepts_and_rejects() {
    use LaneKind::*;
    use LaneWidth::*;
    let w128 = Some(Width::W128);
    assert!(check(lane_op(Add, L16, 0, &[0, 0]), w128).is_ok());
    assert!(check(lane_op(Add, L16, 0, &[0, 0]), Some(Width::W64)).is_err());
    assert!(check(lane_op(Add, L16, 0, &[0, 0]), None).is_err());
    assert!(check(lane_op(Add, L16, 0, &[0, 1]), w128).is_err());
    assert!(check(lane_op(Add, L16, 0, &[0]), w128).is_err());
    assert!(check(lane_op(Add, L16, 1, &[0, 0]), w128).is_err());
    assert!(check(lane_op(AddSatS, L32, 0, &[0, 0]), w128).is_err());
    assert!(check(lane_op(MulLo, L8, 0, &[0, 0]), w128).is_err());
    assert!(check(lane_op(And, L16, 0, &[0, 0]), w128).is_err());
    assert!(check(lane_op(And, L64, 0, &[0, 0]), w128).is_ok());
    assert!(check(lane_op(ShlImm, L16, 15, &[0]), w128).is_ok());
    assert!(check(lane_op(ShlImm, L16, 16, &[0]), w128).is_err());
    assert!(check(lane_op(Broadcast, L16, 7, &[0]), w128).is_ok());
    assert!(check(lane_op(Broadcast, L16, 8, &[0]), w128).is_err());
    assert!(check(lane_op(Extract, L64, 1, &[0]), Some(Width::W64)).is_ok());
    assert!(check(lane_op(Extract, L64, 2, &[0]), Some(Width::W64)).is_err());
    assert!(check(lane_op(Extract, L64, 1, &[0]), w128).is_err());
    assert!(check(lane_op(Extract, L16, 1, &[0]), Some(Width::W64)).is_err());
    assert!(check(lane_op(Insert, L64, 1, &[0, 1]), w128).is_ok());
    assert!(check(lane_op(Insert, L32, 1, &[0, 1]), w128).is_err());
    assert!(check(lane_op(Splat, L64, 0, &[1]), w128).is_ok());
    assert!(check(lane_op(Splat, L64, 0, &[0]), w128).is_err());
    assert!(check(lane_op(Select, L64, 0, &[0, 0, 0]), w128).is_ok());
    assert!(check(lane_op(WidenLoS, L64, 0, &[0]), w128).is_err());
    assert!(check(lane_op(NarrowTrunc, L8, 0, &[0, 0]), w128).is_err());
    let perm = |a: u32, b: Option<u32>| {
        Op::Permute(Box::new(Permute {
            pattern: [0; 16],
            a: Value(a),
            b: b.map(Value),
        }))
    };
    assert!(check(perm(0, None), w128).is_ok());
    assert!(check(perm(0, Some(0)), w128).is_ok());
    assert!(check(perm(1, None), w128).is_err());
    assert!(check(perm(0, Some(1)), w128).is_err());
    assert!(check(perm(0, None), Some(Width::W64)).is_err());
}

#[test]
fn lane_ops_are_pure_and_cse_safe() {
    let op = lane_op(LaneKind::Add, LaneWidth::L16, 0, &[0, 0]);
    assert!(!op.has_effect());
    let same = lane_op(LaneKind::Add, LaneWidth::L16, 0, &[0, 0]);
    assert_eq!(op, same);
    let mut h = std::collections::HashSet::new();
    h.insert(op.clone());
    assert!(h.contains(&same));
    assert!(!h.contains(&lane_op(LaneKind::Add, LaneWidth::L32, 0, &[0, 0])));
    assert!(std::mem::size_of::<Op>() <= 56);
}

#[test]
fn dce_drops_unused_lane_ops_and_simplify_keeps_used_ones() {
    let mut b = Builder::new(file(), Width::W64, 0);
    let x = b.get(Reg(0));
    let y = b.get(Reg(1));
    let _dead = b.lane2(LaneKind::Add, LaneWidth::L16, x, y);
    let _dead_perm = b.permute(x, [0; 16]);
    let live = b.lane2(LaneKind::MulLo, LaneWidth::L16, x, y);
    let block = finish(b, live);
    let kept = relift_ir::opt::dce(&block);
    assert!(kept.verify().is_ok());
    let text = kept.to_string();
    assert!(!text.contains("lane.add"), "{text}");
    assert!(!text.contains("lane.perm"), "{text}");
    assert!(text.contains("lane.mul.16"), "{text}");
    let regs = [
        0x0003_0002_0001_0004u128 | 5u128 << 112,
        7u128 << 16 | 3,
        0,
        0,
    ];
    assert_eq!(
        run_block(&relift_ir::opt::simplify(&block), regs),
        run_block(&block, regs)
    );
}

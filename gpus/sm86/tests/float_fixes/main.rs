//! Value tests for the float ALU: synthetic programs translated for a host
//! preset and run by a SPIR-V interpreter that plays a device's float
//! behavior. Expected bits were measured with CUDA on an SM86 GPU.

mod eval;

use eval::{Device, FLUSH, PRESERVE};
use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::{HostProfile, Options, translate_with};

const OUT: u64 = 0x1000;

fn word(op: u32, form: u32, fields: &[(u32, u32, u64)]) -> u128 {
    let mut w: u128 =
        u128::from(op) | (u128::from(form) << 9) | (7u128 << 12) | (7u128 << 110) | (7u128 << 113);
    for &(lo, len, v) in fields {
        let m = ((1u128 << len) - 1) << lo;
        w = (w & !m) | ((u128::from(v) << lo) & m);
    }
    w
}

fn mov(dst: u64, v: u32) -> u128 {
    word(
        0x002,
        4,
        &[(16, 8, dst), (32, 32, u64::from(v)), (72, 4, 15)],
    )
}

fn stg(reg: u64, off: u64) -> u128 {
    word(
        0x186,
        4,
        &[
            (24, 8, 2),
            (32, 8, reg),
            (40, 24, off),
            (64, 8, 4),
            (72, 8, 0x19),
            (80, 8, 0x10),
            (88, 8, 0x0c),
        ],
    )
}

/// Float ALU modifiers in the encodings of FADD (0x021), FMUL (0x020) and
/// FFMA (0x023).
#[derive(Clone, Copy, Default)]
struct M {
    rnd: u64,
    ftz: bool,
    sat: bool,
    na: bool,
    nb: bool,
    nc: bool,
}

const RN: M = M {
    rnd: 0,
    ftz: false,
    sat: false,
    na: false,
    nb: false,
    nc: false,
};

impl M {
    fn rnd(self, r: u64) -> M {
        M { rnd: r, ..self }
    }
    fn ftz(self) -> M {
        M { ftz: true, ..self }
    }
    fn sat(self) -> M {
        M { sat: true, ..self }
    }
}

fn alu(op: u32, m: M) -> u128 {
    let mut f = vec![
        (16, 8, 14),
        (24, 8, 4),
        (32, 8, 5),
        (72, 1, u64::from(m.na)),
        (63, 1, u64::from(m.nb)),
        (77, 1, u64::from(m.sat)),
        (78, 2, m.rnd),
        (80, 1, u64::from(m.ftz)),
    ];
    if op == 0x023 {
        f.extend([(64, 8, 6), (75, 1, u64::from(m.nc))]);
    }
    if op == 0x020 {
        f.push((84, 3, 4));
    }
    word(op, 1, &f)
}

fn fadd(m: M) -> u128 {
    alu(0x021, m)
}

fn fmul(m: M) -> u128 {
    alu(0x020, m)
}

fn ffma(m: M) -> u128 {
    alu(0x023, m)
}

/// FMNMX R14, R4, R5, !PT (the maximum) or PT (the minimum).
fn fmnmx(min: bool, ftz: bool) -> u128 {
    word(
        0x009,
        1,
        &[
            (16, 8, 14),
            (24, 8, 4),
            (32, 8, 5),
            (87, 3, 7),
            (90, 1, u64::from(!min)),
            (80, 1, u64::from(ftz)),
        ],
    )
}

/// `inst` on R4 = a, R5 = b, R6 = c, its R14 stored at [`OUT`].
fn program(inst: u128, a: u32, b: u32, c: u32) -> Program {
    let body = [
        mov(2, OUT as u32),
        mov(3, 0),
        mov(4, a),
        mov(5, b),
        mov(6, c),
        inst,
        stg(14, 0),
        word(0x14d, 4, &[(87, 3, 7)]),
        word(0x147, 4, &[(34, 48, 0xffff_ffff_fffc), (87, 3, 7)]),
    ];
    let bytes: Vec<u8> = body.iter().flat_map(|w| w.to_le_bytes()).collect();
    Program::parse(&bytes)
}

fn host(h: HostProfile) -> Options {
    Options {
        host: h,
        ..Options::default()
    }
}

fn eval(o: &Options, dev: Device, inst: u128, a: u32, b: u32, c: u32) -> u32 {
    let p = program(inst, a, b, c);
    let s = translate_with(&p, o).unwrap_or_else(|e| panic!("{e}"));
    let r = eval::run(&s.spirv, dev);
    *r.mem.get(&OUT).expect("no store")
}

/// Checks `(a, b, c, want)` cases of `inst` for the host and the devices
/// that may run it.
fn check(name: &str, o: &Options, devs: &[Device], inst: u128, cases: &[(u32, u32, u32, u32)]) {
    let mut bad = Vec::new();
    for dev in devs {
        for &(a, b, c, want) in cases {
            let got = eval(o, *dev, inst, a, b, c);
            if got != want {
                bad.push(format!(
                    "{name} {} {dev:?}: a={a:08x} b={b:08x} c={c:08x} want {want:08x} got {got:08x}",
                    o.host.name
                ));
            }
        }
    }
    assert!(bad.is_empty(), "{}", bad.join("\n"));
}

/// The devices a preset may run on: as promised, preserving where the
/// translator asks for DenormPreserve, or anything for a host that promises
/// no denormal mode; all with a GLSL `Fma` that rounds twice.
fn devices(h: &HostProfile) -> Vec<Device> {
    use relift_gpu_sm86::translate::Denorm;
    let unfused = |d: Device| Device { fused: false, ..d };
    let flush: Vec<Device> = (0..4)
        .map(|k| Device {
            flush_ldexp: k & 1 != 0,
            flush_cmp: k & 2 != 0,
            ..unfused(FLUSH)
        })
        .collect();
    match h.denorm {
        Denorm::Flush => flush,
        Denorm::Unknown if h.denorm_preserve_mode => vec![unfused(PRESERVE)],
        Denorm::Preserve => vec![unfused(PRESERVE)],
        Denorm::Unknown => [&flush[..], &[unfused(PRESERVE)]].concat(),
    }
}

#[test]
fn interpreter_runs_a_plain_add() {
    let o = host(HostProfile::lavapipe());
    let one = 1.0f32.to_bits();
    let two = 2.0f32.to_bits();
    check(
        "FADD",
        &o,
        &[PRESERVE, FLUSH],
        fadd(RN),
        &[(one, two, 0, 3.0f32.to_bits())],
    );
}

/// Directed FADD with operands more than 126 binades apart: the smaller one
/// only breaks the tie, and scaling it must not leave it subnormal where the
/// device flushes. A denormal under `.FTZ` must not break it.
#[test]
fn directed_fadd_keeps_a_far_smaller_operand() {
    type Row = (u32, u32, ([u32; 4], [u32; 4]));
    let same = |w: [u32; 4]| (w, w);
    let rows: [Row; 9] = [
        (
            0x4480_0000,
            0x0380_0000,
            same([0x4480_0000, 0x4480_0000, 0x4480_0001, 0x4480_0000]),
        ),
        (
            0x4480_0000,
            0x8380_0000,
            same([0x4480_0000, 0x447f_ffff, 0x4480_0000, 0x447f_ffff]),
        ),
        (
            0xc480_0000,
            0x0380_0000,
            same([0xc480_0000, 0xc480_0000, 0xc47f_ffff, 0xc47f_ffff]),
        ),
        (
            0x4000_0000,
            0x0080_0000,
            same([0x4000_0000, 0x4000_0000, 0x4000_0001, 0x4000_0000]),
        ),
        (
            0x4000_0000,
            0x8080_0000,
            same([0x4000_0000, 0x3fff_ffff, 0x4000_0000, 0x3fff_ffff]),
        ),
        (
            0x7f00_0000,
            0x0080_0000,
            same([0x7f00_0000, 0x7f00_0000, 0x7f00_0001, 0x7f00_0000]),
        ),
        (
            0x3f80_0000,
            0x0080_0001,
            same([0x3f80_0000, 0x3f80_0000, 0x3f80_0001, 0x3f80_0000]),
        ),
        (
            0x4480_0000,
            0x0000_0001,
            (
                [0x4480_0000, 0x4480_0000, 0x4480_0001, 0x4480_0000],
                [0x4480_0000; 4],
            ),
        ),
        (
            0x4480_0000,
            0x8040_0000,
            (
                [0x4480_0000, 0x447f_ffff, 0x4480_0000, 0x447f_ffff],
                [0x4480_0000; 4],
            ),
        ),
    ];
    for h in HostProfile::presets() {
        let o = host(h.clone());
        for ftz in [false, true] {
            for rnd in 0..4 {
                let m = if ftz { RN.rnd(rnd).ftz() } else { RN.rnd(rnd) };
                let cases: Vec<_> = rows
                    .iter()
                    .map(|&(a, b, (w, wf))| (a, b, 0, if ftz { wf } else { w }[rnd as usize]))
                    .collect();
                check("FADD", &o, &devices(&h), fadd(m), &cases);
            }
        }
    }
}

/// Every rounding mode, with and without `.FTZ`, with and without `.SAT`.
fn modes() -> Vec<M> {
    let mut v = Vec::new();
    for sat in [false, true] {
        for ftz in [false, true] {
            for rnd in 0..4 {
                let m = RN.rnd(rnd);
                let m = if ftz { m.ftz() } else { m };
                v.push(if sat { m.sat() } else { m });
            }
        }
    }
    v
}

/// The devices of [`devices`], each also with a host NaN of `0x7fc00000`.
fn nan_devices(h: &HostProfile) -> Vec<Device> {
    let mut v = devices(h);
    let c: Vec<Device> = v
        .iter()
        .map(|d| Device {
            canonical_nan: true,
            ..*d
        })
        .collect();
    v.extend(c);
    v
}

/// A NaN from FADD, FMUL or FFMA is `0x7fffffff` whatever its inputs, and
/// `.SAT` makes it `+0`.
#[test]
fn arithmetic_nans_are_canonical() {
    type Row = (u32, u32, u32, u32, u32);
    let fadd_rows: [Row; 8] = [
        (0x7fa0_0000, 0x3f80_0000, 0x3f80_0000, 0x7fff_ffff, 0),
        (0x3f80_0000, 0xffc0_0001, 0x3f80_0000, 0x7fff_ffff, 0),
        (0x7f80_0000, 0xff80_0000, 0x3f80_0000, 0x7fff_ffff, 0),
        (
            0x7f80_0000,
            0x0000_0000,
            0x3f80_0000,
            0x7f80_0000,
            0x3f80_0000,
        ),
        (
            0x3f80_0000,
            0x3f80_0000,
            0xff80_0001,
            0x4000_0000,
            0x3f80_0000,
        ),
        (
            0x7f80_0000,
            0x3f80_0000,
            0xff80_0000,
            0x7f80_0000,
            0x3f80_0000,
        ),
        (0xffc1_2345, 0x7f81_2345, 0x7fc0_0000, 0x7fff_ffff, 0),
        (
            0x0000_0000,
            0x7f80_0000,
            0x7fc0_0000,
            0x7f80_0000,
            0x3f80_0000,
        ),
    ];
    let fmul_rows: [Row; 8] = [
        (0x7fa0_0000, 0x3f80_0000, 0x3f80_0000, 0x7fff_ffff, 0),
        (0x3f80_0000, 0xffc0_0001, 0x3f80_0000, 0x7fff_ffff, 0),
        (0x7f80_0000, 0xff80_0000, 0x3f80_0000, 0xff80_0000, 0),
        (0x7f80_0000, 0x0000_0000, 0x3f80_0000, 0x7fff_ffff, 0),
        (
            0x3f80_0000,
            0x3f80_0000,
            0xff80_0001,
            0x3f80_0000,
            0x3f80_0000,
        ),
        (
            0x7f80_0000,
            0x3f80_0000,
            0xff80_0000,
            0x7f80_0000,
            0x3f80_0000,
        ),
        (0xffc1_2345, 0x7f81_2345, 0x7fc0_0000, 0x7fff_ffff, 0),
        (0x0000_0000, 0x7f80_0000, 0x7fc0_0000, 0x7fff_ffff, 0),
    ];
    let ffma_rows: [Row; 8] = [
        (0x7fa0_0000, 0x3f80_0000, 0x3f80_0000, 0x7fff_ffff, 0),
        (0x3f80_0000, 0xffc0_0001, 0x3f80_0000, 0x7fff_ffff, 0),
        (0x7f80_0000, 0xff80_0000, 0x3f80_0000, 0xff80_0000, 0),
        (0x7f80_0000, 0x0000_0000, 0x3f80_0000, 0x7fff_ffff, 0),
        (0x3f80_0000, 0x3f80_0000, 0xff80_0001, 0x7fff_ffff, 0),
        (0x7f80_0000, 0x3f80_0000, 0xff80_0000, 0x7fff_ffff, 0),
        (0xffc1_2345, 0x7f81_2345, 0x7fc0_0000, 0x7fff_ffff, 0),
        (0x0000_0000, 0x7f80_0000, 0x7fc0_0000, 0x7fff_ffff, 0),
    ];
    for h in HostProfile::presets() {
        let o = host(h.clone());
        let devs = nan_devices(&h);
        for m in modes() {
            let pick = |rows: &[Row]| -> Vec<(u32, u32, u32, u32)> {
                rows.iter()
                    .map(|&(a, b, c, w, ws)| (a, b, c, if m.sat { ws } else { w }))
                    .collect()
            };
            check("FADD", &o, &devs, fadd(m), &pick(&fadd_rows));
            check("FMUL", &o, &devs, fmul(m), &pick(&fmul_rows));
            check("FFMA", &o, &devs, ffma(m), &pick(&ffma_rows));
        }
    }
}

/// FMNMX of two NaNs is `0x7fffffff`; of one NaN, the other operand.
#[test]
fn fmnmx_of_two_nans_is_canonical() {
    let rows: [(u32, u32, [u32; 2]); 5] = [
        (0x7fa0_0000, 0xffc0_0001, [0x7fff_ffff, 0x7fff_ffff]),
        (0x7fa0_0000, 0x3f80_0000, [0x3f80_0000, 0x3f80_0000]),
        (0x8000_0001, 0x7fc0_0000, [0x8000_0001, 0x8000_0000]),
        (0xffff_ffff, 0x7f80_0001, [0x7fff_ffff, 0x7fff_ffff]),
        (0x0000_0001, 0xff80_0001, [0x0000_0001, 0x0000_0000]),
    ];
    for h in HostProfile::presets() {
        let o = host(h.clone());
        let devs = nan_devices(&h);
        for ftz in [false, true] {
            let cases: Vec<_> = rows
                .iter()
                .map(|&(a, b, w)| (a, b, 0, w[usize::from(ftz)]))
                .collect();
            for min in [false, true] {
                check("FMNMX", &o, &devs, fmnmx(min, ftz), &cases);
            }
        }
    }
}

/// `.SAT` on result bits: a sign-set result (`-0`, a negative denormal) is
/// `+0`, a positive denormal stays.
#[test]
fn sat_clamps_by_bits() {
    let fadd_rows: [(u32, u32, u32, [u32; 2]); 9] = [
        (0x8000_0000, 0x8000_0000, 0, [0, 0]),
        (0x8000_0001, 0x8000_0000, 0, [0, 0]),
        (0x0000_0001, 0x0000_0000, 0, [1, 0]),
        (0x3f80_0000, 0x0000_0001, 0, [0x3f80_0000; 2]),
        (0x7f80_0000, 0x3f80_0000, 0, [0x3f80_0000; 2]),
        (0xff80_0000, 0x3f80_0000, 0, [0, 0]),
        (0x3f00_0000, 0x3f80_0000, 0, [0x3f80_0000; 2]),
        (0x3f80_0000, 0x8000_0000, 0, [0x3f80_0000; 2]),
        (0xbf80_0000, 0x3f80_0000, 0, [0, 0]),
    ];
    let mul_rows: [(u32, u32, u32, [u32; 2]); 6] = [
        (0x8000_0000, 0x8000_0000, 0x8000_0000, [0, 0]),
        (0x3f80_0000, 0x0000_0001, 0x8000_0000, [1, 0]),
        (0x3f80_0000, 0x8000_0000, 0x8000_0000, [0, 0]),
        (0xbf80_0000, 0x3f80_0000, 0, [0, 0]),
        (0x3f00_0000, 0x3f80_0000, 0x8000_0000, [0x3f00_0000; 2]),
        (0x7f80_0000, 0x3f80_0000, 0x8000_0000, [0x3f80_0000; 2]),
    ];
    for h in HostProfile::presets() {
        let o = host(h.clone());
        let devs = devices(&h);
        for ftz in [false, true] {
            for rnd in 0..4 {
                let m = RN.rnd(rnd).sat();
                let m = if ftz { m.ftz() } else { m };
                let pick = |rows: &[(u32, u32, u32, [u32; 2])]| -> Vec<(u32, u32, u32, u32)> {
                    rows.iter()
                        .map(|&(a, b, c, w)| (a, b, c, w[usize::from(ftz)]))
                        .collect()
                };
                check("FADD", &o, &devs, fadd(m), &pick(&fadd_rows));
                check("FMUL", &o, &devs, fmul(m), &pick(&mul_rows));
                check("FFMA", &o, &devs, ffma(m), &pick(&mul_rows));
            }
        }
    }
}

type Vector = (u32, u32, u32, [u32; 8]);

/// FFMA on R4, R5, R6 (`c` in R0 when measured) in the order RN, RM, RP,
/// RZ, then the same with `.FTZ`.
const FFMA_VECTORS: [Vector; 32] = [
    (
        0x3f800000,
        0x3f800000,
        0xbf800000,
        [
            0x00000000, 0x80000000, 0x00000000, 0x00000000, 0x00000000, 0x80000000, 0x00000000,
            0x00000000,
        ],
    ),
    (
        0x3f800001,
        0x3f7fffff,
        0xbf800000,
        [
            0x337ffffe, 0x337ffffe, 0x337ffffe, 0x337ffffe, 0x337ffffe, 0x337ffffe, 0x337ffffe,
            0x337ffffe,
        ],
    ),
    (
        0x3fffffff,
        0x3fffffff,
        0xc0800000,
        [
            0xb5000000, 0xb5000000, 0xb4ffffff, 0xb4ffffff, 0xb5000000, 0xb5000000, 0xb4ffffff,
            0xb4ffffff,
        ],
    ),
    (
        0x3fc00000,
        0x3fc00000,
        0x53800000,
        [
            0x53800000, 0x53800000, 0x53800001, 0x53800000, 0x53800000, 0x53800000, 0x53800001,
            0x53800000,
        ],
    ),
    (
        0x3fc00000,
        0xbfc00000,
        0x53800000,
        [
            0x53800000, 0x537fffff, 0x53800000, 0x537fffff, 0x53800000, 0x537fffff, 0x53800000,
            0x537fffff,
        ],
    ),
    (
        0x53800000,
        0x40400000,
        0x0da24260,
        [
            0x54400000, 0x54400000, 0x54400001, 0x54400000, 0x54400000, 0x54400000, 0x54400001,
            0x54400000,
        ],
    ),
    (
        0x53800000,
        0x40400000,
        0x8da24260,
        [
            0x54400000, 0x543fffff, 0x54400000, 0x543fffff, 0x54400000, 0x543fffff, 0x54400000,
            0x543fffff,
        ],
    ),
    (
        0x3f800001,
        0x3f800001,
        0x80000001,
        [
            0x3f800002, 0x3f800002, 0x3f800003, 0x3f800002, 0x3f800002, 0x3f800002, 0x3f800003,
            0x3f800002,
        ],
    ),
    (
        0x20000000,
        0x20000000,
        0x00800000,
        [
            0x01000000, 0x01000000, 0x01000000, 0x01000000, 0x01000000, 0x01000000, 0x01000000,
            0x01000000,
        ],
    ),
    (
        0x1c800000,
        0x3fe00000,
        0x80000200,
        [
            0x1ce00000, 0x1cdfffff, 0x1ce00000, 0x1cdfffff, 0x1ce00000, 0x1ce00000, 0x1ce00000,
            0x1ce00000,
        ],
    ),
    (
        0x00400000,
        0x40000000,
        0x00000001,
        [
            0x00800001, 0x00800001, 0x00800001, 0x00800001, 0x00000000, 0x00000000, 0x00000000,
            0x00000000,
        ],
    ),
    (
        0x00000003,
        0x3f000000,
        0x00000000,
        [
            0x00000002, 0x00000001, 0x00000002, 0x00000001, 0x00000000, 0x00000000, 0x00000000,
            0x00000000,
        ],
    ),
    (
        0x3f7fffff,
        0x00800001,
        0x80000000,
        [
            0x00800000, 0x00800000, 0x00800001, 0x00800000, 0x00800000, 0x00800000, 0x00800001,
            0x00800000,
        ],
    ),
    (
        0x3f7ffffe,
        0x00800001,
        0x80000000,
        [
            0x00800000, 0x007fffff, 0x00800000, 0x007fffff, 0x00800000, 0x00000000, 0x00800000,
            0x00000000,
        ],
    ),
    (
        0x3f800000,
        0x00800000,
        0x80000001,
        [
            0x007fffff, 0x007fffff, 0x007fffff, 0x007fffff, 0x00800000, 0x00800000, 0x00800000,
            0x00800000,
        ],
    ),
    (
        0x7f7fffff,
        0x40000000,
        0xff7fffff,
        [
            0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff,
            0x7f7fffff,
        ],
    ),
    (
        0x7f7fffff,
        0x3f800001,
        0x00000000,
        [
            0x7f800000, 0x7f7fffff, 0x7f800000, 0x7f7fffff, 0x7f800000, 0x7f7fffff, 0x7f800000,
            0x7f7fffff,
        ],
    ),
    (
        0xff7fffff,
        0x3f800001,
        0x00000000,
        [
            0xff800000, 0xff800000, 0xff7fffff, 0xff7fffff, 0xff800000, 0xff800000, 0xff7fffff,
            0xff7fffff,
        ],
    ),
    (
        0x3f800800,
        0x3f800800,
        0x00000000,
        [
            0x3f801000, 0x3f801000, 0x3f801001, 0x3f801000, 0x3f801000, 0x3f801000, 0x3f801001,
            0x3f801000,
        ],
    ),
    (
        0x3f800800,
        0x3f800800,
        0x80000000,
        [
            0x3f801000, 0x3f801000, 0x3f801001, 0x3f801000, 0x3f801000, 0x3f801000, 0x3f801001,
            0x3f801000,
        ],
    ),
    (
        0x80000000,
        0x3f800000,
        0x80000000,
        [
            0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000,
            0x80000000,
        ],
    ),
    (
        0x80000000,
        0x3f800000,
        0x00000000,
        [
            0x00000000, 0x80000000, 0x00000000, 0x00000000, 0x00000000, 0x80000000, 0x00000000,
            0x00000000,
        ],
    ),
    (
        0x00000000,
        0x7f7fffff,
        0x00000001,
        [
            0x00000001, 0x00000001, 0x00000001, 0x00000001, 0x00000000, 0x00000000, 0x00000000,
            0x00000000,
        ],
    ),
    (
        0x1f800001,
        0x1f800001,
        0x80800000,
        [
            0x805fffff, 0x80600000, 0x805fffff, 0x805fffff, 0x80000000, 0x80000000, 0x80000000,
            0x80000000,
        ],
    ),
    (
        0x45e5c8e7,
        0xba78a235,
        0xc9deb4ff,
        [
            0xc9deb537, 0xc9deb537, 0xc9deb536, 0xc9deb536, 0xc9deb537, 0xc9deb537, 0xc9deb536,
            0xc9deb536,
        ],
    ),
    (
        0xc6bb9985,
        0xc2f8bfe9,
        0x32d31199,
        [
            0x4a36495b, 0x4a36495b, 0x4a36495c, 0x4a36495b, 0x4a36495b, 0x4a36495b, 0x4a36495c,
            0x4a36495b,
        ],
    ),
    (
        0x33bc0a74,
        0xb71f8fb2,
        0x35c599f4,
        [
            0x35c599ed, 0x35c599ec, 0x35c599ed, 0x35c599ec, 0x35c599ed, 0x35c599ec, 0x35c599ed,
            0x35c599ec,
        ],
    ),
    (
        0x355d976d,
        0x44343e4b,
        0x39974e41,
        [
            0x3a67ab85, 0x3a67ab84, 0x3a67ab85, 0x3a67ab84, 0x3a67ab85, 0x3a67ab84, 0x3a67ab85,
            0x3a67ab84,
        ],
    ),
    (
        0xcae608e9,
        0x3e11c321,
        0xb74f08af,
        [
            0xc982fa62, 0xc982fa63, 0xc982fa62, 0xc982fa62, 0xc982fa62, 0xc982fa63, 0xc982fa62,
            0xc982fa62,
        ],
    ),
    (
        0x407c758c,
        0xb600ae19,
        0x3663051e,
        [
            0xb68c4a5e, 0xb68c4a5f, 0xb68c4a5e, 0xb68c4a5e, 0xb68c4a5e, 0xb68c4a5f, 0xb68c4a5e,
            0xb68c4a5e,
        ],
    ),
    (
        0x38a825ba,
        0xb7197534,
        0xb77024aa,
        [
            0xb77027d0, 0xb77027d1, 0xb77027d0, 0xb77027d0, 0xb77027d0, 0xb77027d1, 0xb77027d0,
            0xb77027d0,
        ],
    ),
    (
        0x47f871e6,
        0xc65866f4,
        0x38fbb861,
        [
            0xced20404, 0xced20405, 0xced20404, 0xced20404, 0xced20404, 0xced20405, 0xced20404,
            0xced20404,
        ],
    ),
];

/// FMUL on R4, R5 in the order of [`FFMA_VECTORS`]; `c` is unused.
const FMUL_VECTORS: [Vector; 32] = [
    (
        0x3f800000,
        0x3f800000,
        0xbf800000,
        [
            0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
            0x3f800000,
        ],
    ),
    (
        0x3f800001,
        0x3f7fffff,
        0xbf800000,
        [
            0x3f800000, 0x3f800000, 0x3f800001, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800001,
            0x3f800000,
        ],
    ),
    (
        0x3fffffff,
        0x3fffffff,
        0xc0800000,
        [
            0x407ffffe, 0x407ffffe, 0x407fffff, 0x407ffffe, 0x407ffffe, 0x407ffffe, 0x407fffff,
            0x407ffffe,
        ],
    ),
    (
        0x3fc00000,
        0x3fc00000,
        0x53800000,
        [
            0x40100000, 0x40100000, 0x40100000, 0x40100000, 0x40100000, 0x40100000, 0x40100000,
            0x40100000,
        ],
    ),
    (
        0x3fc00000,
        0xbfc00000,
        0x53800000,
        [
            0xc0100000, 0xc0100000, 0xc0100000, 0xc0100000, 0xc0100000, 0xc0100000, 0xc0100000,
            0xc0100000,
        ],
    ),
    (
        0x53800000,
        0x40400000,
        0x0da24260,
        [
            0x54400000, 0x54400000, 0x54400000, 0x54400000, 0x54400000, 0x54400000, 0x54400000,
            0x54400000,
        ],
    ),
    (
        0x53800000,
        0x40400000,
        0x8da24260,
        [
            0x54400000, 0x54400000, 0x54400000, 0x54400000, 0x54400000, 0x54400000, 0x54400000,
            0x54400000,
        ],
    ),
    (
        0x3f800001,
        0x3f800001,
        0x80000001,
        [
            0x3f800002, 0x3f800002, 0x3f800003, 0x3f800002, 0x3f800002, 0x3f800002, 0x3f800003,
            0x3f800002,
        ],
    ),
    (
        0x20000000,
        0x20000000,
        0x00800000,
        [
            0x00800000, 0x00800000, 0x00800000, 0x00800000, 0x00800000, 0x00800000, 0x00800000,
            0x00800000,
        ],
    ),
    (
        0x1c800000,
        0x3fe00000,
        0x80000200,
        [
            0x1ce00000, 0x1ce00000, 0x1ce00000, 0x1ce00000, 0x1ce00000, 0x1ce00000, 0x1ce00000,
            0x1ce00000,
        ],
    ),
    (
        0x00400000,
        0x40000000,
        0x00000001,
        [
            0x00800000, 0x00800000, 0x00800000, 0x00800000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000,
        ],
    ),
    (
        0x00000003,
        0x3f000000,
        0x00000000,
        [
            0x00000002, 0x00000001, 0x00000002, 0x00000001, 0x00000000, 0x00000000, 0x00000000,
            0x00000000,
        ],
    ),
    (
        0x3f7fffff,
        0x00800001,
        0x80000000,
        [
            0x00800000, 0x00800000, 0x00800001, 0x00800000, 0x00800000, 0x00800000, 0x00800001,
            0x00800000,
        ],
    ),
    (
        0x3f7ffffe,
        0x00800001,
        0x80000000,
        [
            0x00800000, 0x007fffff, 0x00800000, 0x007fffff, 0x00800000, 0x00000000, 0x00800000,
            0x00000000,
        ],
    ),
    (
        0x3f800000,
        0x00800000,
        0x80000001,
        [
            0x00800000, 0x00800000, 0x00800000, 0x00800000, 0x00800000, 0x00800000, 0x00800000,
            0x00800000,
        ],
    ),
    (
        0x7f7fffff,
        0x40000000,
        0xff7fffff,
        [
            0x7f800000, 0x7f7fffff, 0x7f800000, 0x7f7fffff, 0x7f800000, 0x7f7fffff, 0x7f800000,
            0x7f7fffff,
        ],
    ),
    (
        0x7f7fffff,
        0x3f800001,
        0x00000000,
        [
            0x7f800000, 0x7f7fffff, 0x7f800000, 0x7f7fffff, 0x7f800000, 0x7f7fffff, 0x7f800000,
            0x7f7fffff,
        ],
    ),
    (
        0xff7fffff,
        0x3f800001,
        0x00000000,
        [
            0xff800000, 0xff800000, 0xff7fffff, 0xff7fffff, 0xff800000, 0xff800000, 0xff7fffff,
            0xff7fffff,
        ],
    ),
    (
        0x3f800800,
        0x3f800800,
        0x00000000,
        [
            0x3f801000, 0x3f801000, 0x3f801001, 0x3f801000, 0x3f801000, 0x3f801000, 0x3f801001,
            0x3f801000,
        ],
    ),
    (
        0x3f800800,
        0x3f800800,
        0x80000000,
        [
            0x3f801000, 0x3f801000, 0x3f801001, 0x3f801000, 0x3f801000, 0x3f801000, 0x3f801001,
            0x3f801000,
        ],
    ),
    (
        0x80000000,
        0x3f800000,
        0x80000000,
        [
            0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000,
            0x80000000,
        ],
    ),
    (
        0x80000000,
        0x3f800000,
        0x00000000,
        [
            0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000,
            0x80000000,
        ],
    ),
    (
        0x00000000,
        0x7f7fffff,
        0x00000001,
        [
            0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000,
        ],
    ),
    (
        0x1f800001,
        0x1f800001,
        0x80800000,
        [
            0x00200001, 0x00200000, 0x00200001, 0x00200000, 0x00000000, 0x00000000, 0x00000000,
            0x00000000,
        ],
    ),
    (
        0x45e5c8e7,
        0xba78a235,
        0xc9deb4ff,
        [
            0xc0df2c38, 0xc0df2c39, 0xc0df2c38, 0xc0df2c38, 0xc0df2c38, 0xc0df2c39, 0xc0df2c38,
            0xc0df2c38,
        ],
    ),
    (
        0xc6bb9985,
        0xc2f8bfe9,
        0x32d31199,
        [
            0x4a36495b, 0x4a36495b, 0x4a36495c, 0x4a36495b, 0x4a36495b, 0x4a36495b, 0x4a36495c,
            0x4a36495b,
        ],
    ),
    (
        0x33bc0a74,
        0xb71f8fb2,
        0x35c599f4,
        [
            0xab6a6815, 0xab6a6816, 0xab6a6815, 0xab6a6815, 0xab6a6815, 0xab6a6816, 0xab6a6815,
            0xab6a6815,
        ],
    ),
    (
        0x355d976d,
        0x44343e4b,
        0x39974e41,
        [
            0x3a1c0464, 0x3a1c0464, 0x3a1c0465, 0x3a1c0464, 0x3a1c0464, 0x3a1c0464, 0x3a1c0465,
            0x3a1c0464,
        ],
    ),
    (
        0xcae608e9,
        0x3e11c321,
        0xb74f08af,
        [
            0xc982fa62, 0xc982fa63, 0xc982fa62, 0xc982fa62, 0xc982fa62, 0xc982fa63, 0xc982fa62,
            0xc982fa62,
        ],
    ),
    (
        0x407c758c,
        0xb600ae19,
        0x3663051e,
        [
            0xb6fdcced, 0xb6fdccee, 0xb6fdcced, 0xb6fdcced, 0xb6fdcced, 0xb6fdccee, 0xb6fdcced,
            0xb6fdcced,
        ],
    ),
    (
        0x38a825ba,
        0xb7197534,
        0xb77024aa,
        [
            0xb049970f, 0xb0499710, 0xb049970f, 0xb049970f, 0xb049970f, 0xb0499710, 0xb049970f,
            0xb049970f,
        ],
    ),
    (
        0x47f871e6,
        0xc65866f4,
        0x38fbb861,
        [
            0xced20404, 0xced20405, 0xced20404, 0xced20404, 0xced20404, 0xced20405, 0xced20404,
            0xced20404,
        ],
    ),
];

fn check_vectors(name: &str, o: &Options, devs: &[Device], op: fn(M) -> u128, rows: &[Vector]) {
    for ftz in [false, true] {
        for rnd in 0..4u64 {
            let m = RN.rnd(rnd);
            let m = if ftz { m.ftz() } else { m };
            let k = rnd as usize + 4 * usize::from(ftz);
            let cases: Vec<_> = rows.iter().map(|&(a, b, c, w)| (a, b, c, w[k])).collect();
            check(name, o, devs, op(m), &cases);
        }
    }
}

/// FFMA and FMUL in every mode on every preset, denormal results included.
#[test]
fn ffma_and_fmul_vectors_on_every_preset() {
    for h in HostProfile::presets() {
        let o = host(h.clone());
        let devs = devices(&h);
        check_vectors("FFMA", &o, &devs, ffma, &FFMA_VECTORS);
        check_vectors("FMUL", &o, &devs, fmul, &FMUL_VECTORS);
    }
}

/// Where the host does not promise a fused GLSL `Fma`, FFMA and the
/// directed FMUL stay exact on a device that multiplies and adds with two
/// roundings.
#[test]
fn ffma_is_exact_without_a_fused_host_fma() {
    for h in HostProfile::presets() {
        let h = HostProfile {
            fused_fma: false,
            ..h
        };
        let o = host(h.clone());
        let devs = devices(&h);
        assert!(devs.iter().all(|d| !d.fused));
        check_vectors("FFMA", &o, &devs, ffma, &FFMA_VECTORS);
        check_vectors("FMUL", &o, &devs, fmul, &FMUL_VECTORS);
    }
}

/// Only presets whose device reports `shaderFmaFloat32` use `OpFmaKHR`, and
/// the report says so.
#[test]
fn only_measured_presets_count_on_a_fused_fma() {
    let fused: Vec<_> = HostProfile::presets()
        .into_iter()
        .chain([HostProfile::minimal()])
        .filter(|h| h.fused_fma)
        .map(|h| h.name)
        .collect();
    assert_eq!(fused, ["nvidia"]);
    for h in HostProfile::presets() {
        let p = program(ffma(RN), 0, 0, 0);
        let s = translate_with(&p, &host(h.clone())).unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(s.interface.host.fma_khr, h.fused_fma, "{}", h.name);
    }
}

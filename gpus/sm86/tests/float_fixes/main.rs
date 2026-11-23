//! Value tests for the float ALU: synthetic programs translated for a host
//! preset and run by a SPIR-V interpreter that plays a device's float
//! behavior. Expected bits were measured with CUDA on an SM86 GPU.

#[path = "../common/mod.rs"]
mod common;
mod eval;

use common::translate_with;
use eval::{Device, FLUSH, PRESERVE};
use relift_gpu_nv::program::Program;
use relift_gpu_nv::translate::{self, HostProfile, Options};
use std::collections::HashSet;

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
    relift_gpu_sm86::program::parse(&bytes)
}

/// Options for host `h`, translated as for a device that imports guest
/// memory: the interpreter has no push constants, which the written-page
/// marks of a mirroring host read (`tests/mem.rs` checks those).
fn host(h: HostProfile) -> Options {
    Options {
        host: HostProfile {
            mirror_writes: false,
            ..h
        },
        ..Options::default()
    }
}

/// The module's words with the value operands of every `OpConstant` zeroed:
/// two modules with the same key differ only in constant values.
fn structure_key(spirv: &[u32]) -> Vec<u32> {
    const OP_CONSTANT: u32 = 43;
    let mut key = spirv.to_vec();
    let mut k = 5;
    while k < key.len() {
        let n = (key[k] >> 16) as usize;
        assert!(n > 0, "zero word count at word {k}");
        if key[k] & 0xffff == OP_CONSTANT {
            key[k + 3..k + n].fill(0);
        }
        k += n;
    }
    key
}

/// Checks `(a, b, c, want)` cases of `inst` for the host and the devices
/// that may run it. The operands are constants in the program, so each case
/// is translated once for all devices. `spirv-val` runs on one module per
/// distinct structure key, and every module's key is asserted validated.
fn check(name: &str, o: &Options, devs: &[Device], inst: u128, cases: &[(u32, u32, u32, u32)]) {
    let mut bad = Vec::new();
    let mut validated: HashSet<Vec<u32>> = HashSet::new();
    for &(a, b, c, want) in cases {
        let s =
            translate::translate_with(&program(inst, a, b, c), o).unwrap_or_else(|e| panic!("{e}"));
        let key = structure_key(&s.spirv);
        if !validated.contains(&key) {
            common::validate(&s.spirv);
            validated.insert(key.clone());
        }
        assert!(validated.contains(&key), "{name}: module not validated");
        for dev in devs {
            let r = eval::run(&s.spirv, *dev);
            let got = *r.mem.get(&OUT).expect("no store");
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
/// translator asks for DenormPreserve, flushing for a host whose probe found
/// only the multiplication corrections (which leave a plain flushing device
/// as it is), or anything for a host that promises no denormal mode; all with a GLSL `Fma` that rounds twice.
fn devices(h: &HostProfile) -> Vec<Device> {
    use relift_gpu_nv::translate::Denorm;
    let unfused = |d: Device| Device { fused: false, ..d };
    let flush: Vec<Device> = (0..4)
        .map(|k| Device {
            flush_ldexp: k & 1 != 0,
            flush_cmp: k & 2 != 0,
            ..unfused(FLUSH)
        })
        .collect();
    let devs = match h.denorm {
        Denorm::Flush => flush,
        Denorm::Unknown if h.flush_fix.is_some() => flush,
        Denorm::Unknown if h.denorm_preserve_mode => vec![unfused(PRESERVE)],
        Denorm::Preserve => vec![unfused(PRESERVE)],
        Denorm::Unknown => [&flush[..], &[unfused(PRESERVE)]].concat(),
    };
    devs.into_iter()
        .map(|d| Device {
            sm86_nan: h.canonical_nan,
            ..d
        })
        .collect()
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

/// Directed FADD on both sides of the operand range whose error the
/// translation takes from the operands themselves (zero, or `2^-103` up to
/// `2^126`): just inside, at, and just outside each end, with the other
/// operand near or far, and an operand just below the range whose error is
/// subnormal unscaled. Every result is normal, so the expected bits are the
/// IEEE 754 roundings, `.FTZ` or not.
#[test]
fn directed_fadd_at_the_ends_of_the_direct_error_range() {
    let rows: [(u32, u32, [u32; 4]); 12] = [
        (
            0x0c80_0000,
            0x0080_0001,
            [0x0c80_0001, 0x0c80_0000, 0x0c80_0001, 0x0c80_0000],
        ),
        (
            0x0c80_0000,
            0x8080_0001,
            [0x0c7f_ffff, 0x0c7f_fffe, 0x0c7f_ffff, 0x0c7f_fffe],
        ),
        (
            0x0c00_0000,
            0x0c00_0001,
            [0x0c80_0000, 0x0c80_0000, 0x0c80_0001, 0x0c80_0000],
        ),
        (
            0x0bff_ffff,
            0x3f80_0000,
            [0x3f80_0000, 0x3f80_0000, 0x3f80_0001, 0x3f80_0000],
        ),
        (
            0x0bff_ffff,
            0xbf80_0000,
            [0xbf80_0000, 0xbf80_0000, 0xbf7f_ffff, 0xbf7f_ffff],
        ),
        (
            0x0c00_0000,
            0x3f80_0000,
            [0x3f80_0000, 0x3f80_0000, 0x3f80_0001, 0x3f80_0000],
        ),
        (
            0x8c00_0000,
            0x3f80_0000,
            [0x3f80_0000, 0x3f7f_ffff, 0x3f80_0000, 0x3f7f_ffff],
        ),
        (
            0x7e7f_ffff,
            0x7280_0001,
            [0x7e80_0000, 0x7e80_0000, 0x7e80_0001, 0x7e80_0000],
        ),
        (
            0x7e7f_ffff,
            0xf280_0001,
            [0x7e7f_fffe, 0x7e7f_fffd, 0x7e7f_fffe, 0x7e7f_fffd],
        ),
        (
            0x7e80_0000,
            0x3f80_0000,
            [0x7e80_0000, 0x7e80_0000, 0x7e80_0001, 0x7e80_0000],
        ),
        (0x0c12_3456, 0x8c00_0001, [0x0a91_a2a8; 4]),
        (
            0x7e7f_ffff,
            0x7e7f_fffe,
            [0x7eff_fffe, 0x7eff_fffe, 0x7eff_ffff, 0x7eff_fffe],
        ),
    ];
    for h in HostProfile::presets() {
        let o = host(h.clone());
        for ftz in [false, true] {
            for rnd in 0..4 {
                let m = if ftz { RN.rnd(rnd).ftz() } else { RN.rnd(rnd) };
                let cases: Vec<_> = rows
                    .iter()
                    .flat_map(|&(a, b, w)| [(a, b, 0, w[rnd as usize]), (b, a, 0, w[rnd as usize])])
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

/// Only presets whose device capture reports `shaderFmaFloat32` use
/// `OpFmaKHR`, and the report says so.
#[test]
fn only_presets_reporting_fma_count_on_a_fused_fma() {
    let fused: Vec<_> = HostProfile::presets()
        .into_iter()
        .chain([HostProfile::minimal()])
        .filter(|h| h.fused_fma)
        .map(|h| h.name)
        .collect();
    assert_eq!(fused, ["nvidia", "radv", "amd_pro", "anv", "moltenvk"]);
    for h in HostProfile::presets() {
        let p = program(ffma(RN), 0, 0, 0);
        let s = translate_with(&p, &host(h.clone())).unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(s.interface.host.fma_khr, h.fused_fma, "{}", h.name);
    }
}

/// `(mode, operand)` of every OpExecutionMode in a module.
fn modes_of(words: &[u32]) -> Vec<(u32, u32)> {
    let mut v = Vec::new();
    let mut k = 5;
    while k < words.len() {
        let n = (words[k] >> 16) as usize;
        if words[k] & 0xffff == 16 && n >= 4 {
            v.push((words[k + 2], words[k + 3]));
        }
        k += n.max(1);
    }
    v
}

const SIGNED_ZERO_INF_NAN_PRESERVE: u32 = 4461;

/// Float programs ask for SignedZeroInfNanPreserve where the host has it and
/// are reported inexact where it does not; integer-only programs are not.
#[test]
fn float_programs_preserve_signed_zeros_infinities_and_nans() {
    let float = program(fadd(RN), 0, 0, 0);
    let int = program(mov(14, 1), 0, 0, 0);
    for h in HostProfile::presets() {
        assert!(h.signed_zero_inf_nan_preserve, "{}", h.name);
        let s = translate_with(&float, &host(h.clone())).unwrap_or_else(|e| panic!("{e}"));
        assert!(
            modes_of(&s.spirv).contains(&(SIGNED_ZERO_INF_NAN_PRESERVE, 32)),
            "{}",
            h.name
        );
        assert!(s.interface.host.inexact.is_empty(), "{}", h.name);
    }
    let h = HostProfile::minimal();
    assert!(!h.signed_zero_inf_nan_preserve);
    let lacking = HostProfile {
        signed_zero_inf_nan_preserve: false,
        ..HostProfile::nvidia()
    };
    let s = translate_with(&float, &host(lacking.clone())).unwrap_or_else(|e| panic!("{e}"));
    assert!(!modes_of(&s.spirv).contains(&(SIGNED_ZERO_INF_NAN_PRESERVE, 32)));
    assert!(
        s.interface
            .host
            .inexact
            .iter()
            .any(|l| l.contains("SignedZeroInfNanPreserve"))
    );
    let s = translate_with(&int, &host(lacking)).unwrap_or_else(|e| panic!("{e}"));
    assert!(s.interface.host.inexact.is_empty());
}

/// F2I R14, R4 from F32 to `size` (0: 8, 1: 16, 2: 32 bits), round to
/// nearest.
fn f2i(signed: bool, size: u64, ntz: bool) -> u128 {
    word(
        0x105,
        1,
        &[
            (16, 8, 14),
            (32, 8, 4),
            (72, 1, u64::from(signed)),
            (75, 2, size),
            (77, 1, u64::from(ntz)),
            (84, 3, 2),
        ],
    )
}

/// F2I of a NaN is the lowest value of the signed type of that width, not
/// sign-extended (also for unsigned types), or zero with `.NTZ`.
#[test]
fn f2i_nan_and_saturation() {
    let rows: [(u32, [u32; 12]); 8] = [
        (
            0x7fc0_0000,
            [
                0x80,
                0,
                0x80,
                0,
                0x8000,
                0,
                0x8000,
                0,
                0x8000_0000,
                0,
                0x8000_0000,
                0,
            ],
        ),
        (
            0xffc0_0000,
            [
                0x80,
                0,
                0x80,
                0,
                0x8000,
                0,
                0x8000,
                0,
                0x8000_0000,
                0,
                0x8000_0000,
                0,
            ],
        ),
        (
            0x7f80_0001,
            [
                0x80,
                0,
                0x80,
                0,
                0x8000,
                0,
                0x8000,
                0,
                0x8000_0000,
                0,
                0x8000_0000,
                0,
            ],
        ),
        (
            0x7f80_0000,
            [
                0xff,
                0xff,
                0x7f,
                0x7f,
                0xffff,
                0xffff,
                0x7fff,
                0x7fff,
                u32::MAX,
                u32::MAX,
                0x7fff_ffff,
                0x7fff_ffff,
            ],
        ),
        (
            0xff80_0000,
            [
                0,
                0,
                0xffff_ff80,
                0xffff_ff80,
                0,
                0,
                0xffff_8000,
                0xffff_8000,
                0,
                0,
                0x8000_0000,
                0x8000_0000,
            ],
        ),
        (
            0x4f80_0000,
            [
                0xff,
                0xff,
                0x7f,
                0x7f,
                0xffff,
                0xffff,
                0x7fff,
                0x7fff,
                u32::MAX,
                u32::MAX,
                0x7fff_ffff,
                0x7fff_ffff,
            ],
        ),
        (
            0xbf80_0000,
            [
                0,
                0,
                u32::MAX,
                u32::MAX,
                0,
                0,
                u32::MAX,
                u32::MAX,
                0,
                0,
                u32::MAX,
                u32::MAX,
            ],
        ),
        (0x3f80_0000, [1; 12]),
    ];
    for h in HostProfile::presets() {
        let o = host(h.clone());
        let devs = devices(&h);
        let mut k = 0;
        for size in 0..3 {
            for signed in [false, true] {
                for ntz in [false, true] {
                    let cases: Vec<_> = rows.iter().map(|&(a, w)| (a, 0, 0, w[k])).collect();
                    check("F2I", &o, &devs, f2i(signed, size, ntz), &cases);
                    k += 1;
                }
            }
        }
    }
}

/// On a host that flushes like SM86 (rounding before the tininess check),
/// `.FTZ` instructions take the host result directly and keep a product
/// that rounds up to `2^-126`; without `.FTZ` the same product is a
/// denormal built by the translator.
#[test]
fn flush_hosts_round_before_the_tininess_check() {
    let o = host(HostProfile::nvidia());
    let p = program(fmul(RN.ftz()), 0, 0, 0);
    let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{e}"));
    assert!(s.interface.host.check_flush);
    assert_eq!(s.interface.host.denorm_fixups, 0);
    let cases = [
        (0x3f7f_fffe, 0x0080_0001, 0, 0x0080_0000),
        (0x3f7f_ffff, 0x0080_0000, 0, 0),
        (0xbf7f_fffe, 0x0080_0001, 0, 0x8080_0000),
    ];
    check("FMUL.FTZ", &o, &[FLUSH], fmul(RN.ftz()), &cases);
    let cases = [
        (0x3f7f_fffe, 0x0080_0001, 0, 0x0080_0000),
        (0x3f7f_ffff, 0x0080_0000, 0, 0x0080_0000),
        (0x3f00_0000, 0x0080_0001, 0, 0x0040_0000),
    ];
    check(
        "FMUL",
        &o,
        &devices(&HostProfile::nvidia()),
        fmul(RN),
        &cases,
    );
}

/// MUFU R14, R4 of `func` (9: TANH, 4: RCP).
fn mufu(func: u64) -> u128 {
    word(0x108, 1, &[(16, 8, 14), (32, 8, 4), (74, 4, func)])
}

/// MUFU.TANH has no model of the unit, so every host reports it inexact.
#[test]
fn mufu_tanh_is_reported_inexact() {
    let tanh = program(mufu(9), 0, 0, 0);
    assert!(
        tanh.code[5]
            .as_ref()
            .is_ok_and(|i| i.to_string().starts_with("MUFU.TANH"))
    );
    for h in HostProfile::presets() {
        let s = translate_with(&tanh, &host(h.clone())).unwrap_or_else(|e| panic!("{e}"));
        let n = s
            .interface
            .host
            .inexact
            .iter()
            .filter(|l| l.contains("TANH"))
            .count();
        assert_eq!(n, 1, "{}", h.name);
    }
}

/// Operands at the edges of the float FFMA path's ranges (exponent fields
/// 86 to 88 and 189 to 190 for the factors, 23 to 25 and 252 to 253 for
/// the addend), of the normal and denormal ranges and of the significand,
/// in both signs, with infinities and NaNs.
fn ffma_edge_pool() -> Vec<u32> {
    let fields = [
        0u32, 1, 2, 23, 24, 25, 86, 87, 88, 100, 126, 127, 128, 150, 189, 190, 252, 253, 254, 255,
    ];
    let sigs = [
        0u32, 1, 0x40_0000, 0x7f_ffff, 0x7f_f000, 0x00_0fff, 0x55_5555,
    ];
    let mut v = Vec::new();
    for f in fields {
        for m in sigs {
            v.push(f << 23 | m);
            v.push(0x8000_0000 | f << 23 | m);
        }
    }
    v
}

/// Round-to-nearest FFMA with and without `.FTZ` on hosts without a fused
/// multiply-add, which take the float path, against the CPU's fused
/// multiply-add: rows from the edge pool, rows whose addend nearly cancels
/// the product and rows whose addend sits at a halfway point of it.
#[test]
fn ffma_float_path_matches_a_fused_multiply_add() {
    let pool = ffma_edge_pool();
    let mut st = 0x9e37_79b9_7f4a_7c15u64;
    let mut rnd = move || {
        st ^= st << 13;
        st ^= st >> 7;
        st ^= st << 17;
        st
    };
    let mut rows = Vec::new();
    for k in 0..900 {
        let r = rnd();
        let pick = |s: u64| pool[(s % pool.len() as u64) as usize];
        let (a, b) = (pick(r), pick(r >> 20));
        let c = match k % 3 {
            0 => pick(r >> 40),
            1 => {
                let p = f32::from_bits(a) * f32::from_bits(b);
                (-p).to_bits()
                    .wrapping_add((r >> 60) as u32)
                    .wrapping_sub(8)
            }
            _ => {
                let p = (f32::from_bits(a) * f32::from_bits(b)).to_bits();
                let e = (p >> 23 & 0xff).saturating_sub(24 + (r >> 58) as u32 % 6);
                (p & 0x8000_0000) ^ (0x8000_0000 * (r >> 63) as u32) | e << 23
            }
        };
        rows.push((a, b, c));
    }
    let fused = |a: u32, b: u32, c: u32| {
        let r = f32::from_bits(a).mul_add(f32::from_bits(b), f32::from_bits(c));
        if r.is_nan() { 0x7fff_ffff } else { r.to_bits() }
    };
    let flush = |x: u32| {
        if x & 0x7f80_0000 == 0 {
            x & 0x8000_0000
        } else {
            x
        }
    };
    let plain: Vec<_> = rows
        .iter()
        .map(|&(a, b, c)| (a, b, c, fused(a, b, c)))
        .collect();
    let ftz: Vec<_> = rows
        .iter()
        .filter_map(|&(a, b, c)| {
            let w = fused(flush(a), flush(b), flush(c));
            let tiny = w & 0x7fff_ffff != 0 && w & 0x7fff_ffff < 0x0100_0000;
            (!tiny).then_some((a, b, c, w))
        })
        .collect();
    for h in HostProfile::presets().into_iter().filter(|h| !h.fused_fma) {
        let o = host(h.clone());
        let devs = devices(&h);
        check("FFMA float path", &o, &devs, ffma(RN), &plain);
        check("FFMA.FTZ float path", &o, &devs, ffma(RN.ftz()), &ftz);
    }
}

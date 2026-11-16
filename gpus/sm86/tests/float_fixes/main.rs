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

/// The devices a preset may run on: as promised, or anything for a host
/// that promises no denormal mode.
fn devices(h: &HostProfile) -> Vec<Device> {
    use relift_gpu_sm86::translate::Denorm;
    let fused = |d: Device| Device {
        fused: h.fused_fma,
        ..d
    };
    let flush: Vec<Device> = (0..4)
        .map(|k| Device {
            flush_ldexp: k & 1 != 0,
            flush_cmp: k & 2 != 0,
            ..fused(FLUSH)
        })
        .collect();
    match h.denorm {
        Denorm::Flush => flush,
        Denorm::Preserve => vec![fused(PRESERVE)],
        Denorm::Unknown => [&flush[..], &[fused(PRESERVE)]].concat(),
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

//! Known answers from an RTX 3090: every recorded program is translated for
//! several hosts, validated, and run by a SPIR-V interpreter under each
//! denormal behavior the host may have; every output must match the device
//! bit for bit. See tests/kat/file.rs for the data and kat/README.md for how
//! it was recorded.

mod common;
#[path = "kat/file.rs"]
mod file;
#[path = "kat/spvi.rs"]
mod spvi;

use common::translate_with;
use relift_gpu_nv::translate::pseudo::{I2F_IABS, I2F_INEG, MUFU_SAT, RRO_EX2, RRO_SINCOS};
use relift_gpu_nv::translate::{
    Denorm as HostDenorm, FIX_ROUND_AFTER, FIX_ZERO_SIGN, HostProfile, Options,
};
use relift_gpu_nv::{Inst, Operand, RegMods};
use spvi::{Denorm, Env, Memory, Module, Run};
use std::collections::{HashMap, HashSet};

/// Variants the translator rejects, as it should: it cannot do them exactly
/// yet. Their vectors stay for when it learns them.
const REJECTED: &[&str] = &[
    "F2FP.BF16.PACK_AB",
    "F2FP.RELU.BF16.PACK_AB",
    "F2FP.BF16.PACK_AB.RZ",
    "F2FP.RELU.BF16.PACK_AB.RZ",
    "F2FP.SATFINITE.BF16.PACK_AB.RZ",
    "HADD2 bit 85",
    "HMUL2 bit 85",
    "HMUL2.FMZ",
    "HFMA2.FMZ",
    "HFMA2.BF16_V2",
];

/// How a known mismatch may differ from the device.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Known {
    /// Only where both are NaN: the device writes 0x7fffffff for every NaN
    /// result, the translation keeps a host NaN.
    NanBits,
}

/// Variants that translate but still differ from the device in a way a fix
/// in flight addresses. Every other difference fails, and so does an entry
/// that no longer differs, so each goes away with its fix.
const KNOWN: &[(&str, &[&str], Known)] = &[(
    "half",
    &[
        "HADD2.F32 -RZ, b",
        "HADD2.F32 -RZ, b.H0_H0",
        "HADD2.F32 -RZ, b.H1_H1",
        "HADD2.F32 -RZ, -|b|.H1_H1",
        "HADD2.F32.FTZ -RZ, b.H1_H1",
    ],
    Known::NanBits,
)];

fn known(family: &str, label: &str) -> Option<Known> {
    KNOWN
        .iter()
        .find(|k| k.0 == family && k.1.contains(&label))
        .map(|k| k.2)
}

fn is_nan(x: u32) -> bool {
    x & 0x7fff_ffff > 0x7f80_0000
}

const CBUF: u64 = 0x1_0000_0000;
const IN: u64 = 0x2_0000_0000;
const OUT: u64 = 0x3_0000_0000;

/// The hosts the programs run for: the ones with buffer device addresses
/// (the interpreter has no segment table). NVIDIA uses the MUFU model too:
/// its native path relies on the driver, which an interpreter cannot check.
fn hosts() -> Vec<HostProfile> {
    let mut v: Vec<HostProfile> = ["nvidia", "radv", "lavapipe", "adreno", "moltenvk"]
        .iter()
        .map(|n| HostProfile::preset(n).unwrap())
        .collect();
    for (f16, nan) in [(false, false), (true, false), (false, true)] {
        let mut h = HostProfile::nvidia();
        h.name = format!("nvidia, native halves {f16}, canonical NaNs {nan}");
        h.native_f16 = f16;
        h.canonical_nan = nan;
        v.push(h);
    }
    v
}

/// The denormal behaviors a host may show for a module.
fn behaviors(m: &Module, host: &HostProfile) -> Vec<(Denorm, bool)> {
    if m.modes.contains(&4459) {
        return vec![(Denorm::Preserve, false)];
    }
    if m.modes.contains(&4460) {
        return vec![(Denorm::FlushBefore, false), (Denorm::FlushAfter, false)];
    }
    if let Some(fix) = host.flush_fix {
        let mut v = Vec::new();
        if fix & FIX_ROUND_AFTER != 0 {
            v.push((Denorm::FlushAfter, fix & FIX_ZERO_SIGN != 0));
        }
        if fix & FIX_ROUND_AFTER == 0 {
            v.push((Denorm::FlushBefore, fix & FIX_ZERO_SIGN != 0));
        }
        return v;
    }
    match host.denorm {
        HostDenorm::Flush => vec![(Denorm::FlushBefore, false)],
        HostDenorm::Preserve => vec![(Denorm::Preserve, false)],
        HostDenorm::Unknown => vec![
            (Denorm::Preserve, false),
            (Denorm::FlushBefore, false),
            (Denorm::FlushAfter, false),
        ],
    }
}

fn spirv_bytes(w: &[u32]) -> Vec<u8> {
    w.iter().flat_map(|x| x.to_le_bytes()).collect()
}

/// Runs one program on one row: the wrapper reads the row from `c[0][0x160]`
/// and writes its outputs to `c[0][0x168]`, both for thread 0.
fn exec(
    m: &Module,
    behavior: (Denorm, bool),
    canonical_nan: bool,
    row: &[u32; 4],
    outs: usize,
) -> Vec<u32> {
    let mut cb = vec![0u8; 0x10000];
    for k in 0..3 {
        cb[4 * k..4 * k + 4].copy_from_slice(&1u32.to_le_bytes());
    }
    cb[0x160..0x168].copy_from_slice(&IN.to_le_bytes());
    cb[0x168..0x170].copy_from_slice(&OUT.to_le_bytes());
    let input: Vec<u8> = row.iter().flat_map(|x| x.to_le_bytes()).collect();
    let mem = Memory {
        regions: vec![(CBUF, cb), (IN, input), (OUT, vec![0; 64])],
    };
    let env = Env {
        denorm: behavior.0,
        canonical_nan,
        mul_zero_positive: behavior.1,
        uniforms: HashMap::from([((1, 0), CBUF)]),
        local_id: [0; 3],
        group_id: [0; 3],
    };
    let mut r = Run::new(m, &env, mem);
    r.main();
    (0..outs)
        .map(|k| r.mem.read(OUT + 4 * k as u64, 4) as u32)
        .collect()
}

fn check_family(name: &str) {
    let f = file::load(name);
    let mut failures = Vec::new();
    for v in &f.variants {
        let p = relift_gpu_sm86::program::parse(&v.code);
        let mut seen = HashSet::new();
        let mut rejected = None;
        let known = known(name, &v.label);
        let mut any_known = false;
        for host in hosts() {
            let o = Options {
                host: host.clone(),
                exact_mufu: true,
                ..Options::default()
            };
            let s = match translate_with(&p, &o) {
                Ok(s) => s,
                Err(e) => {
                    rejected = Some(e.to_string());
                    continue;
                }
            };
            let m = Module::parse(&s.spirv);
            for d in behaviors(&m, &host) {
                if !seen.insert((spirv_bytes(&s.spirv), d)) {
                    continue;
                }
                let mut bad = 0;
                for (i, row) in f.rows.iter().enumerate() {
                    let got = exec(&m, d, host.canonical_nan, row, v.outs);
                    let want = &v.expect[i * v.outs..(i + 1) * v.outs];
                    if got == want {
                        continue;
                    }
                    let nan_only = got
                        .iter()
                        .zip(want)
                        .all(|(g, w)| g == w || is_nan(*g) && is_nan(*w));
                    if known == Some(Known::NanBits) && nan_only {
                        any_known = true;
                        continue;
                    }
                    bad += 1;
                    if bad <= 3 {
                        failures.push(format!(
                            "{name}: {} on {} ({d:?}): in {:08x?} device {:08x?} translated {:08x?}",
                            v.label,
                            host.name,
                            &row[..f.nin],
                            want,
                            got
                        ));
                    }
                }
                if bad > 3 {
                    failures.push(format!(
                        "{name}: {} on {} ({d:?}): {bad} of {} rows differ",
                        v.label,
                        host.name,
                        f.rows.len()
                    ));
                }
            }
        }
        if known.is_some() && !any_known && rejected.is_none() {
            failures.push(format!(
                "{name}: {} now matches: drop it from KNOWN",
                v.label
            ));
        }
        match (rejected, REJECTED.contains(&v.label.as_str())) {
            (Some(e), false) => failures.push(format!("{name}: {} rejected: {e}", v.label)),
            (None, true) => failures.push(format!(
                "{name}: {} now translates: drop it from REJECTED",
                v.label
            )),
            _ => {}
        }
    }
    assert!(
        failures.is_empty(),
        "{} failures:\n{}",
        failures.len(),
        failures.join("\n")
    );
}

/// Maxwell's `MUFU` with `.SAT`, a pseudo-instruction in place of the
/// `MUFU` word of each recorded single-precision variant: every output
/// must be the device's result clamped to [0, 1] with NaN and the negative
/// sign to +0, the saturation of every float result.
#[test]
fn maxwell_mufu_sat_clamps_the_device_result() {
    let sat = |x: u32| {
        if is_nan(x) || x & 0x8000_0000 != 0 {
            0
        } else if x >= 0x3f80_0000 {
            0x3f80_0000
        } else {
            x
        }
    };
    let host = HostProfile::preset("lavapipe").unwrap();
    let o = Options {
        host: host.clone(),
        exact_mufu: true,
        ..Options::default()
    };
    let mut checked = 0;
    for name in [
        "mufu_rcp",
        "mufu_rsq",
        "mufu_sqrt",
        "mufu_ex2",
        "mufu_lg2",
        "mufu_sin",
        "mufu_cos",
    ] {
        let f = file::load(name);
        for v in f.variants.iter().filter(|v| !v.label.contains("F16")) {
            let mut p = relift_gpu_sm86::program::parse(&v.code);
            let decoded = p.code.clone();
            let Some(k) = decoded
                .iter()
                .position(|d| d.as_ref().is_ok_and(|i| i.mnemonic == "MUFU"))
            else {
                continue;
            };
            let mut i = decoded[k].clone().unwrap();
            i.mods.push(MUFU_SAT);
            p.code[k] = Ok(i);
            let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{name} {}: {e}", v.label));
            let m = Module::parse(&s.spirv);
            for d in behaviors(&m, &host) {
                for (r, row) in f.rows.iter().enumerate() {
                    let got = exec(&m, d, host.canonical_nan, row, v.outs);
                    let want: Vec<u32> = v.expect[r * v.outs..(r + 1) * v.outs]
                        .iter()
                        .map(|x| sat(*x))
                        .collect();
                    assert_eq!(got, want, "{name} {} row {row:08x?}", v.label);
                }
            }
            checked += 1;
        }
    }
    assert!(checked >= 7, "{checked} variants");
}

/// Maxwell's `RRO.SINCOS` and `RRO.EX2`, pseudo-instructions in place of
/// the `MUFU` word of the recorded `MUFU.SIN` and `MUFU.EX2` programs, on
/// their input rows: `.SINCOS` gives the operand in turns (times
/// `1 / (2 pi)`, rounded toward zero, denormals kept) and `.EX2` the
/// operand itself, with the operand's negation and absolute value.
#[test]
fn maxwell_rro_gives_the_mufu_operand() {
    let rz = |x: u32| {
        let p = f64::from(f32::from_bits(x)) * f64::from(f32::from_bits(0x3e22_f983));
        let mut r = p as f32;
        if f64::from(r).abs() > p.abs() {
            r = f32::from_bits(r.to_bits() - 1);
        }
        r.to_bits()
    };
    let host = HostProfile::preset("lavapipe").unwrap();
    let o = Options {
        host: host.clone(),
        ..Options::default()
    };
    for (name, kind) in [("mufu_sin", RRO_SINCOS), ("mufu_ex2", RRO_EX2)] {
        let f = file::load(name);
        let v = f
            .variants
            .iter()
            .find(|v| !v.label.contains("F16"))
            .expect("a variant");
        let mut p = relift_gpu_sm86::program::parse(&v.code);
        let decoded = p.code.clone();
        let k = decoded
            .iter()
            .position(|d| d.as_ref().is_ok_and(|i| i.mnemonic == "MUFU"))
            .expect("a MUFU");
        let mufu = decoded[k].clone().unwrap();
        let src = match &mufu.ops[1] {
            Operand::Reg(n, _) => *n,
            o => panic!("MUFU operand {o}"),
        };
        for (neg, abs) in [(false, false), (true, false), (false, true)] {
            let mut i = Inst::new("RRO");
            i.mods.push(kind);
            i.ops.push(mufu.ops[0].clone());
            i.ops.push(Operand::Reg(
                src,
                RegMods {
                    neg,
                    abs,
                    ..RegMods::default()
                },
            ));
            p.code[k] = Ok(i);
            let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{name}: {e}"));
            let m = Module::parse(&s.spirv);
            let mut n = 0;
            for row in f.rows.iter().filter(|r| !is_nan(r[0])) {
                let mut x = row[0];
                if abs {
                    x &= 0x7fff_ffff;
                }
                if neg {
                    x ^= 0x8000_0000;
                }
                let want = if kind == RRO_SINCOS { rz(x) } else { x };
                let got = exec(
                    &m,
                    (Denorm::Preserve, false),
                    host.canonical_nan,
                    row,
                    v.outs,
                );
                assert_eq!(
                    got[0], want,
                    "{name} {kind} -{neg} |{abs}| in {:08x}",
                    row[0]
                );
                n += 1;
            }
            assert!(n > 100, "{name}: {n} rows");
        }
    }
}

/// Maxwell's `I2F` with an integer `.ABS` or `.NEG`, pseudo-instructions in
/// place of the `I2F` word of the recorded 32-bit round-to-nearest
/// variants: the device's results equal Rust's conversion on every row, and
/// the pseudo-instruction's results equal that conversion of the source's
/// wrapping absolute value, then its wrapping negation.
#[test]
fn maxwell_i2f_converts_the_integer_abs_and_neg() {
    let host = HostProfile::preset("lavapipe").unwrap();
    let o = Options {
        host: host.clone(),
        ..Options::default()
    };
    let f = file::load("i2f");
    let mut checked = 0;
    for label in ["I2F.F32.S32.RN", "I2F.F32.U32.RN"] {
        let signed = label.contains("S32");
        let cvt = |x: u32| {
            if signed {
                (x as i32 as f32).to_bits()
            } else {
                (x as f32).to_bits()
            }
        };
        let v = f
            .variants
            .iter()
            .find(|v| v.label == label)
            .unwrap_or_else(|| panic!("{label} recorded"));
        for (r, row) in f.rows.iter().enumerate() {
            assert_eq!(
                v.expect[r * v.outs],
                cvt(row[0]),
                "{label} device {:08x}",
                row[0]
            );
        }
        let mut p = relift_gpu_sm86::program::parse(&v.code);
        let decoded = p.code.clone();
        let k = decoded
            .iter()
            .position(|d| d.as_ref().is_ok_and(|i| i.mnemonic == "I2F"))
            .expect("an I2F");
        let plain = decoded[k].clone().unwrap();
        for (abs, neg) in [(true, false), (false, true), (true, true)] {
            let mut i = plain.clone();
            if abs {
                i.mods.push(I2F_IABS);
            }
            if neg {
                i.mods.push(I2F_INEG);
            }
            p.code[k] = Ok(i);
            let s = translate_with(&p, &o).unwrap_or_else(|e| panic!("{label}: {e}"));
            let m = Module::parse(&s.spirv);
            for row in &f.rows {
                let mut x = row[0];
                if abs {
                    x = (x as i32).wrapping_abs() as u32;
                }
                if neg {
                    x = x.wrapping_neg();
                }
                let got = exec(
                    &m,
                    (Denorm::Preserve, false),
                    host.canonical_nan,
                    row,
                    v.outs,
                );
                assert_eq!(got[0], cvt(x), "{label} |{abs}| -{neg} in {:08x}", row[0]);
            }
            checked += 1;
        }
    }
    assert_eq!(checked, 6);
}

macro_rules! families {
    ($($n:ident),*) => {
        $(#[test] fn $n() { check_family(stringify!($n)); })*

        #[test]
        fn every_file_has_a_test() {
            let listed: Vec<&str> = vec![$(stringify!($n)),*];
            for n in file::names() {
                assert!(listed.contains(&n.as_str()), "tests/kat/{n}.kat has no test");
            }
        }
    };
}

families!(
    shf, fadd, fmul, ffma, fcmp, f2i, f2i_f16, i2f, f2f, frnd, f2fp, half, int, mufu_f16, mufu_64h,
    mufu_rcp, mufu_rsq, mufu_sqrt, mufu_ex2, mufu_lg2, mufu_sin, mufu_cos
);

/// A pseudo-random 32-bit sequence (xorshift32).
fn xorshift(x: &mut u32) -> u32 {
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    *x
}

/// Operand pairs at the edge of the normal range: random bits, denormals and
/// the smallest normals, and pairs whose product lands just below the
/// smallest normal number (a quarter with the mantissas that put it in the
/// window where rounding before and after the flush differ).
fn edge_rows(n: usize) -> Vec<[u32; 4]> {
    let mut x = 0x2545_f491;
    let mut rows = vec![
        [0x00ff_ffff, 0x3f00_0000, 0, 0],
        [0x3f00_0000, 0x00ff_ffff, 0, 0],
        [0x0000_0000, 0xbf80_0000, 0, 0],
        [0x8000_0000, 0x3f80_0000, 0, 0],
        [0x0000_0001, 0xbf80_0000, 0, 0],
        [0x0080_0000, 0xbf00_0000, 0, 0],
    ];
    for _ in 0..n {
        let (kind, r1, r2, r3) = (
            xorshift(&mut x) % 4,
            xorshift(&mut x),
            xorshift(&mut x),
            xorshift(&mut x),
        );
        let (mut a, mut b) = (r1, r2);
        if kind == 1 {
            a = (a & 0x807f_ffff) | (r3 % 4) << 23;
            b = (b & 0x807f_ffff) | ((r3 >> 4) % 4) << 23;
        } else if kind >= 2 {
            let ea = 1 + r3 % 127;
            let eb = (128 - ea as i32 + (r3 >> 8) as i32 % 5 - 2).max(0) as u32;
            a = (a & 0x807f_ffff) | ea << 23;
            b = (b & 0x807f_ffff) | eb << 23;
            if kind == 3 {
                a |= 0x007f_ffff;
                b &= 0xff80_0000;
            }
            if r3 >> 12 & 1 != 0 {
                std::mem::swap(&mut a, &mut b);
            }
        }
        rows.push([a, b, 0, 0]);
    }
    rows
}

/// A host whose probe found only corrections of the flushing model gets the
/// results SM86 gives, on the device it is: the translation for the NVIDIA
/// preset (which flushes as SM86 does) is the reference, and the corrected
/// translation, run on a device with that host's deviation, must equal it
/// on every `.FTZ` multiplication and addition row, edge and random.
#[test]
fn probed_multiplication_fixes_give_the_flushing_results() {
    let rows = edge_rows(20_000);
    for (name, family) in [("fmul", "FMUL.FTZ.RN"), ("fadd", "FADD.FTZ.RN")] {
        let f = file::load(name);
        let v = f.variants.iter().find(|v| v.label == family).unwrap();
        let p = relift_gpu_sm86::program::parse(&v.code);
        let reference = {
            let o = Options {
                host: HostProfile::nvidia(),
                exact_mufu: true,
                ..Options::default()
            };
            let s = translate_with(&p, &o).unwrap();
            let m = Module::parse(&s.spirv);
            rows.iter()
                .map(|r| exec(&m, (Denorm::FlushBefore, false), true, r, v.outs))
                .collect::<Vec<_>>()
        };
        for host in ["adreno", "moltenvk"] {
            let h = HostProfile::preset(host).unwrap();
            let o = Options {
                host: h.clone(),
                exact_mufu: true,
                ..Options::default()
            };
            let s = translate_with(&p, &o).unwrap();
            let m = Module::parse(&s.spirv);
            let behavior = behaviors(&m, &h);
            assert_eq!(behavior.len(), 1, "{host}");
            let mut bad = 0;
            for (r, want) in rows.iter().zip(&reference) {
                let got = exec(&m, behavior[0], h.canonical_nan, r, v.outs);
                let same = got
                    .iter()
                    .zip(want)
                    .all(|(g, w)| g == w || is_nan(*g) && is_nan(*w));
                if !same {
                    bad += 1;
                    assert!(bad < 4, "{name} {host}: more than 3 rows differ");
                    eprintln!("{name} {host}: in {r:08x?} want {want:08x?} got {got:08x?}");
                }
            }
            assert_eq!(bad, 0, "{name} {host}");
        }
    }
}

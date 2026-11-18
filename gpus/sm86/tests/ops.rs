//! Instructions added for full corpus coverage: each synthetic word decodes
//! to the expected text and its program translates.

use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::{HostProfile, Options, WarpPath, translate, translate_with};

const EXIT: u128 = 0x000f_ea00_0380_0000_0000_0000_0000_794d;
const BRA_SELF: u128 = 0x000f_c000_0383_ffff_ffff_fff0_0000_7947;

fn program(body: &[u128]) -> Program {
    let mut b = Vec::new();
    for w in body.iter().chain(&[EXIT, BRA_SELF]) {
        b.extend(w.to_le_bytes());
    }
    Program::parse(&b)
}

/// A word from its two halves, without operand reuse flags.
fn w(hi: u64, lo: u64) -> u128 {
    ((u128::from(hi) << 64) | u128::from(lo)) & !(0xf << 122)
}

fn check(words: &[u128], text: &[&str]) -> Vec<u32> {
    let p = program(words);
    let shown: Vec<String> = p
        .decode()
        .into_iter()
        .map(|d| d.map(|i| i.to_string()).unwrap_or_default())
        .collect();
    for (n, t) in text.iter().enumerate() {
        assert!(
            shown[n].starts_with(t),
            "decoded {:?}, wanted {t:?}",
            shown[n]
        );
    }
    let s = translate(&p).unwrap_or_else(|e| panic!("{text:?}: {e}"));
    assert_eq!(s.spirv[0], 0x0723_0203);
    s.spirv
}

#[test]
fn fmnmx_nan() {
    check(
        &[
            w(0x0c1f_e400_0782_0000, 0x0000_0007_0611_7209),
            w(0x000f_e200_0782_0000, 0x3f80_0000_0607_7809),
        ],
        &["FMNMX.NAN R17, R6, R7, !PT", "FMNMX.NAN R7, R6, 1, !PT"],
    );
}

#[test]
fn f2ip() {
    check(
        &[
            w(0x1c0f_e200_0000_0408, 0x0000_0007_060b_7243),
            w(0x0c4f_e200_0000_d508, 0x0000_0007_060d_7243),
            w(0x000f_e200_0000_04ff, 0x0000_0006_ff05_7243),
        ],
        &[
            "F2IP.U8.F32.NTZ R11, R6, R7, R8",
            "F2IP.S8.F32.TRUNC.NTZ R13, R6, R7, R8.H1",
            "F2IP.U8.F32.NTZ R5, RZ, R6, RZ",
        ],
    );
}

#[test]
fn idp() {
    check(
        &[
            w(0x1c0f_e400_0000_1008, 0x0000_0007_060f_7226),
            w(0x000f_c400_0000_3608, 0x0000_0007_0615_7226),
            w(0x008f_c800_0000_0209, 0x0000_0007_061b_7226),
            w(0x000f_c600_0000_0408, 0x0000_0007_061f_7226),
        ],
        &[
            "IDP.2A.LO.U16.U8 R15, R6, R7, R8",
            "IDP.2A.HI.S16.S8 R21, R6, R7, R8",
            "IDP.4A.S8.U8 R27, R6, R7, R9",
            "IDP.4A.U8.S8 R31, R6, R7, R8",
        ],
    );
}

#[test]
fn mov_quad_lane_mask() {
    check(
        &[w(0x008f_ca00_0000_0500, 0x0000_0008_0009_7202)],
        &["MOV R9, R8, 0x5"],
    );
}

#[test]
fn suquery_compression_status() {
    check(
        &[w(0x002f_6200_000e_0000, 0x8000_5800_0407_7bb3)],
        &["SUQUERY.2D_ARRAY R7, [R4], COMP_STATUS, 0x0, 0x58"],
    );
}

#[test]
fn ldgsts() {
    check(
        &[
            w(0x0003_e800_0810_1c44, 0x0000_0000_0213_7fae),
            w(0x0003_e800_0b92_1a44, 0x0002_0008_0213_7fae),
            w(0x0003_e800_0b92_1844, 0x0002_8014_0213_7fae),
            w(0x000f_e800_0894_1c44, 0x0003_0000_1013_7fae),
            w(0x000e_2200_0000_0000, 0x0000_0000_0000_79af),
        ],
        &[
            "LDGSTS.E.BYPASS.128 [R19], [R2.64], P0",
            "LDGSTS.E.64 [R19+0x20], [R2.64+0x8]",
            "LDGSTS.E [R19+0x28], [R2.64+0x14]",
            "LDGSTS.E.BYPASS.128.ZFILL [R19+0x30], [R16.64], P1",
            "LDGDEPBAR",
        ],
    );
}

/// The warp path a warp-wide program takes on an 8-wide host.
fn narrow_warp_path(words: &[u128]) -> WarpPath {
    let opts = Options {
        host: HostProfile::lavapipe(),
        ..Options::default()
    };
    let s = translate_with(&program(words), &opts).unwrap_or_else(|e| panic!("{e}"));
    assert!(s.interface.host.inexact.is_empty());
    s.interface.host.warp
}

#[test]
fn movm_plans_the_warp() {
    let movm = w(0x0040_6200_0000_0000, 0x0000_0000_020d_723a);
    check(&[movm], &["MOVM.16.MT88 R13, R2"]);
    assert_eq!(narrow_warp_path(&[movm]), WarpPath::Shared);
}

#[test]
fn hmma_f16() {
    let words = [
        w(0x0c4f_f000_0000_080a, 0x0000_0008_040c_723c),
        w(0x000f_ee00_0000_08ff, 0x0000_0008_040e_723c),
        w(0x000f_e200_0000_0006, 0x0000_0009_0408_723c),
    ];
    let spirv = check(
        &words,
        &[
            "HMMA.16816.F16 R12, R4, R8, R10",
            "HMMA.16816.F16 R14, R4, R8, RZ",
            "HMMA.1688.F16 R8, R4, R9, R6",
        ],
    );
    let calls = |spirv: &[u32]| spirv_ops(spirv).iter().filter(|&&o| o == 57).count();
    assert!(HostProfile::nvidia().native_f16);
    assert_eq!(
        calls(&spirv),
        8 + 8 + 4 + 1,
        "steps, and the float step's call of the decoded one"
    );
    let mut host = HostProfile::nvidia();
    host.native_f16 = false;
    let opts = Options {
        host,
        ..Options::default()
    };
    let s = translate_with(&program(&words), &opts).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(calls(&s.spirv), 8 + 8 + 4);
    assert_eq!(narrow_warp_path(&words), WarpPath::Shared);
}

fn spirv_ops(words: &[u32]) -> Vec<u32> {
    let mut ops = Vec::new();
    let mut k = 5;
    while k < words.len() {
        ops.push(words[k] & 0xffff);
        k += ((words[k] >> 16) as usize).max(1);
    }
    ops
}

#[test]
fn ldc_segmented_reads_the_next_bank() {
    let p = program(&[w(0x000e_2200_0000_8800, 0x0080_0000_0607_7b82)]);
    let shown = p.decode()[0].as_ref().map(|i| i.to_string()).unwrap();
    assert!(shown.starts_with("LDC.IS R7, c[0x2][R6]"), "{shown}");
    let s = translate(&p).unwrap_or_else(|e| panic!("{e}"));
    assert!(s.interface.cbufs.contains(&2) && s.interface.cbufs.contains(&3));
}

#[test]
fn double_precision() {
    check(
        &[
            w(0x040e_2200_0390_f000, 0x0000_0004_1e00_722a),
            w(0x1c0e_a200_0000_8006, 0x0000_0004_1e1c_722b),
            w(0x000f_e200_03f0_6200, 0x0000_0008_0600_722a),
            w(0x001f_e200_0030_5000, 0x0000_0006_0022_7310),
            w(0x010e_6200_0020_1800, 0x0000_0004_001c_7310),
            w(0x000f_e200_0000_0000, 0x4690_0000_1e10_7828),
            w(0x000f_c600_0000_4808, 0x0000_0000_0624_7229),
            w(0x001e_0600_0000_0306, 0x0000_0008_1e1e_722b),
        ],
        &[
            "DSETP.MAX.AND P0, P1, R30, R4, PT",
            "DFMA.RP R28, R30, R4, R6",
            "DSETP.GE.AND P0, PT, |R6|, R8, PT",
            "F2F.F32.F64.RM R34, R6",
            "F2F.F64.F32 R28, R4",
            "DMUL R16, R30, 8.11296384146066816958e+31",
            "DADD.RM R36, R6, -R8",
            "DFMA R30, -|R30|, R8, R6",
        ],
    );
}

//! Instructions added for full corpus coverage: each synthetic word decodes
//! to the expected text and its program translates.

use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::translate;

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

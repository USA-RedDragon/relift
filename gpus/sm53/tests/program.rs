use relift_gpu_nv::program::Stage;
use relift_gpu_sm53::program::{CODE_HEADER_LEN, CODE_MAGIC, SPH_LEN};
use relift_gpu_sm53::{ParseError, Program};

const EXIT: u64 = 0xe300_0000_0007_000f;
const NOP: u64 = 0x50b0_0000_0007_0f00;
const BRA: u64 = 0xe240_0000_0007_000f;
const SSY: u64 = 0xe290_0000_0000_0000;
/// EXIT under predicate P0.
const EXIT_P0: u64 = EXIT & !(0xf << 16);

/// A branch at code offset `at` to `to`.
fn branch(base: u64, at: u64, to: u64) -> u64 {
    let imm = (to as i64 - at as i64 - 8) as u64 & 0xff_ffff;
    base | imm << 20
}

/// A program: code header, an SPH whose first word is `sph0` (none when 0),
/// then the bundles (scheduling word 0, then three instructions each).
fn program(sph0: u32, bundles: &[[u64; 3]]) -> Vec<u8> {
    let mut b = CODE_MAGIC.to_le_bytes().to_vec();
    b.resize(CODE_HEADER_LEN, 0);
    if sph0 != 0 {
        b.extend(sph0.to_le_bytes());
        b.resize(CODE_HEADER_LEN + SPH_LEN, 0);
    }
    for i in bundles {
        b.extend(0u64.to_le_bytes());
        for w in i {
            b.extend(w.to_le_bytes());
        }
    }
    b
}

/// SPH word 0: type 1 (or 2 for pixel), version 3, the stage in bits 10 to 13.
fn sph0(stage: u32) -> u32 {
    let kind = if stage == 5 { 2 } else { 1 };
    kind | 3 << 5 | stage << 10
}

#[test]
fn ends_at_the_first_exit() {
    let b = program(sph0(1), &[[NOP, EXIT, NOP], [NOP, NOP, NOP]]);
    let p = Program::parse(&b).unwrap();
    assert_eq!(p.stage(), Stage::Vertex);
    assert_eq!(p.code.len(), 32);
    assert!(!p.indirect);
}

#[test]
fn follows_branches_past_a_predicated_exit() {
    let b = program(
        sph0(5),
        &[
            [branch(BRA & !(0xf << 16), 8, 0x48), EXIT_P0, NOP],
            [NOP, EXIT, NOP],
            [NOP, EXIT, NOP],
        ],
    );
    let p = Program::parse(&b).unwrap();
    assert_eq!(p.stage(), Stage::Fragment);
    assert_eq!(p.code.len(), 96);
}

#[test]
fn reaches_an_ssy_target_after_the_exit() {
    let b = program(
        sph0(1),
        &[[branch(SSY, 8, 0x28), EXIT, NOP], [EXIT, NOP, NOP]],
    );
    assert_eq!(Program::parse(&b).unwrap().code.len(), 64);
}

#[test]
fn a_target_on_a_scheduling_word_runs_the_bundle() {
    let b = program(
        sph0(1),
        &[
            [branch(BRA & !(0xf << 16), 8, 32), EXIT, NOP],
            [EXIT, NOP, NOP],
        ],
    );
    assert_eq!(Program::parse(&b).unwrap().code.len(), 64);
}

#[test]
fn bra_to_itself_ends_a_path() {
    let b = program(sph0(1), &[[NOP, branch(BRA, 16, 16), NOP]]);
    assert_eq!(Program::parse(&b).unwrap().code.len(), 32);
}

#[test]
fn without_an_sph_it_is_compute() {
    let b = program(0, &[[NOP, NOP, EXIT]]);
    let p = Program::parse(&b).unwrap();
    assert_eq!(p.stage(), Stage::Compute);
    assert!(p.sph.is_none());
}

#[test]
fn reports_what_stops_it() {
    assert_eq!(Program::parse(&[0; 64]), Err(ParseError::NoHeader));
    let b = program(sph0(1), &[[NOP, NOP, NOP]]);
    assert_eq!(Program::parse(&b), Err(ParseError::OutOfBounds { at: 40 }));
    let b = program(sph0(1), &[[branch(BRA, 8, 12), NOP, NOP]]);
    assert_eq!(
        Program::parse(&b),
        Err(ParseError::BadTarget { at: 8, target: 12 })
    );
    let b = program(sph0(1), &[[u64::MAX, EXIT, NOP]]);
    assert!(matches!(
        Program::parse(&b),
        Err(ParseError::Unknown { at: 8, .. }) | Ok(_)
    ));
}

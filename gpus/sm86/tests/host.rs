use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::{Denorm, HostProfile, Options, translate_with};

fn word(op: u32, form: u32, fields: &[(u32, u32, u64)]) -> [u8; 16] {
    let mut w: u128 =
        u128::from(op) | (u128::from(form) << 9) | (7u128 << 12) | (7u128 << 110) | (7u128 << 113);
    for &(lo, len, v) in fields {
        let m = ((1u128 << len) - 1) << lo;
        w = (w & !m) | ((u128::from(v) << lo) & m);
    }
    w.to_le_bytes()
}

fn exit_and_halt(code: &mut Vec<u8>) {
    code.extend(word(0x14d, 4, &[(87, 3, 7)]));
    code.extend(word(0x147, 4, &[(34, 48, 0xffff_ffff_fffc), (87, 3, 7)]));
}

fn fadd(ftz: bool) -> Program {
    let mut b = Vec::new();
    b.extend(word(
        0x021,
        1,
        &[(16, 8, 2), (24, 8, 0), (32, 8, 1), (80, 1, u64::from(ftz))],
    ));
    exit_and_halt(&mut b);
    Program::parse(&b)
}

fn count(words: &[u32], opcode: u32) -> usize {
    let mut k = 5;
    let mut n = 0;
    while k < words.len() {
        let len = (words[k] >> 16) as usize;
        if words[k] & 0xffff == opcode {
            n += 1;
        }
        k += len.max(1);
    }
    n
}

const FUNCTION_CALL: u32 = 57;
const EXECUTION_MODE: u32 = 16;

#[test]
fn presets_have_distinct_names() {
    let p = HostProfile::presets();
    let mut names: Vec<_> = p.iter().map(|h| h.name).collect();
    names.sort_unstable();
    names.dedup();
    assert_eq!(names.len(), p.len());
    for h in &p {
        assert_eq!(HostProfile::preset(h.name).as_ref(), Some(h));
    }
    assert!(HostProfile::preset("minimal").is_some());
}

#[test]
fn fadd_is_native_where_the_host_matches_the_guest() {
    let ftz = fadd(true);
    assert!(ftz.decode()[0].as_ref().is_ok_and(|i| i.has("FTZ")));
    let nv = Options {
        host: HostProfile::nvidia(),
        ..Options::default()
    };
    let s = translate_with(&ftz, &nv).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(s.interface.host.denorm_fixups, 0);
    assert!(s.interface.host.check_flush);
    assert_eq!(count(&s.spirv, FUNCTION_CALL), 0);
    let s = translate_with(&fadd(false), &nv).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(s.interface.host.denorm_fixups, 1);
    assert_eq!(count(&s.spirv, FUNCTION_CALL), 1);
}

#[test]
fn preserving_hosts_ask_for_denormals_and_flush_ftz_inputs() {
    let radv = Options {
        host: HostProfile::radv(),
        ..Options::default()
    };
    let s = translate_with(&fadd(false), &radv).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(s.interface.host.denorm_mode, Some(Denorm::Preserve));
    assert_eq!(s.interface.host.denorm_fixups, 0);
    assert!(count(&s.spirv, EXECUTION_MODE) >= 2);
    let s = translate_with(&fadd(true), &radv).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(s.interface.host.denorm_fixups, 1);
}

#[test]
fn unknown_hosts_repair_both_forms() {
    let mvk = Options {
        host: HostProfile::moltenvk(),
        ..Options::default()
    };
    for ftz in [false, true] {
        let s = translate_with(&fadd(ftz), &mvk).unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(s.interface.host.denorm_mode, None);
        assert_eq!(s.interface.host.denorm_fixups, 1);
        assert_eq!(s.interface.host.host, "moltenvk");
    }
}

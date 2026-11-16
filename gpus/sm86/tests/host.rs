use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::{
    ClockPath, Denorm, HostProfile, Options, WarpPath, translate_with,
};

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

fn spirv_ops(words: &[u32]) -> Vec<u32> {
    let mut out = Vec::new();
    let mut k = 5;
    while k < words.len() {
        out.push(words[k] & 0xffff);
        k += ((words[k] >> 16) as usize).max(1);
    }
    out
}

fn warp_program(guarded_exit: bool) -> Program {
    let mut b = Vec::new();
    if guarded_exit {
        b.extend(word(0x14d, 4, &[(12, 3, 0), (87, 3, 7)]));
    }
    b.extend(word(
        0x189,
        1,
        &[
            (16, 8, 2),
            (24, 8, 3),
            (32, 8, 4),
            (58, 2, 3),
            (64, 8, 5),
            (81, 3, 7),
        ],
    ));
    b.extend(word(
        0x006,
        1,
        &[(16, 8, 6), (72, 2, 1), (81, 3, 7), (87, 3, 7)],
    ));
    b.extend(word(0x3c4, 1, &[(16, 8, 7), (24, 8, 2), (78, 3, 4)]));
    exit_and_halt(&mut b);
    Program::parse(&b)
}

fn on(p: &Program, host: HostProfile) -> relift_gpu_sm86::translate::Shader {
    translate_with(
        p,
        &Options {
            host,
            ..Options::default()
        },
    )
    .unwrap_or_else(|e| panic!("{e}"))
}

const CONTROL_BARRIER: u32 = 224;
const GROUP_SHUFFLE: u32 = 345;
const GROUP_BALLOT: u32 = 339;
const VECTOR_EXTRACT_DYNAMIC: u32 = 77;
const READ_CLOCK: u32 = 5056;

#[test]
fn warp_paths_follow_the_host() {
    let p = warp_program(false);
    let s = on(&p, HostProfile::nvidia());
    assert_eq!(s.interface.host.warp, WarpPath::Subgroup);
    assert!(!s.interface.host.check_warp32);
    assert_eq!(s.interface.host.warp_ops, 3);
    let s = on(&p, HostProfile::moltenvk());
    assert_eq!(s.interface.host.warp, WarpPath::Subgroup);
    assert!(s.interface.host.check_warp32);
    let s = on(&p, HostProfile::adreno());
    assert_eq!(s.interface.host.warp, WarpPath::Cluster);
    assert!(spirv_ops(&s.spirv).contains(&VECTOR_EXTRACT_DYNAMIC));
    for h in [
        HostProfile::lavapipe(),
        HostProfile::mali(),
        HostProfile::minimal(),
    ] {
        let s = on(&p, h);
        assert_eq!(s.interface.host.warp, WarpPath::Shared);
        assert!(s.interface.host.inexact.is_empty());
        let ops = spirv_ops(&s.spirv);
        assert!(!ops.contains(&GROUP_SHUFFLE));
        assert!(!ops.contains(&GROUP_BALLOT));
        assert!(ops.iter().filter(|&&o| o == CONTROL_BARRIER).count() >= 6);
    }
}

#[test]
fn divergent_warp_operations_are_reported() {
    let p = warp_program(true);
    assert!(p.decode()[0].as_ref().is_ok_and(|i| i.guard.is_some()));
    let s = on(&p, HostProfile::lavapipe());
    assert_eq!(s.interface.host.warp, WarpPath::Host);
    assert_eq!(s.interface.host.inexact.len(), 1);
    let s = on(&p, HostProfile::nvidia());
    assert_eq!(s.interface.host.warp, WarpPath::Subgroup);
    assert!(s.interface.host.inexact.is_empty());
}

#[test]
fn clock_without_a_host_clock_counts() {
    let mut b = Vec::new();
    b.extend(word(0x805, 1, &[(16, 8, 2), (72, 8, 0x50)]));
    exit_and_halt(&mut b);
    let p = Program::parse(&b);
    let s = on(&p, HostProfile::nvidia());
    assert_eq!(s.interface.host.clock, ClockPath::Device);
    assert!(spirv_ops(&s.spirv).contains(&READ_CLOCK));
    let s = on(&p, HostProfile::moltenvk());
    assert_eq!(s.interface.host.clock, ClockPath::Counter);
    assert!(!spirv_ops(&s.spirv).contains(&READ_CLOCK));
    assert!(s.interface.cbufs.contains(&0));
}

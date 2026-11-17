mod common;

use common::translate_with;
use relift_gpu_sm86::program::Program;
use relift_gpu_sm86::translate::{
    Atomic64Path, ClockPath, Denorm, GlobalPath, HostProfile, Options, WarpPath,
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
    let mut names: Vec<_> = p.iter().map(|h| h.name.as_str()).collect();
    names.sort_unstable();
    names.dedup();
    assert_eq!(names.len(), p.len());
    for h in &p {
        assert_eq!(HostProfile::preset(&h.name).as_ref(), Some(h));
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
const GROUP_ALL_EQUAL: u32 = 336;
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
fn r2ur_with_a_predicate_plans_a_warp_path() {
    let mut b = Vec::new();
    b.extend(word(0x1c2, 1, &[(16, 6, 4), (24, 8, 5), (81, 3, 0)]));
    exit_and_halt(&mut b);
    let p = Program::parse(&b);
    assert_eq!(text_of(&p)[0], "R2UR P0, UR4, R5");
    let s = on(&p, HostProfile::nvidia());
    assert_eq!(s.interface.host.warp, WarpPath::Subgroup);
    assert_eq!(s.interface.host.warp_ops, 1);
    let s = on(&p, HostProfile::minimal());
    assert_eq!(s.interface.host.warp, WarpPath::Shared);
    assert!(!spirv_ops(&s.spirv).contains(&GROUP_ALL_EQUAL));
}

#[test]
fn ldsm_alone_plans_a_warp_path() {
    let mut b = Vec::new();
    b.extend(word(
        0x83b,
        4,
        &[
            (16, 8, 8),
            (24, 8, 4),
            (40, 24, 0x40),
            (72, 2, 2),
            (78, 2, 0),
        ],
    ));
    exit_and_halt(&mut b);
    let p = Program::parse(&b);
    assert!(text_of(&p)[0].starts_with("LDSM"), "{:?}", text_of(&p));
    let s = on(&p, HostProfile::nvidia());
    assert_eq!(s.interface.host.warp, WarpPath::Subgroup);
    assert_eq!(s.interface.host.warp_ops, 1);
    let s = on(&p, HostProfile::minimal());
    assert_eq!(s.interface.host.warp, WarpPath::Shared);
    assert!(!spirv_ops(&s.spirv).contains(&GROUP_SHUFFLE));
}

const LOOP_MERGE: u32 = 246;
const CAPABILITY: u32 = 17;
const CAP_GROUP_NON_UNIFORM: u32 = 61;
const DECORATE: u32 = 71;

fn has_cap(words: &[u32], cap: u32) -> bool {
    let mut k = 5;
    while k < words.len() {
        let len = ((words[k] >> 16) as usize).max(1);
        if words[k] & 0xffff == CAPABILITY && words.get(k + 1) == Some(&cap) {
            return true;
        }
        k += len;
    }
    false
}

/// The id decorated as the `WorkgroupSize` built-in.
fn workgroup_size_id(words: &[u32]) -> Option<u32> {
    let mut k = 5;
    while k < words.len() {
        let len = ((words[k] >> 16) as usize).max(1);
        if words[k] & 0xffff == DECORATE && words[k + 2] == 11 && words[k + 3] == 25 {
            return Some(words[k + 1]);
        }
        k += len;
    }
    None
}

#[test]
fn divergent_warp_operations_run_in_software() {
    let p = warp_program(true);
    assert!(p.decode()[0].as_ref().is_ok_and(|i| i.guard.is_some()));
    for h in [
        HostProfile::lavapipe(),
        HostProfile::mali(),
        HostProfile::minimal(),
    ] {
        let s = on(&p, h);
        assert_eq!(s.interface.host.warp, WarpPath::Simt);
        assert!(s.interface.host.inexact.is_empty());
        let ops = spirv_ops(&s.spirv);
        assert!(!ops.contains(&GROUP_SHUFFLE));
        assert!(!ops.contains(&GROUP_BALLOT));
        assert!(!ops.contains(&CONTROL_BARRIER));
        assert!(!has_cap(&s.spirv, CAP_GROUP_NON_UNIFORM));
        assert!(workgroup_size_id(&s.spirv).is_some());
    }
    let s = on(&p, HostProfile::nvidia());
    assert_eq!(s.interface.host.warp, WarpPath::Subgroup);
    assert!(s.interface.host.inexact.is_empty());
}

/// A shuffle, then a vote that only some lanes reach, in a loop that some
/// lanes go round: `@P0 BRA` over the vote and `@P1 BRA` back to the top.
fn divergent_loop() -> Program {
    let mut b = Vec::new();
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
    b.extend(word(0x147, 4, &[(12, 3, 0), (34, 48, 4), (87, 3, 7)]));
    b.extend(word(
        0x006,
        1,
        &[(16, 8, 6), (72, 2, 1), (81, 3, 7), (87, 3, 7)],
    ));
    b.extend(word(
        0x147,
        4,
        &[(12, 3, 1), (34, 48, 0xffff_ffff_fff0), (87, 3, 7)],
    ));
    exit_and_halt(&mut b);
    Program::parse(&b)
}

#[test]
fn warp_operations_in_a_divergent_loop() {
    let p = divergent_loop();
    let s = on(&p, HostProfile::nvidia());
    assert_eq!(s.interface.host.warp, WarpPath::Subgroup);
    let s = on(&p, HostProfile::lavapipe());
    assert_eq!(s.interface.host.warp, WarpPath::Simt);
    assert!(s.interface.host.inexact.is_empty());
    let ops = spirv_ops(&s.spirv);
    assert!(!ops.contains(&GROUP_SHUFFLE));
    assert!(ops.iter().filter(|&&o| o == LOOP_MERGE).count() >= 4);
}

fn execution_modes(words: &[u32]) -> Vec<u32> {
    let mut out = Vec::new();
    let mut k = 5;
    while k < words.len() {
        if words[k] & 0xffff == EXECUTION_MODE {
            out.push(words[k + 2]);
        }
        k += ((words[k] >> 16) as usize).max(1);
    }
    out
}

const MAXIMALLY_RECONVERGES: u32 = 6023;

/// BRA.DIV over a SHFL to a VOTE: the fast path a compiler emits for a
/// shuffle that may run in a diverged warp.
fn fast_path_program() -> Program {
    let mut b = Vec::new();
    b.extend(word(0x147, 4, &[(32, 2, 2), (34, 48, 4), (87, 3, 7)]));
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
    exit_and_halt(&mut b);
    Program::parse(&b)
}

#[test]
fn fast_paths_ask_for_maximal_reconvergence() {
    let p = fast_path_program();
    assert!(text_of(&p)[0].starts_with("BRA.DIV"), "{:?}", text_of(&p));
    let s = on(&p, HostProfile::nvidia());
    assert_eq!(s.interface.host.warp, WarpPath::Subgroup);
    assert!(s.interface.host.max_reconvergence);
    assert!(execution_modes(&s.spirv).contains(&MAXIMALLY_RECONVERGES));
    assert!(s.interface.host.inexact.is_empty());
    let lacking = HostProfile {
        maximal_reconvergence: false,
        ..HostProfile::nvidia()
    };
    let s = on(&p, lacking);
    assert!(!s.interface.host.max_reconvergence);
    assert!(!execution_modes(&s.spirv).contains(&MAXIMALLY_RECONVERGES));
    assert_eq!(s.interface.host.inexact.len(), 1);
    assert!(s.interface.host.inexact[0].contains("reconvergence"));
    let s = on(&warp_program(true), HostProfile::nvidia());
    assert!(s.interface.host.max_reconvergence);
    let s = on(&warp_program(false), HostProfile::nvidia());
    assert!(!s.interface.host.max_reconvergence);
    assert!(!execution_modes(&s.spirv).contains(&MAXIMALLY_RECONVERGES));
}

fn capabilities(words: &[u32]) -> Vec<u32> {
    let mut out = Vec::new();
    let mut k = 5;
    while k < words.len() {
        if words[k] & 0xffff == 17 {
            out.push(words[k + 1]);
        }
        k += ((words[k] >> 16) as usize).max(1);
    }
    out
}

const GROUP_NON_UNIFORM_CAPS: std::ops::RangeInclusive<u32> = 61..=68;

#[test]
fn subgroup_capabilities_follow_the_operations_emitted() {
    let mut b = Vec::new();
    b.extend(word(0x119, 1, &[(16, 8, 2), (72, 8, 0x21)]));
    exit_and_halt(&mut b);
    let s = on(&Program::parse(&b), HostProfile::nvidia());
    let caps = capabilities(&s.spirv);
    assert!(
        !caps.iter().any(|c| GROUP_NON_UNIFORM_CAPS.contains(c)),
        "S2R SR_TID.X: {caps:?}"
    );
    let s = on(&warp_program(false), HostProfile::lavapipe());
    assert_eq!(s.interface.host.warp, WarpPath::Shared);
    let caps = capabilities(&s.spirv);
    assert!(
        !caps.iter().any(|c| GROUP_NON_UNIFORM_CAPS.contains(c)),
        "shared path: {caps:?}"
    );
    let s = on(&warp_program(false), HostProfile::nvidia());
    let caps = capabilities(&s.spirv);
    for c in [61, 62, 63, 64, 65] {
        assert!(caps.contains(&c), "subgroup path lacks {c}: {caps:?}");
    }
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

fn global_program() -> Program {
    let mut b = Vec::new();
    b.extend(word(
        0x181,
        1,
        &[
            (16, 8, 6),
            (24, 8, 4),
            (40, 24, 8),
            (72, 1, 1),
            (73, 3, 5),
            (81, 3, 7),
        ],
    ));
    b.extend(word(
        0x1a8,
        1,
        &[
            (16, 8, 8),
            (24, 8, 4),
            (32, 8, 2),
            (40, 24, 16),
            (72, 1, 1),
            (73, 3, 2),
            (81, 3, 7),
            (87, 4, 0),
        ],
    ));
    exit_and_halt(&mut b);
    Program::parse(&b)
}

const ATOMIC_COMPARE_EXCHANGE: u32 = 230;
const ATOMIC_I_ADD: u32 = 234;
const CONVERT_U_TO_PTR: u32 = 120;

#[test]
fn global_memory_without_device_addresses_uses_segments() {
    let p = global_program();
    let t = text_of(&p);
    assert!(
        t[0].starts_with("LDG.E") && t[1].contains("ATOMG.E.ADD"),
        "{t:?}"
    );
    let s = on(&p, HostProfile::lavapipe());
    assert_eq!(s.interface.host.global, GlobalPath::Bda);
    assert_eq!(s.interface.host.atomics64, Atomic64Path::None);
    assert!(s.interface.global_memory);
    let ops = spirv_ops(&s.spirv);
    assert!(ops.contains(&CONVERT_U_TO_PTR));
    assert!(ops.contains(&ATOMIC_I_ADD));
    let s = on(&p, HostProfile::minimal());
    assert_eq!(s.interface.host.global, GlobalPath::Segments { shift: 27 });
    assert_eq!(s.interface.host.atomics64, Atomic64Path::Locked);
    assert!(!s.interface.global_memory);
    let ops = spirv_ops(&s.spirv);
    assert!(!ops.contains(&CONVERT_U_TO_PTR));
    assert!(!ops.contains(&ATOMIC_I_ADD));
    assert!(ops.contains(&ATOMIC_COMPARE_EXCHANGE));
}

fn text_of(p: &Program) -> Vec<String> {
    p.decode()
        .into_iter()
        .map(|d| d.map(|i| i.to_string()).unwrap_or_default())
        .collect()
}

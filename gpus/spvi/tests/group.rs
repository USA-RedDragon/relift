//! Workgroup runs (`relift_gpu_spvi::group`): barriers, shared memory and
//! subgroup shuffles across invocations, and the report of a runaway loop.

use std::collections::HashMap;

use relift_gpu_spvi::group::{Note, run_workgroup};
use relift_gpu_spvi::{Denorm, Env, Memory};

/// glslang (SPIR-V 1.5), 64 invocations: `s[i] = i * 3 + 1`, a barrier,
/// `v = s[(i + 1) % 64]`, then `out[i] = subgroupShuffle(v, lane ^ 1)`;
/// `out` is the storage buffer at set 0, binding 0.
const WG: &[u32] = &[
    0x07230203, 0x00010500, 0x000d000b, 0x00000031, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x0000003d, 0x00020011, 0x00000041, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0009000f, 0x00000005, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000a, 0x0000000f, 0x00000024, 0x00000029, 0x00060010, 0x00000004, 0x00000011,
    0x00000040, 0x00000001, 0x00000001, 0x00040047, 0x0000000a, 0x0000000b, 0x0000001d, 0x00040047,
    0x00000021, 0x00000006, 0x00000004, 0x00030047, 0x00000022, 0x00000002, 0x00050048, 0x00000022,
    0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x00000024, 0x00000021, 0x00000000, 0x00040047,
    0x00000024, 0x00000022, 0x00000000, 0x00030047, 0x00000029, 0x00000000, 0x00040047, 0x00000029,
    0x0000000b, 0x00000029, 0x00030047, 0x0000002a, 0x00000000, 0x00030047, 0x0000002b, 0x00000000,
    0x00040047, 0x00000030, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040020, 0x00000009, 0x00000001,
    0x00000006, 0x0004003b, 0x00000009, 0x0000000a, 0x00000001, 0x0004002b, 0x00000006, 0x0000000c,
    0x00000040, 0x0004001c, 0x0000000d, 0x00000006, 0x0000000c, 0x00040020, 0x0000000e, 0x00000004,
    0x0000000d, 0x0004003b, 0x0000000e, 0x0000000f, 0x00000004, 0x0004002b, 0x00000006, 0x00000012,
    0x00000003, 0x0004002b, 0x00000006, 0x00000014, 0x00000001, 0x00040020, 0x00000016, 0x00000004,
    0x00000006, 0x0004002b, 0x00000006, 0x00000018, 0x00000002, 0x0004002b, 0x00000006, 0x00000019,
    0x00000108, 0x0004002b, 0x00000006, 0x0000001d, 0x0000003f, 0x0003001d, 0x00000021, 0x00000006,
    0x0003001e, 0x00000022, 0x00000021, 0x00040020, 0x00000023, 0x0000000c, 0x00000022, 0x0004003b,
    0x00000023, 0x00000024, 0x0000000c, 0x00040015, 0x00000025, 0x00000020, 0x00000001, 0x0004002b,
    0x00000025, 0x00000026, 0x00000000, 0x0004003b, 0x00000009, 0x00000029, 0x00000001, 0x00040020,
    0x0000002d, 0x0000000c, 0x00000006, 0x00040017, 0x0000002f, 0x00000006, 0x00000003, 0x0006002c,
    0x0000002f, 0x00000030, 0x0000000c, 0x00000014, 0x00000014, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000006, 0x0000000b, 0x0000000a,
    0x00050084, 0x00000006, 0x00000013, 0x0000000b, 0x00000012, 0x00050080, 0x00000006, 0x00000015,
    0x00000013, 0x00000014, 0x00050041, 0x00000016, 0x00000017, 0x0000000f, 0x0000000b, 0x0003003e,
    0x00000017, 0x00000015, 0x000400e0, 0x00000018, 0x00000018, 0x00000019, 0x00050080, 0x00000006,
    0x0000001c, 0x0000000b, 0x00000014, 0x000500c7, 0x00000006, 0x0000001e, 0x0000001c, 0x0000001d,
    0x00050041, 0x00000016, 0x0000001f, 0x0000000f, 0x0000001e, 0x0004003d, 0x00000006, 0x00000020,
    0x0000001f, 0x0004003d, 0x00000006, 0x0000002a, 0x00000029, 0x000500c6, 0x00000006, 0x0000002b,
    0x0000002a, 0x00000014, 0x00060159, 0x00000006, 0x0000002c, 0x00000012, 0x00000020, 0x0000002b,
    0x00060041, 0x0000002d, 0x0000002e, 0x00000024, 0x00000026, 0x0000000b, 0x0003003e, 0x0000002e,
    0x0000002c, 0x000100fd, 0x00010038,
];

const OUT: u64 = 0x1000;

fn env() -> Env {
    Env {
        denorm: Denorm::Preserve,
        canonical_nan: false,
        mul_zero_positive: false,
        uniforms: HashMap::from([((0, 0), OUT)]),
        arrays: HashMap::new(),
        local_id: [0; 3],
        group_id: [0; 3],
        local_index: 0,
        subgroup_size: 32,
        fold_identity: false,
    }
}

#[test]
fn a_workgroup_shares_memory_across_its_barrier_and_shuffles_in_subgroups() {
    let mem = Memory {
        regions: vec![(OUT, vec![0; 256])],
    };
    let done = run_workgroup(
        WG,
        &env(),
        &Memory::default(),
        mem,
        [64, 1, 1],
        32,
        1_000_000,
    )
    .unwrap_or_else(|f| panic!("{}", f.message));
    let mut mem = done.mem;
    for i in 0..64u64 {
        let j = ((i ^ 1) + 1) & 63;
        assert_eq!(mem.read(OUT + 4 * i, 4), j * 3 + 1, "out[{i}]");
    }
    assert!(done.notes.is_empty(), "{:?}", done.notes);
}

#[test]
fn a_shuffle_with_lanes_outside_the_workgroup_says_so() {
    let mem = Memory {
        regions: vec![(OUT, vec![0; 256])],
    };
    let done = run_workgroup(
        WG,
        &env(),
        &Memory::default(),
        mem,
        [48, 1, 1],
        32,
        1_000_000,
    )
    .unwrap_or_else(|f| panic!("{}", f.message));
    assert!(
        done.notes
            .keys()
            .any(|n| matches!(n, Note::PartialShuffle { active: 16, .. })),
        "{:?}",
        done.notes
    );
}

#[test]
fn memory_outside_every_region_stops_the_workgroup() {
    let r = run_workgroup(
        WG,
        &env(),
        &Memory::default(),
        Memory::default(),
        [64, 1, 1],
        32,
        1_000_000,
    );
    let f = r.err().expect("the store to out has no memory");
    assert!(f.message.contains("unmapped memory"), "{}", f.message);
    assert_eq!(f.at.len(), 64);
}

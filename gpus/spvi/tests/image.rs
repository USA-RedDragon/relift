//! Images (`relift_gpu_spvi::Image`): fetches decode a texel's format,
//! writes encode it, size and level queries read the image, writes outside
//! the image are dropped, and an array length reads the buffer's region.
//! The program is `data/img.comp`, compiled with
//! `glslc --target-env=vulkan1.2 -O img.comp -o img.spv`.

use std::collections::HashMap;

use relift_gpu_spvi::group::run_workgroup;
use relift_gpu_spvi::{Denorm, Env, Image, Memory};

const SPV: &[u8] = include_bytes!("data/img.spv");
const TEX: u64 = 0x10_0000;
const IMG: u64 = 0x20_0000;
const BUF: u64 = 0x30_0000;

#[test]
fn fetch_write_and_queries() {
    let words: Vec<u32> = SPV
        .chunks(4)
        .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect();
    // A 16x8 RGBA8 texture with 2 levels: texel (x, y) of level 0 is
    // (x, y, 10, 255); level 1 follows.
    let mut tex = Vec::new();
    for y in 0..8u8 {
        for x in 0..16u8 {
            tex.extend_from_slice(&[x, y, 10, 255]);
        }
    }
    tex.extend(vec![0u8; 8 * 4 * 4]);
    let mut images = HashMap::new();
    images.insert(
        (2, 2, 1),
        Image {
            addr: TEX,
            width: 16,
            height: 8,
            depth: 1,
            layers: 1,
            levels: 2,
            format: 37,
        },
    );
    // The storage image: 104x2 A2B10G10R10.
    images.insert(
        (2, 35, 3),
        Image {
            addr: IMG,
            width: 104,
            height: 2,
            depth: 1,
            layers: 1,
            levels: 1,
            format: 64,
        },
    );
    let mut uniforms = HashMap::new();
    uniforms.insert((0, 0), BUF);
    let env = Env {
        denorm: Denorm::Preserve,
        canonical_nan: false,
        mul_zero_positive: false,
        uniforms,
        arrays: HashMap::new(),
        images,
        log_stores: true,
        local_id: [0; 3],
        group_id: [0; 3],
        local_index: 0,
        subgroup_size: 32,
        fold_identity: false,
    };
    let mem = Memory {
        regions: vec![
            (TEX, tex),
            (IMG, vec![0xee; 104 * 2 * 4]),
            (BUF, vec![0; 40]),
        ],
    };
    let done = run_workgroup(
        &words,
        &env,
        &Memory::default(),
        mem,
        [4, 2, 1],
        32,
        100_000,
    )
    .unwrap_or_else(|f| panic!("{}", f.message));
    let mut mem = done.mem;
    let n: Vec<u64> = (0..5).map(|k| mem.read(BUF + 4 * k, 4)).collect();
    assert_eq!(
        &n[..4],
        &[8, 4, 10, 2],
        "level 1 size, array length, levels"
    );
    assert_eq!(f32::from_bits(n[4] as u32), 9.0 / 255.0, "texel (9, 0) red");
    let px = |m: &mut Memory, x: u64, y: u64| m.read(IMG + (y * 104 + x) * 4, 4) as u32;
    let unorm10 = |v: u32| (v as f32 / 255.0 * 1023.0 + 0.5).floor() as u32;
    let want = |r: u32, g: u32, b: u32| unorm10(r) | unorm10(g) << 10 | unorm10(b) << 20 | 3 << 30;
    // .bgra at (x, y) and the texel as is at (x + 100, y).
    assert_eq!(px(&mut mem, 3, 1), want(10, 1, 3));
    assert_eq!(px(&mut mem, 103, 1), want(3, 1, 10));
    assert_eq!(px(&mut mem, 4, 0), 0xeeee_eeee, "untouched");
    // Eight invocations store twice each into the image, and invocation 0
    // five words into the buffer.
    let image_stores = done
        .stores
        .iter()
        .filter(|s| s.addr >= IMG && s.addr < BUF)
        .count();
    assert_eq!(image_stores, 16);
    assert!(
        done.stores
            .iter()
            .any(|s| s.addr == BUF && s.invocation == 0)
    );
}

/// `data/subbar.comp`: a workgroup of two subgroups where the second
/// returns at once and the first meets at `subgroupBarrier()`. A barrier
/// of subgroup scope waits for its own subgroup only, so the run finishes
/// with no note, and each lane reads its neighbor's store.
#[test]
fn a_subgroup_barrier_waits_for_its_subgroup_only() {
    const SUB: &[u8] = include_bytes!("data/subbar.spv");
    let words: Vec<u32> = SUB
        .chunks(4)
        .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect();
    let mut uniforms = HashMap::new();
    uniforms.insert((0, 0), BUF);
    let env = Env {
        denorm: Denorm::Preserve,
        canonical_nan: false,
        mul_zero_positive: false,
        uniforms,
        arrays: HashMap::new(),
        images: HashMap::new(),
        log_stores: false,
        local_id: [0; 3],
        group_id: [0; 3],
        local_index: 0,
        subgroup_size: 32,
        fold_identity: false,
    };
    let mem = Memory {
        regions: vec![(BUF, vec![0; 32 * 4])],
    };
    let done = run_workgroup(
        &words,
        &env,
        &Memory::default(),
        mem,
        [64, 1, 1],
        32,
        100_000,
    )
    .unwrap_or_else(|f| panic!("{}", f.message));
    assert!(done.notes.is_empty(), "{:?}", done.notes);
    let mut mem = done.mem;
    for i in 0..32u64 {
        assert_eq!(mem.read(BUF + 4 * i, 4), (i ^ 1) * 7 + 1);
    }
}

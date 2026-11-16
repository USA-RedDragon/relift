//! KTGL index walks over crafted `.rdb` files: lengths that overflow, run
//! past the file or do not advance stop the walk and are reported.

use relift_switch_shaders::ktgl::{self, Location, shader_locations};

fn loc(offset: u32) -> Location {
    Location {
        index: 0,
        offset,
        size: 0x10,
    }
}

fn with_len(mut e: Vec<u8>, elen: u64) -> Vec<u8> {
    e[8..16].copy_from_slice(&elen.to_le_bytes());
    e
}

#[test]
fn well_formed_entries_are_all_found() {
    let rdb = ktgl::build_rdb(ktgl::TYPE_S2G, &[loc(0), loc(0x40), loc(0x80)]);
    let l = shader_locations(&rdb);
    assert_eq!(l.found, vec![loc(0), loc(0x40), loc(0x80)]);
    assert_eq!(l.bad, None);
}

#[test]
fn a_length_that_overflows_stops_the_walk() {
    let mut rdb = ktgl::build_rdb(ktgl::TYPE_S2G, &[loc(0)]);
    let at = rdb.len() as u64;
    let bad = ktgl::build_rdb(ktgl::TYPE_S2G, &[loc(0x40)]);
    rdb.extend(with_len(bad.clone(), u64::MAX));
    rdb.extend(with_len(bad, u64::MAX - at + 1));
    let l = shader_locations(&rdb);
    assert_eq!(l.found, vec![loc(0)]);
    assert!(l.bad.is_some());
}

#[test]
fn a_length_that_wraps_to_the_same_entry_stops_the_walk() {
    let mut rdb = ktgl::build_rdb(ktgl::TYPE_S2G, &[loc(0)]);
    let at = rdb.len() as u64;
    let bad = ktgl::build_rdb(ktgl::TYPE_S2G, &[loc(0x40)]);
    rdb.extend(with_len(bad, u64::MAX - at + 1));
    let l = shader_locations(&rdb);
    assert_eq!(l.found, vec![loc(0)]);
    assert!(l.bad.is_some());
}

#[test]
fn a_length_shorter_than_the_header_or_past_the_file_is_bad() {
    for elen in [0, 1, 8, 0x2F, 0x1000] {
        let rdb = with_len(ktgl::build_rdb(ktgl::TYPE_S2G, &[loc(0)]), elen);
        let l = shader_locations(&rdb);
        assert!(l.found.is_empty(), "{elen:#x}");
        assert!(l.bad.is_some(), "{elen:#x}");
    }
}

#[test]
fn a_stored_size_outside_the_entry_finds_nothing() {
    let mut rdb = ktgl::build_rdb(ktgl::TYPE_S2G, &[loc(0)]);
    rdb[16..24].copy_from_slice(&u64::MAX.to_le_bytes());
    let l = shader_locations(&rdb);
    assert!(l.found.is_empty());
    assert_eq!(l.bad, None);
}

#[test]
fn a_chunk_length_past_the_entry_is_an_error() {
    let mut chunks = u32::MAX.to_le_bytes().to_vec();
    chunks.extend_from_slice(&[0u8; 8]);
    let e = ktgl::build_entry(ktgl::TYPE_S2G, 0x10_0000, &chunks);
    assert!(ktgl::inflate_entry(&e).is_err());
}

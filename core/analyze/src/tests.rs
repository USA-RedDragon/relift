use relift_guest::toy::{Toy, branch, word};
use relift_guest::{Module, Pad, Root, RootKind, Section, SectionKind};

use super::*;

const BASE: u64 = 0x1000;

fn module(code: &[[u8; 4]], data: &[u8], roots: &[u64]) -> Module {
    let mut image: Vec<u8> = code.iter().flatten().copied().collect();
    let code_size = image.len() as u64;
    image.extend_from_slice(data);
    Module {
        name: "t".into(),
        base: BASE,
        image,
        sections: vec![
            Section {
                addr: BASE,
                size: code_size,
                kind: SectionKind::Code,
            },
            Section {
                addr: BASE + code_size,
                size: data.len() as u64,
                kind: SectionKind::Data,
            },
        ],
        roots: roots
            .iter()
            .map(|&addr| Root {
                addr,
                kind: RootKind::Entry,
                name: None,
            })
            .collect(),
        pads: Vec::new(),
    }
}

fn entries(a: &Analysis) -> Vec<u64> {
    a.functions.keys().copied().collect()
}

#[test]
fn calls_and_loops_give_functions_with_reachable_extents() {
    let m = module(
        &[
            branch(0x02, 0, 4),
            word(0x09, 0, 1),
            branch(0x07, 0, -1),
            word(0x03, 0, 0),
            word(0x04, 1, 7),
            word(0x03, 0, 0),
            word(0x00, 0, 0),
        ],
        &[],
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(entries(&a), [BASE, BASE + 16]);
    let main = &a.functions[&BASE];
    assert_eq!(main.kind, EntryKind::Root(RootKind::Entry));
    assert_eq!(
        main.insts.keys().copied().collect::<Vec<_>>(),
        [BASE, BASE + 4, BASE + 8, BASE + 12]
    );
    let callee = &a.functions[&(BASE + 16)];
    assert_eq!(callee.kind, EntryKind::Called);
    assert_eq!(callee.insts.len(), 2);
    assert_eq!(callee.span(4), 8);
}

#[test]
fn a_jump_to_another_entry_is_a_tail_call_not_a_copy() {
    let m = module(
        &[branch(0x01, 0, 2), word(0x00, 0, 0), word(0x03, 0, 0)],
        &[],
        &[BASE, BASE + 8],
    );
    let a = discover(&Toy, &m);
    assert_eq!(a.functions[&BASE].insts.len(), 1);
    assert_eq!(a.functions[&(BASE + 8)].insts.len(), 1);
}

#[test]
fn computed_code_addresses_become_entries() {
    let m = module(
        &[
            word(0x04, 0, 0x1000),
            word(0x09, 0, 0x14),
            word(0x0c, 0, 0),
            word(0x04, 1, 0x1018),
            word(0x03, 0, 0),
            word(0x03, 0, 0),
            word(0x03, 0, 0),
        ],
        &[],
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(entries(&a), [BASE, BASE + 0x14, BASE + 0x18]);
    assert_eq!(a.functions[&(BASE + 0x14)].kind, EntryKind::AddressTaken);
}

#[test]
fn a_constant_known_on_one_path_only_is_forgotten_at_the_join() {
    let m = module(
        &[
            branch(0x07, 1, 2),
            word(0x04, 0, 0x1014),
            word(0x09, 0, 0),
            word(0x03, 0, 0),
            word(0x00, 0, 0),
            word(0x03, 0, 0),
        ],
        &[],
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(entries(&a), [BASE, BASE + 0x14]);
    let m = module(
        &[
            word(0x04, 0, 0x1014),
            branch(0x07, 1, 2),
            word(0x04, 0, 0x2000),
            word(0x09, 0, 0),
            word(0x03, 0, 0),
            word(0x03, 0, 0),
        ],
        &[],
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(
        entries(&a),
        [BASE, BASE + 0x14],
        "the value written before the branch is still an address, but the add after the join adds none"
    );
}

#[test]
fn undefined_words_are_kept_as_failures() {
    let m = module(&[word(0x04, 0, 1), word(0xff, 0, 0)], &[], &[BASE]);
    let a = discover(&Toy, &m);
    assert_eq!(a.failures(), 1);
    assert!(matches!(
        a.functions[&BASE].insts[&(BASE + 4)],
        Insn::Failed(_)
    ));
}

#[test]
fn roots_outside_code_or_misaligned_are_ignored() {
    let m = module(&[word(0x03, 0, 0)], &[0; 8], &[BASE, BASE + 2, BASE + 4]);
    let a = discover(&Toy, &m);
    assert_eq!(entries(&a), [BASE]);
}

#[test]
fn jump_tables_are_resolved_into_the_function() {
    // r1 = index; if r1 == 0 skip... toy has no compare-with-bound, so the
    // bound comes from cmpz on a derived value: r2 = r1 - 3 (bz when r1 == 3
    // means "out of range" for indexes 0..3).
    let table = BASE + 0x40;
    let m = module(
        &[
            word(0x04, 2, 0),              // 0x00 li r2, 0
            word(0x05, 2, 1),              // 0x04 add r2, r1
            word(0x09, 2, (-3i16) as u16), // 0x08 addi r2, -3
            word(0x0d, 2, 0),              // 0x0c cmpz r2
            branch(0x0e, 0, 8),            // 0x10 bz +8 -> 0x30
            word(0x04, 0, table as u16),   // 0x14 li r0, table
            word(0x05, 1, 1),              // 0x18 add r1, r1
            word(0x05, 1, 1),              // 0x1c add r1, r1
            word(0x05, 1, 1),              // 0x20 add r1, r1 (r1 *= 8)
            word(0x05, 0, 1),              // 0x24 add r0, r1
            word(0x0a, 0, 0),              // 0x28 ld r0, [r0]
            word(0x06, 0, 0),              // 0x2c br r0
            word(0x03, 0, 0),              // 0x30 ret
            word(0x03, 0, 0),              // 0x34 ret
            word(0x03, 0, 0),              // 0x38 ret
            word(0x03, 0, 0),              // 0x3c ret
        ],
        &[0x1000u64 + 0x34, 0x1000 + 0x38, 0x1000 + 0x3c]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    let f = &a.functions[&BASE];
    assert_eq!(
        f.tables[&(BASE + 0x2c)],
        [BASE + 0x34, BASE + 0x38, BASE + 0x3c]
    );
    assert!(f.insts.contains_key(&(BASE + 0x3c)));
    assert_eq!(a.functions.len(), 1);
    assert_eq!(unresolved_transfers(f, &Toy.preserved()), []);
}

#[test]
fn unresolved_transfers_say_where_their_target_came_from() {
    let m = module(
        &[
            word(0x0a, 1, 0),
            word(0x0c, 1, 0),
            word(0x0c, 0, 0),
            branch(0x07, 1, 3),
            word(0x05, 2, 2),
            word(0x06, 2, 0),
            word(0x06, 2, 0),
        ],
        &[],
        &[BASE],
    );
    let a = discover(&Toy, &m);
    let t = |pc: u64, kind, source| Transfer {
        pc: BASE + pc,
        kind,
        source,
    };
    assert_eq!(
        unresolved_transfers(&a.functions[&BASE], &Toy.preserved()),
        [
            t(0x04, TransferKind::Call, Source::Loaded),
            t(0x08, TransferKind::Call, Source::Returned),
            t(0x14, TransferKind::Jump, Source::Computed),
            t(0x18, TransferKind::Jump, Source::Argument),
        ]
    );
}

#[test]
fn a_target_set_on_two_paths_has_an_unknown_source() {
    let m = module(
        &[branch(0x07, 1, 2), word(0x0a, 0, 1), word(0x06, 0, 0)],
        &[],
        &[BASE],
    );
    let a = discover(&Toy, &m);
    let got = unresolved_transfers(&a.functions[&BASE], &Toy.preserved());
    assert_eq!(
        got,
        [Transfer {
            pc: BASE + 8,
            kind: TransferKind::Jump,
            source: Source::Unknown,
        }]
    );
}

#[test]
fn an_address_taken_entry_does_not_cut_the_function_around_it() {
    // The table base is set before an address the code also takes (as a
    // loader does with a loop head it computes with adr); the function
    // must run through that address to know the base at the jump.
    let table = BASE + 0x44;
    let m = module(
        &[
            word(0x04, 0, table as u16),   // 0x00 li r0, table
            word(0x04, 3, 0x1010),         // 0x04 li r3, 0x1010 (address taken)
            word(0x04, 2, 0),              // 0x08 li r2, 0
            word(0x05, 2, 1),              // 0x0c add r2, r1
            word(0x09, 2, (-3i16) as u16), // 0x10 addi r2, -3
            word(0x0d, 2, 0),              // 0x14 cmpz r2
            branch(0x0e, 0, 8),            // 0x18 bz +8 -> 0x38
            word(0x05, 1, 1),              // 0x1c add r1, r1
            word(0x05, 1, 1),              // 0x20 add r1, r1
            word(0x05, 1, 1),              // 0x24 add r1, r1 (r1 *= 8)
            word(0x05, 0, 1),              // 0x28 add r0, r1
            word(0x0a, 0, 0),              // 0x2c ld r0, [r0]
            word(0x06, 0, 0),              // 0x30 br r0
            word(0x03, 0, 0),              // 0x34 ret
            word(0x03, 0, 0),              // 0x38 ret
            word(0x03, 0, 0),              // 0x3c ret
            word(0x03, 0, 0),              // 0x40 ret
        ],
        &[0x1000u64 + 0x34, 0x1000 + 0x3c, 0x1000 + 0x40]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(a.functions[&(BASE + 0x10)].kind, EntryKind::AddressTaken);
    let f = &a.functions[&BASE];
    assert!(f.insts.contains_key(&(BASE + 0x10)), "{:?}", f.insts.keys());
    assert_eq!(
        f.tables[&(BASE + 0x30)],
        [BASE + 0x34, BASE + 0x3c, BASE + 0x40]
    );
}

#[test]
fn a_jump_table_check_skips_stores_between_compare_and_branch() {
    // The compiler put a store between the compare and its branch; the
    // check runs the compare and the branch and leaves the store out.
    let table = BASE + 0x48;
    let m = module(
        &[
            word(0x04, 2, 0),              // 0x00 li r2, 0
            word(0x05, 2, 1),              // 0x04 add r2, r1
            word(0x09, 2, (-3i16) as u16), // 0x08 addi r2, -3
            word(0x0d, 2, 0),              // 0x0c cmpz r2
            word(0x04, 3, 0x2000),         // 0x10 li r3, 0x2000
            word(0x0b, 0, 3),              // 0x14 st [r3], r0
            branch(0x0e, 0, 8),            // 0x18 bz +8 -> 0x38
            word(0x04, 0, table as u16),   // 0x1c li r0, table
            word(0x05, 1, 1),              // 0x20 add r1, r1
            word(0x05, 1, 1),              // 0x24 add r1, r1
            word(0x05, 1, 1),              // 0x28 add r1, r1 (r1 *= 8)
            word(0x05, 0, 1),              // 0x2c add r0, r1
            word(0x0a, 0, 0),              // 0x30 ld r0, [r0]
            word(0x06, 0, 0),              // 0x34 br r0
            word(0x03, 0, 0),              // 0x38 ret
            word(0x03, 0, 0),              // 0x3c ret
            word(0x03, 0, 0),              // 0x40 ret
            word(0x03, 0, 0),              // 0x44 ret
        ],
        &[0x1000u64 + 0x3c, 0x1000 + 0x40, 0x1000 + 0x44]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(
        a.functions[&BASE].tables[&(BASE + 0x34)],
        [BASE + 0x3c, BASE + 0x40, BASE + 0x44]
    );
}

#[test]
fn a_jump_table_body_leaves_out_loads_the_target_does_not_need() {
    // The compiler scheduled a load from an argument pointer between the
    // check and the table load; the body runs without it, so it cannot fault.
    let table = BASE + 0x48;
    let m = module(
        &[
            word(0x04, 2, 0),              // 0x00 li r2, 0
            word(0x05, 2, 1),              // 0x04 add r2, r1
            word(0x09, 2, (-3i16) as u16), // 0x08 addi r2, -3
            word(0x0d, 2, 0),              // 0x0c cmpz r2
            branch(0x0e, 0, 10),           // 0x10 bz +10 -> 0x38
            word(0x0a, 4, 5),              // 0x14 ld r4, [r5]
            word(0x04, 0, table as u16),   // 0x18 li r0, table
            word(0x05, 1, 1),              // 0x1c add r1, r1
            word(0x05, 1, 1),              // 0x20 add r1, r1
            word(0x05, 1, 1),              // 0x24 add r1, r1 (r1 *= 8)
            word(0x05, 0, 1),              // 0x28 add r0, r1
            word(0x0a, 0, 0),              // 0x2c ld r0, [r0]
            word(0x06, 0, 0),              // 0x30 br r0
            word(0x03, 0, 0),              // 0x34 ret
            word(0x03, 0, 0),              // 0x38 ret
            word(0x03, 0, 0),              // 0x3c ret
            word(0x03, 0, 0),              // 0x40 ret
            word(0x03, 0, 0),              // 0x44 ret
        ],
        &[0x1000u64 + 0x3c, 0x1000 + 0x40, 0x1000 + 0x44]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(
        a.functions[&BASE].tables[&(BASE + 0x30)],
        [BASE + 0x3c, BASE + 0x40, BASE + 0x44]
    );
}

#[test]
fn a_jump_table_body_may_store_to_the_stack() {
    // A spill to a stack slot between the check and the table load; r2 is
    // the stack pointer, unknown here, so running the store would fault.
    let table = BASE + 0x48;
    let m = module(
        &[
            word(0x04, 0, 0),              // 0x00 li r0, 0
            word(0x05, 0, 1),              // 0x04 add r0, r1
            word(0x09, 0, (-3i16) as u16), // 0x08 addi r0, -3
            word(0x0d, 0, 0),              // 0x0c cmpz r0
            branch(0x0e, 0, 10),           // 0x10 bz +10 -> 0x38
            word(0x0b, 3, 2),              // 0x14 st [r2], r3
            word(0x04, 0, table as u16),   // 0x18 li r0, table
            word(0x05, 1, 1),              // 0x1c add r1, r1
            word(0x05, 1, 1),              // 0x20 add r1, r1
            word(0x05, 1, 1),              // 0x24 add r1, r1 (r1 *= 8)
            word(0x05, 0, 1),              // 0x28 add r0, r1
            word(0x0a, 0, 0),              // 0x2c ld r0, [r0]
            word(0x06, 0, 0),              // 0x30 br r0
            word(0x03, 0, 0),              // 0x34 ret
            word(0x03, 0, 0),              // 0x38 ret
            word(0x03, 0, 0),              // 0x3c ret
            word(0x03, 0, 0),              // 0x40 ret
            word(0x03, 0, 0),              // 0x44 ret
        ],
        &[0x1000u64 + 0x3c, 0x1000 + 0x40, 0x1000 + 0x44]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(
        a.functions[&BASE].tables[&(BASE + 0x30)],
        [BASE + 0x3c, BASE + 0x40, BASE + 0x44]
    );
}

#[test]
fn a_jump_table_base_spilled_to_a_frame_slot_is_read_back() {
    // The table address is stored to the frame once and the body loads it
    // back from there (newlib's __svfscanf_r does this).
    let table = BASE + 0x48;
    let m = module(
        &[
            word(0x04, 3, table as u16),   // 0x00 li r3, table
            word(0x0b, 3, 2),              // 0x04 st [r2], r3
            word(0x04, 0, 0),              // 0x08 li r0, 0
            word(0x05, 0, 1),              // 0x0c add r0, r1
            word(0x09, 0, (-3i16) as u16), // 0x10 addi r0, -3
            word(0x0d, 0, 0),              // 0x14 cmpz r0
            branch(0x0e, 0, 8),            // 0x18 bz +8 -> 0x38
            word(0x0a, 0, 2),              // 0x1c ld r0, [r2]
            word(0x05, 1, 1),              // 0x20 add r1, r1
            word(0x05, 1, 1),              // 0x24 add r1, r1
            word(0x05, 1, 1),              // 0x28 add r1, r1 (r1 *= 8)
            word(0x05, 0, 1),              // 0x2c add r0, r1
            word(0x0a, 0, 0),              // 0x30 ld r0, [r0]
            word(0x06, 0, 0),              // 0x34 br r0
            word(0x03, 0, 0),              // 0x38 ret
            word(0x03, 0, 0),              // 0x3c ret
            word(0x03, 0, 0),              // 0x40 ret
            word(0x03, 0, 0),              // 0x44 ret
        ],
        &[0x1000u64 + 0x3c, 0x1000 + 0x40, 0x1000 + 0x44]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(
        a.functions[&BASE].tables[&(BASE + 0x34)],
        [BASE + 0x3c, BASE + 0x40, BASE + 0x44]
    );
}

#[test]
fn a_jump_table_base_in_a_slot_stored_twice_with_different_values_is_left_alone() {
    let table = BASE + 0x50;
    let m = module(
        &[
            word(0x04, 3, table as u16),   // 0x00 li r3, table
            word(0x0b, 3, 2),              // 0x04 st [r2], r3
            word(0x04, 3, 0),              // 0x08 li r3, 0
            word(0x0b, 3, 2),              // 0x0c st [r2], r3
            word(0x04, 0, 0),              // 0x10 li r0, 0
            word(0x05, 0, 1),              // 0x14 add r0, r1
            word(0x09, 0, (-3i16) as u16), // 0x18 addi r0, -3
            word(0x0d, 0, 0),              // 0x1c cmpz r0
            branch(0x0e, 0, 8),            // 0x20 bz +8 -> 0x40
            word(0x0a, 0, 2),              // 0x24 ld r0, [r2]
            word(0x05, 1, 1),              // 0x28 add r1, r1
            word(0x05, 1, 1),              // 0x2c add r1, r1
            word(0x05, 1, 1),              // 0x30 add r1, r1 (r1 *= 8)
            word(0x05, 0, 1),              // 0x34 add r0, r1
            word(0x0a, 0, 0),              // 0x38 ld r0, [r0]
            word(0x06, 0, 0),              // 0x3c br r0
            word(0x03, 0, 0),              // 0x40 ret
            word(0x03, 0, 0),              // 0x44 ret
            word(0x03, 0, 0),              // 0x48 ret
            word(0x03, 0, 0),              // 0x4c ret
        ],
        &[0x1000u64 + 0x44, 0x1000 + 0x48, 0x1000 + 0x4c]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert!(!a.functions[&BASE].tables.contains_key(&(BASE + 0x3c)));
}

#[test]
fn a_jump_table_body_that_stores_through_another_register_is_left_alone() {
    let table = BASE + 0x48;
    let m = module(
        &[
            word(0x04, 0, 0),              // 0x00 li r0, 0
            word(0x05, 0, 1),              // 0x04 add r0, r1
            word(0x09, 0, (-3i16) as u16), // 0x08 addi r0, -3
            word(0x0d, 0, 0),              // 0x0c cmpz r0
            branch(0x0e, 0, 10),           // 0x10 bz +10 -> 0x38
            word(0x0b, 3, 3),              // 0x14 st [r3], r3
            word(0x04, 0, table as u16),   // 0x18 li r0, table
            word(0x05, 1, 1),              // 0x1c add r1, r1
            word(0x05, 1, 1),              // 0x20 add r1, r1
            word(0x05, 1, 1),              // 0x24 add r1, r1 (r1 *= 8)
            word(0x05, 0, 1),              // 0x28 add r0, r1
            word(0x0a, 0, 0),              // 0x2c ld r0, [r0]
            word(0x06, 0, 0),              // 0x30 br r0
            word(0x03, 0, 0),              // 0x34 ret
            word(0x03, 0, 0),              // 0x38 ret
            word(0x03, 0, 0),              // 0x3c ret
            word(0x03, 0, 0),              // 0x40 ret
            word(0x03, 0, 0),              // 0x44 ret
        ],
        &[0x1000u64 + 0x3c, 0x1000 + 0x40, 0x1000 + 0x44]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert!(!a.functions[&BASE].tables.contains_key(&(BASE + 0x30)));
}

#[test]
fn a_jump_table_entered_by_a_taken_branch() {
    let m = module(
        &[
            word(0x0d, 1, 0),      // 0x00 cmpz r1
            branch(0x0e, 0, 3),    // 0x04 bz +3 -> 0x10 (index 0 only)
            word(0x04, 0, 0),      // 0x08 li r0, 0
            word(0x03, 0, 0),      // 0x0c ret
            word(0x04, 0, 0x1020), // 0x10 li r0, table
            word(0x05, 0, 1),      // 0x14 add r0, r1
            word(0x0a, 0, 0),      // 0x18 ld r0, [r0]
            word(0x06, 0, 0),      // 0x1c br r0
        ],
        &(0x1000u64 + 0x0c).to_le_bytes(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(a.functions[&BASE].tables[&(BASE + 0x1c)], [BASE + 0x0c]);
}

#[test]
fn a_jump_table_check_may_compare_a_copy_of_the_index() {
    // The bound is checked on r2 and the table indexed by r3, both copies of
    // r1 made before the check.
    let table = BASE + 0x48;
    let m = module(
        &[
            word(0x04, 2, 0),              // 0x00 li r2, 0
            word(0x05, 2, 1),              // 0x04 add r2, r1
            word(0x04, 3, 0),              // 0x08 li r3, 0
            word(0x05, 3, 1),              // 0x0c add r3, r1
            word(0x09, 2, (-3i16) as u16), // 0x10 addi r2, -3
            word(0x0d, 2, 0),              // 0x14 cmpz r2
            branch(0x0e, 0, 8),            // 0x18 bz +8 -> 0x38
            word(0x04, 0, table as u16),   // 0x1c li r0, table
            word(0x05, 3, 3),              // 0x20 add r3, r3
            word(0x05, 3, 3),              // 0x24 add r3, r3
            word(0x05, 3, 3),              // 0x28 add r3, r3 (r3 *= 8)
            word(0x05, 0, 3),              // 0x2c add r0, r3
            word(0x0a, 0, 0),              // 0x30 ld r0, [r0]
            word(0x06, 0, 0),              // 0x34 br r0
            word(0x03, 0, 0),              // 0x38 ret
            word(0x03, 0, 0),              // 0x3c ret
            word(0x03, 0, 0),              // 0x40 ret
            word(0x03, 0, 0),              // 0x44 ret
        ],
        &[0x1000u64 + 0x3c, 0x1000 + 0x40, 0x1000 + 0x44]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(
        a.functions[&BASE].tables[&(BASE + 0x34)],
        [BASE + 0x3c, BASE + 0x40, BASE + 0x44]
    );
}

#[test]
fn a_jump_table_check_on_an_unrelated_register_is_left_alone() {
    let table = BASE + 0x48;
    let m = module(
        &[
            word(0x04, 2, 0),              // 0x00 li r2, 0
            word(0x05, 2, 0),              // 0x04 add r2, r0
            word(0x04, 3, 0),              // 0x08 li r3, 0
            word(0x05, 3, 1),              // 0x0c add r3, r1
            word(0x09, 2, (-3i16) as u16), // 0x10 addi r2, -3
            word(0x0d, 2, 0),              // 0x14 cmpz r2
            branch(0x0e, 0, 8),            // 0x18 bz +8 -> 0x38
            word(0x04, 0, table as u16),   // 0x1c li r0, table
            word(0x05, 3, 3),              // 0x20 add r3, r3
            word(0x05, 3, 3),              // 0x24 add r3, r3
            word(0x05, 3, 3),              // 0x28 add r3, r3 (r3 *= 8)
            word(0x05, 0, 3),              // 0x2c add r0, r3
            word(0x0a, 0, 0),              // 0x30 ld r0, [r0]
            word(0x06, 0, 0),              // 0x34 br r0
            word(0x03, 0, 0),              // 0x38 ret
            word(0x03, 0, 0),              // 0x3c ret
            word(0x03, 0, 0),              // 0x40 ret
            word(0x03, 0, 0),              // 0x44 ret
        ],
        &[0x1000u64 + 0x3c, 0x1000 + 0x40, 0x1000 + 0x44]
            .iter()
            .flat_map(|v| v.to_le_bytes())
            .collect::<Vec<u8>>(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert!(!a.functions[&BASE].tables.contains_key(&(BASE + 0x34)));
}

#[test]
fn a_jump_table_base_survives_a_call_that_flows_into_the_body() {
    // The bound check's other way out is a call whose return point is the
    // body (a call to a function that never returns, as the flow graph
    // sees it); the call forgets r0, but the body is entered from the
    // branch with r0 set.
    let m = module(
        &[
            word(0x04, 0, 0x1024), // 0x00 li r0, table
            word(0x0d, 1, 0),      // 0x04 cmpz r1
            branch(0x0e, 0, 2),    // 0x08 bz +2 -> 0x10
            branch(0x02, 0, 5),    // 0x0c bl 0x20, returns to 0x10
            word(0x05, 0, 1),      // 0x10 add r0, r1
            word(0x0a, 0, 0),      // 0x14 ld r0, [r0]
            word(0x06, 0, 0),      // 0x18 br r0
            word(0x03, 0, 0),      // 0x1c ret
            word(0x03, 0, 0),      // 0x20 ret
        ],
        &(0x1000u64 + 0x1c).to_le_bytes(),
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(a.functions[&BASE].tables[&(BASE + 0x18)], [BASE + 0x1c]);
}

#[test]
fn progress_reports_every_entry_and_function_with_real_counts() {
    let m = module(
        &[
            word(0x04, 0, 1),
            word(0xff, 0, 0),
            word(0x03, 0, 0),
            word(0x03, 0, 0),
        ],
        &[],
        &[BASE, BASE + 12],
    );
    let mut seen = Vec::new();
    let a = discover_with(&Toy, &m, &mut |p| seen.push(p));
    assert_eq!(a, discover(&Toy, &m));
    let found: Vec<(usize, usize)> = seen
        .iter()
        .filter_map(|p| match p {
            Progress::Found { entries, explored } => Some((*entries, *explored)),
            Progress::Lifted { .. } => None,
        })
        .collect();
    assert_eq!(found, [(2, 1), (2, 2)]);
    let lifted: Vec<(usize, usize, usize)> = seen
        .iter()
        .filter_map(|p| match p {
            Progress::Lifted {
                done,
                total,
                unlifted,
            } => Some((*done, *total, *unlifted)),
            Progress::Found { .. } => None,
        })
        .collect();
    assert_eq!(lifted, [(1, 2, 1), (2, 2, 1)]);
    assert_eq!(a.failures(), 1);
}

#[test]
fn discover_more_returns_only_what_a_new_root_reaches() {
    let m = module(
        &[
            branch(0x02, 0, 4),
            word(0x03, 0, 0),
            branch(0x02, 0, 2),
            word(0x03, 0, 0),
            word(0x04, 1, 7),
            word(0x03, 0, 0),
            branch(0x02, 0, 2),
            word(0x03, 0, 0),
            word(0x03, 0, 0),
        ],
        &[],
        &[BASE],
    );
    let a = discover(&Toy, &m);
    assert_eq!(entries(&a), [BASE, BASE + 16]);
    let known: BTreeMap<u64, EntryKind> = a.functions.values().map(|f| (f.entry, f.kind)).collect();
    let one = discover_more(&Toy, &m, &known, &[BASE + 8]);
    assert_eq!(entries(&one), [BASE + 8]);
    assert_eq!(
        one.functions[&(BASE + 8)].kind,
        EntryKind::Root(RootKind::Hint)
    );
    assert_eq!(one.functions[&(BASE + 8)].insts.len(), 2);
    let two = discover_more(&Toy, &m, &known, &[BASE + 24]);
    assert_eq!(entries(&two), [BASE + 24, BASE + 32]);
    assert_eq!(two.functions[&(BASE + 32)].kind, EntryKind::Called);
    assert!(
        discover_more(&Toy, &m, &known, &[BASE, BASE + 2])
            .functions
            .is_empty()
    );
}

#[test]
fn a_landing_pad_that_serves_a_call_joins_the_function() {
    let mut m = module(
        &[
            branch(0x02, 0, 3),
            word(0x03, 0, 0),
            word(0x03, 0, 0),
            word(0x03, 0, 0),
        ],
        &[],
        &[BASE],
    );
    let plain = discover(&Toy, &m);
    assert!(!plain.functions[&BASE].insts.contains_key(&(BASE + 8)));
    m.pads = vec![Pad {
        site: BASE,
        len: 4,
        addr: BASE + 8,
    }];
    let a = discover(&Toy, &m);
    assert_eq!(entries(&a), [BASE, BASE + 12]);
    let f = &a.functions[&BASE];
    assert!(f.insts.contains_key(&(BASE + 8)));
    assert_eq!(f.pads, BTreeSet::from([BASE + 8]));
}

#[test]
fn a_pad_whose_site_is_in_no_function_is_ignored() {
    let mut m = module(&[word(0x03, 0, 0), word(0x03, 0, 0)], &[], &[BASE]);
    m.pads = vec![Pad {
        site: BASE + 0x100,
        len: 4,
        addr: BASE + 4,
    }];
    let a = discover(&Toy, &m);
    assert!(a.functions[&BASE].pads.is_empty());
    assert_eq!(a.functions[&BASE].insts.len(), 1);
}

#[test]
fn stack_slots_follow_the_stack_pointer_across_calls() {
    let code = [
        word(0x09, 2, 0xFFC0),
        word(0x04, 0, 8),
        word(0x05, 0, 2),
        word(0x04, 1, 0x1234),
        word(0x0b, 1, 0),
        word(0x04, 1, 0x20),
        word(0x0c, 1, 0),
        word(0x09, 2, 8),
        word(0x03, 0, 0),
    ];
    let m = module(&code, &[], &[BASE]);
    let insts: BTreeMap<u64, Insn> = (0..code.len() as u64)
        .map(|k| {
            let pc = BASE + 4 * k;
            let b = Toy.lift(m.bytes_at(pc).expect("code"), pc).expect("lifts");
            (pc, Insn::Lifted(Arc::new(b)))
        })
        .collect();
    let s = consts::stack_slots(&insts, BASE, &Toy.preserved(), Reg(2));
    assert!(!s.contains_key(&(BASE + 0x10)));
    let v = 0x1234u64.to_le_bytes().to_vec();
    assert_eq!(
        s.get(&(BASE + 0x1c)),
        Some(&BTreeMap::from([(8, v.clone())]))
    );
    assert_eq!(s.get(&(BASE + 0x20)), Some(&BTreeMap::from([(0, v)])));
}

fn escapes_of(code: &[[u8; 4]], roots: &[u64]) -> escape::Escapes {
    let m = module(code, &[], roots);
    escape::return_escapes(&Toy, &discover(&Toy, &m))
}

#[test]
fn a_call_to_a_function_that_saves_its_return_address_elsewhere_is_a_resume_point() {
    let e = escapes_of(
        &[
            branch(0x02, 0, 2),
            word(0x03, 0, 0),
            word(0x0b, 3, 1),
            word(0x03, 0, 0),
        ],
        &[BASE],
    );
    assert_eq!(e.resume_points, [(BASE, BASE + 4)]);
    assert!(e.continuations.is_empty());
}

#[test]
fn saving_the_return_address_in_the_functions_own_frame_is_not_an_escape() {
    let e = escapes_of(
        &[
            branch(0x02, 0, 2),
            word(0x03, 0, 0),
            word(0x0b, 3, 2),
            word(0x03, 0, 0),
        ],
        &[BASE],
    );
    assert_eq!(e, escape::Escapes::default());
}

#[test]
fn a_value_derived_from_the_return_address_counts_and_a_tail_call_inherits_the_escape() {
    let e = escapes_of(
        &[
            branch(0x02, 0, 2),
            word(0x03, 0, 0),
            branch(0x01, 0, 1),
            word(0x05, 1, 3),
            word(0x0b, 1, 0),
            word(0x03, 0, 0),
        ],
        &[BASE, BASE + 12],
    );
    assert_eq!(e.resume_points, [(BASE, BASE + 4)]);
}

#[test]
fn an_escaper_that_calls_an_escaper_has_a_continuation_and_its_caller_a_resume_point() {
    let e = escapes_of(
        &[
            word(0x0b, 3, 1),
            branch(0x02, 0, 2),
            word(0x03, 0, 0),
            word(0x0b, 3, 1),
            word(0x03, 0, 0),
            branch(0x02, 0, -5),
            word(0x03, 0, 0),
        ],
        &[BASE, BASE + 20],
    );
    assert_eq!(e.continuations, BTreeSet::from([BASE + 8]));
    assert_eq!(e.resume_points, [(BASE + 20, BASE + 24)]);
}

#[test]
fn applying_the_escapes_adds_the_pads() {
    let m = module(
        &[
            branch(0x02, 0, 2),
            word(0x03, 0, 0),
            word(0x0b, 3, 1),
            word(0x03, 0, 0),
        ],
        &[],
        &[BASE],
    );
    let mut a = discover(&Toy, &m);
    let e = escape::return_escapes(&Toy, &a);
    escape::apply(&mut a, &e);
    assert_eq!(a.functions[&BASE].pads, BTreeSet::from([BASE + 4]));
}

#[test]
fn a_return_address_parked_in_a_register_across_a_call_still_escapes() {
    let e = escapes_of(
        &[
            branch(0x02, 0, 2),
            word(0x03, 0, 0),
            word(0x05, 1, 3),
            branch(0x02, 0, 3),
            word(0x0b, 1, 0),
            word(0x03, 0, 0),
            word(0x03, 0, 0),
        ],
        &[BASE],
    );
    assert_eq!(e.resume_points, [(BASE, BASE + 4)]);
}

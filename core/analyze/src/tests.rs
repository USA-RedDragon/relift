use relift_guest::toy::{Toy, branch, word};
use relift_guest::{Module, Root, RootKind, Section, SectionKind};

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

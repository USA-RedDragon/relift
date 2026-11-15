//! Function discovery over synthetic cartridges.

use relift_gb_analyze::{CallKind, Loc, Report, TableBound, UnresolvedKind, analyze};
use relift_gb_formats::banks::ROM_BANK_SIZE;
use relift_gb_formats::synth::{self, Spec};

struct Cart {
    rom: Vec<u8>,
}

impl Cart {
    fn new(cart_type: u8, rom_size: u8) -> Cart {
        let mut rom = synth::rom(&Spec {
            cart_type,
            rom_size,
            ..Spec::default()
        });
        rom[..0x100].fill(0xFF);
        rom[0x150..].fill(0xFF);
        for bank in rom.chunks_mut(ROM_BANK_SIZE).skip(1) {
            bank.fill(0xFF);
        }
        rom[0x100..0x104].copy_from_slice(&[0x00, 0xC3, 0x50, 0x01]);
        Cart { rom }
    }

    fn put(mut self, bank: usize, addr: u16, code: &[u8]) -> Cart {
        let at = if addr < 0x4000 {
            usize::from(addr)
        } else {
            bank * ROM_BANK_SIZE + usize::from(addr - 0x4000)
        };
        self.rom[at..at + code.len()].copy_from_slice(code);
        self
    }

    fn analyze(mut self) -> Report {
        synth::fix_checksums(&mut self.rom);
        analyze(&self.rom).expect("analyzes")
    }
}

fn has(r: &Report, bank: u16, addr: u16) -> bool {
    r.functions.contains_key(&Loc { bank, addr })
}

fn kinds(r: &Report) -> Vec<UnresolvedKind> {
    r.unresolved.iter().map(|u| u.kind).collect()
}

#[test]
fn entry_follows_jumps_calls_and_branches() {
    let r = Cart::new(0x19, 0x04)
        .put(
            0,
            0x150,
            &[0xCD, 0x00, 0x02, 0x28, 0x01, 0x00, 0xC3, 0x60, 0x01],
        )
        .put(0, 0x160, &[0xC9])
        .put(0, 0x200, &[0x20, 0x02, 0x3E, 0x01, 0xC9])
        .analyze();
    assert!(has(&r, 0, 0x100));
    assert!(has(&r, 0, 0x200));
    let entry = &r.functions[&Loc::fixed(0x100)];
    assert!(entry.body.contains_key(&Loc::fixed(0x160)));
    assert!(entry.body.contains_key(&Loc::fixed(0x155)));
    assert_eq!(r.functions[&Loc::fixed(0x200)].bytes, 5);
    assert!(r.unresolved.is_empty());
}

#[test]
fn constant_bank_write_names_the_callee_bank() {
    let r = Cart::new(0x19, 0x05)
        .put(
            0,
            0x150,
            &[0x3E, 0x07, 0xEA, 0x00, 0x20, 0xCD, 0x00, 0x40, 0xC9],
        )
        .put(7, 0x4000, &[0xC9])
        .analyze();
    assert!(has(&r, 7, 0x4000));
    assert!(!has(&r, 1, 0x4000));
    assert_eq!(r.coverage[&7].functions, 1);
}

#[test]
fn reset_state_maps_bank_one() {
    let r = Cart::new(0x19, 0x02)
        .put(0, 0x150, &[0xCD, 0x00, 0x40, 0xC9])
        .put(1, 0x4000, &[0xC9])
        .analyze();
    assert!(has(&r, 1, 0x4000));
}

#[test]
fn mbc5_uses_nine_bits() {
    let r = Cart::new(0x19, 0x08)
        .put(
            0,
            0x150,
            &[
                0x3E, 0x23, 0xEA, 0x00, 0x20, 0x3E, 0x01, 0xEA, 0x00, 0x30, 0xCD, 0x00, 0x50, 0xC9,
            ],
        )
        .put(0x123, 0x5000, &[0xC9])
        .analyze();
    assert!(has(&r, 0x123, 0x5000), "{:?}", r.functions.keys());
}

#[test]
fn mbc1_zero_selects_bank_one_and_upper_bits_join() {
    let r = Cart::new(0x01, 0x06)
        .put(
            0,
            0x150,
            &[
                0xAF, 0xEA, 0x00, 0x20, 0xCD, 0x00, 0x40, 0x3E, 0x01, 0xEA, 0x00, 0x40, 0x3E, 0x02,
                0xEA, 0x00, 0x20, 0xCD, 0x00, 0x41, 0xC9,
            ],
        )
        .put(1, 0x4000, &[0xC9])
        .put(0x22, 0x4100, &[0xC9])
        .analyze();
    assert!(has(&r, 1, 0x4000));
    assert!(has(&r, 0x22, 0x4100), "{:?}", r.functions.keys());
}

#[test]
fn an_unbanked_cartridge_resolves_the_window_from_a_vector() {
    let r = Cart::new(0x00, 0x00)
        .put(0, 0x150, &[0xC9])
        .put(0, 0x40, &[0xCD, 0x00, 0x40, 0xD9])
        .put(1, 0x4000, &[0xC9])
        .analyze();
    assert!(has(&r, 1, 0x4000));
    assert!(r.unresolved.is_empty());
}

#[test]
fn a_call_through_a_jump_helper_with_a_variable_address_is_listed() {
    let r = Cart::new(0x19, 0x02)
        .put(0, 0x300, &[0xE9])
        .put(
            0,
            0x150,
            &[0xFA, 0x00, 0xC0, 0x6F, 0x26, 0x00, 0xCD, 0x00, 0x03, 0xC9],
        )
        .analyze();
    assert_eq!(kinds(&r), vec![UnresolvedKind::IndirectCall]);
}

fn dispatch_cart() -> Cart {
    Cart::new(0x19, 0x02).put(
        0,
        0x300,
        &[0xE1, 0x5F, 0x16, 0x00, 0x19, 0x19, 0x2A, 0x66, 0x6F, 0xE9],
    )
}

#[test]
fn a_call_to_an_inline_table_dispatcher_ends_the_path_and_is_listed() {
    let r = dispatch_cart()
        .put(0, 0x150, &[0xCD, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00])
        .analyze();
    assert_eq!(kinds(&r), vec![UnresolvedKind::InlineTable]);
    assert_eq!(r.unresolved[0].at, Loc::fixed(0x150));
    assert!(
        r.unresolved[0].detail.contains("$0153"),
        "{:?}",
        r.unresolved
    );
    let entry = &r.functions[&Loc::fixed(0x100)];
    assert!(!entry.body.contains_key(&Loc::fixed(0x153)));
}

#[test]
fn a_jump_to_the_popped_return_address_is_a_return() {
    let r = Cart::new(0x19, 0x02)
        .put(0, 0x300, &[0xE1, 0xE9])
        .put(0, 0x150, &[0x21, 0x00, 0x03, 0xE5, 0xCD, 0x00, 0x03, 0xC9])
        .analyze();
    assert!(r.unresolved.is_empty(), "{:?}", r.unresolved);
    let entry = &r.functions[&Loc::fixed(0x100)];
    assert!(entry.body.contains_key(&Loc::fixed(0x157)));
}

#[test]
fn a_constant_pointer_to_unreached_rom_is_listed_not_followed() {
    let r = Cart::new(0x19, 0x02)
        .put(0, 0x150, &[0x21, 0x00, 0x03, 0xC9])
        .put(0, 0x300, &[0xC9])
        .analyze();
    assert!(!has(&r, 0, 0x300));
    assert_eq!(r.unreached_pointers.len(), 1);
    assert_eq!(r.unreached_pointers[0].target, Some(Loc::fixed(0x300)));
    assert_eq!(r.unreached_pointers[0].stored_at, None);
}

#[test]
fn a_pointer_stored_as_two_constant_bytes_is_listed_with_its_ram_address() {
    let r = Cart::new(0x19, 0x02)
        .put(
            0,
            0x150,
            &[
                0x3E, 0x00, 0xEA, 0x9B, 0xCD, 0x3E, 0x03, 0xEA, 0x9C, 0xCD, 0xC9,
            ],
        )
        .put(0, 0x300, &[0xC9])
        .analyze();
    let stored: Vec<_> = r
        .unreached_pointers
        .iter()
        .filter(|p| p.stored_at == Some(0xCD9B))
        .collect();
    assert_eq!(stored.len(), 1);
    assert_eq!(stored[0].value, 0x300);
    assert_eq!(stored[0].target, Some(Loc::fixed(0x300)));
}

#[test]
fn a_pointer_into_found_code_is_not_listed() {
    let r = Cart::new(0x19, 0x02)
        .put(0, 0x150, &[0x21, 0x00, 0x03, 0xCD, 0x00, 0x03, 0xC9])
        .put(0, 0x300, &[0xC9])
        .analyze();
    assert!(r.unreached_pointers.is_empty());
}

#[test]
fn an_unused_vector_is_not_a_root() {
    let r = Cart::new(0x19, 0x02)
        .put(0, 0x150, &[0xC9])
        .put(0, 0x58, &[0xFC])
        .analyze();
    assert!(r.unresolved.is_empty());
    assert!(!has(&r, 0, 0x58));
}

#[test]
fn a_vector_cannot_assume_a_bank() {
    let r = Cart::new(0x19, 0x04)
        .put(0, 0x150, &[0xC9])
        .put(0, 0x40, &[0xCD, 0x00, 0x40, 0xD9])
        .analyze();
    assert_eq!(kinds(&r), vec![UnresolvedKind::UnknownBank]);
    assert_eq!(r.unresolved[0].at, Loc::fixed(0x40));
}

#[test]
fn window_code_calls_within_its_bank() {
    let r = Cart::new(0x19, 0x04)
        .put(
            0,
            0x150,
            &[0x3E, 0x02, 0xEA, 0x00, 0x20, 0xCD, 0x00, 0x40, 0xC9],
        )
        .put(2, 0x4000, &[0xCD, 0x10, 0x40, 0xC9])
        .put(2, 0x4010, &[0xC9])
        .analyze();
    assert!(has(&r, 2, 0x4010));
    assert!(r.unresolved.is_empty());
}

#[test]
fn window_code_keeps_its_bank_across_a_switching_call() {
    let r = Cart::new(0x19, 0x04)
        .put(
            0,
            0x150,
            &[0x3E, 0x02, 0xEA, 0x00, 0x20, 0xCD, 0x00, 0x40, 0xC9],
        )
        .put(
            0,
            0x300,
            &[
                0xF5, 0x3E, 0x03, 0xEA, 0x00, 0x20, 0xF1, 0xEA, 0x00, 0x20, 0xC9,
            ],
        )
        .put(2, 0x4000, &[0xCD, 0x00, 0x03, 0xCD, 0x10, 0x40, 0xC9])
        .put(2, 0x4010, &[0xC9])
        .analyze();
    assert!(has(&r, 2, 0x4010), "{:?}", r.unresolved);
    assert!(r.functions[&Loc::fixed(0x300)].switches_bank);
    assert_eq!(r.dynamic_writes, vec![Loc::fixed(0x307)]);
}

#[test]
fn a_fixed_caller_takes_the_bank_a_callee_leaves() {
    let r = Cart::new(0x19, 0x04)
        .put(0, 0x150, &[0xCD, 0x00, 0x03, 0xCD, 0x00, 0x40, 0xC9])
        .put(0, 0x300, &[0x3E, 0x02, 0xEA, 0x00, 0x20, 0xC9])
        .put(2, 0x4000, &[0xC9])
        .analyze();
    assert!(has(&r, 2, 0x4000));
    assert!(r.unresolved.is_empty(), "{:?}", r.unresolved);
}

#[test]
fn a_fixed_caller_loses_the_bank_after_a_variable_switch() {
    let r = Cart::new(0x19, 0x04)
        .put(0, 0x150, &[0xCD, 0x00, 0x03, 0xCD, 0x00, 0x40, 0xC9])
        .put(0, 0x300, &[0xFA, 0x00, 0xC0, 0xEA, 0x00, 0x20, 0xC9])
        .put(2, 0x4000, &[0xC9])
        .analyze();
    assert_eq!(kinds(&r), vec![UnresolvedKind::UnknownBank]);
}

#[test]
fn a_bank_select_helper_sets_the_callers_bank() {
    let r = Cart::new(0x19, 0x04)
        .put(
            0,
            0x150,
            &[0x3E, 0x03, 0xCD, 0x00, 0x03, 0xCD, 0x00, 0x40, 0xC9],
        )
        .put(0, 0x300, &[0xEA, 0x00, 0x20, 0xC9])
        .put(3, 0x4000, &[0xC9])
        .analyze();
    assert!(has(&r, 3, 0x4000), "{:?}", r.unresolved);
    assert!(r.unresolved.is_empty());
}

fn tramp_cart() -> Cart {
    Cart::new(0x19, 0x04)
        .put(0, 0x300, &[0xEA, 0x00, 0x20, 0xE9])
        .put(3, 0x4100, &[0xC9])
        .put(2, 0x4200, &[0xC9])
}

#[test]
fn a_trampoline_call_resolves_from_constants() {
    let r = tramp_cart()
        .put(
            0,
            0x150,
            &[
                0x3E, 0x03, 0x21, 0x00, 0x41, 0xCD, 0x00, 0x03, 0x3E, 0x02, 0x21, 0x00, 0x42, 0xCD,
                0x00, 0x03, 0xC9,
            ],
        )
        .analyze();
    assert!(has(&r, 3, 0x4100));
    assert!(has(&r, 2, 0x4200));
    let t = r.functions[&Loc::fixed(0x300)].trampoline.expect("found");
    assert_eq!(t.bank_reg, Some('A'));
    assert_eq!(t.target_regs, Some(('H', 'L')));
    assert!(r.edges.iter().any(|e| e.kind == CallKind::Trampoline));
    assert!(r.unresolved.is_empty(), "{:?}", r.unresolved);
}

#[test]
fn a_wrapper_over_a_trampoline_is_resolved_at_its_callers() {
    let r = tramp_cart()
        .put(0, 0x310, &[0xCD, 0x00, 0x03, 0xC9])
        .put(
            0,
            0x150,
            &[0x3E, 0x03, 0x21, 0x00, 0x41, 0xCD, 0x10, 0x03, 0xC9],
        )
        .analyze();
    assert!(has(&r, 3, 0x4100), "{:?}", r.unresolved);
    assert!(r.unresolved.is_empty(), "{:?}", r.unresolved);
}

#[test]
fn a_trampoline_call_with_a_variable_bank_is_listed() {
    let r = tramp_cart()
        .put(
            0,
            0x150,
            &[0xFA, 0x00, 0xC0, 0x21, 0x00, 0x41, 0xCD, 0x00, 0x03, 0xC9],
        )
        .analyze();
    assert_eq!(kinds(&r), vec![UnresolvedKind::TrampolineBank]);
    assert!(!has(&r, 3, 0x4100));
}

#[test]
fn trampoline_arguments_may_travel_in_other_registers() {
    let r = Cart::new(0x19, 0x04)
        .put(0, 0x300, &[0x78, 0xEA, 0x00, 0x20, 0x62, 0x6B, 0xE9])
        .put(3, 0x4100, &[0xC9])
        .put(
            0,
            0x150,
            &[0x06, 0x03, 0x11, 0x00, 0x41, 0xCD, 0x00, 0x03, 0xC9],
        )
        .analyze();
    assert!(has(&r, 3, 0x4100), "{:?}", r.unresolved);
    let t = r.functions[&Loc::fixed(0x300)].trampoline.expect("found");
    assert_eq!(t.bank_reg, Some('B'));
    assert_eq!(t.target_regs, Some(('D', 'E')));
}

#[test]
fn a_jump_through_an_unknown_hl_is_listed() {
    let r = Cart::new(0x19, 0x02)
        .put(0, 0x150, &[0xFA, 0x00, 0xC0, 0x6F, 0x26, 0x40, 0xE9])
        .analyze();
    assert_eq!(kinds(&r), vec![UnresolvedKind::IndirectJump]);
}

#[test]
fn a_constant_jump_through_hl_is_followed() {
    let r = Cart::new(0x19, 0x02)
        .put(0, 0x150, &[0x21, 0x00, 0x03, 0xE9])
        .put(0, 0x300, &[0xC9])
        .analyze();
    assert!(r.unresolved.is_empty());
    assert!(
        r.functions[&Loc::fixed(0x100)]
            .body
            .contains_key(&Loc::fixed(0x300))
    );
}

#[test]
fn code_copied_to_ram_is_reported_with_its_copy_loop() {
    let r = Cart::new(0x19, 0x02)
        .put(
            0,
            0x150,
            &[
                0x21, 0x00, 0x03, 0x11, 0x00, 0xC0, 0x06, 0x04, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA,
                0xCD, 0x00, 0xC0, 0xC9,
            ],
        )
        .put(0, 0x300, &[0x00, 0x00, 0x00, 0xC9])
        .analyze();
    assert_eq!(r.ram_code.len(), 1, "{:?}", r.ram_code);
    let ram = &r.ram_code[0];
    assert_eq!(ram.transfer.target, 0xC000);
    assert_eq!(ram.copied_by.len(), 1);
    let (_, c) = &ram.copied_by[0];
    assert_eq!(c.sources, vec![0x300]);
    assert_eq!(c.dests, vec![0xC000]);
    assert_eq!(c.counts, vec![4]);
}

#[test]
fn a_high_page_copy_loop_names_its_destination() {
    let r = Cart::new(0x19, 0x02)
        .put(
            0,
            0x150,
            &[
                0x0E, 0x80, 0x06, 0x03, 0x21, 0x00, 0x03, 0x2A, 0xE2, 0x0C, 0x05, 0x20, 0xFA, 0xCD,
                0x80, 0xFF, 0xC9,
            ],
        )
        .put(0, 0x300, &[0xC9])
        .analyze();
    assert_eq!(r.ram_code.len(), 1, "{:?}", r.ram_code);
    assert_eq!(r.ram_code[0].transfer.target, 0xFF80);
    assert_eq!(r.ram_code[0].copied_by.len(), 1);
}

#[test]
fn an_unused_opcode_ends_the_path_and_is_listed() {
    let r = Cart::new(0x19, 0x02).put(0, 0x150, &[0xD3]).analyze();
    assert_eq!(kinds(&r), vec![UnresolvedKind::Illegal]);
}

#[test]
fn coverage_counts_distinct_bytes_per_bank() {
    let r = Cart::new(0x19, 0x04)
        .put(
            0,
            0x150,
            &[0x3E, 0x02, 0xEA, 0x00, 0x20, 0xCD, 0x00, 0x40, 0xC9],
        )
        .put(2, 0x4000, &[0x00, 0x00, 0xC9])
        .analyze();
    assert_eq!(r.coverage[&2].bytes, 3);
    assert_eq!(r.coverage[&0].bytes, 4 + 9);
}

#[test]
fn unmodelled_mappers_are_refused() {
    let mut rom = synth::rom(&Spec {
        cart_type: 0xFE,
        rom_size: 0x01,
        ..Spec::default()
    });
    synth::fix_checksums(&mut rom);
    assert!(analyze(&rom).is_err());
}

const JP_THROUGH_C100: [u8; 7] = [0x21, 0x00, 0xC1, 0x2A, 0x66, 0x6F, 0xE9];

fn table(r: &Report, site: Loc) -> &relift_gb_analyze::JumpTable {
    r.tables
        .iter()
        .find(|t| t.site == site)
        .expect("table followed")
}

#[test]
fn a_jump_through_a_ram_pointer_follows_the_constants_stored_there() {
    let r = Cart::new(0x19, 0x02)
        .put(
            0,
            0x150,
            &[
                0x3E, 0x00, 0xEA, 0x00, 0xC1, 0x3E, 0x03, 0xEA, 0x01, 0xC1, 0xCD, 0x00, 0x02, 0xC9,
            ],
        )
        .put(0, 0x200, &JP_THROUGH_C100)
        .put(0, 0x300, &[0xC9])
        .analyze();
    assert!(has(&r, 0, 0x300), "{:?}", r.functions.keys());
    assert!(r.unresolved.is_empty(), "{:?}", r.unresolved);
    let p = &r.ram_pointers[0];
    assert_eq!((p.cell, p.targets, p.partial), (0xC100, 1, false));
}

#[test]
fn a_ram_pointer_stored_by_code_found_through_another_is_followed() {
    let r = Cart::new(0x19, 0x02)
        .put(
            0,
            0x150,
            &[
                0x3E, 0x00, 0xEA, 0x00, 0xC1, 0x3E, 0x03, 0xEA, 0x01, 0xC1, 0xCD, 0x00, 0x02, 0xC9,
            ],
        )
        .put(0, 0x200, &JP_THROUGH_C100)
        .put(
            0,
            0x300,
            &[
                0x3E, 0x00, 0xEA, 0x02, 0xC1, 0x3E, 0x04, 0xEA, 0x03, 0xC1, 0xCD, 0x10, 0x02, 0xC9,
            ],
        )
        .put(0, 0x210, &[0x21, 0x02, 0xC1, 0x2A, 0x66, 0x6F, 0xE9])
        .put(0, 0x400, &[0xC9])
        .analyze();
    assert!(has(&r, 0, 0x400), "{:?}", r.functions.keys());
}

#[test]
fn a_ram_pointer_to_data_is_not_followed_and_is_listed() {
    let r = Cart::new(0x19, 0x02)
        .put(
            0,
            0x150,
            &[
                0x3E, 0x00, 0xEA, 0x00, 0xC1, 0x3E, 0x05, 0xEA, 0x01, 0xC1, 0xCD, 0x00, 0x02, 0xC9,
            ],
        )
        .put(0, 0x200, &JP_THROUGH_C100)
        .put(0, 0x500, &[0xD3])
        .analyze();
    assert!(!has(&r, 0, 0x500));
    assert_eq!(kinds(&r), vec![UnresolvedKind::RamPointer]);
}

#[test]
fn a_ram_pointer_cell_without_a_constant_store_is_listed() {
    let r = Cart::new(0x19, 0x02)
        .put(0, 0x150, &[0xCD, 0x00, 0x02, 0xC9])
        .put(0, 0x200, &JP_THROUGH_C100)
        .analyze();
    assert_eq!(kinds(&r), vec![UnresolvedKind::RamPointer]);
}

#[test]
fn a_ram_pointer_into_the_window_takes_the_bank_at_the_jump() {
    let r = Cart::new(0x19, 0x02)
        .put(
            0,
            0x150,
            &[
                0x3E, 0x03, 0xEA, 0x00, 0x20, 0x3E, 0x00, 0xEA, 0x00, 0xC1, 0x3E, 0x41, 0xEA, 0x01,
                0xC1, 0xCD, 0x00, 0x02, 0xC9,
            ],
        )
        .put(0, 0x200, &JP_THROUGH_C100)
        .put(3, 0x4100, &[0xC9])
        .analyze();
    assert!(has(&r, 3, 0x4100), "{:?}", r.functions.keys());
    assert!(r.unresolved.is_empty(), "{:?}", r.unresolved);
}

const DISPATCH: [u8; 9] = [0x5F, 0x16, 0x00, 0x19, 0x2A, 0x66, 0x6F, 0xE9, 0x00];

fn table_cart(prefix: &[u8], words: &[u16]) -> Cart {
    let mut code = prefix.to_vec();
    code.extend_from_slice(&[0x21, 0x00, 0x03]);
    code.extend_from_slice(&DISPATCH[..8]);
    let mut bytes = Vec::new();
    for w in words {
        bytes.extend_from_slice(&w.to_le_bytes());
    }
    Cart::new(0x19, 0x02)
        .put(0, 0x150, &code)
        .put(0, 0x300, &bytes)
        .put(0, 0x400, &[0xC9])
        .put(0, 0x410, &[0xC9])
        .put(0, 0x420, &[0xC9])
        .put(0, 0x430, &[0xC9])
        .put(0, 0x500, &[0xD3])
}

#[test]
fn a_table_bounded_by_a_compare_follows_only_the_entries_below_it() {
    let r = table_cart(
        &[0xFA, 0x00, 0xC0, 0xFE, 0x03, 0xD0, 0x87],
        &[0x400, 0x410, 0x420, 0x430],
    )
    .analyze();
    assert!(has(&r, 0, 0x400) && has(&r, 0, 0x410) && has(&r, 0, 0x420));
    assert!(!has(&r, 0, 0x430));
    let t = table(&r, Loc::fixed(0x161));
    assert_eq!((t.entries, t.bound), (3, TableBound::Index));
}

#[test]
fn an_unbounded_table_ends_at_the_first_entry_that_is_not_code() {
    let r = table_cart(&[0xFA, 0x00, 0xC0, 0x87], &[0x400, 0x410, 0x500, 0x420]).analyze();
    assert!(has(&r, 0, 0x400) && has(&r, 0, 0x410));
    assert!(!has(&r, 0, 0x500));
    assert!(!has(&r, 0, 0x420), "the table must not run across data");
    let t = table(&r, Loc::fixed(0x15E));
    assert_eq!((t.entries, t.bound), (2, TableBound::Run));
}

#[test]
fn a_table_of_data_words_is_not_followed() {
    let r = table_cart(&[0xFA, 0x00, 0xC0, 0x87], &[0x500, 0x500]).analyze();
    assert!(!has(&r, 0, 0x500));
    assert_eq!(kinds(&r), vec![UnresolvedKind::JumpTable]);
}

#[test]
fn an_index_scaled_by_a_pure_helper_bounds_its_table() {
    let mut words = Vec::new();
    for w in [0x400u16, 0x410, 0x420, 0x430] {
        words.extend_from_slice(&w.to_le_bytes());
        words.push(0);
    }
    let r = Cart::new(0x19, 0x02)
        .put(
            0,
            0x150,
            &[
                0xFA, 0x00, 0xC0, 0xFE, 0x03, 0xD0, 0xCD, 0x50, 0x02, 0x11, 0x00, 0x03, 0x19, 0x2A,
                0x66, 0x6F, 0xE9,
            ],
        )
        .put(0, 0x250, &[0x6F, 0x26, 0x00, 0x54, 0x5D, 0x29, 0x19, 0xC9])
        .put(0, 0x300, &words)
        .put(0, 0x400, &[0xC9])
        .put(0, 0x410, &[0xC9])
        .put(0, 0x420, &[0xC9])
        .put(0, 0x430, &[0xC9])
        .analyze();
    assert!(has(&r, 0, 0x400) && has(&r, 0, 0x410) && has(&r, 0, 0x420));
    assert!(!has(&r, 0, 0x430));
    let t = table(&r, Loc::fixed(0x160));
    assert_eq!((t.step, t.entries, t.bound), (3, 3, TableBound::Index));
}

#[test]
fn a_code_pointer_passed_in_a_register_is_followed_at_the_caller() {
    let r = Cart::new(0x19, 0x02)
        .put(0, 0x150, &[0x11, 0x00, 0x04, 0xCD, 0x00, 0x02, 0xC9])
        .put(0, 0x200, &[0xD5, 0xC9])
        .put(0, 0x400, &[0xC9])
        .analyze();
    assert!(has(&r, 0, 0x400), "{:?}", r.functions.keys());
    assert!(r.unresolved.is_empty(), "{:?}", r.unresolved);
}

#[test]
fn a_table_entry_inside_a_found_instruction_is_not_followed() {
    let r = table_cart(&[0xFA, 0x00, 0xC0, 0x87], &[0x151, 0x400]).analyze();
    assert!(!r.functions.contains_key(&Loc::fixed(0x151)));
}

#[test]
fn a_call_that_returns_past_an_inline_string_continues_after_it() {
    let r = Cart::new(0x19, 0x02)
        .put(
            0,
            0x150,
            &[0xCD, 0x00, 0x03, 0x41, 0x42, 0x00, 0xCD, 0x00, 0x04, 0xC9],
        )
        .put(0, 0x300, &[0xE1, 0xCD, 0x10, 0x03, 0xE9])
        .put(0, 0x310, &[0x2A, 0xB7, 0x20, 0xFC, 0xC9])
        .put(0, 0x400, &[0xC9])
        .analyze();
    assert!(
        has(&r, 0, 0x400),
        "{:?} {:?}",
        r.functions.keys(),
        r.unresolved
    );
    let entry = &r.functions[&Loc::fixed(0x100)];
    assert!(!entry.body.contains_key(&Loc::fixed(0x153)));
}

#[test]
fn a_byte_stored_to_an_io_register_is_not_assumed_to_read_back() {
    let r = Cart::new(0x19, 0x02)
        .put(
            0,
            0x150,
            &[
                0x3E, 0x55, 0xEA, 0xA0, 0xFE, 0xFA, 0xA0, 0xFE, 0xFE, 0x55, 0x28, 0x03, 0xCD, 0x00,
                0x03, 0xC9,
            ],
        )
        .put(0, 0x300, &[0xC9])
        .analyze();
    assert!(has(&r, 0, 0x300), "{:?}", r.functions.keys());
}

#[test]
fn code_after_a_call_that_waits_for_an_interrupt_is_found() {
    let r = Cart::new(0x19, 0x04)
        .put(0, 0x150, &[0xCD, 0x00, 0x02, 0x3E, 0x01, 0x18, 0xFE])
        .put(0, 0x200, &[0xFB, 0x76, 0x18, 0xFC])
        .analyze();
    let entry = &r.functions[&Loc::fixed(0x100)];
    assert!(entry.body.contains_key(&Loc::fixed(0x153)));
}

#[test]
fn code_walked_only_inside_a_caller_is_part_of_the_callee() {
    let r = Cart::new(0x19, 0x04)
        .put(
            0,
            0x150,
            &[0x3E, 0x01, 0xEA, 0x00, 0x20, 0xCD, 0x00, 0x40, 0x18, 0xFE],
        )
        .put(1, 0x4000, &[0xCD, 0x00, 0x02, 0xC9])
        .put(0, 0x200, &[0xCD, 0x00, 0x41, 0x3E, 0x05, 0xC9])
        .put(1, 0x4100, &[0xC9])
        .analyze();
    let f = &r.functions[&Loc::fixed(0x200)];
    assert!(f.body.contains_key(&Loc::fixed(0x203)), "{:?}", f.body);
}

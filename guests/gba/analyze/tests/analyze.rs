//! Discovery on synthetic ROMs assembled here. The body of a synthetic
//! image is a counting pattern, so any instruction found outside the
//! assembled code is false code.

use std::collections::BTreeSet;

use relift_gba_analyze::{
    CopyKind, EdgeKind, Loc, Origin, Report, UnresolvedKind, analyze, analyze_multiboot,
};
use relift_gba_formats::synth::{self, Spec};

const BASE: u32 = 0x0800_0000;

struct Asm {
    rom: Vec<u8>,
}

impl Asm {
    fn new() -> Asm {
        Asm {
            rom: synth::rom(&Spec {
                len: 0x1000,
                ..Spec::default()
            }),
        }
    }

    fn w32(&mut self, addr: u32, v: u32) {
        let o = (addr - BASE) as usize;
        self.rom[o..o + 4].copy_from_slice(&v.to_le_bytes());
    }

    fn w16(&mut self, addr: u32, v: u16) {
        let o = (addr - BASE) as usize;
        self.rom[o..o + 2].copy_from_slice(&v.to_le_bytes());
    }

    fn arm(&mut self, addr: u32, words: &[u32]) {
        for (i, w) in words.iter().enumerate() {
            self.w32(addr + 4 * i as u32, *w);
        }
    }

    fn thumb(&mut self, addr: u32, halves: &[u16]) {
        for (i, h) in halves.iter().enumerate() {
            self.w16(addr + 2 * i as u32, *h);
        }
    }

    fn entry_to(&mut self, target: u32) {
        self.w32(BASE, a_b(BASE, target));
        synth::fix_complement(&mut self.rom);
    }

    fn run(mut self, entry: u32) -> Report {
        self.entry_to(entry);
        analyze(&self.rom).expect("synthetic image analyzes")
    }
}

fn a_ldr_lit(rd: u32, at: u32, pool: u32) -> u32 {
    0xE59F_0000 | rd << 12 | (pool - (at + 8))
}

fn a_bx(rm: u32) -> u32 {
    0xE12F_FF10 | rm
}

fn a_b(at: u32, to: u32) -> u32 {
    0xEA00_0000 | (to.wrapping_sub(at + 8) >> 2 & 0x00FF_FFFF)
}

fn a_str(rd: u32, rn: u32) -> u32 {
    0xE580_0000 | rn << 16 | rd << 12
}

fn a_swi(n: u32) -> u32 {
    0xEF00_0000 | n << 16
}

const A_MOV_LR_PC: u32 = 0xE1A0_E00F;
const A_BX_LR: u32 = 0xE12F_FF1E;
const A_MOV_R0_1: u32 = 0xE3A0_0001;

fn t_ldr_lit(rd: u16, at: u32, pool: u32) -> u16 {
    let pc = (at + 4) & !3;
    0x4800 | rd << 8 | ((pool - pc) / 4) as u16
}

fn t_bl(at: u32, to: u32) -> [u16; 2] {
    let off = to.wrapping_sub(at + 4);
    [
        0xF000 | (off >> 12 & 0x7FF) as u16,
        0xF800 | (off >> 1 & 0x7FF) as u16,
    ]
}

fn t_b(at: u32, to: u32) -> u16 {
    0xE000 | (to.wrapping_sub(at + 4) >> 1 & 0x7FF) as u16
}

fn t_bhi(at: u32, to: u32) -> u16 {
    0xD800 | (to.wrapping_sub(at + 4) >> 1 & 0xFF) as u16
}

fn t_bx(rm: u16) -> u16 {
    0x4700 | rm << 3
}

fn t_mov_pc(rm: u16) -> u16 {
    0x4687 | rm << 3
}

const T_PUSH_LR: u16 = 0xB500;
const T_POP_R0: u16 = 0xBC01;
const T_BX_LR: u16 = 0x4770;
const T_MOVS_R0_1: u16 = 0x2001;
const T_CMP_R0_2: u16 = 0x2802;
const T_LSLS_R0_R0_2: u16 = 0x0080;
const T_ADDS_R0_R0_R1: u16 = 0x1840;
const T_LDR_R0_R0: u16 = 0x6800;
const T_STR_R0_R4: u16 = 0x6020;

fn insts(r: &Report) -> BTreeSet<(u32, bool)> {
    r.instructions().keys().map(|l| (l.addr, l.thumb)).collect()
}

fn arm(addrs: &[u32]) -> Vec<(u32, bool)> {
    addrs.iter().map(|a| (*a, false)).collect()
}

fn thumb(addrs: &[u32]) -> Vec<(u32, bool)> {
    addrs.iter().map(|a| (*a, true)).collect()
}

fn set(parts: &[Vec<(u32, bool)>]) -> BTreeSet<(u32, bool)> {
    parts.iter().flatten().copied().collect()
}

#[test]
fn bx_interworks_and_thumb_bl_pairs_call() {
    let mut a = Asm::new();
    let e = 0x0800_0100;
    a.arm(
        e,
        &[a_ldr_lit(0, e, e + 0xC), a_bx(0), 0xE7FF_DEFE, 0x0800_0201],
    );
    let t = 0x0800_0200;
    let bl = t_bl(t + 2, 0x0800_0240);
    a.thumb(t, &[T_PUSH_LR, bl[0], bl[1], T_POP_R0, t_bx(0)]);
    a.thumb(0x0800_0240, &[T_MOVS_R0_1, T_BX_LR]);
    let r = a.run(e);
    assert_eq!(
        insts(&r),
        set(&[
            arm(&[BASE, e, e + 4]),
            thumb(&[t, t + 2, t + 4, t + 6, t + 8, 0x0800_0240, 0x0800_0242]),
        ])
    );
    assert!(r.data.contains(&(e + 0xC, 4)));
    let callee = r
        .functions
        .get(&Loc::rom(0x0800_0240, true))
        .expect("callee");
    assert_eq!(callee.origin, Origin::Called);
    assert!(callee.returns);
    assert!(r.unresolved.is_empty(), "{:?}", r.unresolved);
}

#[test]
fn literal_pools_are_data_not_code() {
    let mut a = Asm::new();
    let t = 0x0800_0200;
    a.arm(
        0x0800_0100,
        &[a_ldr_lit(0, 0x0800_0100, 0x0800_0108), a_bx(0), t | 1],
    );
    a.thumb(
        t,
        &[
            t_ldr_lit(1, t, t + 8),
            t_b(t + 2, t + 0xC),
            0xBEEF,
            0xBEEF,
            0x1234,
            0x5678,
            T_BX_LR,
        ],
    );
    let r = a.run(0x0800_0100);
    let found = insts(&r);
    assert!(found.contains(&(t + 0xC, true)));
    for at in [t + 4, t + 6, t + 8, t + 0xA] {
        assert!(!found.contains(&(at, true)), "{at:#x} decoded");
    }
    assert!(r.data.contains(&(t + 8, 4)));
    assert!(r.conflicts.is_empty());
}

#[test]
fn bounded_thumb_jump_table_resolves_every_case() {
    let mut a = Asm::new();
    let t = 0x0800_0200;
    a.arm(
        0x0800_0100,
        &[a_ldr_lit(1, 0x0800_0100, 0x0800_0108), a_bx(1), t | 1],
    );
    let table = 0x0800_0220;
    let cases = [0x0800_0240, 0x0800_0244, 0x0800_0248];
    a.thumb(
        t,
        &[
            T_CMP_R0_2,
            t_bhi(t + 2, t + 0x10),
            T_LSLS_R0_R0_2,
            t_ldr_lit(1, t + 6, t + 0x1C),
            T_ADDS_R0_R0_R1,
            T_LDR_R0_R0,
            t_mov_pc(0),
            0,
            T_BX_LR,
        ],
    );
    a.w32(t + 0x1C, table);
    for (i, c) in cases.iter().enumerate() {
        a.w32(table + 4 * i as u32, *c);
        a.thumb(*c, &[T_MOVS_R0_1, T_BX_LR]);
    }
    a.w32(table + 12, 0x0800_0301);
    let r = a.run(0x0800_0100);
    let f = r.functions.get(&Loc::rom(BASE, false)).expect("switch");
    assert_eq!(f.tables.len(), 1);
    assert_eq!(f.tables[0].max, 2);
    assert_eq!(
        f.tables[0]
            .targets
            .iter()
            .map(|l| l.addr)
            .collect::<Vec<_>>(),
        cases
    );
    let found = insts(&r);
    for c in cases {
        assert!(found.contains(&(c, true)));
    }
    assert!(!found.contains(&(0x0800_0300, true)));
    assert!(r.unresolved.is_empty(), "{:?}", r.unresolved);
}

#[test]
fn cpuset_copy_to_iwram_is_lifted_at_its_run_address() {
    let mut a = Asm::new();
    let e = 0x0800_0100;
    let src = 0x0800_0400;
    a.arm(
        e,
        &[
            a_ldr_lit(0, e, e + 0x24),
            a_ldr_lit(1, e + 4, e + 0x28),
            a_ldr_lit(2, e + 8, e + 0x2C),
            a_swi(0x0B),
            a_ldr_lit(0, e + 0x10, e + 0x28),
            A_MOV_LR_PC,
            a_bx(0),
            a_b(e + 0x1C, e + 0x1C),
            0,
            src,
            0x0300_0000,
            2 | 1 << 26,
        ],
    );
    a.arm(src, &[A_MOV_R0_1, A_BX_LR]);
    let r = a.run(e);
    assert_eq!(r.copies.len(), 1);
    let c = &r.copies[0];
    assert_eq!(c.kind, CopyKind::CpuSet);
    assert_eq!((c.src, c.dst, c.len()), (Some(src), 0x0300_0000, 8));
    let ram = Loc {
        addr: 0x0300_0000,
        thumb: false,
        copy: Some(0),
    };
    let f = r.functions.get(&ram).expect("function in IWRAM");
    assert_eq!(f.instructions.len(), 2);
    let caller = r.functions.get(&Loc::rom(BASE, false)).expect("caller");
    assert!(
        caller
            .edges
            .iter()
            .any(|x| x.target == ram && x.kind == EdgeKind::LinkedJump)
    );
    assert!(insts(&r).contains(&(e + 0x1C, false)));
    assert_eq!(r.swis.len(), 1);
}

#[test]
fn irq_vector_store_roots_the_handler() {
    let mut a = Asm::new();
    let e = 0x0800_0100;
    a.arm(
        e,
        &[
            a_ldr_lit(0, e, e + 0x10),
            a_ldr_lit(1, e + 4, e + 0x14),
            a_str(0, 1),
            a_b(e + 0xC, e + 0xC),
            0x0800_0300,
            0x0300_7FFC,
        ],
    );
    a.arm(0x0800_0300, &[A_BX_LR]);
    let r = a.run(e);
    let h = Loc::rom(0x0800_0300, false);
    assert_eq!(r.irq_handlers, vec![h]);
    assert_eq!(r.functions[&h].origin, Origin::Irq);
}

#[test]
fn pointer_stored_in_ram_resolves_a_later_call() {
    let mut a = Asm::new();
    let e = 0x0800_0100;
    a.arm(
        e,
        &[
            a_ldr_lit(0, e, e + 0x24),
            a_ldr_lit(1, e + 4, e + 0x28),
            a_str(0, 1),
            0xE591_0000,
            A_MOV_LR_PC,
            a_bx(0),
            a_b(e + 0x18, e + 0x18),
            0,
            0,
            0x0800_0301,
            0x0300_1000,
        ],
    );
    a.thumb(0x0800_0300, &[T_MOVS_R0_1, T_BX_LR]);
    let r = a.run(e);
    let t = Loc::rom(0x0800_0300, true);
    assert!(r.functions.contains_key(&t));
    assert!(
        r.functions[&Loc::rom(BASE, false)]
            .edges
            .iter()
            .any(|x| x.target == t && x.kind == EdgeKind::Pointer)
    );
    assert!(insts(&r).contains(&(e + 0x18, false)));
}

#[test]
fn unknown_targets_are_listed_not_guessed() {
    let mut a = Asm::new();
    let e = 0x0800_0100;
    a.arm(e, &[0xE591_0000, a_bx(0)]);
    let r = a.run(e);
    assert_eq!(insts(&r), set(&[arm(&[BASE, e, e + 4])]));
    assert_eq!(r.unresolved.len(), 1);
    assert_eq!(r.unresolved[0].kind, UnresolvedKind::IndirectJump);
    assert_eq!(r.unresolved[0].at, Loc::rom(e + 4, false));
}

#[test]
fn thumb_code_address_in_a_pool_is_an_entry_once_it_lifts_cleanly() {
    let mut a = Asm::new();
    let t = 0x0800_0200;
    a.arm(
        0x0800_0100,
        &[a_ldr_lit(0, 0x0800_0100, 0x0800_0108), a_bx(0), t | 1],
    );
    a.thumb(
        t,
        &[
            t_ldr_lit(0, t, t + 8),
            t_ldr_lit(1, t + 2, t + 0xC),
            T_STR_R0_R4,
            T_BX_LR,
        ],
    );
    a.w32(t + 8, 0x0800_0301);
    a.w32(t + 0xC, 0x0800_0381);
    a.thumb(0x0800_0300, &[T_MOVS_R0_1, T_BX_LR]);
    a.thumb(0x0800_0380, &[0xDE00]);
    let r = a.run(0x0800_0100);
    let good = Loc::rom(0x0800_0300, true);
    assert_eq!(r.functions[&good].origin, Origin::AddressTaken);
    assert!(!r.functions.contains_key(&Loc::rom(0x0800_0380, true)));
}

#[test]
fn shared_epilogue_reached_by_bl_ends_the_callers_frame() {
    let mut a = Asm::new();
    let t = 0x0800_0200;
    a.arm(
        0x0800_0100,
        &[a_ldr_lit(0, 0x0800_0100, 0x0800_0108), a_bx(0), t | 1],
    );
    let epi = 0x0800_0240;
    let bl = t_bl(t + 2, epi);
    a.thumb(t, &[T_PUSH_LR, bl[0], bl[1], 0xFFFF, 0xFFFF]);
    a.thumb(epi, &[T_POP_R0, t_bx(0)]);
    let r = a.run(0x0800_0100);
    let found = insts(&r);
    assert!(!found.contains(&(t + 6, true)));
    assert!(r.functions[&Loc::rom(BASE, false)].returns);
    assert!(r.unresolved.is_empty(), "{:?}", r.unresolved);
}

#[test]
fn multiboot_image_runs_from_work_ram_at_its_entry() {
    let mut mb = synth::multiboot(0x400);
    let at = |a: u32| (a - 0x0200_0000) as usize;
    let e = 0x0200_0100;
    for (i, w) in [
        a_ldr_lit(0, e, e + 0x10),
        A_MOV_LR_PC,
        a_bx(0),
        a_b(e + 0xC, e + 0xC),
        0x0200_0201,
    ]
    .iter()
    .enumerate()
    {
        mb[at(e) + 4 * i..at(e) + 4 * i + 4].copy_from_slice(&w.to_le_bytes());
    }
    mb[at(0x0200_0200)..at(0x0200_0204)]
        .copy_from_slice(&[T_MOVS_R0_1.to_le_bytes(), T_BX_LR.to_le_bytes()].concat());
    let r = analyze_multiboot(&mb).expect("multiboot image analyzes");
    assert_eq!(r.copies[0].kind, CopyKind::Load);
    assert_eq!((r.copies[0].dst, r.copies[0].len()), (0x0200_0000, 0x400));
    let ram = |addr, thumb| Loc {
        addr,
        thumb,
        copy: Some(0),
    };
    let found: BTreeSet<Loc> = r.instructions().keys().copied().collect();
    for l in [
        ram(0x0200_00C0, false),
        ram(e, false),
        ram(e + 0xC, false),
        ram(0x0200_0200, true),
        ram(0x0200_0202, true),
    ] {
        assert!(found.contains(&l), "{l} missed");
    }
    assert_eq!(found.len(), 7);
    assert!(r.data.contains(&(e + 0x10, 4)));
    assert!(analyze(&mb).is_ok());
    assert!(analyze_multiboot(&synth::rom(&Spec::default())).is_err());
}

#[test]
fn dma3_copy_to_ewram_is_lifted_at_its_run_address() {
    let mut a = Asm::new();
    let e = 0x0800_0100;
    let src = 0x0800_0400;
    let dst = 0x0200_0100;
    a.arm(
        e,
        &[
            a_ldr_lit(0, e, e + 0x34),
            a_ldr_lit(1, e + 4, e + 0x38),
            a_str(1, 0),
            a_ldr_lit(1, e + 0xC, e + 0x3C),
            0xE580_1004,
            a_ldr_lit(1, e + 0x14, e + 0x40),
            0xE580_1008,
            a_ldr_lit(0, e + 0x1C, e + 0x3C),
            A_MOV_LR_PC,
            a_bx(0),
            a_b(e + 0x28, e + 0x28),
            0,
            0,
            0x0400_00D4,
            src,
            dst,
            0x8400_0002,
        ],
    );
    a.arm(src, &[A_MOV_R0_1, A_BX_LR]);
    let r = a.run(e);
    assert_eq!(r.copies.len(), 1);
    assert_eq!(r.copies[0].kind, CopyKind::Dma(3));
    assert_eq!((r.copies[0].dst, r.copies[0].len()), (dst, 8));
    let ram = Loc {
        addr: dst,
        thumb: false,
        copy: Some(0),
    };
    assert_eq!(r.functions[&ram].instructions.len(), 2);
}

#[test]
fn copy_loop_called_with_constants_is_run_to_learn_ram_code() {
    let mut a = Asm::new();
    let t = 0x0800_0200;
    let src = 0x0800_0400;
    let copy = 0x0800_0280;
    a.arm(
        0x0800_0100,
        &[a_ldr_lit(0, 0x0800_0100, 0x0800_0108), a_bx(0), t | 1],
    );
    let bl = t_bl(t + 6, copy);
    a.thumb(
        t,
        &[
            t_ldr_lit(0, t, t + 0x10),
            t_ldr_lit(1, t + 2, t + 0x14),
            0x2208,
            bl[0],
            bl[1],
            t_ldr_lit(3, t + 0xA, t + 0x10),
            t_bx(3),
            0,
        ],
    );
    a.w32(t + 0x10, 0x0300_0000);
    a.w32(t + 0x14, src);
    a.thumb(
        copy,
        &[0x780B, 0x7003, 0x3001, 0x3101, 0x3A01, 0xD1F9, T_BX_LR],
    );
    a.arm(src, &[A_MOV_R0_1, A_BX_LR]);
    let r = a.run(0x0800_0100);
    assert_eq!(r.copies.len(), 1);
    assert_eq!(r.copies[0].kind, CopyKind::Run);
    assert_eq!(r.copies[0].src, None);
    assert_eq!(r.copies[0].len(), 8);
    let found: BTreeSet<Loc> = r.instructions().keys().copied().collect();
    let ram = |addr| Loc {
        addr,
        thumb: false,
        copy: Some(0),
    };
    assert!(found.contains(&ram(0x0300_0000)) && found.contains(&ram(0x0300_0004)));
}

#[test]
fn index_times_a_constant_reads_a_table_of_that_stride() {
    let mut a = Asm::new();
    let t = 0x0800_0200;
    a.arm(
        0x0800_0100,
        &[a_ldr_lit(1, 0x0800_0100, 0x0800_0108), a_bx(1), t | 1],
    );
    let table = 0x0800_0300;
    a.thumb(
        t,
        &[
            0x210C,
            0x4348,
            t_ldr_lit(2, t + 4, t + 0xC),
            0x5810,
            t_bx(0),
            0,
        ],
    );
    a.w32(t + 0xC, table);
    let cases = [0x0800_0340, 0x0800_0350];
    for (i, c) in cases.iter().enumerate() {
        a.arm(table + 12 * i as u32, &[c | 1, 0, 0]);
        a.thumb(*c, &[T_MOVS_R0_1, T_BX_LR]);
    }
    a.w32(table + 24, 0x1234_5678);
    let r = a.run(0x0800_0100);
    let found = insts(&r);
    for c in cases {
        assert!(found.contains(&(c, true)), "{c:#x} missed");
    }
    assert!(r.data.contains(&(table, 4)) && r.data.contains(&(table + 12, 4)));
    assert!(!found.iter().any(|(x, _)| (table..table + 0x30).contains(x)));
}

#[test]
fn only_literal_loads_read_the_copy_of_ram_code_as_constants() {
    let mut a = Asm::new();
    let e = 0x0800_0100;
    let src = 0x0800_0400;
    a.arm(
        e,
        &[
            a_ldr_lit(0, e, e + 0x24),
            a_ldr_lit(1, e + 4, e + 0x28),
            a_ldr_lit(2, e + 8, e + 0x2C),
            a_swi(0x0B),
            a_ldr_lit(0, e + 0x10, e + 0x28),
            A_MOV_LR_PC,
            a_bx(0),
            a_b(e + 0x1C, e + 0x1C),
            0,
            src,
            0x0300_0000,
            8 | 1 << 26,
        ],
    );
    a.arm(
        src,
        &[
            a_ldr_lit(1, 0x0300_0000, 0x0300_0018),
            0xE5D1_0000,
            0xE350_0000,
            0x012F_FF1E,
            A_MOV_R0_1,
            A_BX_LR,
            0x0300_001C,
            0,
        ],
    );
    let r = a.run(e);
    let ram = |addr| Loc {
        addr,
        thumb: false,
        copy: Some(0),
    };
    let f = &r.functions[&ram(0x0300_0000)];
    assert_eq!(
        f.instructions.keys().map(|l| l.addr).collect::<Vec<_>>(),
        (0..6).map(|i| 0x0300_0000 + 4 * i).collect::<Vec<_>>()
    );
    assert!(r.data.contains(&(0x0300_0018, 4)));
    assert!(!r.data.iter().any(|(x, _)| *x == 0x0300_001C));
}

#[test]
fn jump_to_another_functions_entry_returns_where_that_function_returns() {
    let mut a = Asm::new();
    let t = 0x0800_0200;
    let f = 0x0800_0280;
    let veneer = 0x0800_0260;
    a.arm(
        0x0800_0100,
        &[a_ldr_lit(0, 0x0800_0100, 0x0800_0108), a_bx(0), t | 1],
    );
    let call_f = t_bl(t + 2, f);
    let call_v = t_bl(t + 6, veneer);
    a.thumb(
        t,
        &[
            T_PUSH_LR,
            call_f[0],
            call_f[1],
            call_v[0],
            call_v[1],
            T_MOVS_R0_1,
            T_POP_R0,
            t_bx(0),
        ],
    );
    a.thumb(veneer, &[t_ldr_lit(3, veneer, veneer + 4), t_bx(3)]);
    a.w32(veneer + 4, f | 1);
    a.thumb(f, &[T_MOVS_R0_1, T_BX_LR]);
    let r = a.run(0x0800_0100);
    let found = insts(&r);
    assert!(found.contains(&(t + 0xA, true)));
    assert!(found.contains(&(t + 0xE, true)));
    let v = &r.functions[&Loc::rom(veneer, true)];
    assert!(v.returns);
    assert!(
        v.edges
            .iter()
            .any(|x| x.target == Loc::rom(f, true) && x.kind == EdgeKind::Tail)
    );
}

#[test]
fn masked_index_reads_only_the_entries_it_can_reach() {
    let mut a = Asm::new();
    let t = 0x0800_0200;
    a.arm(
        0x0800_0100,
        &[a_ldr_lit(1, 0x0800_0100, 0x0800_0108), a_bx(1), t | 1],
    );
    let table = 0x0800_0300;
    a.thumb(
        t,
        &[
            0x220C,
            0x4010,
            t_ldr_lit(1, t + 4, t + 0xC),
            0x5808,
            t_bx(0),
            0,
        ],
    );
    a.w32(t + 0xC, table);
    let cases: Vec<u32> = (0..5).map(|i| 0x0800_0340 + 0x10 * i).collect();
    for (i, c) in cases.iter().enumerate() {
        a.w32(table + 4 * i as u32, *c);
        a.arm(*c, &[A_MOV_R0_1, A_BX_LR]);
    }
    let r = a.run(0x0800_0100);
    let found = insts(&r);
    for c in &cases[..4] {
        assert!(found.contains(&(*c, false)), "{c:#x} missed");
    }
    assert!(!found.contains(&(cases[4], false)));
    assert!(!r.data.contains(&(table + 16, 4)));
}

#[test]
fn rom_table_ends_where_the_next_table_starts() {
    let mut a = Asm::new();
    let e = 0x0800_0100;
    let (t, u) = (0x0800_0200, 0x0800_0240);
    a.arm(
        e,
        &[
            a_ldr_lit(0, e, e + 0x20),
            A_MOV_LR_PC,
            a_bx(0),
            a_ldr_lit(0, e + 0xC, e + 0x24),
            A_MOV_LR_PC,
            a_bx(0),
            a_b(e + 0x18, e + 0x18),
            0,
            t | 1,
            u | 1,
        ],
    );
    let (ta, tb) = (0x0800_0300, 0x0800_030C);
    for (f, table) in [(t, ta), (u, tb)] {
        a.thumb(
            f,
            &[
                T_LSLS_R0_R0_2,
                t_ldr_lit(1, f + 2, f + 0xC),
                0x5808,
                t_bx(0),
                0,
                0,
            ],
        );
        a.w32(f + 0xC, table);
    }
    let cases: Vec<u32> = (0..5).map(|i| 0x0800_0340 + 0x10 * i).collect();
    for (i, c) in cases.iter().enumerate() {
        a.w32(ta + 4 * i as u32, *c);
        a.arm(*c, &[A_MOV_R0_1, A_BX_LR]);
    }
    a.w32(tb + 8, 0x1234_5678);
    let r = a.run(e);
    let body = |f: u32| -> BTreeSet<u32> {
        r.functions[&Loc::rom(f, true)]
            .instructions
            .keys()
            .map(|l| l.addr)
            .collect()
    };
    let (bt, bu) = (body(t), body(u));
    for c in &cases[..3] {
        assert!(bt.contains(c) && !bu.contains(c), "{c:#x}");
    }
    for c in &cases[3..] {
        assert!(bu.contains(c) && !bt.contains(c), "{c:#x}");
    }
}

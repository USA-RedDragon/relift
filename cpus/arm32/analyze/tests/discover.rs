//! Discovery on tiny code images assembled here, one rule each, on a
//! machine of image segments in main RAM.

use relift_cpu_arm32_analyze::{
    BiosCall, Config, CopyKind, Dma, Kind, Limit, Loc, Options, Origin, Platform, Profile, RamCopy,
    Report, discover,
};

const BASE: u32 = 0x0200_0000;
const OTHER: u32 = 0x0210_0000;

struct Machine {
    profile: Profile,
    segments: Vec<(u32, Vec<u8>)>,
    external: Option<(u32, u32)>,
    irq_entry: Option<(u32, u32)>,
    writable: bool,
}

impl Machine {
    fn new(profile: Profile) -> Machine {
        Machine {
            profile,
            segments: vec![(BASE, vec![0; 0x1000])],
            external: None,
            irq_entry: None,
            writable: false,
        }
    }

    fn segment(&mut self, base: u32) {
        self.segments.push((base, vec![0; 0x1000]));
    }

    fn put(&mut self, addr: u32, bytes: &[u8]) {
        let (b, d) = self
            .segments
            .iter_mut()
            .find(|(b, d)| addr.wrapping_sub(*b) < d.len() as u32)
            .expect("address in a segment");
        let o = (addr - *b) as usize;
        d[o..o + bytes.len()].copy_from_slice(bytes);
    }

    fn arm(&mut self, addr: u32, words: &[u32]) {
        for (i, w) in words.iter().enumerate() {
            self.put(addr + 4 * i as u32, &w.to_le_bytes());
        }
    }

    fn thumb(&mut self, addr: u32, halves: &[u16]) {
        for (i, h) in halves.iter().enumerate() {
            self.put(addr + 2 * i as u32, &h.to_le_bytes());
        }
    }

    fn run(&self, options: Options, root: Loc) -> Report {
        let config = Config {
            options,
            roots: vec![root],
            ..Config::default()
        };
        discover(self, &config)
    }
}

impl Platform for Machine {
    fn profile(&self) -> Profile {
        self.profile
    }

    fn kind(&self, addr: u32) -> Kind {
        if self.image(addr, 1).is_some() {
            Kind::Image
        } else if addr >> 24 == 0x02 {
            Kind::Ram
        } else {
            Kind::Other
        }
    }

    fn image(&self, addr: u32, n: u32) -> Option<&[u8]> {
        self.segments.iter().find_map(|(b, d)| {
            let o = addr.checked_sub(*b)? as usize;
            d.get(o..o + n as usize)
        })
    }

    fn code_addr(&self, addr: u32) -> bool {
        self.image(addr, 1).is_some()
    }

    fn irq_entry(&self) -> Option<(u32, u32)> {
        self.irq_entry
    }

    fn irq_vector(&self, _: u32) -> bool {
        false
    }

    fn bios_call(&self, _: u8) -> BiosCall {
        BiosCall::Returns
    }

    fn dma(&self, _: &Dma) -> Option<(u32, u32, u32)> {
        None
    }

    fn segments(&self) -> Vec<(u32, &[u8])> {
        self.segments
            .iter()
            .map(|(b, d)| (*b, d.as_slice()))
            .collect()
    }

    fn image_writable(&self) -> bool {
        self.writable
    }

    fn external(&self, addr: u32) -> bool {
        self.external
            .is_some_and(|(lo, hi)| (lo..hi).contains(&addr))
    }
}

fn a_ldr_lit(rd: u32, at: u32, pool: u32) -> u32 {
    0xE59F_0000 | rd << 12 | (pool - (at + 8))
}

fn a_b(at: u32, to: u32) -> u32 {
    0xEA00_0000 | (to.wrapping_sub(at + 8) >> 2 & 0x00FF_FFFF)
}

fn a_bl(at: u32, to: u32) -> u32 {
    0xEB00_0000 | (to.wrapping_sub(at + 8) >> 2 & 0x00FF_FFFF)
}

const A_BX_LR: u32 = 0xE12F_FF1E;
const A_BLX_R1: u32 = 0xE12F_FF31;
const A_MOV_R0_1: u32 = 0xE3A0_0001;
const A_MOVEQ_R0_1: u32 = 0x03A0_0001;
const A_PUSH_R4_LR: u32 = 0xE92D_4010;
const A_POP_R4_PC: u32 = 0xE8BD_8010;
const A_CMP_R0_2: u32 = 0xE350_0002;
const A_MOV_R1_R1: u32 = 0xE1A0_1001;
const A_ADDLS_PC_R0_LSL2: u32 = 0x908F_F100;
const A_ADD_PC_R0_LSL2: u32 = 0xE08F_F100;
const A_AND_R0_R0: u32 = 0xE000_0000;
const T_MOVS_R0_1: u16 = 0x2001;
const T_BX_LR: u16 = 0x4770;

fn arm(addr: u32) -> Loc {
    Loc::rom(addr, false)
}

fn thumb(addr: u32) -> Loc {
    Loc::rom(addr, true)
}

fn found(r: &Report, l: Loc) -> bool {
    r.instructions().contains_key(&l)
}

fn all() -> Options {
    Options {
        arm_pointers: true,
        scan_words: true,
        switches: true,
        image_calls: true,
        reachable_locals: true,
    }
}

#[test]
fn an_even_word_code_reads_is_an_arm_function_with_arm_pointers() {
    let mut m = Machine::new(Profile::V5te);
    m.arm(
        BASE,
        &[
            a_ldr_lit(0, BASE, BASE + 0x100),
            a_ldr_lit(1, BASE + 4, BASE + 0x104),
            A_BX_LR,
        ],
    );
    m.arm(BASE + 0x100, &[BASE + 0x200, BASE + 0x300]);
    m.arm(BASE + 0x200, &[A_MOV_R0_1, A_BX_LR]);
    m.arm(BASE + 0x300, &[A_MOVEQ_R0_1, A_BX_LR]);
    let off = m.run(Options::default(), arm(BASE));
    assert!(!found(&off, arm(BASE + 0x200)));
    let on = m.run(
        Options {
            arm_pointers: true,
            ..Options::default()
        },
        arm(BASE),
    );
    assert_eq!(
        on.functions.get(&arm(BASE + 0x200)).map(|f| f.origin),
        Some(Origin::AddressTaken)
    );
    assert!(
        !found(&on, arm(BASE + 0x300)),
        "a conditional first instruction is no function entry"
    );
}

#[test]
fn a_load_of_the_pc_interworks_on_armv5_only() {
    let mut m = Machine::new(Profile::V5te);
    m.arm(BASE, &[a_ldr_lit(15, BASE, BASE + 0x100)]);
    m.arm(BASE + 0x100, &[BASE + 0x201]);
    m.thumb(BASE + 0x200, &[T_MOVS_R0_1, T_BX_LR]);
    let v5 = m.run(Options::default(), arm(BASE));
    assert!(found(&v5, thumb(BASE + 0x200)));
    m.profile = Profile::V4t;
    let v4 = m.run(Options::default(), arm(BASE));
    assert!(!found(&v4, thumb(BASE + 0x200)));
}

#[test]
fn every_image_word_is_a_candidate_with_scan_words() {
    let mut m = Machine::new(Profile::V5te);
    m.segment(OTHER);
    m.arm(BASE, &[A_BX_LR]);
    m.arm(BASE + 0x800, &[BASE + 0x201, OTHER, OTHER + 0x100]);
    m.thumb(BASE + 0x200, &[T_MOVS_R0_1, T_BX_LR]);
    m.arm(OTHER, &[A_PUSH_R4_LR, A_MOV_R0_1, A_POP_R4_PC]);
    m.arm(OTHER + 0x100, &[A_MOV_R0_1, A_BX_LR]);
    let off = m.run(
        Options {
            scan_words: false,
            ..all()
        },
        arm(BASE),
    );
    assert!(!found(&off, thumb(BASE + 0x200)));
    let on = m.run(all(), arm(BASE));
    assert!(found(&on, thumb(BASE + 0x200)));
    assert!(found(&on, arm(OTHER)), "a word in another segment");
    assert!(
        !found(&on, arm(OTHER + 0x100)),
        "a word in another segment needs code that opens a frame"
    );
}

#[test]
fn a_scanned_thumb_candidate_over_arm_code_is_rejected() {
    let mut m = Machine::new(Profile::V5te);
    m.arm(BASE, &[A_BX_LR]);
    m.arm(BASE + 0x400, &[A_AND_R0_R0; 8]);
    m.arm(BASE + 0x420, &[u32::from(T_BX_LR) << 16]);
    m.arm(BASE + 0x800, &[BASE + 0x401, BASE + 0x601]);
    m.thumb(BASE + 0x600, &[T_MOVS_R0_1, T_BX_LR]);
    let r = m.run(all(), arm(BASE));
    assert!(found(&r, thumb(BASE + 0x600)), "control: real Thumb code");
    assert!(!found(&r, thumb(BASE + 0x400)));
    let mut read = Machine::new(Profile::V5te);
    read.segments = m.segments.clone();
    read.arm(BASE, &[a_ldr_lit(0, BASE, BASE + 0x800), A_BX_LR]);
    let taken = read.run(all(), arm(BASE));
    assert!(
        found(&taken, thumb(BASE + 0x400)),
        "the rule is the scan's: a word code reads stays a candidate"
    );
}

fn switch(m: &mut Machine) {
    m.arm(
        BASE,
        &[
            A_CMP_R0_2,
            A_MOV_R1_R1,
            A_ADDLS_PC_R0_LSL2,
            a_b(BASE + 0xC, BASE + 0x100),
            a_b(BASE + 0x10, BASE + 0x200),
            a_b(BASE + 0x14, BASE + 0x300),
            a_b(BASE + 0x18, BASE + 0x400),
        ],
    );
    for a in [0x100, 0x200, 0x300, 0x400] {
        m.arm(BASE + a, &[A_MOV_R0_1, A_BX_LR]);
    }
}

#[test]
fn a_compare_further_back_bounds_a_switch() {
    let mut m = Machine::new(Profile::V5te);
    switch(&mut m);
    let off = m.run(Options::default(), arm(BASE));
    assert!(found(&off, arm(BASE + 0x100)), "control: the default");
    assert!(!found(&off, arm(BASE + 0x200)));
    let on = m.run(
        Options {
            switches: true,
            ..Options::default()
        },
        arm(BASE),
    );
    for a in [0x100, 0x200, 0x300, 0x400] {
        assert!(found(&on, arm(BASE + a)), "{a:#x}");
    }
    assert_eq!(on.functions[&arm(BASE)].tables.len(), 1);
}

#[test]
fn an_unbounded_add_pc_enters_a_branch_table_or_unrolled_code() {
    let opts = Options {
        switches: true,
        ..Options::default()
    };
    let mut m = Machine::new(Profile::V5te);
    m.arm(
        BASE,
        &[
            A_ADD_PC_R0_LSL2,
            A_MOV_R0_1,
            a_b(BASE + 8, BASE + 0x100),
            a_b(BASE + 0xC, BASE + 0x200),
            A_MOV_R0_1,
            A_BX_LR,
        ],
    );
    m.arm(BASE + 0x100, &[A_MOV_R0_1, A_BX_LR]);
    m.arm(BASE + 0x200, &[A_MOV_R0_1, A_BX_LR]);
    let off = m.run(Options::default(), arm(BASE));
    assert!(!found(&off, arm(BASE + 0x100)));
    let on = m.run(opts, arm(BASE));
    assert!(found(&on, arm(BASE + 0x100)));
    assert!(found(&on, arm(BASE + 0x200)));
    assert!(
        !found(&on, arm(BASE + 0x10)),
        "the table ends at the first instruction that is no branch"
    );
    let mut u = Machine::new(Profile::V5te);
    u.arm(
        BASE,
        &[
            A_ADD_PC_R0_LSL2,
            A_MOV_R0_1,
            A_MOV_R0_1,
            A_MOV_R0_1,
            A_BX_LR,
        ],
    );
    let unrolled = u.run(opts, arm(BASE));
    assert!(found(&unrolled, arm(BASE + 8)));
    assert!(found(&unrolled, arm(BASE + 0x10)));
    assert!(!found(&unrolled, arm(BASE + 4)));
}

#[test]
fn an_unbounded_sub_pc_enters_the_unrolled_code_after_it() {
    let opts = Options {
        switches: true,
        ..Options::default()
    };
    let mut m = Machine::new(Profile::V5te);
    m.arm(
        BASE,
        &[0xE04F_F002, A_MOV_R0_1, A_MOV_R0_1, A_MOV_R0_1, A_BX_LR],
    );
    let on = m.run(opts, arm(BASE));
    assert!(found(&on, arm(BASE + 8)));
    assert!(found(&on, arm(BASE + 0x10)));
    assert!(!found(&on, arm(BASE + 4)));
}

#[test]
fn the_return_of_a_function_s_call_to_itself_is_found() {
    let mut m = Machine::new(Profile::V5te);
    m.arm(
        BASE,
        &[
            A_PUSH_R4_LR,
            A_CMP_R0_2,
            0x18BD_8010,
            a_bl(BASE + 0xC, BASE),
            A_MOV_R0_1,
            A_POP_R4_PC,
        ],
    );
    m.arm(
        BASE + 0x100,
        &[A_PUSH_R4_LR, a_bl(BASE + 0x104, BASE), A_POP_R4_PC],
    );
    let r = m.run(Options::default(), arm(BASE + 0x100));
    assert!(found(&r, arm(BASE + 0x10)));
    assert!(found(&r, arm(BASE + 0x14)));
}

#[test]
fn calls_into_another_unit_seed_its_run() {
    let mut a = Machine::new(Profile::V5te);
    a.arm(
        BASE,
        &[A_PUSH_R4_LR, a_bl(BASE + 4, OTHER), A_MOV_R0_1, A_POP_R4_PC],
    );
    let alone = a.run(Options::default(), arm(BASE));
    assert!(alone.outside.contains(&arm(OTHER)));
    a.external = Some((OTHER, OTHER + 0x1000));
    let linked = a.run(Options::default(), arm(BASE));
    assert!(
        found(&linked, arm(BASE + 8)),
        "a call into another unit returns"
    );
    let mut b = Machine::new(Profile::V5te);
    b.segments = vec![(OTHER, vec![0; 0x1000])];
    b.arm(OTHER, &[A_PUSH_R4_LR, A_MOV_R0_1, A_POP_R4_PC]);
    b.arm(OTHER + 0x100, &[A_MOV_R0_1, A_BX_LR]);
    let config = Config {
        seeds: vec![arm(OTHER), arm(OTHER + 0x100)],
        ..Config::default()
    };
    let seeded = discover(&b, &config);
    assert!(found(&seeded, arm(OTHER)));
    assert!(found(&seeded, arm(OTHER + 0x100)));
}

#[test]
fn a_call_may_clear_a_flag_it_was_given_the_address_of() {
    let mut m = Machine::new(Profile::V5te);
    m.arm(
        BASE,
        &[
            A_PUSH_R4_LR,
            0xE24D_D008,
            0xE3A0_4001,
            0xE58D_4004,
            0xE28D_0004,
            a_bl(BASE + 0x14, BASE + 0x100),
            0xE59D_0004,
            0xE350_0000,
            0x1AFF_FFFC,
            0xE28D_D008,
            A_POP_R4_PC,
        ],
    );
    m.arm(BASE + 0x100, &[A_BX_LR]);
    let off = m.run(Options::default(), arm(BASE));
    assert!(
        !found(&off, arm(BASE + 0x24)),
        "control: the flag reads as the 1 stored"
    );
    let on = m.run(
        Options {
            reachable_locals: true,
            ..Options::default()
        },
        arm(BASE),
    );
    assert!(found(&on, arm(BASE + 0x24)));
    assert!(on.functions[&arm(BASE)].returns);
}

#[test]
fn a_call_is_noreturn_only_when_its_callee_is() {
    let mut m = Machine::new(Profile::V5te);
    m.arm(
        BASE,
        &[
            A_PUSH_R4_LR,
            a_bl(BASE + 4, 0x0100_0000),
            A_MOV_R0_1,
            a_ldr_lit(1, BASE + 0xC, BASE + 0x100),
            A_BLX_R1,
            A_MOV_R0_1,
            a_bl(BASE + 0x18, BASE + 0x200),
            A_MOV_R0_1,
            A_POP_R4_PC,
        ],
    );
    m.arm(BASE + 0x100, &[0x0300_0000]);
    m.arm(BASE + 0x200, &[a_b(BASE + 0x200, BASE + 0x200)]);
    let r = m.run(Options::default(), arm(BASE));
    assert!(found(&r, arm(BASE + 8)), "a call outside code may return");
    assert!(
        found(&r, arm(BASE + 0x14)),
        "a call through a register to no code may return"
    );
    assert!(
        !found(&r, arm(BASE + 0x1C)),
        "control: a call to a loop that never leaves does not return"
    );
}

#[test]
fn a_call_to_a_veneer_into_ram_no_copy_holds_returns() {
    let mut m = Machine::new(Profile::V5te);
    m.arm(
        BASE,
        &[
            A_PUSH_R4_LR,
            a_bl(BASE + 4, BASE + 0x40),
            A_MOV_R0_1,
            A_POP_R4_PC,
        ],
    );
    m.arm(
        BASE + 0x40,
        &[
            a_ldr_lit(12, BASE + 0x40, BASE + 0x48),
            0xE12F_FF1C,
            0x0230_0000,
        ],
    );
    let r = m.run(Options::default(), arm(BASE));
    assert!(
        found(&r, arm(BASE + 8)),
        "a jump to code another unit holds returns to the veneer's caller"
    );
}

#[test]
fn an_image_word_code_stores_to_is_a_variable() {
    let flag = BASE + 0x800;
    let mut m = Machine::new(Profile::V5te);
    m.arm(
        BASE,
        &[
            A_PUSH_R4_LR,
            a_ldr_lit(4, BASE + 4, BASE + 0x100),
            0xE594_0000,
            0xE350_0000,
            0x0A00_0001,
            A_MOV_R0_1,
            A_POP_R4_PC,
            A_MOV_R0_1,
            A_POP_R4_PC,
        ],
    );
    m.arm(BASE + 0x100, &[flag]);
    let never = m.run(Options::default(), arm(BASE));
    assert!(
        !found(&never, arm(BASE + 0x14)),
        "control: an image word no code stores to keeps its value"
    );
    m.arm(
        BASE + 0x200,
        &[
            a_ldr_lit(1, BASE + 0x200, BASE + 0x210),
            0xE581_0000,
            A_BX_LR,
        ],
    );
    m.arm(
        BASE + 0x40,
        &[
            A_PUSH_R4_LR,
            a_bl(BASE + 0x44, BASE + 0x200),
            a_bl(BASE + 0x48, BASE),
            A_POP_R4_PC,
        ],
    );
    m.arm(BASE + 0x210, &[flag]);
    let stored = m.run(Options::default(), arm(BASE + 0x40));
    assert!(
        found(&stored, arm(BASE + 0x14)),
        "a store makes the word a variable: its first value is no constant"
    );
}

#[test]
fn a_linked_jump_through_an_empty_table_returns() {
    let mut m = Machine::new(Profile::V5te);
    m.arm(
        BASE,
        &[
            A_PUSH_R4_LR,
            a_ldr_lit(4, BASE + 4, BASE + 0x20),
            0xE794_3100,
            0xE1A0_E00F,
            0xE12F_FF13,
            A_MOV_R0_1,
            A_POP_R4_PC,
        ],
    );
    m.arm(BASE + 0x20, &[BASE + 0x800]);
    let r = m.run(Options::default(), arm(BASE));
    assert!(
        found(&r, arm(BASE + 0x14)),
        "the instruction after a linked jump is reached"
    );
}

#[test]
fn a_flag_loaded_from_a_writable_image_decides_no_branch() {
    let mut m = Machine::new(Profile::V5te);
    m.arm(
        BASE,
        &[
            a_ldr_lit(1, BASE, BASE + 0x20),
            0xE591_0000,
            0xE350_0000,
            0x0A00_0000,
            A_MOV_R0_1,
            A_BX_LR,
        ],
    );
    m.arm(BASE + 0x20, &[BASE + 0x40]);
    let ro = m.run(Options::default(), arm(BASE));
    assert!(
        !found(&ro, arm(BASE + 0x10)),
        "a read-only zero flag takes the branch"
    );
    m.writable = true;
    let rw = m.run(Options::default(), arm(BASE));
    assert!(found(&rw, arm(BASE + 0x10)));
}

#[test]
fn a_leaf_seed_is_an_entry_when_its_run_checks_out() {
    let mut m = Machine::new(Profile::V5te);
    m.segments = vec![(OTHER, vec![0; 0x1000])];
    m.arm(OTHER, &[A_PUSH_R4_LR, A_MOV_R0_1, A_POP_R4_PC]);
    m.arm(OTHER + 0x100, &[A_MOV_R0_1, A_BX_LR]);
    m.arm(OTHER + 0x200, &[0xE7F0_00F0]);
    let config = Config {
        seeds: vec![arm(OTHER + 0x100), arm(OTHER + 0x200)],
        roots: vec![arm(OTHER)],
        ..Config::default()
    };
    let r = discover(&m, &config);
    assert!(found(&r, arm(OTHER + 0x104)));
    assert!(
        !found(&r, arm(OTHER + 0x200)),
        "an undefined seed is no entry"
    );
}

#[test]
fn a_call_through_a_word_that_also_holds_unknown_values_returns() {
    let mut m = Machine::new(Profile::V5te);
    let word = 0x02F0_0000;
    let code = [
        A_PUSH_R4_LR,
        a_ldr_lit(0, BASE + 4, BASE + 0x100),
        a_ldr_lit(1, BASE + 8, BASE + 0x104),
        0xE580_1000,
        0xE592_1000,
        0xE580_1000,
        0xE590_1000,
        A_BLX_R1,
        A_MOV_R0_1,
        A_POP_R4_PC,
    ];
    m.arm(BASE, &code);
    m.arm(BASE + 0x100, &[word, BASE + 0x200]);
    m.arm(BASE + 0x200, &[a_b(BASE + 0x200, BASE + 0x200)]);
    let r = m.run(all(), arm(BASE));
    assert!(
        found(&r, arm(BASE + 4 * 8)),
        "the word's one constant never returns, but its other values may"
    );
}

#[test]
fn code_a_function_copies_with_its_own_loop_and_jumps_to_is_found() {
    let mut m = Machine::new(Profile::V4t);
    let ram = 0x02F0_0000;
    let pool = BASE + 0x80;
    m.arm(
        BASE,
        &[
            a_ldr_lit(0, BASE, pool),
            a_ldr_lit(1, BASE + 4, pool + 4),
            0xE3A0_2002,
            0xE490_3004,
            0xE481_3004,
            0xE252_2001,
            0x1AFF_FFFB,
            a_ldr_lit(0, BASE + 28, pool + 4),
            0xE12F_FF10,
        ],
    );
    m.arm(pool, &[BASE + 0x100, ram]);
    m.arm(BASE + 0x100, &[A_MOV_R0_1, A_BX_LR]);
    let r = m.run(all(), arm(BASE));
    assert!(r.copies.iter().any(|c| c.dst == ram), "{:?}", r.copies);
    assert!(
        r.instructions().keys().any(|l| l.addr == ram),
        "the copied code is lifted where it runs"
    );
}

#[test]
fn code_a_boot_copies_after_it_waits_on_an_io_register_is_found() {
    let mut m = Machine::new(Profile::V4t);
    let ram = 0x02F0_0000;
    let pool = BASE + 0x80;
    m.arm(
        BASE,
        &[
            a_ldr_lit(2, BASE, pool),
            0xE592_3000,
            0xE313_0001,
            0x0AFF_FFFC,
            a_ldr_lit(0, BASE + 16, pool + 4),
            a_ldr_lit(1, BASE + 20, pool + 8),
            0xE3A0_2002,
            0xE490_3004,
            0xE481_3004,
            0xE252_2001,
            0x1AFF_FFFB,
            a_ldr_lit(0, BASE + 44, pool + 8),
            0xE12F_FF10,
        ],
    );
    m.arm(pool, &[0x0400_0180, BASE + 0x100, ram]);
    m.arm(BASE + 0x100, &[A_MOV_R0_1, A_BX_LR]);
    let r = m.run(all(), arm(BASE));
    assert!(r.copies.iter().any(|c| c.dst == ram), "{:?}", r.copies);
    assert!(
        r.instructions().keys().any(|l| l.addr == ram),
        "the copied code is lifted where it runs"
    );
}

#[test]
fn a_code_address_in_a_copy_s_literal_pool_is_a_function_in_that_copy() {
    let mut m = Machine::new(Profile::V4t);
    let ram = 0x02F0_0000;
    let pool = BASE + 0x80;
    let src = BASE + 0x100;
    m.arm(
        BASE,
        &[
            a_ldr_lit(0, BASE, pool),
            a_ldr_lit(1, BASE + 4, pool + 4),
            0xE3A0_2005,
            0xE490_3004,
            0xE481_3004,
            0xE252_2001,
            0x1AFF_FFFB,
            a_ldr_lit(0, BASE + 28, pool + 4),
            0xE12F_FF10,
        ],
    );
    m.arm(pool, &[src, ram]);
    m.arm(
        src,
        &[
            a_ldr_lit(0, ram, ram + 0x10),
            A_BX_LR,
            A_MOV_R0_1,
            A_BX_LR,
            ram + 8,
        ],
    );
    let r = m.run(all(), arm(BASE));
    let f = r
        .functions
        .iter()
        .find(|(l, _)| l.addr == ram + 8 && l.copy.is_some());
    assert_eq!(
        f.map(|(_, f)| f.origin),
        Some(Origin::AddressTaken),
        "{:?}",
        r.functions.keys().collect::<Vec<_>>()
    );
}

#[test]
fn a_callback_only_an_earlier_run_of_its_wrapper_called_is_a_function() {
    let mut m = Machine::new(Profile::V5te);
    let wrap = BASE + 0x40;
    let callee = BASE + 0x80;
    let cb = BASE + 0xC0;
    m.arm(
        BASE,
        &[
            A_PUSH_R4_LR,
            a_bl(BASE + 4, wrap),
            a_bl(BASE + 8, callee),
            A_POP_R4_PC,
        ],
    );
    m.arm(
        wrap,
        &[
            a_ldr_lit(12, wrap, wrap + 0x20),
            a_ldr_lit(2, wrap + 4, wrap + 0x24),
            0xE12F_FF1C,
        ],
    );
    m.arm(wrap + 0x20, &[callee, cb]);
    m.arm(callee, &[A_PUSH_R4_LR, 0xE12F_FF32, A_POP_R4_PC]);
    m.arm(cb, &[0xE3A0_0002, A_BX_LR]);
    let r = m.run(all(), arm(BASE));
    assert!(
        r.functions.contains_key(&arm(cb)),
        "{:?}",
        r.functions.keys().collect::<Vec<_>>()
    );
}

#[test]
fn vectors_a_boot_copies_to_a_mirror_of_the_irq_entry_are_an_interrupt_root() {
    let mut m = Machine::new(Profile::V5te);
    let ram = 0x02F0_0000;
    let entry = 0x02E0_0018;
    let pool = BASE + 0x80;
    let src = BASE + 0x100;
    m.arm(
        BASE,
        &[
            a_ldr_lit(0, BASE, pool),
            a_ldr_lit(1, BASE + 4, pool + 4),
            0xE3A0_2008,
            0xE490_3004,
            0xE481_3004,
            0xE252_2001,
            0x1AFF_FFFB,
            a_ldr_lit(0, BASE + 28, pool + 4),
            0xE12F_FF10,
        ],
    );
    m.arm(pool, &[src, ram]);
    m.arm(src, &[A_BX_LR, 0, 0, 0, 0, 0, A_MOV_R0_1, 0xE25E_F004]);
    let r = m.run(all(), arm(BASE));
    assert!(!r.functions.keys().any(|l| l.addr == entry));
    m.irq_entry = Some((entry, ram + 0x18));
    let r = m.run(all(), arm(BASE));
    let f = r.functions.iter().find(|(l, _)| l.addr == entry);
    assert_eq!(
        f.map(|(_, f)| f.origin),
        Some(Origin::Irq),
        "{:?}",
        r.functions.keys().collect::<Vec<_>>()
    );
}

#[test]
fn an_image_word_naming_code_in_a_copy_is_a_function_in_that_copy() {
    let mut m = Machine::new(Profile::V5te);
    let ram = 0x02F0_0000;
    let pool = BASE + 0x80;
    let src = BASE + 0x100;
    m.arm(
        BASE,
        &[
            a_ldr_lit(0, BASE, pool),
            a_ldr_lit(1, BASE + 4, pool + 4),
            0xE3A0_2004,
            0xE490_3004,
            0xE481_3004,
            0xE252_2001,
            0x1AFF_FFFB,
            a_ldr_lit(0, BASE + 28, pool + 4),
            0xE12F_FF30,
            a_ldr_lit(0, BASE + 36, pool + 8),
            0xE580_0000,
            A_BX_LR,
        ],
    );
    m.arm(pool, &[src, ram, ram + 8]);
    m.arm(src, &[A_BX_LR, 0, A_MOV_R0_1, A_BX_LR]);
    let r = m.run(all(), arm(BASE));
    let f = r
        .functions
        .iter()
        .find(|(l, _)| l.addr == ram + 8 && l.copy.is_some());
    assert_eq!(
        f.map(|(_, f)| f.origin),
        Some(Origin::AddressTaken),
        "{:?}",
        r.functions.keys().collect::<Vec<_>>()
    );
}

#[test]
fn code_a_boot_stores_over_its_own_copy_before_jumping_there_is_found() {
    let mut m = Machine::new(Profile::V4t);
    let ram = 0x02F0_0000;
    let pool = BASE + 0x80;
    let src = BASE + 0x100;
    let after = BASE + 0x40;
    let done = BASE + 0x140;
    m.arm(
        BASE,
        &[
            a_ldr_lit(0, BASE, pool),
            a_ldr_lit(1, BASE + 4, pool + 4),
            0xE3A0_2004,
            0xE490_3004,
            0xE481_3004,
            0xE252_2001,
            0x1AFF_FFFB,
            a_ldr_lit(0, BASE + 28, pool + 4),
            0xE12F_FF10,
        ],
    );
    m.arm(
        after,
        &[
            a_ldr_lit(13, after, pool + 8),
            a_ldr_lit(5, after + 4, pool + 12),
            a_ldr_lit(7, after + 8, pool + 16),
            0xE897_0300,
            0xE90D_0300,
            0xE24D_F008,
        ],
    );
    m.arm(
        pool,
        &[
            src,
            ram,
            ram + 16,
            done,
            pool + 20,
            0xE3A0_0002,
            0xE12F_FF15,
        ],
    );
    m.arm(src, &[A_MOV_R0_1, a_b(ram + 4, after), A_MOV_R0_1, A_BX_LR]);
    m.arm(done, &[A_MOVEQ_R0_1, A_BX_LR]);
    let r = m.run(all(), arm(BASE));
    assert!(
        r.instructions().keys().any(|l| l.addr == done),
        "the code the boot stored runs on to {done:#x}"
    );
}

#[test]
fn a_switch_on_a_halfword_offset_table_resolves_on_a_writable_image() {
    let mut m = Machine::new(Profile::V5te);
    m.writable = true;
    let pool = BASE + 0x80;
    let tab = BASE + 0x90;
    let jump = BASE + 20;
    let case = |k: u32| jump + 8 + 4 * k;
    m.arm(
        BASE,
        &[
            a_ldr_lit(8, BASE, pool),
            0xE350_0002,
            0x8A00_0010,
            0xE080_1000,
            0xE198_10F1,
            0xE08F_F101,
            A_MOV_R1_R1,
            A_BX_LR,
            A_MOV_R0_1,
            A_BX_LR,
            0xE3A0_0002,
            A_BX_LR,
        ],
    );
    m.arm(pool, &[tab]);
    m.put(tab, &[0, 0, 2, 0, 4, 0]);
    let r = m.run(all(), arm(BASE));
    for k in [2, 4] {
        assert!(
            r.instructions().contains_key(&arm(case(k))),
            "case {k} at {:#x}",
            case(k)
        );
    }
}

fn t_bl(at: u32, to: u32) -> [u16; 2] {
    let off = to.wrapping_sub(at + 4) >> 1;
    [
        0xF000 | (off >> 11 & 0x7FF) as u16,
        0xF800 | (off & 0x7FF) as u16,
    ]
}

#[test]
fn a_thumb_switch_through_a_case_helper_reaches_every_case() {
    let mut m = Machine::new(Profile::V4t);
    let f = BASE + 0x40;
    let helper = BASE + 0x100;
    m.arm(BASE, &[0xE28F_1039, 0xE12F_FF11]);
    let call = f + 4;
    let bl = t_bl(call, helper);
    m.thumb(
        f,
        &[
            0x2802, 0xD806, bl[0], bl[1], 0x0402, 0x0006, 0x2001, 0x4770, 0x2002, 0x4770, 0x2003,
            0x4770,
        ],
    );
    m.thumb(
        helper,
        &[
            0xB402, 0x4671, 0x0849, 0x0049, 0x5C09, 0x0049, 0x448E, 0xBC02, 0x4770,
        ],
    );
    let r = m.run(all(), arm(BASE));
    for case in [f + 16, f + 20, f + 12] {
        assert!(
            r.instructions().contains_key(&thumb(case)),
            "case at {case:#x}: {:?}",
            r.instructions()
                .keys()
                .filter(|l| l.thumb)
                .collect::<Vec<_>>()
        );
    }
}

#[test]
fn a_veneer_s_constant_target_is_a_function_of_its_own() {
    let mut m = Machine::new(Profile::V4t);
    let veneer = BASE + 0x40;
    let target = BASE + 0x80;
    m.arm(BASE, &[A_PUSH_R4_LR, a_bl(BASE + 4, veneer), A_POP_R4_PC]);
    m.arm(veneer, &[0xE59F_C000, 0xE12F_FF1C, target | 1]);
    m.thumb(target, &[0xDF06, T_BX_LR]);
    let r = m.run(all(), arm(BASE));
    assert!(
        r.functions.contains_key(&thumb(target)),
        "the lifted veneer reaches {target:#x} through dispatch: {:?}",
        r.functions.keys().collect::<Vec<_>>()
    );
}

#[test]
fn each_function_of_a_table_a_writer_copies_to_fast_ram_is_found_there() {
    let mut m = Machine::new(Profile::V5te);
    let fast = 0x02F0_0000;
    let pool = BASE + 0x80;
    let table = BASE + 0xA0;
    let copy = BASE + 0x200;
    let k = [BASE + 0x100, BASE + 0x140];
    m.arm(
        BASE,
        &[
            A_PUSH_R4_LR,
            a_ldr_lit(3, BASE + 4, pool),
            0xE7931100,
            a_ldr_lit(0, BASE + 12, pool + 4),
            0xE3A0_2040,
            0xE1A0_0000,
            a_bl(BASE + 24, copy),
            a_ldr_lit(4, BASE + 28, pool + 4),
            0xE12F_FF34,
            A_POP_R4_PC,
        ],
    );
    m.arm(pool, &[table, fast]);
    m.arm(table, &[k[0], k[1]]);
    m.arm(k[0], &[A_MOV_R0_1, A_BX_LR]);
    m.arm(k[1], &[0xE3A0_0002, A_BX_LR]);
    m.arm(
        copy,
        &[0xE491_3004, 0xE480_3004, 0xE252_2004, 0x1AFF_FFFB, A_BX_LR],
    );
    let r = m.run(all(), arm(BASE));
    let at: Vec<_> = r.functions.keys().filter(|l| l.addr == fast).collect();
    assert_eq!(
        at.len(),
        2,
        "one function per table entry at {fast:#x}: {at:?}"
    );
}

#[test]
fn table_functions_at_one_address_are_alternatives_not_rewrites() {
    let mut m = Machine::new(Profile::V5te);
    let fast = 0x02F0_0000;
    let pool = BASE + 0x80;
    let table = BASE + 0xA0;
    let copy = BASE + 0x200;
    let k = [BASE + 0x100, BASE + 0x140];
    m.arm(
        BASE,
        &[
            A_PUSH_R4_LR,
            a_ldr_lit(3, BASE + 4, pool),
            0xE793_1100,
            a_ldr_lit(0, BASE + 12, pool + 4),
            0xE3A0_2040,
            0xE1A0_0000,
            a_bl(BASE + 24, copy),
            a_ldr_lit(4, BASE + 28, pool + 4),
            0xE12F_FF34,
            A_POP_R4_PC,
        ],
    );
    m.arm(pool, &[table, fast]);
    m.arm(table, &[k[0], k[1]]);
    for (i, &at) in k.iter().enumerate() {
        m.arm(at, &[0xE28F_C004, 0xE38C_C001, 0xE12F_FF1C]);
        m.thumb(at + 12, &[0x2001 + i as u16, 0x2001, T_BX_LR]);
    }
    m.arm(
        copy,
        &[0xE491_3004, 0xE480_3004, 0xE252_2004, 0x1AFF_FFFB, A_BX_LR],
    );
    let r = m.run(all(), arm(BASE));
    let mut at: Vec<_> = r
        .functions
        .keys()
        .filter(|l| l.addr >= fast && l.addr < fast + 0x40)
        .map(|l| (l.addr - fast, l.thumb))
        .collect();
    at.sort_unstable();
    assert_eq!(
        at,
        [(0, false), (0, false), (12, true), (12, true)],
        "each table function and its Thumb part, nothing inside them"
    );
}

#[test]
fn functions_of_a_table_in_a_copy_that_code_copies_on_are_found() {
    let mut m = Machine::new(Profile::V4t);
    let wram = 0x02F0_0000;
    let slot = 0x02F8_0420;
    let pool = BASE + 0x40;
    let src = BASE + 0x100;
    let copy = BASE + 0x200;
    m.arm(
        BASE,
        &[
            a_ldr_lit(0, BASE, pool),
            a_ldr_lit(1, BASE + 4, pool + 4),
            0xE3A0_2020,
            0xE490_3004,
            0xE481_3004,
            0xE252_2001,
            0x1AFF_FFFB,
            a_ldr_lit(0, BASE + 28, pool + 4),
            0xE12F_FF10,
        ],
    );
    m.arm(pool, &[src, wram]);
    let mut block = vec![
        A_PUSH_R4_LR,
        a_ldr_lit(3, wram + 4, wram + 0x30),
        0xE793_1104,
        a_ldr_lit(0, wram + 12, wram + 0x34),
        0xE3A0_2008,
        a_bl(wram + 20, copy),
        a_ldr_lit(4, wram + 24, wram + 0x34),
        0xE12F_FF34,
        A_POP_R4_PC,
    ];
    block.resize(12, 0);
    block.extend([wram + 0x40, slot, 0, 0]);
    block.extend([wram + 0x60, wram + 0x70, 0, 0, 0, 0, 0, 0]);
    block.extend([A_MOV_R0_1, A_BX_LR, 0, 0, 0xE3A0_0002, A_BX_LR, 0, 0]);
    m.arm(src, &block);
    m.arm(
        copy,
        &[0xE491_3004, 0xE480_3004, 0xE252_2004, 0x1AFF_FFFB, A_BX_LR],
    );
    let r = m.run(all(), arm(BASE));
    let at: Vec<_> = r.functions.keys().filter(|l| l.addr == slot).collect();
    assert_eq!(
        at.len(),
        2,
        "one function per table entry at {slot:#x}: {at:?}"
    );
}

#[test]
fn a_word_in_a_copy_naming_code_of_that_copy_is_a_candidate() {
    let mut m = Machine::new(Profile::V4t);
    let wram = 0x02F0_0000;
    let pool = BASE + 0x40;
    let src = BASE + 0x100;
    m.arm(
        BASE,
        &[
            a_ldr_lit(0, BASE, pool),
            a_ldr_lit(1, BASE + 4, pool + 4),
            0xE3A0_2009,
            0xE8B0_0078,
            0xE8A1_0078,
            0xE252_2001,
            0x1AFF_FFFB,
            a_ldr_lit(0, BASE + 28, pool + 4),
            0xE12F_FF10,
        ],
    );
    m.arm(pool, &[src, wram]);
    let mut block = vec![A_BX_LR; 0x20];
    block.extend([(wram + 0x84) | 1, 0x4770_2001]);
    block.resize(0x24, 0);
    m.arm(src, &block);
    let r = m.run(all(), arm(BASE));
    assert!(
        r.functions
            .keys()
            .any(|l| l.addr == wram + 0x84 && l.thumb && l.copy.is_some()),
        "{:?}",
        r.functions.keys().collect::<Vec<_>>()
    );
}

/// The program of `each_function_of_a_table_a_writer_copies_to_fast_ram_is_found_there`
/// with the destination `fast`, the length the writer gets in `r2`
/// (`mov r2, #imm`) and the table of `k`.
fn table_writer(fast: u32, mov_r2: u32, k: &[u32]) -> Machine {
    let mut m = Machine::new(Profile::V5te);
    let pool = BASE + 0x80;
    let table = BASE + 0xA0;
    let copy = BASE + 0x200;
    m.arm(
        BASE,
        &[
            A_PUSH_R4_LR,
            a_ldr_lit(3, BASE + 4, pool),
            0xE7931100,
            a_ldr_lit(0, BASE + 12, pool + 4),
            mov_r2,
            0xE1A0_0000,
            a_bl(BASE + 24, copy),
            a_ldr_lit(4, BASE + 28, pool + 4),
            0xE12F_FF34,
            A_POP_R4_PC,
        ],
    );
    m.arm(pool, &[table, fast]);
    m.arm(table, k);
    m.arm(
        copy,
        &[0xE491_3004, 0xE480_3004, 0xE252_2004, 0x1AFF_FFFB, A_BX_LR],
    );
    m
}

#[test]
fn a_constant_length_past_ram_does_not_size_a_table_copy() {
    let k = [BASE + 0x100, BASE + 0x140];
    let mut m = table_writer(0x02FF_FF00, 0xE3A0_2C02, &k);
    m.arm(k[0], &[A_MOV_R0_1, A_BX_LR]);
    m.arm(k[1], &[0xE3A0_0002, A_BX_LR]);
    let r = m.run(all(), arm(BASE));
    let first = r
        .copies
        .iter()
        .find(|c| c.src == Some(k[0]))
        .expect("a copy of the first function");
    assert_eq!(first.len(), k[1] - k[0], "up to the next function");
}

#[test]
fn table_copies_stop_at_a_total_size() {
    let n = 3;
    let step = 0x8_0000;
    let k: Vec<u32> = (0..n).map(|i| OTHER + i * step).collect();
    let mut m = table_writer(0x02F0_0000, 0xE3A0_2702, &k);
    m.segments.push((OTHER, vec![0; (n * step) as usize]));
    for (i, &f) in k.iter().enumerate() {
        m.arm(f, &[A_MOV_R0_1 + i as u32, A_BX_LR]);
    }
    let r = m.run(
        Options {
            scan_words: false,
            ..all()
        },
        arm(BASE),
    );
    let total: u32 = r
        .copies
        .iter()
        .filter(|c| matches!(c.kind, CopyKind::Table(_)))
        .map(RamCopy::len)
        .sum();
    assert!(total <= 1 << 20, "{total:#x} bytes of table copies");
    assert!(r.limits.contains(&Limit::TableCopies));
}

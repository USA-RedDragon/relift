//! Discovery on tiny code images assembled here, one rule each, on a
//! machine of image segments in main RAM.

use relift_cpu_arm32_analyze::{
    BiosCall, Config, Dma, Kind, Loc, Options, Origin, Platform, Profile, Report, discover,
};

const BASE: u32 = 0x0200_0000;
const OTHER: u32 = 0x0210_0000;

struct Machine {
    profile: Profile,
    segments: Vec<(u32, Vec<u8>)>,
    external: Option<(u32, u32)>,
}

impl Machine {
    fn new(profile: Profile) -> Machine {
        Machine {
            profile,
            segments: vec![(BASE, vec![0; 0x1000])],
            external: None,
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
    assert!(
        !found(&seeded, arm(OTHER + 0x100)),
        "a seed must open a stack frame"
    );
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

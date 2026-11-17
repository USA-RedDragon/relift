//! The RSP's issue pipeline, for its cycle counts: which instructions issue
//! together and how long a group waits on the results of the groups before
//! it. The rules are the hardware's as n64brew's "Reality Signal
//! Processor/CPU Pipeline" page and RSP timing tests describe them, in the
//! form ares's interpreter keeps (`rsp.hpp` `Pipeline`, `decoder.cpp`):
//!
//! - an instruction of the scalar unit and one of the vector unit (a COP2
//!   computational instruction) issue together when the first starts a
//!   group, is no branch, comes after no branch or taken branch to an odd
//!   word, and writes no vector register or flag register the second uses
//!   ([`pairs`]);
//! - a group takes one cycle, plus stalls: reading a scalar register a load,
//!   `MFC0`, `MFC2` or `CFC2` wrote one group before waits two cycles, two
//!   groups before one; reading a vector register written one, two or three
//!   groups before waits three, two or one; a store waits while the group
//!   two before it loaded; each stall is a cycle in which no group issues;
//! - after the delay slot of a taken branch the RSP stalls one cycle.
//!
//! [`State`] is the pipeline between groups with each group named by the
//! word it starts at, so it can be written as a number ([`State::encode`])
//! that the runtime reads back against IMEM (`rl_rsp_pipe_step`).

/// The pipeline classes of one instruction word.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Op {
    /// [`LOAD`], [`STORE`], [`BRANCH`], [`VECTOR`], [`VNOP_GROUP`], [`BYPASS`].
    pub flags: u8,
    /// Scalar registers read.
    pub r_use: u32,
    /// Scalar registers written.
    pub r_def: u32,
    /// Vector registers read.
    pub v_use: u32,
    /// Vector registers written.
    pub v_def: u32,
    /// Flag registers read (bit 0 VCO, 1 VCC, 2 VCE).
    pub vc_use: u8,
    /// Flag registers written.
    pub vc_def: u8,
    /// Vector registers a single-lane instruction names in a field the
    /// dual-issue check misreads as a register.
    pub vfake: u32,
}

/// A load from DMEM, or a COP0 or COP2 move.
pub const LOAD: u8 = 1;
/// A store to DMEM, or a COP0 or COP2 move.
pub const STORE: u8 = 2;
/// A branch, a jump or `BREAK`: the next group issues alone.
pub const BRANCH: u8 = 4;
/// A computational COP2 instruction, run by the vector unit.
pub const VECTOR: u8 = 8;
/// `MTC2`, `LTV` and `VNOP`, between which the misread field conflicts.
pub const VNOP_GROUP: u8 = 16;
/// The scalar result is ready for the next group.
pub const BYPASS: u8 = 32;

impl Op {
    /// Whether it loads.
    #[must_use]
    pub fn load(&self) -> bool {
        self.flags & LOAD != 0
    }
    /// Whether it stores.
    #[must_use]
    pub fn store(&self) -> bool {
        self.flags & STORE != 0
    }
    /// Whether it is a branch, a jump or `BREAK`.
    #[must_use]
    pub fn branch(&self) -> bool {
        self.flags & BRANCH != 0
    }
    /// Whether the vector unit runs it.
    #[must_use]
    pub fn vector(&self) -> bool {
        self.flags & VECTOR != 0
    }
    /// The scalar registers whose new values the next groups wait for.
    #[must_use]
    pub fn r_write(&self) -> u32 {
        if self.flags & BYPASS != 0 {
            0
        } else {
            self.r_def & !1
        }
    }
}

/// The classes of `word`.
#[must_use]
#[allow(clippy::too_many_lines)]
pub fn op(word: u32) -> Op {
    let rd = (word >> 11) & 31;
    let rt = (word >> 16) & 31;
    let rs = (word >> 21) & 31;
    let vd = (word >> 6) & 31;
    let vs = (word >> 11) & 31;
    let vt = (word >> 16) & 31;
    let mut o = Op::default();
    let alu = |o: &mut Op, d: u32, uses: &[u32]| {
        o.r_def |= 1 << d;
        o.flags |= BYPASS;
        for u in uses {
            o.r_use |= 1 << u;
        }
    };
    let mem = |o: &mut Op, f: u8| {
        o.r_use |= 1 << rs;
        o.flags |= f;
    };
    match word >> 26 {
        0x00 => match word & 0x3F {
            0x00 | 0x02 | 0x03 => alu(&mut o, rd, &[rt]),
            0x04 | 0x06 | 0x07 => alu(&mut o, rd, &[rt, rs]),
            0x08 => {
                o.r_use |= 1 << rs;
                o.flags |= BRANCH;
            }
            0x09 => {
                alu(&mut o, rd, &[rs]);
                o.flags |= BRANCH;
            }
            0x0D => o.flags |= BRANCH,
            0x20..=0x27 | 0x2A | 0x2B => alu(&mut o, rd, &[rs, rt]),
            _ => {}
        },
        0x01 => match rt {
            0x00 | 0x01 => {
                o.r_use |= 1 << rs;
                o.flags |= BRANCH;
            }
            0x10 | 0x11 => {
                alu(&mut o, 31, &[rs]);
                o.flags |= BRANCH;
            }
            _ => {}
        },
        0x02 => o.flags |= BRANCH,
        0x03 => {
            alu(&mut o, 31, &[]);
            o.flags |= BRANCH;
        }
        0x04 | 0x05 => {
            o.r_use |= 1 << rs | 1 << rt;
            o.flags |= BRANCH;
        }
        0x06 | 0x07 => {
            o.r_use |= 1 << rs;
            o.flags |= BRANCH;
        }
        0x08..=0x0E => alu(&mut o, rt, &[rs]),
        0x0F => alu(&mut o, rt, &[]),
        0x10 => match rs {
            0x00 => {
                o.r_def |= 1 << rt;
                o.flags |= LOAD | STORE;
            }
            0x04 => {
                o.r_use |= 1 << rt;
                o.flags |= LOAD | STORE;
            }
            _ => {}
        },
        0x12 => cop2(word, &mut o, rs, rt, rd, vd, vs, vt),
        0x20 | 0x21 | 0x23 | 0x24 | 0x25 | 0x27 => {
            o.r_def |= 1 << rt;
            mem(&mut o, LOAD);
        }
        0x28 | 0x29 | 0x2B => {
            o.r_use |= 1 << rt;
            mem(&mut o, STORE);
        }
        0x32 => match rd {
            0x00..=0x09 => {
                o.v_def |= 1 << vt;
                mem(&mut o, LOAD);
            }
            0x0B => {
                o.v_def |= 0xFF << (vt & !7);
                mem(&mut o, LOAD | VNOP_GROUP);
            }
            _ => {}
        },
        0x3A => match rd {
            0x00..=0x0A => {
                o.v_use |= 1 << vt;
                mem(&mut o, STORE);
            }
            0x0B => {
                o.v_use |= 0xFF << (vt & !7);
                mem(&mut o, STORE);
            }
            _ => {}
        },
        _ => {}
    }
    o
}

const VCO: u8 = 1;
const VCC: u8 = 2;
const VCE: u8 = 4;

#[allow(clippy::too_many_arguments)]
fn cop2(word: u32, o: &mut Op, rs: u32, rt: u32, rd: u32, vd: u32, vs: u32, vt: u32) {
    match rs {
        0x00 => {
            o.r_def |= 1 << rt;
            o.v_use |= 1 << vs;
            o.flags |= LOAD | STORE;
            return;
        }
        0x02 => {
            o.r_def |= 1 << rt;
            o.vc_use |= 1 << (rd & 3);
            o.flags |= LOAD | STORE;
            return;
        }
        0x04 => {
            o.r_use |= 1 << rt;
            o.v_def |= 1 << vs;
            o.flags |= LOAD | STORE | VNOP_GROUP;
            return;
        }
        0x06 => {
            o.r_use |= 1 << rt;
            o.vc_def |= 1 << (rd & 3);
            o.flags |= LOAD | STORE;
            return;
        }
        0x01 | 0x03 | 0x05 | 0x07..=0x0F => return,
        _ => {}
    }
    o.flags |= VECTOR;
    let dst = |o: &mut Op, s: bool, t: bool| {
        o.v_def |= 1 << vd;
        if s {
            o.v_use |= 1 << vs;
        }
        if t {
            o.v_use |= 1 << vt;
        }
    };
    let flags = |o: &mut Op, f: u8| {
        o.vc_use |= f;
        o.vc_def |= f;
    };
    match word & 0x3F {
        0x00 | 0x01 | 0x03..=0x09 | 0x0C..=0x0F | 0x28..=0x2D => dst(o, true, true),
        0x02 | 0x0A => dst(o, false, true),
        0x0B | 0x1D => dst(o, false, false),
        0x10 | 0x11 | 0x13..=0x15 => {
            dst(o, true, true);
            flags(o, VCO);
        }
        0x20..=0x23 | 0x27 => {
            dst(o, true, true);
            flags(o, VCO | VCC);
        }
        0x24..=0x26 => {
            dst(o, true, true);
            flags(o, VCO | VCC | VCE);
        }
        0x30..=0x36 => {
            dst(o, false, true);
            o.vfake |= 1 << vs;
        }
        0x37 => {
            o.vfake |= 1 << vd;
            o.flags |= VNOP_GROUP;
        }
        _ => {}
    }
}

/// Whether `b` issues with `a` when `a` starts a group the pipeline lets
/// pair: one runs on each unit, `a` writes no vector or flag register `b`
/// uses or writes, and no misread field of a single-lane `b` names a
/// register `a` writes.
#[must_use]
pub fn pairs(a: &Op, b: &Op) -> bool {
    a.vector() != b.vector()
        && a.v_def & (b.v_use | b.v_def) == 0
        && a.vc_def & (b.vc_use | b.vc_def) == 0
        && !((a.flags & VNOP_GROUP != 0 || b.flags & VNOP_GROUP == 0) && a.v_def & b.vfake != 0)
}

/// One cycle of the pipeline: what the group issued then writes, or nothing
/// for a stall.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Stage {
    /// Scalar registers whose results are not ready yet.
    pub r: u32,
    /// Vector registers.
    pub v: u32,
    /// The group loaded.
    pub load: bool,
}

/// The pipeline between two words: the last three cycles (newest first),
/// whether the next group issues alone, and whether the word before
/// started a pair the next word completes. A scalar result is never waited
/// for three cycles on, nor a load looked at, so the oldest stage keeps
/// only its vector registers.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct State {
    /// The stages.
    pub s: [Stage; 3],
    /// The next group issues alone.
    pub single: bool,
    /// The previous word starts a pair with the next one.
    pub pend: bool,
}

/// The words of an image and their classes.
pub trait Words {
    /// The classes of word `w` (an IMEM word index, wrapping at 4 KiB), or
    /// `None` when it is not known.
    fn op(&self, w: u16) -> Option<Op>;
}

impl<F: Fn(u16) -> Option<Op>> Words for F {
    fn op(&self, w: u16) -> Option<Op> {
        self(w)
    }
}

/// The word after `w`, wrapping at 4 KiB.
#[must_use]
pub fn next(w: u16) -> u16 {
    (w + 1) & 0x3FF
}

/// The word before `w`, wrapping at 4 KiB.
#[must_use]
pub fn prev(w: u16) -> u16 {
    w.wrapping_sub(1) & 0x3FF
}

impl State {
    fn shift(&mut self, g: Stage) {
        self.s = [
            g,
            self.s[0],
            Stage {
                r: 0,
                v: self.s[1].v,
                load: false,
            },
        ];
    }

    /// Ends a group that reads and writes as `o`: its stalls, then it
    /// enters the pipeline. Returns its cycles.
    fn end(&mut self, o: &Op) -> u32 {
        let mut cycles = 1;
        let r = if o.r_use & self.s[0].r != 0 {
            2
        } else {
            u32::from(o.r_use & self.s[1].r != 0)
        };
        for _ in 0..r {
            self.shift(Stage::default());
        }
        let v = if o.v_use & self.s[0].v != 0 {
            3
        } else if o.v_use & self.s[1].v != 0 {
            2
        } else {
            u32::from(o.v_use & self.s[2].v != 0)
        };
        for _ in 0..v {
            self.shift(Stage::default());
        }
        cycles += r + v;
        if o.store() {
            while self.s[1].load {
                self.shift(Stage::default());
                cycles += 1;
            }
        }
        self.single = o.branch();
        self.shift(Stage {
            r: o.r_write(),
            v: o.v_def,
            load: o.load(),
        });
        cycles
    }

    /// Runs word `w`, the next of the stream: returns its cycles, 0 when it
    /// starts a pair (the pair's cycles come with its second word). A word
    /// that is not known runs as a `NOP`.
    pub fn word(&mut self, w: u16, ws: &impl Words) -> u32 {
        let a = ws.op(w).unwrap_or_default();
        if self.pend {
            self.pend = false;
            let f = ws.op(prev(w)).unwrap_or_default();
            let g = Op {
                flags: (f.flags | a.flags) & !BYPASS,
                r_use: f.r_use | a.r_use,
                r_def: f.r_write() | a.r_write(),
                v_use: f.v_use | a.v_use,
                v_def: f.v_def | a.v_def,
                vc_use: 0,
                vc_def: 0,
                vfake: 0,
            };
            return self.end(&g);
        }
        if !self.single && !a.branch() && ws.op(next(w)).is_some_and(|b| pairs(&a, &b)) {
            self.pend = true;
            return 0;
        }
        self.end(&a)
    }

    /// The stall after the delay slot of a taken branch to word `t`; a
    /// group at an odd word then issues alone.
    pub fn taken(&mut self, t: u16) {
        self.shift(Stage::default());
        self.single |= t & 1 != 0;
    }

    /// As three numbers: the scalar stages, the vector stages 0 and 1, and
    /// vector stage 2 with the flags above it (bit 32 load 0, 33 load 1,
    /// 34 single, 35 pend), as the runtime takes them (`rl_rsp_pipe`).
    #[must_use]
    pub fn pack(&self) -> [u64; 3] {
        [
            u64::from(self.s[0].r) | u64::from(self.s[1].r) << 32,
            u64::from(self.s[0].v) | u64::from(self.s[1].v) << 32,
            u64::from(self.s[2].v)
                | u64::from(self.s[0].load) << 32
                | u64::from(self.s[1].load) << 33
                | u64::from(self.single) << 34
                | u64::from(self.pend) << 35,
        ]
    }
}

/// The most words [`delta`] follows.
pub const WINDOW: u32 = 16;

/// What the cycles from word `w` on differ by when the pipeline there is
/// `x` instead of `e`: both run word by word along the straight line from
/// `w` until they are equal, or to the end of the first branch's delay
/// slot (equal whether the branch is taken or not), or [`WINDOW`] words.
/// Returns the difference and whether they met.
#[must_use]
pub fn delta(x: State, e: State, w: u16, ws: &impl Words) -> (i64, bool) {
    let (mut x, mut e) = (x, e);
    let mut d = 0i64;
    let mut w = w;
    let mut slot = false;
    for _ in 0..WINDOW {
        if x == e {
            return (d, true);
        }
        let br = ws.op(w).is_some_and(|o| o.branch());
        d += i64::from(x.word(w, ws)) - i64::from(e.word(w, ws));
        if slot {
            return (d, x == e);
        }
        slot = br;
        w = next(w);
    }
    (d, x == e)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(words: &[u32]) -> (u32, State) {
        let ws = |w: u16| words.get(usize::from(w)).map(|x| op(*x));
        let mut s = State::default();
        let mut c = 0;
        for w in 0..words.len() as u16 {
            c += s.word(w, &ws);
        }
        (c, s)
    }

    const LW_T0: u32 = 0x8E08_0000;
    const SLL_T0: u32 = 0x0008_4080;
    const VSUBC_V2: u32 = 0x4B0B_5095;
    const VLT_V2: u32 = 0x4B0C_10A0;
    const VNOP: u32 = 0x4A00_0037;
    const NOP: u32 = 0;

    #[test]
    fn a_load_result_waits_two_cycles() {
        assert_eq!(run(&[LW_T0, SLL_T0]).0, 4);
        assert_eq!(run(&[LW_T0, NOP, SLL_T0]).0, 4);
        assert_eq!(run(&[LW_T0, NOP, NOP, SLL_T0]).0, 4);
    }

    #[test]
    fn a_vector_result_waits_three_cycles() {
        assert_eq!(run(&[VSUBC_V2, VLT_V2]).0, 5);
        assert_eq!(run(&[VSUBC_V2, NOP, VLT_V2]).0, 5);
    }

    #[test]
    fn a_scalar_and_a_vector_instruction_pair() {
        let vadd = 0x4A04_1890;
        assert_eq!(run(&[NOP, vadd, NOP, vadd]).0, 2);
        assert_eq!(run(&[NOP, vadd, NOP]).0, 2);
        let reads_it = 0x4A02_1890;
        assert_eq!(run(&[NOP, vadd, NOP, reads_it]).0, 5);
    }

    #[test]
    fn a_write_of_a_vector_register_the_next_reads_keeps_them_apart() {
        let lqv_v1 = 0xC801_2000;
        let vadd_v1 = 0x4A01_0850;
        assert!(!pairs(&op(lqv_v1), &op(vadd_v1)));
    }

    #[test]
    fn a_single_lane_instruction_misreads_its_field() {
        let mtc2_v3 = 0x4888_1800;
        let vnop_v3 = 0x4A00_00F7;
        assert!(!pairs(&op(mtc2_v3), &op(vnop_v3)));
        assert!(pairs(&op(mtc2_v3), &op(VNOP)));
    }

    #[test]
    fn a_store_waits_for_a_load_two_groups_before() {
        let sw = 0xAE09_0000;
        assert_eq!(run(&[LW_T0, NOP, sw]).0, 4);
        assert_eq!(run(&[LW_T0, sw]).0, 2);
    }

    #[test]
    fn the_delay_slot_issues_alone() {
        let beq = 0x1000_0004;
        let vadd = 0x4A00_0010;
        assert_eq!(run(&[beq, NOP, vadd]).0, 3);
        assert_eq!(run(&[NOP, NOP, vadd]).0, 2);
    }

    #[test]
    fn a_window_counts_what_another_pipeline_costs() {
        let words = [SLL_T0, NOP, NOP, NOP];
        let ws = |w: u16| words.get(usize::from(w)).map(|x| op(*x));
        let mut x = State::default();
        x.word(9, &|_: u16| Some(op(LW_T0)));
        assert_eq!(delta(x, State::default(), 0, &ws), (2, true));
        assert_eq!(delta(State::default(), State::default(), 0, &ws), (0, true));
    }

    #[test]
    fn packing_keeps_the_flags() {
        let mut s = State {
            single: true,
            ..State::default()
        };
        s.s[1].load = true;
        let p = s.pack();
        assert_eq!(p[2] >> 32, 0b0110);
    }
}

//! The test states and instruction words: the generators of the random
//! check, the oracle's input format and the FP special-value corpus.

use std::fmt::Write as _;

/// A xorshift generator; the seed fixes every state of a run.
pub struct Rng(pub u64);
impl Rng {
    pub fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    pub fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
    pub fn chance(&mut self, pct: u64) -> bool {
        self.below(100) < pct
    }
    pub fn gpr_val(&mut self) -> u64 {
        match self.below(4) {
            0 => self.next(),
            1 => self.below(16),
            _ => self.next() as u32 as i32 as i64 as u64,
        }
    }
    pub fn f32bits(&mut self) -> u32 {
        let s = (self.below(2) as u32) << 31;
        let e = (110 + self.below(35)) as u32;
        s | e << 23 | (self.next() as u32 & 0x7F_FFFF)
    }
    pub fn f64bits(&mut self) -> u64 {
        let s = self.below(2) << 63;
        let e = 1000 + self.below(47);
        s | e << 52 | (self.next() & 0xF_FFFF_FFFF_FFFF)
    }
}

/// The address of the first word: kseg1, physical 0x1000.
pub const PC: u64 = 0xFFFF_FFFF_A000_1000;
/// The data region, kseg1.
pub const DATA: u64 = 0xFFFF_FFFF_A010_0000;
/// The data region's physical address.
pub const DATA_PA: u32 = 0x0010_0000;
/// The data region's length.
pub const DATA_LEN: usize = 0x80;

/// One test: the words at `PC` and the machine state before them.
#[derive(Clone)]
pub struct Case {
    pub name: String,
    pub words: Vec<u32>,
    pub gpr: [u64; 32],
    pub hi: u64,
    pub lo: u64,
    pub fpr: [u64; 32],
    pub fcr31: u32,
    pub status: u32,
    pub cop0: Vec<(&'static str, u64)>,
    pub data: Vec<u8>,
}
fn r_type(rs: u32, rt: u32, rd: u32, sa: u32, f: u32) -> u32 {
    rs << 21 | rt << 16 | rd << 11 | sa << 6 | f
}
fn i_type(op: u32, rs: u32, rt: u32, imm: u32) -> u32 {
    op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF)
}

/// A generator: the instruction word of one case, and any state it needs.
pub type Gen = fn(&mut Rng, &mut Case) -> u32;

fn reg(r: &mut Rng) -> u32 {
    1 + r.below(31) as u32
}
/// The kinds of instruction the check covers, each with a generator.
pub fn cases() -> Vec<(&'static str, Gen)> {
    macro_rules! rr {
        ($f:expr) => {
            |r: &mut Rng, _c: &mut Case| r_type(reg(r), reg(r), reg(r), 0, $f)
        };
    }
    macro_rules! sh {
        ($f:expr) => {
            |r: &mut Rng, _c: &mut Case| r_type(0, reg(r), reg(r), r.below(32) as u32, $f)
        };
    }
    macro_rules! ri {
        ($op:expr) => {
            |r: &mut Rng, _c: &mut Case| i_type($op, reg(r), reg(r), r.next() as u32)
        };
    }
    macro_rules! md {
        ($f:expr) => {
            |r: &mut Rng, c: &mut Case| {
                let (s, t) = (reg(r), reg(r));
                if r.chance(15) {
                    c.gpr[t as usize] = 0;
                }
                r_type(s, t, 0, 0, $f)
            }
        };
    }
    macro_rules! mem {
        ($op:expr, $al:expr) => {
            |r: &mut Rng, c: &mut Case| {
                let (b, t) = (reg(r), reg(r));
                let off = (r.below(0x40) as u32) & !(if r.chance(90) { $al - 1 } else { 0 });
                c.gpr[b as usize] = DATA + 0x20 - (r.below(4) * 8);
                i_type($op, b, t, off.wrapping_sub(0x10))
            }
        };
    }
    macro_rules! br {
        ($w:expr) => {
            |r: &mut Rng, c: &mut Case| {
                let (s, t) = (reg(r), reg(r));
                if r.chance(40) {
                    c.gpr[t as usize] = c.gpr[s as usize];
                }
                if r.chance(20) {
                    c.gpr[s as usize] = 0;
                }
                let w: u32 = $w(s, t, 0x10u32);
                slot(r, c);
                w
            }
        };
    }
    let v: Vec<(&'static str, Gen)> = vec![
        ("ADD", rr!(0x20)),
        ("ADDU", rr!(0x21)),
        ("SUB", rr!(0x22)),
        ("SUBU", rr!(0x23)),
        ("AND", rr!(0x24)),
        ("OR", rr!(0x25)),
        ("XOR", rr!(0x26)),
        ("NOR", rr!(0x27)),
        ("SLT", rr!(0x2A)),
        ("SLTU", rr!(0x2B)),
        ("DADD", rr!(0x2C)),
        ("DADDU", rr!(0x2D)),
        ("DSUB", rr!(0x2E)),
        ("DSUBU", rr!(0x2F)),
        ("SLLV", rr!(0x04)),
        ("SRLV", rr!(0x06)),
        ("SRAV", rr!(0x07)),
        ("DSLLV", rr!(0x14)),
        ("DSRLV", rr!(0x16)),
        ("DSRAV", rr!(0x17)),
        ("SLL", sh!(0x00)),
        ("SRL", sh!(0x02)),
        ("SRA", sh!(0x03)),
        ("DSLL", sh!(0x38)),
        ("DSRL", sh!(0x3A)),
        ("DSRA", sh!(0x3B)),
        ("DSLL32", sh!(0x3C)),
        ("DSRL32", sh!(0x3E)),
        ("DSRA32", sh!(0x3F)),
        ("ADDI", ri!(0x08)),
        ("ADDIU", ri!(0x09)),
        ("SLTI", ri!(0x0A)),
        ("SLTIU", ri!(0x0B)),
        ("ANDI", ri!(0x0C)),
        ("ORI", ri!(0x0D)),
        ("XORI", ri!(0x0E)),
        ("DADDI", ri!(0x18)),
        ("DADDIU", ri!(0x19)),
        ("LUI", |r, _c| i_type(0x0F, 0, reg(r), r.next() as u32)),
        ("MULT", md!(0x18)),
        ("MULTU", md!(0x19)),
        ("DIV", md!(0x1A)),
        ("DIVU", md!(0x1B)),
        ("DMULT", md!(0x1C)),
        ("DMULTU", md!(0x1D)),
        ("DDIV", md!(0x1E)),
        ("DDIVU", md!(0x1F)),
        ("MFHI", |r, _c| r_type(0, 0, reg(r), 0, 0x10)),
        ("MFLO", |r, _c| r_type(0, 0, reg(r), 0, 0x12)),
        ("MTHI", |r, _c| r_type(reg(r), 0, 0, 0, 0x11)),
        ("MTLO", |r, _c| r_type(reg(r), 0, 0, 0, 0x13)),
        ("LB", mem!(0x20, 1)),
        ("LH", mem!(0x21, 2)),
        ("LWL", mem!(0x22, 1)),
        ("LW", mem!(0x23, 4)),
        ("LBU", mem!(0x24, 1)),
        ("LHU", mem!(0x25, 2)),
        ("LWR", mem!(0x26, 1)),
        ("LWU", mem!(0x27, 4)),
        ("LDL", mem!(0x1A, 1)),
        ("LDR", mem!(0x1B, 1)),
        ("LD", mem!(0x37, 8)),
        ("LL", mem!(0x30, 4)),
        ("SB", mem!(0x28, 1)),
        ("SH", mem!(0x29, 2)),
        ("SWL", mem!(0x2A, 1)),
        ("SW", mem!(0x2B, 4)),
        ("SDL", mem!(0x2C, 1)),
        ("SDR", mem!(0x2D, 1)),
        ("SWR", mem!(0x2E, 1)),
        ("SD", mem!(0x3F, 8)),
        ("SC", mem!(0x38, 4)),
        ("BEQ", br!(|s, t, o| i_type(0x04, s, t, o))),
        ("BNE", br!(|s, t, o| i_type(0x05, s, t, o))),
        ("BLEZ", br!(|s, _t, o| i_type(0x06, s, 0, o))),
        ("BGTZ", br!(|s, _t, o| i_type(0x07, s, 0, o))),
        ("BEQL", br!(|s, t, o| i_type(0x14, s, t, o))),
        ("BNEL", br!(|s, t, o| i_type(0x15, s, t, o))),
        ("BLEZL", br!(|s, _t, o| i_type(0x16, s, 0, o))),
        ("BGTZL", br!(|s, _t, o| i_type(0x17, s, 0, o))),
        ("BLTZ", br!(|s, _t, o| i_type(0x01, s, 0x00, o))),
        ("BGEZ", br!(|s, _t, o| i_type(0x01, s, 0x01, o))),
        ("BLTZL", br!(|s, _t, o| i_type(0x01, s, 0x02, o))),
        ("BGEZL", br!(|s, _t, o| i_type(0x01, s, 0x03, o))),
        ("BLTZAL", br!(|s, _t, o| i_type(0x01, s, 0x10, o))),
        ("BGEZAL", br!(|s, _t, o| i_type(0x01, s, 0x11, o))),
        ("J", |r, c| {
            slot(r, c);
            0x0800_0000 | (((PC as u32 & 0x0FFF_FFFF) >> 2) + 0x40)
        }),
        ("JAL", |r, c| {
            slot(r, c);
            0x0C00_0000 | (((PC as u32 & 0x0FFF_FFFF) >> 2) + 0x40)
        }),
        ("JR", |r, c| {
            let s = reg(r);
            c.gpr[s as usize] = PC + 0x100;
            slot(r, c);
            r_type(s, 0, 0, 0, 0x08)
        }),
        ("JALR", |r, c| {
            let s = reg(r);
            c.gpr[s as usize] = PC + 0x100;
            slot(r, c);
            r_type(s, 0, reg(r), 0, 0x09)
        }),
        ("TEQ", |r, c| {
            let (s, t) = (reg(r), reg(r));
            if r.chance(50) {
                c.gpr[t as usize] = c.gpr[s as usize];
            }
            r_type(s, t, 0, 0, 0x34)
        }),
        ("TLTU", rr!(0x33)),
        ("MFC0.EPC", |r, _c| 0x4000_0000 | reg(r) << 16 | 14 << 11),
        ("DMFC0.EPC", |r, _c| 0x4020_0000 | reg(r) << 16 | 14 << 11),
        ("MFC0.PRId", |r, _c| 0x4000_0000 | reg(r) << 16 | 15 << 11),
        ("MFC0.Config", |r, _c| 0x4000_0000 | reg(r) << 16 | 16 << 11),
        ("MTC0.EPC", |r, _c| 0x4080_0000 | reg(r) << 16 | 14 << 11),
        ("DMTC0.EPC", |r, _c| 0x40A0_0000 | reg(r) << 16 | 14 << 11),
        ("MTC0.WatchLo", |r, _c| {
            0x4080_0000 | reg(r) << 16 | 18 << 11
        }),
        ("MTC0.TagLo", |r, _c| 0x4080_0000 | reg(r) << 16 | 28 << 11),
        ("MTC0.EntryHi", |r, _c| {
            0x4080_0000 | reg(r) << 16 | 10 << 11
        }),
        ("MTC0.PageMask", |r, _c| {
            0x4080_0000 | reg(r) << 16 | 5 << 11
        }),
        ("MTC0.Index", |r, _c| 0x4080_0000 | reg(r) << 16),
        ("MTC0.Cause", |r, _c| 0x4080_0000 | reg(r) << 16 | 13 << 11),
        ("MTC1", |r, _c| {
            0x4480_0000 | reg(r) << 16 | (r.below(32) as u32) << 11
        }),
        ("MFC1", |r, _c| {
            0x4400_0000 | reg(r) << 16 | (r.below(32) as u32) << 11
        }),
        ("DMTC1", |r, _c| {
            0x44A0_0000 | reg(r) << 16 | (r.below(32) as u32) << 11
        }),
        ("DMFC1", |r, _c| {
            0x4420_0000 | reg(r) << 16 | (r.below(32) as u32) << 11
        }),
        ("CFC1", |r, _c| 0x4440_0000 | reg(r) << 16 | 31 << 11),
        ("CTC1", |r, c| {
            let t = reg(r);
            c.gpr[t as usize] = r.next() & 0x0180_0F83;
            0x44C0_0000 | t << 16 | 31 << 11
        }),
        ("LWC1", mem!(0x31, 4)),
        ("LDC1", mem!(0x35, 8)),
        ("SWC1", mem!(0x39, 4)),
        ("SDC1", mem!(0x3D, 8)),
        ("ADD.S", |r, _c| fpop(r, 0x10, 0)),
        ("SUB.S", |r, _c| fpop(r, 0x10, 1)),
        ("MUL.S", |r, _c| fpop(r, 0x10, 2)),
        ("DIV.S", |r, _c| fpop(r, 0x10, 3)),
        ("SQRT.S", |r, c| {
            let w = fpop(r, 0x10, 4);
            let fs = (w >> 11) & 31;
            for i in [fs, fs & !1] {
                c.fpr[i as usize] &= !(1 << 31);
            }
            w
        }),
        ("ADD.D", |r, _c| fpop(r, 0x11, 0)),
        ("SUB.D", |r, _c| fpop(r, 0x11, 1)),
        ("MUL.D", |r, _c| fpop(r, 0x11, 2)),
        ("DIV.D", |r, _c| fpop(r, 0x11, 3)),
        ("ABS.D", |r, _c| fpop(r, 0x11, 5)),
        ("MOV.S", |r, _c| fpop(r, 0x10, 6)),
        ("NEG.S", |r, _c| fpop(r, 0x10, 7)),
        ("CVT.D.S", |r, _c| fpop(r, 0x10, 33)),
        ("CVT.S.D", |r, _c| fpop(r, 0x11, 32)),
        ("TRUNC.W.S", |r, _c| fpop(r, 0x10, 13)),
        ("TRUNC.W.D", |r, _c| fpop(r, 0x11, 13)),
        ("C.EQ.S", |r, c| {
            let w = fpop(r, 0x10, 0x32);
            if r.chance(40) {
                let (s, t) = ((w >> 11) & 31, (w >> 16) & 31);
                c.fpr[t as usize] = c.fpr[s as usize];
                c.fpr[(t & !1) as usize] = c.fpr[(s & !1) as usize];
            }
            w & !(31 << 6)
        }),
        ("C.LT.D", |r, _c| fpop(r, 0x11, 0x3C) & !(31 << 6)),
        ("BC1T", |r, c| {
            c.fcr31 |= (r.below(2) as u32) << 23;
            slot(r, c);
            0x4501_0010
        }),
        ("ERET", |r, c| {
            c.status |= 0x2 | if r.chance(30) { 0x4 } else { 0 };
            c.cop0.retain(|(n, _)| *n != "epc");
            c.cop0.push(("epc", PC + 0x200));
            c.cop0.push(("errorepc", PC + 0x300));
            0x4200_0018
        }),
        ("MFC0.Status", |r, _c| 0x4000_0000 | reg(r) << 16 | 12 << 11),
        ("MFC0.Cause", |r, _c| 0x4000_0000 | reg(r) << 16 | 13 << 11),
        ("DMFC0.ErrorEPC", |r, _c| {
            0x4020_0000 | reg(r) << 16 | 30 << 11
        }),
        ("MTC0.Status", |r, c| {
            let t = reg(r);
            c.gpr[t as usize] = (r.next() & 0x0FFF_FF07) | 0x3000_0000;
            0x4080_0000 | t << 16 | 12 << 11
        }),
        ("MTC0.Compare", |r, c| {
            c.cop0.retain(|(n, _)| *n != "cause");
            c.cop0.push(("cause", 0x8000));
            0x4080_0000 | reg(r) << 16 | 11 << 11
        }),
        ("LLD", mem!(0x34, 8)),
        ("SCD", mem!(0x3C, 8)),
        ("CVT.S.W", |r, c| {
            let w = fpop(r, 0x14, 32);
            ints(r, c, w);
            w
        }),
        ("CVT.D.W", |r, c| {
            let w = fpop(r, 0x14, 33);
            ints(r, c, w);
            w
        }),
        ("CVT.W.S", |r, _c| fpop(r, 0x10, 36)),
        ("ROUND.W.D", |r, _c| fpop(r, 0x11, 12)),
        ("CEIL.W.S", |r, _c| fpop(r, 0x10, 14)),
        ("FLOOR.L.D", |r, _c| fpop(r, 0x11, 11)),
        ("TRUNC.L.S", |r, _c| fpop(r, 0x10, 9)),
        ("NEG.D", |r, _c| fpop(r, 0x11, 7)),
        ("ABS.S", |r, _c| fpop(r, 0x10, 5)),
        ("MOV.D", |r, _c| fpop(r, 0x11, 6)),
        ("SQRT.D", |r, c| {
            let w = fpop(r, 0x11, 4);
            let fs = (w >> 11) & 31;
            for i in [fs, fs & !1] {
                c.fpr[i as usize] &= !(1 << 63);
            }
            w
        }),
        ("C.ULE.D", |r, _c| fpop(r, 0x11, 0x37) & !(31 << 6)),
        ("C.UN.S", |r, _c| fpop(r, 0x10, 0x31) & !(31 << 6)),
        ("BC1F", |r, c| {
            c.fcr31 |= (r.below(2) as u32) << 23;
            slot(r, c);
            0x4500_0010
        }),
        ("BC1TL", |r, c| {
            c.fcr31 |= (r.below(2) as u32) << 23;
            slot(r, c);
            0x4503_0010
        }),
        ("BC1FL", |r, c| {
            c.fcr31 |= (r.below(2) as u32) << 23;
            slot(r, c);
            0x4502_0010
        }),
    ];
    v
}

/// Small integers in the source register of a conversion from W.
fn ints(r: &mut Rng, c: &mut Case, w: u32) {
    let fs = ((w >> 11) & 31) as usize;
    for i in [fs, fs & !1] {
        let x = (r.next() as i32 >> r.below(31)) as u32;
        c.fpr[i] = (c.fpr[i] & !0xFFFF_FFFF) | u64::from(x);
    }
}

fn fpop(r: &mut Rng, fmt: u32, funct: u32) -> u32 {
    0x4400_0000
        | fmt << 21
        | (r.below(32) as u32) << 16
        | (r.below(32) as u32) << 11
        | (r.below(32) as u32) << 6
        | funct
}

/// A delay slot: an ADDIU that writes a register.
fn slot(r: &mut Rng, c: &mut Case) {
    c.words.push(i_type(0x09, reg(r), reg(r), r.next() as u32));
}

/// A random machine state for the kind `name`.
pub fn random_case(r: &mut Rng, name: &str) -> Case {
    let mut gpr = [0u64; 32];
    for g in gpr.iter_mut().skip(1) {
        *g = r.gpr_val();
    }
    let mut fpr = [0u64; 32];
    for f in &mut fpr {
        *f = if r.chance(50) {
            r.f64bits()
        } else {
            (u64::from(r.f32bits()) << 32) | u64::from(r.f32bits())
        };
    }
    let fr = r.below(2) as u32;
    Case {
        name: name.into(),
        words: Vec::new(),
        gpr,
        hi: r.gpr_val(),
        lo: r.gpr_val(),
        fpr,
        fcr31: 0,
        status: 0x3000_0000 | fr << 26,
        cop0: vec![
            ("epc", r.next()),
            ("watchlo", 0),
            ("taglo", r.next() & 0xFFFF_FFFF),
            ("entryhi", 0),
            ("pagemask", 0),
            ("index", 0),
            ("compare", 0x7FFF_FFFF),
            ("cause", 0),
            ("lladdr", 0),
        ],
        data: (0..DATA_LEN).map(|_| r.next() as u8).collect(),
    }
}

/// The oracle's input for `c`.
pub fn state_json(c: &Case) -> String {
    let mut j = String::from("{\n \"gpr\": [");
    for (i, g) in c.gpr.iter().enumerate() {
        let _ = write!(j, "{}\"{:#018x}\"", if i > 0 { ", " } else { "" }, g);
    }
    let _ = write!(
        j,
        "],\n \"hi\": \"{:#018x}\", \"lo\": \"{:#018x}\", \"pc\": \"{:#018x}\",\n \"cop0\": {{\"status\": \"{:#x}\"",
        c.hi, c.lo, PC, c.status
    );
    for (n, v) in &c.cop0 {
        let _ = write!(j, ", \"{n}\": \"{v:#x}\"");
    }
    j.push_str("},\n \"fpr\": [");
    for (i, f) in c.fpr.iter().enumerate() {
        let _ = write!(j, "{}\"{:#018x}\"", if i > 0 { ", " } else { "" }, f);
    }
    let _ = write!(
        j,
        "],\n \"fcr31\": \"{:#x}\",\n \"mem\": [{{\"addr\": \"{:#x}\", \"bytes\": \"",
        c.fcr31,
        PC as u32 & 0x1FFF_FFFF
    );
    let mut words = c.words.clone();
    while words.len() < 4 {
        words.push(0);
    }
    for w in &words {
        let _ = write!(j, "{w:08x}");
    }
    let _ = write!(j, "\"}}, {{\"addr\": \"{DATA_PA:#x}\", \"bytes\": \"");
    for b in &c.data {
        let _ = write!(j, "{b:02x}");
    }
    j.push_str("\"}]\n}\n");
    j
}

/// The FP special-value corpus: every arithmetic and conversion operation over
/// denormal, NaN, infinite and range-edge operands.
pub fn special_cases() -> Vec<Case> {
    let s32: Vec<u32> = vec![
        0,
        0x8000_0000,
        1,
        0x007F_FFFF,
        0x8000_0001,
        0x0080_0000,
        0x3F80_0000,
        0xBF80_0000,
        0x3F00_0000,
        0x4020_0000,
        0x7F7F_FFFF,
        0xFF7F_FFFF,
        0x7F80_0000,
        0xFF80_0000,
        0x7FBF_FFFF,
        0x7FC0_0000,
        0x4F00_0000,
        0xCF00_0000,
        0x4EFF_FFFF,
        0x5A00_0000,
        0xDA00_0000,
        0x0000_0400,
        0x1F80_0000,
        0x0100_0000,
    ];
    let s64: Vec<u64> = vec![
        0,
        1 << 63,
        1,
        0x000F_FFFF_FFFF_FFFF,
        0x8000_0000_0000_0001,
        0x0010_0000_0000_0000,
        0x3FF0_0000_0000_0000,
        0xBFF0_0000_0000_0000,
        0x3FE0_0000_0000_0000,
        0x4004_0000_0000_0000,
        0x7FEF_FFFF_FFFF_FFFF,
        0xFFEF_FFFF_FFFF_FFFF,
        0x7FF0_0000_0000_0000,
        0xFFF0_0000_0000_0000,
        0x7FF7_FFFF_FFFF_FFFF,
        0x7FF8_0000_0000_0000,
        0x41E0_0000_0000_0000,
        0xC1E0_0000_0000_0000,
        0x41DF_FFFF_FFC0_0000,
        0x4340_0000_0000_0000,
        0xC340_0000_0000_0000,
        0x37D0_0000_0000_0000,
        0x47E1_0000_0000_0000,
        0x0000_0001_0000_0000,
    ];
    let mut out = Vec::new();
    let mut idx = 0usize;
    let mut push =
        |name: &str, fmt: u32, funct: u32, x: u64, y: u64, fcr: u32, out: &mut Vec<Case>| {
            let mut fpr = [0x1111_1111_2222_2222u64; 32];
            fpr[2] = x;
            fpr[4] = y;
            fpr[6] = 0xDEAD_BEEF_CAFE_F00D;
            let c = Case {
                name: format!("{name}-{idx:04}"),
                words: vec![0x4400_0000 | fmt << 21 | 4 << 16 | 2 << 11 | 6 << 6 | funct],
                gpr: [0; 32],
                hi: 0,
                lo: 0,
                fpr,
                fcr31: fcr,
                status: 0x3400_0000,
                cop0: vec![("compare", 0x7FFF_FFFF), ("cause", 0)],
                data: vec![0; DATA_LEN],
            };
            idx += 1;
            out.push(c);
        };
    let bin = [("ADD", 0u32), ("SUB", 1), ("MUL", 2), ("DIV", 3)];
    for (n, f) in bin {
        for fcr in [0u32, 1 << 24] {
            let k = if fcr == 0 { 24 } else { 14 };
            for &a in s32.iter().take(k) {
                for &b in s32.iter().take(k) {
                    push(
                        &format!("{n}.S"),
                        0x10,
                        f,
                        u64::from(a),
                        u64::from(b),
                        fcr,
                        &mut out,
                    );
                }
            }
            for &a in s64.iter().take(k) {
                for &b in s64.iter().take(k) {
                    push(&format!("{n}.D"), 0x11, f, a, b, fcr, &mut out);
                }
            }
        }
    }
    for (n, f) in [("SQRT", 4u32), ("ABS", 5), ("NEG", 7)] {
        for fcr in [0u32, 1 << 24] {
            for &a in &s32 {
                push(&format!("{n}.S"), 0x10, f, u64::from(a), 0, fcr, &mut out);
            }
            for &a in &s64 {
                push(&format!("{n}.D"), 0x11, f, a, 0, fcr, &mut out);
            }
        }
    }
    for fcr in [0u32, 1 << 24] {
        for &a in &s64 {
            push("CVT.S.D", 0x11, 32, a, 0, fcr, &mut out);
        }
        for &a in &s32 {
            push("CVT.D.S", 0x10, 33, u64::from(a), 0, fcr, &mut out);
        }
    }
    for (n, f) in [
        ("ROUND.L", 8u32),
        ("TRUNC.L", 9),
        ("CEIL.L", 10),
        ("FLOOR.L", 11),
        ("ROUND.W", 12),
        ("TRUNC.W", 13),
        ("CEIL.W", 14),
        ("FLOOR.W", 15),
        ("CVT.W", 36),
        ("CVT.L", 37),
    ] {
        for &a in &s32 {
            push(&format!("{n}.S"), 0x10, f, u64::from(a), 0, 0, &mut out);
        }
        for &a in &s64 {
            push(&format!("{n}.D"), 0x11, f, a, 0, 0, &mut out);
        }
    }
    out
}

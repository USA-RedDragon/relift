use std::fmt;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Word(pub u128);

impl Word {
    pub fn from_bytes(b: &[u8; 16]) -> Word {
        Word(u128::from_le_bytes(*b))
    }

    pub fn get(self, lo: u32, len: u32) -> u32 {
        ((self.0 >> lo) & ((1u128 << len) - 1)) as u32
    }

    pub fn get64(self, lo: u32, len: u32) -> u64 {
        ((self.0 >> lo) & ((1u128 << len) - 1)) as u64
    }

    pub fn bit(self, n: u32) -> bool {
        (self.0 >> n) & 1 == 1
    }

    pub fn opcode(self) -> u32 {
        self.get(0, 12)
    }

    pub fn sched(self) -> Sched {
        Sched {
            stall: self.get(105, 4) as u8,
            yield_: self.bit(109),
            wr_bar: self.get(110, 3) as u8,
            rd_bar: self.get(113, 3) as u8,
            wait_mask: self.get(116, 6) as u8,
            reuse: self.get(122, 4) as u8,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Sched {
    pub stall: u8,
    pub yield_: bool,
    pub wr_bar: u8,
    pub rd_bar: u8,
    pub wait_mask: u8,
    pub reuse: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Swz {
    #[default]
    None,
    H0H0,
    H1H1,
    H1H0,
    H0NH1,
    F32,
    B(u8),
    H(u8),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct RegMods {
    pub neg: bool,
    pub abs: bool,
    pub not: bool,
    pub reuse: bool,
    pub swz: Swz,
}

#[derive(Clone, Debug, PartialEq)]
pub enum CBank {
    Imm(u32),
    UReg(u8),
}

#[derive(Clone, Debug, PartialEq)]
pub enum Operand {
    Reg(u8, RegMods),
    UReg(u8, RegMods),
    Pred(u8, bool),
    UPred(u8, bool),
    Barrier(u8),
    ScoreBoard(u8),
    Int(i64),
    Hex(u64),
    F32(u32, bool),
    F16x2(u16, u16),
    F64Hi(u32),
    CBuf {
        bank: CBank,
        index: Option<Box<Operand>>,
        offset: i64,
        mods: RegMods,
        space: bool,
    },
    Attr {
        index: Option<Box<Operand>>,
        offset: u32,
        patch: bool,
    },
    Mem {
        base: Option<Box<Operand>>,
        ubase: Option<Box<Operand>>,
        offset: i64,
        stride: u8,
    },
    Target(i64),
    Sym(String),
}

#[derive(Clone, Debug, PartialEq)]
pub struct Inst {
    pub guard: Option<Operand>,
    pub mnemonic: &'static str,
    pub mods: Vec<&'static str>,
    pub ops: Vec<Operand>,
}

impl Inst {
    pub fn new(mnemonic: &'static str) -> Inst {
        Inst {
            guard: None,
            mnemonic,
            mods: Vec::new(),
            ops: Vec::new(),
        }
    }

    pub fn has(&self, m: &str) -> bool {
        self.mods.contains(&m)
    }
}

fn fmt_reg(f: &mut fmt::Formatter<'_>, prefix: &str, n: u8, zero: u8, m: &RegMods) -> fmt::Result {
    if m.neg {
        f.write_str("-")?;
    }
    if m.not {
        f.write_str("~")?;
    }
    if m.abs {
        f.write_str("|")?;
    }
    if n == zero {
        write!(f, "{prefix}Z")?;
    } else {
        write!(f, "{prefix}{n}")?;
    }
    let inner = m.abs && matches!(m.swz, Swz::H(_));
    if inner {
        fmt_swz(f, m.swz)?;
    }
    if m.abs {
        f.write_str("|")?;
    }
    if m.reuse {
        f.write_str(".reuse")?;
    }
    if inner { Ok(()) } else { fmt_swz(f, m.swz) }
}

fn fmt_swz(f: &mut fmt::Formatter<'_>, s: Swz) -> fmt::Result {
    match s {
        Swz::None => Ok(()),
        Swz::H0H0 => f.write_str(".H0_H0"),
        Swz::H1H1 => f.write_str(".H1_H1"),
        Swz::H1H0 => f.write_str(".H1_H0"),
        Swz::H0NH1 => f.write_str(".H0_NH1"),
        Swz::F32 => f.write_str(".F32"),
        Swz::B(n) => write!(f, ".B{n}"),
        Swz::H(n) => write!(f, ".H{n}"),
    }
}

pub fn fmt_hex(v: i64) -> String {
    if v < 0 {
        format!("-{:#x}", v.unsigned_abs())
    } else {
        format!("{v:#x}")
    }
}

pub fn fmt_f64(v: f64) -> String {
    if v.is_nan() {
        return if v.is_sign_negative() {
            "-QNAN ".into()
        } else {
            "+QNAN ".into()
        };
    }
    if v.is_infinite() {
        return if v < 0.0 {
            "-INF ".into()
        } else {
            "+INF ".into()
        };
    }
    if v == 0.0 {
        return if v.is_sign_negative() {
            "-0".into()
        } else {
            "0".into()
        };
    }
    if v.abs() >= 1e9 {
        let s = format!("{v:.20e}");
        let (m, e) = s.split_once('e').unwrap_or((&s, "0"));
        let e: i32 = e.parse().unwrap_or(0);
        return format!("{m}e{}{:02}", if e < 0 { '-' } else { '+' }, e.abs());
    }
    let p = 20i32;
    let sci = format!("{:.*e}", (p - 1) as usize, v);
    let (mant, exp) = sci.split_once('e').unwrap_or((&sci, "0"));
    let exp: i32 = exp.parse().unwrap_or(0);
    if exp < -4 || exp >= p {
        let mant = trim_zeros(mant);
        let sign = if exp < 0 { '-' } else { '+' };
        format!("{mant}e{sign}{:02}", exp.abs())
    } else {
        let decimals = (p - 1 - exp).max(0) as usize;
        trim_zeros(&format!("{v:.decimals$}")).to_string()
    }
}

fn trim_zeros(s: &str) -> &str {
    if s.contains('.') {
        s.trim_end_matches('0').trim_end_matches('.')
    } else {
        s
    }
}

impl fmt::Display for Operand {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Operand::Reg(n, m) => fmt_reg(f, "R", *n, 255, m),
            Operand::UReg(n, m) => fmt_reg(f, "UR", *n, 63, m),
            Operand::Pred(n, not) => {
                let not = if *not { "!" } else { "" };
                if *n == 7 {
                    write!(f, "{not}PT")
                } else {
                    write!(f, "{not}P{n}")
                }
            }
            Operand::UPred(n, not) => {
                let not = if *not { "!" } else { "" };
                if *n == 7 {
                    write!(f, "{not}UPT")
                } else {
                    write!(f, "{not}UP{n}")
                }
            }
            Operand::Barrier(n) => write!(f, "B{n}"),
            Operand::ScoreBoard(n) => write!(f, "SB{n}"),
            Operand::Int(v) => f.write_str(&fmt_hex(*v)),
            Operand::Hex(v) => write!(f, "{v:#x}"),
            Operand::F32(bits, neg) => {
                let v = f32::from_bits(*bits) as f64;
                let s = fmt_f64(v);
                if *neg && !s.starts_with('-') {
                    write!(f, "-{s}")
                } else {
                    f.write_str(&s)
                }
            }
            Operand::F16x2(a, b) => write!(
                f,
                "{}, {}",
                fmt_f64(f16_to_f64(*a)),
                fmt_f64(f16_to_f64(*b))
            ),
            Operand::F64Hi(bits) => f.write_str(&fmt_f64(f64::from_bits((*bits as u64) << 32))),
            Operand::CBuf {
                bank,
                index,
                offset,
                mods,
                space,
            } => {
                if mods.neg {
                    f.write_str("-")?;
                }
                if mods.not {
                    f.write_str("~")?;
                }
                if mods.abs {
                    f.write_str("|")?;
                }
                match bank {
                    CBank::Imm(b) => write!(f, "c[{b:#x}]")?,
                    CBank::UReg(u) => {
                        if *u == 63 {
                            f.write_str("cx[URZ]")?
                        } else {
                            write!(f, "cx[UR{u}]")?
                        }
                    }
                }
                if *space {
                    f.write_str(" ")?;
                }
                match index {
                    Some(r) => {
                        if *offset != 0 {
                            write!(
                                f,
                                "[{r}{}{:#x}]",
                                if *offset < 0 { "-" } else { "+" },
                                offset.unsigned_abs()
                            )?
                        } else {
                            write!(f, "[{r}]")?
                        }
                    }
                    None => write!(f, "[{}]", fmt_hex(*offset))?,
                }
                if mods.abs {
                    f.write_str("|")?;
                }
                fmt_swz(f, mods.swz)
            }
            Operand::Attr {
                index,
                offset,
                patch,
            } => {
                let p = if *patch { ".P" } else { "" };
                match index {
                    Some(r) => {
                        if *offset != 0 {
                            write!(f, "a{p}[{r}+{offset:#x}]")
                        } else {
                            write!(f, "a{p}[{r}]")
                        }
                    }
                    None => write!(f, "a{p}[{offset:#x}]"),
                }
            }
            Operand::Mem {
                base,
                ubase,
                offset,
                stride,
            } => {
                f.write_str("[")?;
                let mut any = false;
                if let Some(b) = base {
                    write!(f, "{b}")?;
                    if *stride > 1 {
                        write!(f, ".X{stride}")?;
                    }
                    any = true;
                }
                if let Some(u) = ubase {
                    if any {
                        f.write_str("+")?;
                    }
                    write!(f, "{u}")?;
                    any = true;
                }
                if *offset != 0 || !any {
                    if any {
                        if *offset < 0 {
                            write!(f, "-{:#x}", offset.unsigned_abs())?;
                        } else {
                            write!(f, "+{offset:#x}")?;
                        }
                    } else {
                        f.write_str(&fmt_hex(*offset))?;
                    }
                }
                f.write_str("]")
            }
            Operand::Target(t) => f.write_str(&fmt_hex(*t)),
            Operand::Sym(s) => f.write_str(s),
        }
    }
}

pub fn f16_to_f64(h: u16) -> f64 {
    let s = if h & 0x8000 != 0 { -1.0 } else { 1.0 };
    let e = ((h >> 10) & 0x1f) as i32;
    let m = (h & 0x3ff) as f64;
    if e == 0 {
        s * m * 2f64.powi(-24)
    } else if e == 31 {
        if m == 0.0 {
            s * f64::INFINITY
        } else {
            f64::NAN
        }
    } else {
        s * (1.0 + m / 1024.0) * 2f64.powi(e - 15)
    }
}

impl fmt::Display for Inst {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(g) = &self.guard {
            write!(f, "@{g} ")?;
        }
        f.write_str(self.mnemonic)?;
        for m in &self.mods {
            write!(f, ".{m}")?;
        }
        for (i, o) in self.ops.iter().enumerate() {
            f.write_str(if i == 0 { " " } else { ", " })?;
            write!(f, "{o}")?;
        }
        Ok(())
    }
}

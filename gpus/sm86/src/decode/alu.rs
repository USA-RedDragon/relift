use super::{D, Num, Phys, Result, Slot};
use crate::inst::{Operand, RegMods, Swz};

const RND: [&str; 4] = ["", "RM", "RP", "RZ"];
const FMUL_SCALE: [&str; 8] = ["?", "D8", "D4", "D2", "", "M2", "M4", "M8"];
const MUFU: [&str; 10] = [
    "COS", "SIN", "EX2", "LG2", "RCP", "RSQ", "RCP64H", "RSQ64H", "SQRT", "TANH",
];
const FCMP: [&str; 16] = [
    "F", "LT", "EQ", "LE", "GT", "NE", "GE", "NUM", "NAN", "LTU", "EQU", "LEU", "GTU", "NEU",
    "GEU", "T",
];
const ICMP: [&str; 8] = ["F", "LT", "EQ", "LE", "GT", "NE", "GE", "T"];
const BOOL: [&str; 4] = ["AND", "OR", "XOR", "?"];

fn rnd(d: &mut D, lo: u32) {
    let r = RND[d.get(lo, 2) as usize];
    if !r.is_empty() {
        d.m(r);
    }
}

pub(crate) fn decode(d: &mut D, op: u32) -> Result<bool> {
    match op {
        0x020 => {
            d.i.mnemonic = "FMUL";
            d.m_if(d.bit(80), "FTZ");
            d.m_if(d.bit(76), "DNZ");
            let s = FMUL_SCALE[d.get(84, 3) as usize];
            if !s.is_empty() {
                d.m(s);
            }
            rnd(d, 78);
            d.m_if(d.bit(77), "SAT");
            let dst = d.dst();
            d.op(dst);
            let a = d.src(Slot::A, Num::F32, true)?;
            d.op(a);
            let b = d.src(Slot::B, Num::F32, true)?;
            d.op(b);
        }
        0x021 => {
            d.i.mnemonic = "FADD";
            d.m_if(d.bit(80), "FTZ");
            rnd(d, 78);
            d.m_if(d.bit(77), "SAT");
            let dst = d.dst();
            d.op(dst);
            let a = d.src(Slot::A, Num::F32, true)?;
            d.op(a);
            let p = if d.form() == 1 {
                Phys::R32
            } else {
                d.phys(Slot::C)
            };
            let b = d.src_phys(p, Slot::C, Num::F32, true)?;
            d.op(b);
        }
        0x023 => {
            d.i.mnemonic = "FFMA";
            d.m_if(d.bit(80), "FTZ");
            d.m_if(d.bit(76), "DNZ");
            rnd(d, 78);
            d.m_if(d.bit(77), "SAT");
            let dst = d.dst();
            d.op(dst);
            for s in [Slot::A, Slot::B, Slot::C] {
                let o = d.src(s, Num::F32, true)?;
                d.op(o);
            }
        }
        0x009 => {
            d.i.mnemonic = "FMNMX";
            d.m_if(d.bit(80), "FTZ");
            d.m_if(d.bit(81), "NAN");
            d.m_if(d.bit(82), "XORSIGN");
            let dst = d.dst();
            d.op(dst);
            let a = d.src(Slot::A, Num::F32, true)?;
            d.op(a);
            let b = d.src(Slot::B, Num::F32, true)?;
            d.op(b);
            let p = d.pred_src(87, 90);
            d.op(p);
        }
        0x108 => {
            d.i.mnemonic = "MUFU";
            let k = d.get(74, 6) as usize;
            d.m(MUFU.get(k).copied().unwrap_or("?"));
            d.m_if(d.bit(73), "F16");
            let dst = d.dst();
            d.op(dst);
            let mut b = d.src(Slot::B, Num::F32, true)?;
            if d.bit(73) && d.bit(60) {
                b = D::swz(b, Swz::H(1));
            }
            d.op(b);
        }
        0x002 => {
            d.i.mnemonic = "MOV";
            let dst = d.dst();
            d.op(dst);
            let b = d.src(Slot::B, Num::UInt, false)?;
            d.op(b);
            let mask = d.get(72, 4);
            if mask != 0xf {
                d.op(Operand::Hex(mask as u64));
            }
        }
        0x00b => {
            d.i.mnemonic = "FSETP";
            d.m(FCMP[d.get(76, 4) as usize]);
            d.m_if(d.bit(80), "FTZ");
            d.m(BOOL[d.get(74, 2) as usize]);
            let p = d.pred_at(81);
            d.op(p);
            let q = d.pred_at(84);
            d.op(q);
            let a = d.src(Slot::A, Num::F32, true)?;
            d.op(a);
            let b = d.src(Slot::B, Num::F32, true)?;
            d.op(b);
            let c = d.pred_src(87, 90);
            d.op(c);
        }
        0x00a => {
            d.i.mnemonic = "FSET";
            d.m("BF");
            d.m(FCMP[d.get(76, 4) as usize]);
            d.m_if(d.bit(80), "FTZ");
            d.m(BOOL[d.get(74, 2) as usize]);
            let dst = d.dst();
            d.op(dst);
            let a = d.src(Slot::A, Num::F32, true)?;
            d.op(a);
            let b = d.src(Slot::B, Num::F32, true)?;
            d.op(b);
            let c = d.pred_src(87, 90);
            d.op(c);
        }
        0x00c | 0x08c => isetp(d, op >= 0x80)?,
        0x010 | 0x090 => iadd3(d, op >= 0x80)?,
        0x012 | 0x092 => lop3(d, op >= 0x80)?,
        0x008 => {
            d.i.mnemonic = "FSEL";
            let dst = d.dst();
            d.op(dst);
            let a = d.src(Slot::A, Num::F32, true)?;
            d.op(a);
            let b = d.src(Slot::B, Num::F32, true)?;
            d.op(b);
            let p = d.pred_src(87, 90);
            d.op(p);
        }
        0x007 | 0x087 => {
            let uni = op >= 0x80;
            d.i.mnemonic = if uni { "USEL" } else { "SEL" };
            let dst = dst_of(d, uni);
            d.op(dst);
            let a = src(d, uni, Slot::A, Num::UInt, false)?;
            d.op(a);
            let b = src(d, uni, Slot::B, Num::UInt, false)?;
            d.op(b);
            let p = psrc(d, uni, 87, 90);
            d.op(p);
        }
        0x013 => {
            d.i.mnemonic = "IABS";
            let dst = d.dst();
            d.op(dst);
            let b = d.src(Slot::B, Num::UInt, false)?;
            d.op(b);
        }
        _ => return other(d, op),
    }
    Ok(true)
}

fn ur(d: &D, lo: u32) -> Operand {
    Operand::UReg(d.get(lo, 6) as u8, RegMods::default())
}

fn is_zero(o: &Operand) -> bool {
    matches!(o, Operand::Reg(255, _) | Operand::UReg(63, _))
}

fn dst_of(d: &D, uni: bool) -> Operand {
    if uni { d.udst() } else { d.dst() }
}

fn pdst(d: &D, uni: bool, lo: u32) -> Operand {
    if uni { d.upred_at(lo) } else { d.pred_at(lo) }
}

fn psrc(d: &D, uni: bool, lo: u32, not: u32) -> Operand {
    if uni {
        d.upred_src(lo, not)
    } else {
        d.pred_src(lo, not)
    }
}

fn pdst_opt(d: &mut D, uni: bool, lo: u32) {
    if d.get(lo, 3) != 7 {
        let p = pdst(d, uni, lo);
        d.op(p);
    }
}

fn src(d: &D, uni: bool, s: Slot, n: Num, mods: bool) -> Result<Operand> {
    if !uni {
        return d.src(s, n, mods);
    }
    let int = matches!(n, Num::Int | Num::UInt);
    let (o, nb, ab) = match (s, d.form()) {
        (Slot::A, _) => (ur(d, 24), 72, 73),
        (Slot::B, 1) => (ur(d, 32), 63, 62),
        (Slot::B, 2) => (ur(d, 64), 75, 74),
        (Slot::B, 4) => return Ok(d.imm32(n)),
        (Slot::C, 1 | 4) => (ur(d, 64), 75, 74),
        (Slot::C, 2) => return Ok(d.imm32(n)),
        _ => return d.err(format!("uniform form {}", d.form())),
    };
    if !mods {
        return Ok(o);
    }
    Ok(D::with_mods(o, d.bit(nb), d.bit(ab), int))
}

fn isetp(d: &mut D, uni: bool) -> Result<()> {
    d.i.mnemonic = if uni { "UISETP" } else { "ISETP" };
    d.m(ICMP[d.get(76, 3) as usize]);
    d.m_if(!d.bit(73), "U32");
    d.m(BOOL[d.get(74, 2) as usize]);
    d.m_if(d.bit(72), "EX");
    let p = pdst(d, uni, 81);
    d.op(p);
    let q = pdst(d, uni, 84);
    d.op(q);
    let a = src(d, uni, Slot::A, Num::Int, false)?;
    d.op(a);
    let b = src(d, uni, Slot::B, Num::Int, false)?;
    d.op(b);
    let c = psrc(d, uni, 87, 90);
    d.op(c);
    if d.bit(72) {
        let e = psrc(d, uni, 68, 71);
        d.op(e);
    }
    Ok(())
}

fn iadd3(d: &mut D, uni: bool) -> Result<()> {
    d.i.mnemonic = if uni { "UIADD3" } else { "IADD3" };
    let x = d.bit(74);
    d.m_if(x, "X");
    let dst = dst_of(d, uni);
    d.op(dst);
    pdst_opt(d, uni, 81);
    pdst_opt(d, uni, 84);
    for s in [Slot::A, Slot::B, Slot::C] {
        let o = src(d, uni, s, Num::Int, false)?;
        let nb = d.src_neg_bit(s).is_some_and(|b| d.bit(b));
        let o = if x {
            D::not_mods(o, nb)
        } else {
            D::with_mods(o, nb, false, true)
        };
        d.op(o);
    }
    if x {
        let p = psrc(d, uni, 87, 90);
        d.op(p);
        let q = psrc(d, uni, 77, 80);
        d.op(q);
    }
    Ok(())
}

fn lop3(d: &mut D, uni: bool) -> Result<()> {
    d.i.mnemonic = if uni { "ULOP3" } else { "LOP3" };
    d.m("LUT");
    let zero_dst = if uni {
        d.get(16, 6) == 63
    } else {
        d.get(16, 8) == 255
    };
    if d.get(81, 3) != 7 || zero_dst {
        let po = pdst(d, uni, 81);
        d.op(po);
    }
    let dst = dst_of(d, uni);
    d.op(dst);
    for s in [Slot::A, Slot::B, Slot::C] {
        let o = src(d, uni, s, Num::UInt, false)?;
        d.op(o);
    }
    d.op(Operand::Hex(d.get(72, 8) as u64));
    let q = psrc(d, uni, 87, 90);
    d.op(q);
    Ok(())
}

fn imad(d: &mut D, uni: bool, wide: bool, hi: bool) -> Result<()> {
    d.i.mnemonic = if uni { "UIMAD" } else { "IMAD" };
    let dst = dst_of(d, uni);
    d.op(dst);
    let a = src(d, uni, Slot::A, Num::Int, false)?;
    let b = src(d, uni, Slot::B, Num::Int, false)?;
    let c = src(d, uni, Slot::C, Num::Int, true)?;
    let x = d.bit(74);
    if !wide && !hi && !x && !uni && d.form() <= 5 {
        let imm = d.get(32, 32);
        let form = d.form();
        let b_zero = match form {
            1 | 2 | 3 | 7 => is_zero(&b),
            _ => false,
        };
        let c_zero = is_zero(&c);
        if is_zero(&a) || b_zero || (form == 4 && imm == 0) {
            d.m("MOV");
        } else if form == 4 && imm == 1 {
            d.m(if c_zero { "MOV" } else { "IADD" });
        } else if form == 4
            && c_zero
            && imm.is_power_of_two()
            && imm != 1
            && imm != 0x10000
            && imm != 0x8000_0000
        {
            d.m("SHL");
        }
    }
    d.m_if(wide, "WIDE");
    d.m_if(hi, "HI");
    d.m_if(x, "X");
    d.m_if(!d.bit(73), "U32");
    d.op(a);
    d.op(b);
    d.op(c);
    if x {
        let p = psrc(d, uni, 87, 90);
        d.op(p);
    }
    Ok(())
}

fn lea(d: &mut D, uni: bool) -> Result<()> {
    d.i.mnemonic = if uni { "ULEA" } else { "LEA" };
    let hi = d.bit(80);
    let x = d.bit(74);
    let sx = d.bit(73);
    d.m_if(hi, "HI");
    d.m_if(x, "X");
    d.m_if(sx, "SX32");
    let dst = dst_of(d, uni);
    d.op(dst);
    pdst_opt(d, uni, 81);
    let a = src(d, uni, Slot::A, Num::Int, false)?;
    d.op(a);
    let b = src(d, uni, Slot::B, Num::UInt, true)?;
    d.op(b);
    if hi && !sx {
        let c = src(d, uni, Slot::C, Num::Int, false)?;
        d.op(c);
    }
    d.op(Operand::Hex(d.get(75, 5) as u64));
    if x {
        let p = psrc(d, uni, 87, 90);
        d.op(p);
    }
    Ok(())
}

fn shf(d: &mut D, uni: bool) -> Result<()> {
    d.i.mnemonic = if uni { "USHF" } else { "SHF" };
    d.m(if d.bit(76) { "R" } else { "L" });
    d.m_if(d.bit(75), "W");
    d.m(["S64", "U64", "S32", "U32"][d.get(73, 2) as usize]);
    d.m_if(d.bit(80), "HI");
    let dst = dst_of(d, uni);
    d.op(dst);
    let a = src(d, uni, Slot::A, Num::UInt, false)?;
    d.op(a);
    let b = src(d, uni, Slot::B, Num::UInt, false)?;
    d.op(b);
    let c = src(d, uni, Slot::C, Num::UInt, false)?;
    d.op(c);
    Ok(())
}

const PRMT: [&str; 8] = ["", "F4E", "B4E", "RC8", "ECL", "ECR", "RC16", "INVALID7"];

fn prmt(d: &mut D, uni: bool) -> Result<()> {
    d.i.mnemonic = if uni { "UPRMT" } else { "PRMT" };
    let m = PRMT[d.get(72, 3) as usize];
    if !m.is_empty() {
        d.m(m);
    }
    let dst = dst_of(d, uni);
    d.op(dst);
    for s in [Slot::A, Slot::B, Slot::C] {
        let o = src(d, uni, s, Num::UInt, false)?;
        d.op(o);
    }
    Ok(())
}

fn plop3(d: &mut D, uni: bool) -> Result<()> {
    d.i.mnemonic = if uni { "UPLOP3" } else { "PLOP3" };
    d.m("LUT");
    let p0 = pdst(d, uni, 81);
    d.op(p0);
    let p1 = pdst(d, uni, 84);
    d.op(p1);
    let s0 = psrc(d, uni, 87, 90);
    d.op(s0);
    let s1 = psrc(d, uni, 77, 80);
    d.op(s1);
    let s2 = if !uni && d.bit(67) {
        d.upred_src(68, 71)
    } else {
        psrc(d, uni, 68, 71)
    };
    d.op(s2);
    let l0 = (d.get(72, 5) << 3) | d.get(64, 3);
    d.op(Operand::Hex(l0 as u64));
    d.op(Operand::Hex(d.get(16, 8) as u64));
    Ok(())
}

fn unary(d: &mut D, uni: bool, name: &'static str, uname: &'static str) -> Result<()> {
    d.i.mnemonic = if uni { uname } else { name };
    let dst = dst_of(d, uni);
    d.op(dst);
    let b = src(d, uni, Slot::B, Num::UInt, false)?;
    let b = D::not_mods(b, d.bit(63));
    d.op(b);
    Ok(())
}

const FSWZ: [&str; 4] = ["PP", "PN", "NP", "ZP"];

fn other(d: &mut D, op: u32) -> Result<bool> {
    let uni = (0x80..0x100).contains(&op);
    match op {
        0x024 | 0x0a4 => imad(d, uni, false, false)?,
        0x025 | 0x0a5 => imad(d, uni, true, false)?,
        0x027 | 0x0a7 => imad(d, uni, false, true)?,
        0x011 | 0x091 => lea(d, uni)?,
        0x019 | 0x099 => shf(d, uni)?,
        0x016 | 0x096 => prmt(d, uni)?,
        0x01c | 0x09c => plop3(d, uni)?,
        0x017 => {
            d.i.mnemonic = "IMNMX";
            d.m_if(!d.bit(73), "U32");
            let dst = d.dst();
            d.op(dst);
            let a = d.src(Slot::A, Num::Int, false)?;
            d.op(a);
            let b = d.src(Slot::B, Num::Int, false)?;
            d.op(b);
            let p = d.pred_src(87, 90);
            d.op(p);
        }
        0x01a | 0x09a => {
            d.i.mnemonic = if uni { "USGXT" } else { "SGXT" };
            d.m_if(d.bit(75), "W");
            d.m_if(!d.bit(73), "U32");
            let dst = dst_of(d, uni);
            d.op(dst);
            let a = src(d, uni, Slot::A, Num::UInt, false)?;
            d.op(a);
            let b = src(d, uni, Slot::B, Num::UInt, false)?;
            d.op(b);
        }
        0x01b | 0x09b => {
            d.i.mnemonic = if uni { "UBMSK" } else { "BMSK" };
            d.m_if(d.bit(75), "W");
            let dst = dst_of(d, uni);
            d.op(dst);
            let a = src(d, uni, Slot::A, Num::UInt, false)?;
            d.op(a);
            let b = src(d, uni, Slot::B, Num::UInt, false)?;
            d.op(b);
        }
        0x100 | 0x0bd => {
            d.m_if(!d.bit(73), "U32");
            d.m_if(d.bit(74), "SH");
            unary(d, uni, "FLO", "UFLO")?;
        }
        0x101 | 0x0be => unary(d, uni, "BREV", "UBREV")?,
        0x109 | 0x0bf => unary(d, uni, "POPC", "UPOPC")?,
        0x003 => {
            d.i.mnemonic = "P2R";
            let dst = d.dst();
            d.op(dst);
            d.op(Operand::Sym("PR".into()));
            let a = d.src(Slot::A, Num::UInt, false)?;
            d.op(a);
            let b = d.src(Slot::B, Num::UInt, false)?;
            d.op(b);
        }
        0x004 => {
            d.i.mnemonic = "R2P";
            d.op(Operand::Sym("PR".into()));
            let a = d.src(Slot::A, Num::UInt, false)?;
            let a = match (a, d.get(76, 2)) {
                (a, 0) => a,
                (a, n) => D::swz(a, Swz::B(n as u8)),
            };
            d.op(a);
            let b = d.src(Slot::B, Num::UInt, false)?;
            d.op(b);
        }
        0x005 => {
            d.i.mnemonic = "CS2R";
            d.m_if(!d.bit(80), "32");
            let dst = d.dst();
            d.op(dst);
            let n = d.get(72, 8);
            d.op(Operand::Sym(if n == 255 {
                "SRZ".to_string()
            } else {
                format!("SR{n}")
            }));
        }
        0x022 => {
            d.i.mnemonic = "FSWZADD";
            d.m_if(d.bit(77), "NDV");
            let r = RND[d.get(78, 2) as usize];
            if !r.is_empty() {
                d.m(r);
            }
            d.m_if(d.bit(80), "FTZ");
            let dst = d.dst();
            d.op(dst);
            let a = d.src(Slot::A, Num::F32, false)?;
            d.op(a);
            let b = d.src_phys(Phys::R64, Slot::C, Num::F32, false)?;
            d.op(b);
            let sub = d.get(32, 8);
            let mut t = String::new();
            for i in (0..4).rev() {
                t.push_str(FSWZ[((sub >> (2 * i)) & 3) as usize]);
            }
            d.op(Operand::Sym(t));
        }
        0x082 => {
            d.i.mnemonic = "UMOV";
            let dst = d.udst();
            d.op(dst);
            let b = if d.form() == 6 {
                ur(d, 32)
            } else {
                d.imm32(Num::UInt)
            };
            d.op(b);
        }
        0x083 => {
            d.i.mnemonic = "UP2UR";
            let dst = d.udst();
            d.op(dst);
            d.op(Operand::Sym("UPR".into()));
            let a = ur(d, 24);
            d.op(a);
            let b = d.imm32(Num::UInt);
            d.op(b);
        }
        _ => return Ok(false),
    }
    Ok(true)
}

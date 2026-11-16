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
    let x = d.bit(74);
    let c = src(d, uni, Slot::C, Num::Int, !x)?;
    let c = match (x, d.src_neg_bit(Slot::C)) {
        (true, Some(nb)) => D::not_mods(c, d.bit(nb)),
        _ => c,
    };
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
    d.m_if(!d.bit(73), "U32");
    d.m_if(x, "X");
    if wide || hi {
        pdst_opt(d, uni, 81);
    }
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
    let a = if x {
        D::not_mods(a, d.bit(72))
    } else {
        D::with_mods(a, d.bit(72), false, true)
    };
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
    if name == "FLO" {
        pdst_opt(d, uni, 81);
    }
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
            if !(d.form() == 4 && d.get(32, 32) == 0xff) {
                let b = d.src(Slot::B, Num::UInt, false)?;
                d.op(b);
            }
        }
        0x005 => {
            d.i.mnemonic = "CS2R";
            d.m_if(!d.bit(80), "32");
            let dst = d.dst();
            d.op(dst);
            let n = d.get(72, 8);
            d.op(Operand::Sym(match SR.get(n as usize) {
                _ if n == 255 => "SRZ".to_string(),
                Some(name) if !name.is_empty() => (*name).to_string(),
                _ => format!("SR{n}"),
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
        _ => return fp(d, op),
    }
    Ok(true)
}

const FCMP_D: [&str; 16] = [
    "MIN", "LT", "EQ", "LE", "GT", "NE", "GE", "NUM", "NAN", "LTU", "EQU", "LEU", "GTU", "NEU",
    "GEU", "MAX",
];

fn space(o: Operand) -> Operand {
    match o {
        Operand::CBuf {
            bank,
            index,
            offset,
            mods,
            ..
        } => Operand::CBuf {
            bank,
            index,
            offset,
            mods,
            space: true,
        },
        o => o,
    }
}

fn himm(d: &D) -> Operand {
    let v = d.get(32, 32);
    Operand::F16x2((v >> 16) as u16, (v & 0xffff) as u16)
}

fn hswz_at(d: &D, sw: u32, slot_b: bool) -> Result<Swz> {
    let v = d.get(sw, 2) | if slot_b { u32::from(d.bit(86)) << 2 } else { 0 };
    match v {
        4 => Ok(Swz::H0NH1),
        0..=3 => Ok(hswz(v)),
        _ => d.err("half swizzle"),
    }
}

fn hswz(v: u32) -> Swz {
    match v {
        1 => Swz::F32,
        2 => Swz::H0H0,
        3 => Swz::H1H1,
        _ => Swz::None,
    }
}

fn hsrc(d: &D, s: Slot, uni: bool) -> Result<Operand> {
    let p = d.phys(s);
    if p == Phys::Imm {
        return Ok(himm(d));
    }
    let (o, nb, ab, sw) = match p {
        Phys::R24 => (d.reg_reuse(24, s), 72, 73, 74),
        Phys::R32 => (d.reg_reuse(32, s), 63, 62, 60),
        Phys::R64 => (d.reg_reuse(64, s), 84, 83, 81),
        Phys::Cb => (d.cbuf_at(32, Some(91)), 63, 62, 60),
        Phys::U32 => (d.ureg_at(32), 63, 62, 60),
        _ => return d.err(format!("form {}", d.form())),
    };
    let _ = uni;
    let o = D::with_mods(o, d.bit(nb), d.bit(ab), false);
    Ok(space(D::swz(o, hswz_at(d, sw, s == Slot::B)?)))
}

fn hsrc2(d: &D, reuse: Slot) -> Result<Operand> {
    let (p, s) = match d.form() {
        2 | 3 | 7 => (d.phys(Slot::C), Slot::C),
        _ => (d.phys(Slot::B), reuse),
    };
    if p == Phys::Imm {
        return Ok(himm(d));
    }
    let (o, nb, ab, sw) = match p {
        Phys::R32 => (d.reg_reuse(32, s), 63, 62, 60),
        Phys::R64 => (d.reg_reuse(64, s), 84, 83, 81),
        Phys::Cb => (d.cbuf_at(32, Some(91)), 63, 62, 60),
        Phys::U32 => (d.ureg_at(32), 63, 62, 60),
        _ => return d.err(format!("form {}", d.form())),
    };
    let o = D::with_mods(o, d.bit(nb), d.bit(ab), false);
    Ok(space(D::swz(o, hswz_at(d, sw, s == Slot::B)?)))
}

fn hmods(d: &mut D, f32: bool, fmz: bool, relu: bool, ftz: bool, sat: bool) {
    d.m_if(d.bit(85), "BF16_V2");
    d.m_if(f32 && d.bit(78), "F32");
    d.m_if(fmz && d.bit(76), "FMZ");
    d.m_if(relu && d.bit(79), "RELU");
    d.m_if(ftz && d.bit(80), "FTZ");
    d.m_if(sat && d.bit(77), "SAT");
}

fn dsrc(d: &D, s: Slot) -> Result<Operand> {
    d.src(s, Num::F64, true)
}

fn dsecond(d: &D, s: Slot) -> Result<Operand> {
    match d.form() {
        2 | 3 | 7 => dsrc(d, Slot::C),
        _ => dsrc(d, s),
    }
}

fn fp(d: &mut D, op: u32) -> Result<bool> {
    match op {
        0x028 | 0x029 | 0x02b => {
            d.i.mnemonic = ["DMUL", "DADD", "?", "DFMA"][(op - 0x028) as usize];
            let r = RND[d.get(78, 2) as usize];
            if !r.is_empty() {
                d.m(r);
            }
            let dst = d.dst();
            d.op(dst);
            let a = dsrc(d, Slot::A)?;
            d.op(a);
            match op {
                0x028 => {
                    let b = dsrc(d, Slot::B)?;
                    d.op(b);
                }
                0x029 => {
                    let c = dsrc(d, Slot::C)?;
                    d.op(c);
                }
                _ => {
                    let b = dsrc(d, Slot::B)?;
                    d.op(b);
                    let c = dsrc(d, Slot::C)?;
                    d.op(c);
                }
            }
        }
        0x02a => {
            d.i.mnemonic = "DSETP";
            d.m(FCMP_D[d.get(76, 4) as usize]);
            d.m(BOOL[d.get(74, 2) as usize]);
            let p = d.pred_at(81);
            d.op(p);
            let q = d.pred_at(84);
            d.op(q);
            let a = dsrc(d, Slot::A)?;
            d.op(a);
            let b = dsecond(d, Slot::B)?;
            d.op(b);
            let c = d.pred_src(87, 90);
            d.op(c);
        }
        0x026 => {
            d.i.mnemonic = "IDP";
            let a_bits = match d.get(76, 2) {
                0 => {
                    d.m("4A");
                    "8"
                }
                1 => {
                    d.m("2A");
                    d.m("LO");
                    "16"
                }
                3 => {
                    d.m("2A");
                    d.m("HI");
                    "16"
                }
                _ => return d.err("idp mode"),
            };
            let at = match (d.bit(73), a_bits) {
                (false, "8") => "U8",
                (true, "8") => "S8",
                (false, _) => "U16",
                (true, _) => "S16",
            };
            d.m(at);
            d.m(if d.bit(74) { "S8" } else { "U8" });
            let dst = d.dst();
            d.op(dst);
            for s in [Slot::A, Slot::B, Slot::C] {
                let o = d.src(s, Num::Int, true)?;
                d.op(o);
            }
        }
        0x030 | 0x032 | 0x040 => {
            d.i.mnemonic = match op {
                0x030 => "HADD2",
                0x032 => "HMUL2",
                _ => "HMNMX2",
            };
            if op == 0x040 {
                d.m_if(d.bit(85), "BF16_V2");
                d.m_if(d.bit(78), "F32");
                d.m_if(d.bit(80), "FTZ");
                d.m_if(d.bit(81), "NAN");
                d.m_if(d.bit(82), "XORSIGN");
            } else {
                hmods(d, true, op == 0x032, false, true, true);
            }
            let dst = d.dst();
            d.op(dst);
            let a = hsrc(d, Slot::A, false)?;
            d.op(a);
            let reuse = if op == 0x030 { Slot::C } else { Slot::B };
            let b = hsrc2(d, reuse)?;
            d.op(b);
            if op == 0x040 {
                let p = d.pred_src(87, 90);
                d.op(p);
            }
        }
        0x031 | 0x035 => {
            d.i.mnemonic = if op == 0x031 { "HFMA2" } else { "HFMA2.MMA" };
            hmods(d, true, true, true, true, true);
            let dst = d.dst();
            d.op(dst);
            for s in [Slot::A, Slot::B, Slot::C] {
                let o = hsrc(d, s, false)?;
                d.op(o);
            }
            if d.bit(79) && (d.get(87, 3) != 7 || d.bit(90)) {
                let p = d.pred_src(87, 90);
                d.op(p);
            }
        }
        0x033 | 0x034 => {
            let set = op == 0x033;
            d.i.mnemonic = if set { "HSET2" } else { "HSETP2" };
            d.m_if(set && d.bit(71), "BF");
            d.m(FCMP[d.get(76, 4) as usize]);
            d.m_if(d.bit(80), "FTZ");
            d.m_if(!set && d.bit(71), "H_AND");
            d.m(BOOL[d.get(69, 2) as usize]);
            if set {
                let dst = d.dst();
                d.op(dst);
            } else {
                let p = d.pred_at(81);
                d.op(p);
                let q = d.pred_at(84);
                d.op(q);
            }
            let a = hsrc(d, Slot::A, false)?;
            d.op(a);
            let b = hsrc2(d, Slot::C)?;
            d.op(b);
            let c = d.pred_src(87, 90);
            d.op(c);
        }
        _ => return cvt(d, op),
    }
    Ok(true)
}

fn fty(v: u32) -> Option<&'static str> {
    match v {
        1 => Some("F16"),
        2 => Some("F32"),
        3 => Some("F64"),
        4 => Some("BF16"),
        _ => None,
    }
}

fn ity(signed: bool, size: u32, show_s32: bool) -> Option<&'static str> {
    match (signed, size) {
        (false, 0) => Some("U8"),
        (true, 0) => Some("S8"),
        (false, 1) => Some("U16"),
        (true, 1) => Some("S16"),
        (false, 2) => Some("U32"),
        (true, 2) => Some(if show_s32 { "S32" } else { "" }),
        (false, 3) => Some("U64"),
        (true, 3) => Some("S64"),
        _ => None,
    }
}

fn cvt_src(d: &D, float: bool) -> Result<Operand> {
    let n = if float { Num::F32 } else { Num::Int };
    let o = d.src(Slot::B, n, float)?;
    if d.phys(Slot::B) == Phys::Imm {
        return Ok(o);
    }
    let sw = d.get(60, 2);
    Ok(match sw {
        0 => o,
        n if d.get(84, 3) == 0 && !float => D::swz(o, Swz::B(n as u8)),
        1 if d.get(84, 3) == 1 && !float => D::swz(o, Swz::H(1)),
        1 if float && d.get(84, 3) == 1 => D::swz(o, Swz::H(1)),
        _ => return d.err("source selector"),
    })
}

fn cvt(d: &mut D, op: u32) -> Result<bool> {
    match op {
        0x045 => {
            d.i.mnemonic = "I2FP";
            if d.get(75, 3) != 2 || d.get(84, 2) != 2 {
                return d.err("i2fp type");
            }
            d.m("F32");
            d.m(if d.bit(74) { "S32" } else { "U32" });
            match d.get(78, 2) {
                0 => {}
                3 => d.m("RZ"),
                _ => return d.err("i2fp rounding"),
            }
            let dst = d.dst();
            d.op(dst);
            let b = d.src(Slot::B, Num::UInt, false)?;
            d.op(b);
        }
        0x043 => {
            d.i.mnemonic = "F2IP";
            if d.get(76, 2) > 1 {
                return d.err("f2ip type");
            }
            d.m(if d.bit(76) { "S8" } else { "U8" });
            d.m("F32");
            match d.get(78, 2) {
                0 => {}
                3 => d.m("TRUNC"),
                _ => return d.err("f2ip rounding"),
            }
            d.m_if(d.bit(74), "NTZ");
            d.m_if(d.bit(75), "RELU");
            let dst = d.dst();
            d.op(dst);
            let a = d.reg_plain(24);
            d.op(a);
            let b = d.src(Slot::B, Num::UInt, false)?;
            d.op(b);
            let mut c = d.reg_plain(64);
            if d.bit(72) {
                c = D::swz(c, Swz::H(1));
            }
            d.op(c);
        }
        0x105 | 0x111 => {
            d.i.mnemonic = "F2I";
            d.m_if(d.bit(80), "FTZ");
            let Some(t) = ity(d.bit(72), d.get(75, 2), false) else {
                return d.err("f2i type");
            };
            if !t.is_empty() {
                d.m(t);
            }
            match d.get(84, 3) {
                2 => {}
                1 => d.m("F16"),
                3 if op == 0x111 => d.m("F64"),
                _ => return d.err("f2i source type"),
            }
            d.m(["", "FLOOR", "CEIL", "TRUNC"][d.get(78, 2) as usize]);
            d.m_if(d.bit(77), "NTZ");
            d.i.mods.retain(|m| !m.is_empty());
            let dst = d.dst();
            d.op(dst);
            let b = cvt_src(d, true)?;
            d.op(b);
        }
        0x106 | 0x112 => {
            d.i.mnemonic = "I2F";
            match d.get(75, 3) {
                1 => d.m("F16"),
                2 => {}
                3 if op == 0x112 => d.m("F64"),
                _ => return d.err("i2f type"),
            }
            let Some(t) = ity(d.bit(74), d.get(84, 2), false) else {
                return d.err("i2f source type");
            };
            d.m(t);
            d.m(RND[d.get(78, 2) as usize]);
            d.i.mods.retain(|m| !m.is_empty());
            let dst = d.dst();
            d.op(dst);
            let b = cvt_src(d, false)?;
            d.op(b);
        }
        0x107 => {
            d.i.mnemonic = "FRND";
            if d.get(75, 3) != d.get(84, 3) {
                return d.err("frnd type");
            }
            d.m_if(d.bit(80), "FTZ");
            match d.get(75, 3) {
                2 => {}
                1 => d.m("F16"),
                _ => return d.err("frnd type"),
            }
            d.m(["", "FLOOR", "CEIL", "TRUNC"][d.get(78, 2) as usize]);
            d.i.mods.retain(|m| !m.is_empty());
            let dst = d.dst();
            d.op(dst);
            let b = cvt_src(d, true)?;
            d.op(b);
        }
        0x104 | 0x110 => {
            d.i.mnemonic = "F2F";
            d.m_if(d.bit(80), "FTZ");
            let (Some(dt), Some(st)) = (fty(d.get(75, 3)), fty(d.get(84, 3))) else {
                return d.err("f2f type");
            };
            d.m(dt);
            d.m(st);
            d.m(RND[d.get(78, 2) as usize]);
            d.i.mods.retain(|m| !m.is_empty());
            let dst = d.dst();
            d.op(dst);
            let b = cvt_src(d, true)?;
            d.op(b);
        }
        0x03e | 0x0ba => {
            let uni = op == 0x0ba;
            d.i.mnemonic = if uni { "UF2FP" } else { "F2FP" };
            if d.bit(81) {
                return d.err("f2fp rs");
            }
            d.m_if(d.bit(77), "SATFINITE");
            d.m_if(d.bit(75), "RELU");
            d.m_if(d.bit(76), "BF16");
            let merge = d.bit(78);
            d.m(if merge { "MERGE_C" } else { "PACK_AB" });
            match d.get(79, 2) {
                0 => {}
                3 => d.m("RZ"),
                _ => return d.err("f2fp rounding"),
            }
            let dst = dst_of(d, uni);
            d.op(dst);
            if merge {
                let b = src(d, uni, Slot::B, Num::F32, false)?;
                d.op(b);
                let c = src(d, uni, Slot::C, Num::F32, false)?;
                d.op(c);
            } else {
                let a = src(d, uni, Slot::A, Num::F32, false)?;
                d.op(a);
                let b = src(d, uni, Slot::B, Num::F32, false)?;
                d.op(b);
            }
        }
        _ => return Ok(false),
    }
    Ok(true)
}

const SR: [&str; 134] = [
    "SR_LANEID",
    "SR_CLOCK",
    "SR_VIRTCFG",
    "SR_VIRTID",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "SR_ORDERING_TICKET",
    "SR_PRIM_TYPE",
    "SR_INVOCATION_ID",
    "SR_Y_DIRECTION",
    "SR_THREAD_KILL",
    "SM_SHADER_TYPE",
    "SR_DIRECTCBEWRITEADDRESSLOW",
    "SR_DIRECTCBEWRITEADDRESSHIGH",
    "SR_DIRECTCBEWRITEENABLED",
    "SR_SW_SCRATCH",
    "SR_MACHINE_ID_1",
    "SR_MACHINE_ID_2",
    "SR_MACHINE_ID_3",
    "SR_AFFINITY",
    "SR_INVOCATION_INFO",
    "SR_WSCALEFACTOR_XY",
    "SR_WSCALEFACTOR_Z",
    "SR_TID",
    "SR_TID.X",
    "SR_TID.Y",
    "SR_TID.Z",
    "",
    "SR_CTAID.X",
    "SR_CTAID.Y",
    "SR_CTAID.Z",
    "SR_NTID",
    "SR_CirQueueIncrMinusOne",
    "SR_NLATC",
    "",
    "SR_SM_SPA_VERSION",
    "SR_MULTIPASSSHADERINFO",
    "SR_LWINHI",
    "SR_SWINHI",
    "SR_SWINLO",
    "SR_SWINSZ",
    "SR_SMEMSZ",
    "SR_SMEMBANKS",
    "SR_LWINLO",
    "SR_LWINSZ",
    "SR_LMEMLOSZ",
    "SR_LMEMHIOFF",
    "SR_EQMASK",
    "SR_LTMASK",
    "SR_LEMASK",
    "SR_GTMASK",
    "SR_GEMASK",
    "SR_REGALLOC",
    "SR_BARRIERALLOC",
    "",
    "SR_GLOBALERRORSTATUS",
    "",
    "SR_WARPERRORSTATUS",
    "SR_VIRTUALSMID",
    "SR_VIRTUALENGINEID",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "SR_CLOCKLO",
    "SR_CLOCKHI",
    "SR_GLOBALTIMERLO",
    "SR_GLOBALTIMERHI",
    "SR_ESR_PC",
    "SR_ESR_PC_HI",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "SR_HWTASKID",
    "SR_CIRCULARQUEUEENTRYINDEX",
    "SR_CIRCULARQUEUEENTRYADDRESSLOW",
    "SR_CIRCULARQUEUEENTRYADDRESSHIGH",
    "SR_PM0",
    "SR_PM_HI0",
    "SR_PM1",
    "SR_PM_HI1",
    "SR_PM2",
    "SR_PM_HI2",
    "SR_PM3",
    "SR_PM_HI3",
    "SR_PM4",
    "SR_PM_HI4",
    "SR_PM5",
    "SR_PM_HI5",
    "SR_PM6",
    "SR_PM_HI6",
    "SR_PM7",
    "SR_PM_HI7",
    "SR_SNAP_PM0",
    "SR_SNAP_PM_HI0",
    "SR_SNAP_PM1",
    "SR_SNAP_PM_HI1",
    "SR_SNAP_PM2",
    "SR_SNAP_PM_HI2",
    "SR_SNAP_PM3",
    "SR_SNAP_PM_HI3",
    "SR_SNAP_PM4",
    "SR_SNAP_PM_HI4",
    "SR_SNAP_PM5",
    "SR_SNAP_PM_HI5",
    "SR_SNAP_PM6",
    "SR_SNAP_PM_HI6",
    "SR_SNAP_PM7",
    "SR_SNAP_PM_HI7",
    "SR_VARIABLE_RATE",
    "__HIR0X000",
];

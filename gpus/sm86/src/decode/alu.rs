use super::{D, Num, Phys, Result, Slot};
use crate::inst::{Operand, Swz};

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

fn pred_dst_opt(d: &mut D, lo: u32) {
    if d.get(lo, 3) != 7 {
        let p = d.pred_at(lo);
        d.op(p);
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
        0x00c => {
            d.i.mnemonic = "ISETP";
            d.m(ICMP[d.get(76, 3) as usize]);
            d.m_if(!d.bit(73), "U32");
            d.m(BOOL[d.get(74, 2) as usize]);
            d.m_if(d.bit(72), "EX");
            let p = d.pred_at(81);
            d.op(p);
            let q = d.pred_at(84);
            d.op(q);
            let a = d.src(Slot::A, Num::Int, false)?;
            d.op(a);
            let b = d.src(Slot::B, Num::Int, false)?;
            d.op(b);
            let c = d.pred_src(87, 90);
            d.op(c);
            if d.bit(72) {
                let e = d.pred_src(68, 71);
                d.op(e);
            }
        }
        0x010 => {
            d.i.mnemonic = "IADD3";
            let x = d.bit(74);
            d.m_if(x, "X");
            let dst = d.dst();
            d.op(dst);
            pred_dst_opt(d, 81);
            pred_dst_opt(d, 84);
            for s in [Slot::A, Slot::B, Slot::C] {
                let o = d.src(s, Num::Int, false)?;
                let nb = d.src_neg_bit(s).is_some_and(|b| d.bit(b));
                let o = if x {
                    D::not_mods(o, nb)
                } else {
                    D::with_mods(o, nb, false, true)
                };
                d.op(o);
            }
            if x {
                let p = d.pred_src(87, 90);
                d.op(p);
                let q = d.pred_src(77, 80);
                d.op(q);
            }
        }
        0x012 => {
            d.i.mnemonic = "LOP3";
            d.m("LUT");
            let p = d.get(81, 3);
            if p != 7 || d.get(16, 8) == 255 {
                let po = d.pred_at(81);
                d.op(po);
            }
            let dst = d.dst();
            d.op(dst);
            for s in [Slot::A, Slot::B, Slot::C] {
                let o = d.src(s, Num::UInt, false)?;
                d.op(o);
            }
            d.op(Operand::Hex(d.get(72, 8) as u64));
            let q = d.pred_src(87, 90);
            d.op(q);
        }
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
        0x007 => {
            d.i.mnemonic = "SEL";
            let dst = d.dst();
            d.op(dst);
            let a = d.src(Slot::A, Num::UInt, false)?;
            d.op(a);
            let b = d.src(Slot::B, Num::UInt, false)?;
            d.op(b);
            let p = d.pred_src(87, 90);
            d.op(p);
        }
        0x013 => {
            d.i.mnemonic = "IABS";
            let dst = d.dst();
            d.op(dst);
            let b = d.src(Slot::B, Num::UInt, false)?;
            d.op(b);
        }
        _ => return Ok(false),
    }
    Ok(true)
}

use super::{D, Result};
use relift_gpu_nv::Operand;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Tex,
    Tld4,
    Tld,
    Tmml,
    Txd,
    Txq,
}

const DIMS: [&str; 8] = [
    "1D",
    "2D",
    "3D",
    "CUBE",
    "ARRAY_1D",
    "ARRAY_2D",
    "",
    "ARRAY_CUBE",
];
const DIMS_LD: [&str; 8] = ["1D", "2D", "3D", "", "ARRAY_1D", "ARRAY_2D", "", ""];
const DIMS_GATHER: [&str; 8] = ["", "2D", "", "CUBE", "", "ARRAY_2D", "", "ARRAY_CUBE"];
const DIMS_GRAD: [&str; 8] = ["1D", "2D", "", "", "ARRAY_1D", "ARRAY_2D", "", ""];

const EVICT: [&str; 8] = ["EF", "", "EL", "LU", "EU", "NA", "", ""];
const LOD: [&str; 8] = ["", "LZ", "LB", "LL", "LC", "LB.LC", "LC.FDV", ""];
const COMP: [&str; 4] = ["R", "G", "B", "A"];
const QUERY: [&str; 4] = [
    "TEX_HEADER_DIMENSION",
    "TEX_HEADER_TEXTURE_TYPE",
    "TEX_HEADER_SAMPLER_POS",
    "",
];

fn named(d: &mut D, s: &'static str) {
    d.m_if(!s.is_empty(), s);
}

fn push_f16(d: &mut D, f16: u32) {
    match f16 {
        1 => d.m("F16.RN"),
        2 => d.m("F16.RZ"),
        _ => {}
    }
}

fn decode_tex(d: &mut D, k: Kind, name: &'static str, form: u32) -> Result<()> {
    d.i.mnemonic = name;
    let uses = !matches!(k, Kind::Tmml | Kind::Txq);
    let f16 = d.get(79, 2);
    if uses && f16 == 3 {
        return d.err("f16");
    }
    let evraw = d.get(84, 3) as usize;
    let ev = EVICT[evraw];
    if uses && ev.is_empty() && evraw != 1 {
        return d.err("evict");
    }
    let lod = d.get(87, 3) as usize;
    let dims = match k {
        Kind::Tld => &DIMS_LD,
        Kind::Tld4 => &DIMS_GATHER,
        Kind::Txd => &DIMS_GRAD,
        _ => &DIMS,
    };
    d.m_if(
        d.bit(60) && matches!(k, Kind::Tex | Kind::Tld4 | Kind::Tld),
        "SCR",
    );
    match k {
        Kind::Tex => {
            if lod == 7 {
                return d.err("lod");
            }
            d.m_if(d.bit(59), "B");
            push_f16(d, f16);
            named(d, LOD[lod]);
            d.m_if(d.bit(76), "AOFFI");
            d.m_if(d.bit(78), "DC");
            named(d, ev);
            d.m_if(d.bit(77), "NDV");
        }
        Kind::Tld4 => {
            d.m(COMP[lod & 3]);
            d.m_if(d.bit(59), "B");
            push_f16(d, f16);
            match d.get(76, 2) {
                1 => d.m("AOFFI"),
                2 => d.m("PTP"),
                3 => return d.err("offset"),
                _ => {}
            }
            d.m_if(d.bit(78), "DC");
            named(d, ev);
        }
        Kind::Tld => {
            d.m_if(d.bit(59), "B");
            push_f16(d, f16);
            match lod {
                1 => d.m("LZ"),
                3 => d.m("LL"),
                _ => return d.err("lod"),
            }
            d.m_if(d.bit(76), "AOFFI");
            named(d, ev);
            d.m_if(d.bit(78), "MS");
            d.m_if(d.bit(77), "CL");
        }
        Kind::Tmml => {
            d.m_if(d.bit(59), "B");
            d.m("LOD");
            d.m_if(d.bit(77), "NDV");
        }
        Kind::Txd => {
            d.m_if(d.bit(59), "B");
            push_f16(d, f16);
            d.m_if(d.bit(87), "LC");
            d.m_if(d.bit(76), "AOFFI");
            named(d, ev);
        }
        Kind::Txq => {
            d.m_if(d.bit(59), "B");
        }
    }
    d.m_if(d.bit(90), "NODEP");
    if !matches!(k, Kind::Tmml | Kind::Txq) && d.get(81, 3) != 7 {
        let p = d.pred_at(81);
        d.op(p);
    }
    let d1 = d.reg_plain(64);
    d.op(d1);
    let d0 = d.dst();
    d.op(d0);
    let b = d.bit(59);
    let s0 = if d.get(24, 8) == 255 && (b || k != Kind::Txq) {
        Operand::Sym("???255".to_string())
    } else {
        d.reg_plain(24)
    };
    d.op(s0);
    if !matches!(k, Kind::Txq) {
        let raw = d.get(32, 8);
        if k == Kind::Txd || raw != 255 {
            let s1 = d.reg_plain(32);
            d.op(s1);
        } else if b {
            d.op(Operand::Sym("???255".to_string()));
        }
    }
    if k == Kind::Txq {
        let q = QUERY[d.get(62, 2) as usize];
        if q.is_empty() {
            return d.err("query");
        }
        d.op(Operand::Sym(q.to_string()));
    }
    match form {
        5 => {
            d.op(Operand::Hex(d.get(54, 5) as u64));
            d.op(Operand::Hex(d.get(40, 14) as u64));
        }
        7 => {
            let u = d.ureg_at(40);
            d.op(u);
            d.op(Operand::Hex(d.get(54, 5) as u64));
        }
        _ => {}
    }
    if k != Kind::Txq {
        let n = dims[d.get(61, 3) as usize];
        if n.is_empty() {
            return d.err("dimension");
        }
        d.op(Operand::Sym(n.to_string()));
    }
    let mask = d.get(72, 4);
    if mask != 0xf || k == Kind::Txq {
        d.op(Operand::Hex(mask as u64));
    }
    Ok(())
}

const ORDER: [&str; 16] = [
    "",
    "CONSTANT.PRIVATE",
    "CONSTANT.CTA",
    "CONSTANT.CTA.PRIVATE",
    "STRONG.SM.PRIVATE",
    "STRONG.SM",
    "STRONG.GPU.PRIVATE",
    "STRONG.GPU",
    "MMIO.GPU",
    "CONSTANT.SM",
    "STRONG.SYS",
    "CONSTANT.SM.PRIVATE",
    "MMIO.SYS",
    "CONSTANT.VC",
    "CONSTANT.VC.PRIVATE",
    "CONSTANT.GPU",
];
const SURF_DIMS: [&str; 8] = [
    "1D",
    "1D_BUFFER",
    "1D_ARRAY",
    "2D",
    "2D_ARRAY",
    "3D",
    "",
    "",
];
const ATOM_OPS: [&str; 16] = [
    "ADD", "MIN", "MAX", "INC", "DEC", "AND", "OR", "XOR", "EXCH", "", "", "", "", "", "", "",
];
const ATOM_TYPES: [&str; 8] = [
    "",
    "S32",
    "U64",
    "F32.FTZ.RN",
    "F16x2.RN",
    "S64",
    "SD32",
    "SD64",
];
const LD_TYPES: [&str; 8] = ["U8", "S8", "U16", "S16", "", "64", "128", ""];

#[derive(Clone, Copy, PartialEq, Eq)]
enum Surf {
    LdP,
    LdD,
    StP,
    StD,
    Atom,
    Cas,
    Red,
    Query,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Handle {
    Reg,
    Imm2,
    Imm3,
}

fn surf_kind(opc: u32) -> Option<(Surf, Handle)> {
    use Handle::{Imm2, Imm3, Reg};
    let r = match opc {
        0x394 => (Surf::Atom, Reg),
        0x396 => (Surf::Cas, Reg),
        0x3a0 => (Surf::Red, Reg),
        0x998 => (Surf::LdP, Reg),
        0x99a => (Surf::LdD, Reg),
        0x99c => (Surf::StP, Reg),
        0x99e => (Surf::StD, Reg),
        0x9b3 => (Surf::Query, Reg),
        0x593 => (Surf::Atom, Imm2),
        0x595 => (Surf::Cas, Imm2),
        0x59f => (Surf::Red, Imm2),
        0xb97 => (Surf::LdP, Imm2),
        0xb99 => (Surf::LdD, Imm2),
        0xb9b => (Surf::StP, Imm2),
        0xb9d => (Surf::StD, Imm2),
        0xbb3 => (Surf::Query, Imm2),
        0x793 => (Surf::Atom, Imm3),
        0x795 => (Surf::Cas, Imm3),
        0x79f => (Surf::Red, Imm3),
        0x797 => (Surf::LdP, Imm3),
        0x799 => (Surf::LdD, Imm3),
        0x79b => (Surf::StP, Imm3),
        0x79d => (Surf::StD, Imm3),
        0x7b3 => (Surf::Query, Imm3),
        _ => return None,
    };
    Some(r)
}

fn decode_surf(d: &mut D, k: Surf, h: Handle) -> Result<()> {
    d.i.mnemonic = match k {
        Surf::LdP | Surf::LdD => "SULD",
        Surf::StP | Surf::StD => "SUST",
        Surf::Atom | Surf::Cas => "SUATOM",
        Surf::Red => "SURED",
        Surf::Query => "SUQUERY",
    };
    let formatted = matches!(k, Surf::LdP | Surf::StP);
    let binary = matches!(k, Surf::LdD | Surf::StD);
    let atomic = matches!(k, Surf::Atom | Surf::Cas | Surf::Red);
    if formatted {
        d.m("P");
    } else if binary || atomic {
        d.m("D");
    }
    d.m_if(!formatted && d.bit(72), "BA");
    let dim = SURF_DIMS[d.get(61, 3) as usize];
    if dim.is_empty() {
        return d.err("dimension");
    }
    d.m(dim);
    let evraw = d.get(84, 3) as usize;
    let ev = EVICT[evraw];
    if ev.is_empty() && evraw != 1 {
        return d.err("evict");
    }
    let mut order = ORDER[d.get(77, 4) as usize];
    if matches!(k, Surf::LdP | Surf::LdD) && d.get(77, 4) == 4 {
        order = "CONSTANT";
    }
    if k != Surf::Query {
        match k {
            Surf::LdP | Surf::StP => {
                named(d, ev);
                named(d, order);
                match d.get(72, 4) {
                    1 => d.m("R"),
                    3 => d.m("RG"),
                    15 => {}
                    _ => return d.err("channels"),
                }
            }
            Surf::LdD | Surf::StD => {
                let t = d.get(73, 3) as usize;
                if t == 7 {
                    return d.err("type");
                }
                named(d, ev);
                named(d, LD_TYPES[t]);
                named(d, order);
            }
            _ => {
                let o = if k == Surf::Cas {
                    "CAS"
                } else {
                    ATOM_OPS[d.get(87, if k == Surf::Red { 3 } else { 4 }) as usize]
                };
                if o.is_empty() {
                    return d.err("atomic op");
                }
                d.m(o);
                let ty = d.get(73, 3) as usize;
                if k == Surf::Cas && ty > 2 {
                    return d.err("type");
                }
                named(d, ev);
                named(d, ATOM_TYPES[ty]);
                named(d, order);
            }
        }
        match d.get(59, 2) {
            0 => d.m("IGN"),
            2 => d.m("TRAP"),
            3 => return d.err("trap"),
            _ => {}
        }
    }
    let has_fault = matches!(
        k,
        Surf::LdP | Surf::LdD | Surf::Atom | Surf::Cas | Surf::Query
    );
    if has_fault && d.get(81, 3) != 7 {
        let p = d.pred_at(81);
        d.op(p);
    }
    let addr = if d.get(24, 8) == 255 {
        Operand::Sym("[???255]".to_string())
    } else {
        Operand::Mem {
            base: Some(Box::new(d.reg_plain(24))),
            ubase: None,
            offset: 0,
            stride: 0,
        }
    };
    match k {
        Surf::LdP | Surf::LdD => {
            let r = d.dst();
            d.op(r);
            d.op(addr);
        }
        Surf::StP | Surf::StD | Surf::Red => {
            d.op(addr);
            let r = d.reg_plain(32);
            d.op(r);
        }
        Surf::Atom | Surf::Cas => {
            let r = d.dst();
            d.op(r);
            d.op(addr);
            let r = d.reg_plain(32);
            d.op(r);
        }
        Surf::Query => {
            let r = d.dst();
            d.op(r);
            d.op(addr);
            let q = if d.bit(87) { "???1" } else { "COMP_STATUS" };
            d.op(Operand::Sym(q.to_string()));
        }
    }
    match h {
        Handle::Reg => {
            if d.get(64, 8) == 255 {
                d.op(Operand::Sym("???255".to_string()));
            } else {
                let r = d.reg_plain(64);
                d.op(r);
            }
        }
        Handle::Imm2 => {
            d.op(Operand::Hex(d.get(54, 5) as u64));
            d.op(Operand::Hex(d.get(40, 14) as u64));
        }
        Handle::Imm3 => {
            d.op(Operand::Hex(d.get(54, 5) as u64));
            d.op(Operand::Hex(d.get(40, 6) as u64));
            d.op(Operand::Hex(d.get(46, 8) as u64));
        }
    }
    Ok(())
}

pub(crate) fn decode(d: &mut D, op: u32) -> Result<bool> {
    if let Some((k, h)) = surf_kind(d.get(0, 12)) {
        decode_surf(d, k, h)?;
        return Ok(true);
    }
    let form = d.form();
    let bindless = form == 1 && matches!(op, 0x161 | 0x164 | 0x167 | 0x16a | 0x16d | 0x170);
    if !bindless && !matches!(form, 5 | 7) {
        return Ok(false);
    }
    let (k, name) = match if bindless { op - 1 } else { op } {
        0x160 => (Kind::Tex, "TEX"),
        0x163 => (Kind::Tld4, "TLD4"),
        0x166 => (Kind::Tld, "TLD"),
        0x169 => (Kind::Tmml, "TMML"),
        0x16c => (Kind::Txd, "TXD"),
        0x16f => (Kind::Txq, "TXQ"),
        _ => return Ok(false),
    };
    decode_tex(d, k, name, form)?;
    Ok(true)
}

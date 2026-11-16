//! Integer and data-movement ALU, special registers and uniform-datapath
//! instructions.

use super::{INPUT, Res, bad};
use crate::spirv::op;
use crate::{Inst, Operand};

use super::Ctx;

mod arith;
mod bits;
pub(crate) mod conv;
mod half;

const BUILTIN_WORKGROUP_ID: u32 = 26;
const BUILTIN_LOCAL_INVOCATION_ID: u32 = 27;
const WORKGROUP_SIZE_KEY: u32 = 0x1_0000;

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "MOV" | "UMOV" => mov(c, i),
        "S2R" | "S2UR" => s2r(c, i),
        "CS2R" => cs2r(c, i),
        "ULDC" | "LDC" => ldc(c, i),
        "R2UR" => r2ur(c, i),
        _ => {
            return arith::inst(c, i)
                .or_else(|| bits::inst(c, i))
                .or_else(|| conv::inst(c, i))
                .or_else(|| half::inst(c, i));
        }
    })
}

impl Ctx {
    /// The compute workgroup size: specialization constants 0, 1, 2.
    pub(crate) fn workgroup_size(&mut self) -> u32 {
        if let Some(&v) = self.builtins.get(&WORKGROUP_SIZE_KEY) {
            return v;
        }
        let u = self.u32t();
        let v3 = self.m.t_vec(u, 3);
        let x = self.m.spec_u32(1, 0);
        let y = self.m.spec_u32(1, 1);
        let z = self.m.spec_u32(1, 2);
        let ws = self.m.spec_composite(v3, &[x, y, z]);
        self.builtins.insert(WORKGROUP_SIZE_KEY, ws);
        ws
    }

    fn uvec3_input(&mut self, b: u32, comp: u32) -> u32 {
        let u = self.u32t();
        let v3 = self.m.t_vec(u, 3);
        let var = self.builtin(b, INPUT, v3);
        let p = self.m.t_ptr(INPUT, u);
        let ci = self.m.c_u32(comp);
        let ptr = self.m.emit(op::ACCESS_CHAIN, p, &[var, ci]);
        self.m.emit(op::LOAD, u, &[ptr])
    }

    /// Writes a 64-bit value given as (low, high) to a register pair.
    pub(crate) fn store_pair(&mut self, o: &Operand, lo: u32, hi: u32) -> Res<()> {
        match o {
            Operand::Reg(255, _) | Operand::UReg(63, _) => Ok(()),
            Operand::Reg(n, _) => {
                self.store_reg(*n, lo);
                self.store_reg(n + 1, hi);
                Ok(())
            }
            Operand::UReg(n, _) => {
                self.store_ureg(*n, lo);
                self.store_ureg(n + 1, hi);
                Ok(())
            }
            o => bad(format!("pair dst {o}")),
        }
    }

    /// Reads a 64-bit operand as (low, high): a register pair or two words
    /// of a constant bank.
    pub(crate) fn src_pair(&mut self, o: &Operand) -> Res<(u32, u32)> {
        match o {
            Operand::Reg(255, _) | Operand::UReg(63, _) => {
                let z = self.m.c_u32(0);
                Ok((z, z))
            }
            Operand::Reg(n, m) if *m == Default::default() || m.reuse => {
                Ok((self.load_reg(*n), self.load_reg(n + 1)))
            }
            Operand::UReg(n, _) => Ok((self.load_ureg(*n), self.load_ureg(n + 1))),
            Operand::CBuf {
                bank,
                index,
                offset,
                mods,
                ..
            } if !mods.neg && !mods.abs && !mods.not => {
                let lo = self.cbuf_operand(bank, index.as_deref(), *offset)?;
                let hi = self.cbuf_operand(bank, index.as_deref(), *offset + 4)?;
                Ok((lo, hi))
            }
            o => bad(format!("64-bit operand {o}")),
        }
    }
}

fn mov(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.ops.len() == 3 && !matches!(i.ops[2], Operand::Hex(0xf) | Operand::Int(0xf)) {
        return bad("MOV with a partial lane mask");
    }
    let v = c.usrc(&i.ops[1])?;
    c.store_dst(&i.ops[0], v)
}

fn s2r(c: &mut Ctx, i: &Inst) -> Res<()> {
    let Operand::Sym(name) = &i.ops[1] else {
        return bad("S2R source");
    };
    let v = match name.as_str() {
        "SRZ" => c.m.c_u32(0),
        "SR_TID.X" => c.uvec3_input(BUILTIN_LOCAL_INVOCATION_ID, 0),
        "SR_TID.Y" => c.uvec3_input(BUILTIN_LOCAL_INVOCATION_ID, 1),
        "SR_TID.Z" => c.uvec3_input(BUILTIN_LOCAL_INVOCATION_ID, 2),
        "SR_CTAID.X" => c.uvec3_input(BUILTIN_WORKGROUP_ID, 0),
        "SR_CTAID.Y" => c.uvec3_input(BUILTIN_WORKGROUP_ID, 1),
        "SR_CTAID.Z" => c.uvec3_input(BUILTIN_WORKGROUP_ID, 2),
        s => match c.vtg_sysval(s) {
            Some(v) => v,
            None => return bad(format!("special register {s}")),
        },
    };
    c.store_dst(&i.ops[0], v)
}

fn cs2r(c: &mut Ctx, i: &Inst) -> Res<()> {
    match &i.ops[1] {
        Operand::Sym(s) if s == "SRZ" => {
            let z = c.m.c_u32(0);
            if i.has("32") {
                c.store_dst(&i.ops[0], z)
            } else {
                c.store_pair(&i.ops[0], z, z)
            }
        }
        o => bad(format!("CS2R {o}")),
    }
}

fn cbuf_words(i: &Inst) -> Res<(u32, Option<(u32, bool)>)> {
    let mut n = 1;
    let mut sub = None;
    for m in &i.mods {
        match *m {
            "32" => {}
            "64" => n = 2,
            "128" => n = 4,
            "U8" => sub = Some((8, false)),
            "S8" => sub = Some((8, true)),
            "U16" => sub = Some((16, false)),
            "S16" => sub = Some((16, true)),
            m => return bad(format!("{}.{m}", i.mnemonic)),
        }
    }
    Ok((n, sub))
}

/// ULDC and LDC: the bank, index register and offset come from the word.
fn ldc(c: &mut Ctx, i: &Inst) -> Res<()> {
    let w = c.word;
    let (n, sub) = cbuf_words(i)?;
    let op9 = w.get(0, 9);
    let ur_bank = match i.mnemonic {
        "ULDC" => op9 == 0xb9 && w.bit(91),
        _ => w.bit(91),
    };
    if ur_bank {
        return bad(format!("{} with a bank in a uniform register", i.mnemonic));
    }
    let bank = w.get(54, 5);
    let off = i64::from(w.get(38, 16) as u16 as i16);
    let index = match (i.mnemonic, op9) {
        ("ULDC", 0xb9) => None,
        ("ULDC", _) => Some(c.load_ureg(w.get(24, 6) as u8)),
        _ => {
            if w.get(78, 2) != 0 {
                return bad("LDC indexed-bank mode");
            }
            let r = w.get(24, 8) as u8;
            if r == 255 { None } else { Some(c.load_reg(r)) }
        }
    };
    let u = c.u32t();
    let mut words = Vec::new();
    for k in 0..n {
        let o = off + 4 * i64::from(k);
        words.push(match index {
            None => {
                if o % 4 != 0 {
                    let base = c.m.c_u32((o & 0xffff) as u32);
                    c.load_cbuf_dyn(bank, base)?
                } else {
                    c.load_cbuf(bank, o)?
                }
            }
            Some(r) => {
                let k = c.m.c_u32(o as u32);
                let a = c.m.emit(op::I_ADD, u, &[r, k]);
                c.load_cbuf_dyn(bank, a)?
            }
        });
    }
    if let Some((bits, signed)) = sub {
        let byte = match index {
            None => c.m.c_u32((off as u32 & 3) * 8),
            Some(r) => {
                let k = c.m.c_u32(off as u32);
                let a = c.m.emit(op::I_ADD, u, &[r, k]);
                let three = c.m.c_u32(3);
                let lo = c.m.emit(op::BITWISE_AND, u, &[a, three]);
                let eight = c.m.c_u32(8);
                c.m.emit(op::I_MUL, u, &[lo, eight])
            }
        };
        let cnt = c.m.c_u32(bits);
        let opc = if signed {
            op::BIT_FIELD_S_EXTRACT
        } else {
            op::BIT_FIELD_U_EXTRACT
        };
        words[0] = c.m.emit(opc, u, &[words[0], byte, cnt]);
    }
    let d = &i.ops[0];
    for (k, v) in words.into_iter().enumerate() {
        match d {
            Operand::Reg(255, _) | Operand::UReg(63, _) => {}
            Operand::Reg(r, _) => c.store_reg(r + k as u8, v),
            Operand::UReg(r, _) => c.store_ureg(r + k as u8, v),
            o => return bad(format!("dst {o}")),
        }
    }
    Ok(())
}

fn r2ur(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.has("OR") || i.ops.len() != 2 {
        return bad("R2UR with a predicate");
    }
    let v = c.usrc(&i.ops[1])?;
    c.store_dst(&i.ops[0], v)
}

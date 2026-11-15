use super::{D, Result};
use crate::inst::Operand;

fn rel_target(d: &D) -> i64 {
    let off = d.w.get64(34, 48) as i64;
    let off = (off << 16) >> 16;
    d.pc as i64 + 16 + off * 4
}

pub(crate) fn decode(d: &mut D, op: u32) -> Result<bool> {
    match op {
        0x147 => {
            d.i.mnemonic = "BRA";
            d.m_if(d.bit(32), "U");
            if d.get(87, 3) != 7 || d.bit(90) {
                let p = d.pred_src(87, 90);
                d.op(p);
            }
            d.op(Operand::Target(rel_target(d)));
        }
        0x14d => {
            d.i.mnemonic = "EXIT";
            if d.get(87, 3) != 7 || d.bit(90) {
                let p = d.pred_src(87, 90);
                d.op(p);
            }
        }
        0x145 => {
            d.i.mnemonic = "BSSY";
            d.op(Operand::Barrier(d.get(16, 4) as u8));
            d.op(Operand::Target(rel_target(d)));
        }
        0x141 => {
            d.i.mnemonic = "BSYNC";
            if d.get(87, 3) != 7 || d.bit(90) {
                let p = d.pred_src(87, 90);
                d.op(p);
            }
            d.op(Operand::Barrier(d.get(16, 4) as u8));
        }
        0x118 => {
            d.i.mnemonic = "NOP";
        }
        _ => return Ok(false),
    }
    Ok(true)
}

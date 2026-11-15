use super::{D, Result};
use crate::inst::Operand;

const ASIZE: [&str; 4] = ["", "64", "96", "128"];

pub(crate) fn decode(d: &mut D, op: u32) -> Result<bool> {
    match op {
        0x121 => {
            d.i.mnemonic = "ALD";
            let s = ASIZE[d.get(74, 2) as usize];
            if !s.is_empty() {
                d.m(s);
            }
            let dst = d.dst();
            d.op(dst);
            let idx = d.get(24, 8);
            let off = d.get(40, 11);
            let index = if idx == 255 {
                None
            } else {
                Some(Box::new(d.reg_plain(24)))
            };
            d.op(Operand::Attr {
                index,
                offset: off,
                patch: false,
            });
            if d.get(32, 8) != 255 {
                let v = d.reg_plain(32);
                d.op(v);
            }
        }
        0x122 => {
            d.i.mnemonic = "AST";
            let s = ASIZE[d.get(74, 2) as usize];
            if !s.is_empty() {
                d.m(s);
            }
            let idx = d.get(24, 8);
            let off = d.get(40, 11);
            let index = if idx == 255 {
                None
            } else {
                Some(Box::new(d.reg_plain(24)))
            };
            d.op(Operand::Attr {
                index,
                offset: off,
                patch: false,
            });
            let src = d.reg_at(32);
            d.op(src);
        }
        _ => return Ok(false),
    }
    Ok(true)
}

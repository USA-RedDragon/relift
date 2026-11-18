//! The named values of a decoded instruction (`relift_gpu_nv::Fields`):
//! for each form, the ones the translator reads.

use crate::Word;
use relift_gpu_nv::Inst;
use relift_gpu_nv::fields::{Cbuf, CbufIndex, Handle, Isbe};

/// The bound handle of a texture or surface word.
fn bound(w: Word) -> Handle {
    Handle::Bound {
        bank: w.get(54, 5),
        offset: 4 * w.get(40, 14),
    }
}

/// Sets the fields of `i`, decoded from `w`.
pub(super) fn fill(w: Word, i: &mut Inst) {
    let f = &mut i.fields;
    let reg = |lo| w.get(lo, 8) as u8;
    match i.mnemonic {
        "RET" | "BRX" | "BRXU" => {
            f.uniform = w.bit(91);
            f.ra = reg(24);
            f.ura = w.get(24, 6) as u8;
            f.offset = ((w.get64(34, 48) as i64) << 16 >> 16) * 4;
        }
        "IMAD" | "UIMAD" | "LEA" | "ULEA" => f.pd = w.get(81, 3) as u8,
        "IADD3" | "UIADD3" => {
            (f.rd, f.ra, f.rc) = (reg(16), reg(24), reg(64));
            f.imm = (i.mnemonic == "IADD3" && w.get(9, 3) == 4).then(|| w.get(32, 32));
            f.pd = w.get(81, 3) as u8;
            f.pd2 = w.get(84, 3) as u8;
        }
        "LDC" => {
            f.cbuf = Cbuf {
                bank_ureg: w.bit(91),
                bank: w.get(54, 5),
                offset: i64::from(w.get(38, 16) as u16 as i16),
                index: match reg(24) {
                    255 => CbufIndex::None,
                    r => CbufIndex::Reg(r),
                },
                mode: w.get(78, 2) as u8,
            };
        }
        "ULDC" => {
            let imm_bank = w.get(0, 9) == 0xb9;
            f.cbuf = Cbuf {
                bank_ureg: imm_bank && w.bit(91),
                bank: w.get(54, 5),
                offset: i64::from(w.get(38, 16) as u16 as i16),
                index: if imm_bank {
                    CbufIndex::None
                } else {
                    CbufIndex::UReg(w.get(24, 6) as u8)
                },
                mode: 0,
            };
        }
        "FSWZADD" => f.swizzle = w.get(32, 8) as u8,
        "TEX" | "TLD" | "TLD4" | "TMML" | "TXD" | "TXQ" => {
            (f.rd, f.ra, f.rb, f.rc) = (reg(16), reg(24), reg(32), reg(64));
            f.mask = w.get(72, 4) as u8;
            let t = &mut f.tex;
            t.handle = match w.get(9, 3) {
                5 => bound(w),
                1 => Handle::Reg,
                7 => Handle::UReg {
                    n: w.get(40, 6) as u8,
                    plain: w.get(46, 8) == 0 && w.get(54, 5) == 0,
                },
                _ => Handle::None,
            };
            t.dim = w.get(61, 3) as u8;
            t.fault = w.get(81, 3) as u8;
            let f16 = w.get(79, 2) as u8;
            match i.mnemonic {
                "TEX" => {
                    t.lod = w.get(87, 3) as u8;
                    t.aoffi = w.bit(76);
                    t.dc = w.bit(78);
                    t.scr = w.bit(60);
                    t.f16 = f16;
                }
                "TLD" => {
                    t.lod = w.get(87, 3) as u8;
                    t.aoffi = w.bit(76);
                    t.ms = w.bit(78);
                    t.cl = w.bit(77);
                    t.scr = w.bit(60);
                    t.f16 = f16;
                }
                "TLD4" => {
                    t.component = w.get(87, 2) as u8;
                    t.offsets = w.get(76, 2) as u8;
                    t.dc = w.bit(78);
                    t.scr = w.bit(60);
                    t.f16 = f16;
                }
                "TXD" => {
                    t.lc = w.bit(87);
                    t.aoffi = w.bit(76);
                    t.f16 = f16;
                }
                "TXQ" => t.query = w.get(62, 2) as u8,
                _ => {}
            }
        }
        "SULD" | "SUST" | "SUATOM" | "SURED" => {
            (f.rd, f.ra, f.rb, f.rc) = (reg(16), reg(24), reg(32), reg(64));
            f.mask = w.get(72, 4) as u8;
            let red = i.mnemonic == "SURED";
            let t = &mut f.tex;
            t.handle = match w.get(0, 12) {
                0x998 | 0x99a | 0x99c | 0x99e | 0x394 | 0x396 | 0x3a0 => Handle::Reg,
                0xb97 | 0xb99 | 0xb9b | 0xb9d | 0x593 | 0x595 | 0x59f => bound(w),
                _ => Handle::Split,
            };
            t.dim = w.get(61, 3) as u8;
            t.ty = w.get(73, 3) as u8;
            t.ba = w.bit(72);
            t.atom = w.get(87, if red { 3 } else { 4 }) as u8;
            t.fault = w.get(81, 3) as u8;
        }
        "SUQUERY" => {
            f.tex.query = u8::from(w.bit(87));
            f.tex.fault = w.get(81, 3) as u8;
        }
        "ISBERD" | "ISBEWR" => {
            f.ra = reg(24);
            f.rb = reg(32);
            f.isbe = Isbe {
                out: w.bit(79),
                skew: w.bit(78),
                class: w.get(76, 2) as u8,
                size: w.get(74, 2) as u8,
                offset: w.get(40, 16),
            };
        }
        _ => {}
    }
}

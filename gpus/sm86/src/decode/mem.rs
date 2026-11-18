use super::{D, Result};
use relift_gpu_nv::{Operand, RegMods, fmt_hex};

const ASIZE: [&str; 4] = ["", "64", "96", "128"];
const EVICT: [&str; 6] = ["EF", "", "EL", "LU", "EU", "NA"];
const SIZE: [&str; 7] = ["U8", "S8", "U16", "S16", "", "64", "128"];
const LDGSTS_SIZE: [&str; 8] = [
    "INVALID0", "INVALID1", "INVALID2", "INVALID3", "", "64", "128", "INVALID7",
];
const LTC: [&str; 4] = ["", "LTC64B", "LTC128B", "LTC256B"];
const ORDER_LOAD: [&str; 16] = [
    "",
    "CONSTANT.PRIVATE",
    "CONSTANT.CTA",
    "CONSTANT.CTA.PRIVATE",
    "CONSTANT",
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
const ATOM_OP: [&str; 9] = [
    "ADD", "MIN", "MAX", "INC", "DEC", "AND", "OR", "XOR", "EXCH",
];
const ATOM_TYPE: [&str; 7] = ["", "S32", "64", "F32.FTZ.RN", "F16x2.RN", "S64", "F64.RN"];
const STRIDE: [&str; 4] = ["", "X4", "X8", "X16"];

fn rn(n: u32) -> String {
    if n == 255 {
        "RZ".into()
    } else {
        format!("R{n}")
    }
}

fn un(n: u32) -> String {
    if n == 63 {
        "URZ".into()
    } else {
        format!("UR{n}")
    }
}

fn sext(v: u32, bits: u32) -> i64 {
    let s = 64 - bits;
    ((u64::from(v) << s) as i64) >> s
}

fn cbuf_text(bank: &str, idx: Option<String>, off: u32) -> String {
    let off = sext(off, 16);
    match idx {
        None => format!("{bank}[{}]", fmt_hex(off)),
        Some(r) if off == 0 => format!("{bank}[{r}]"),
        Some(r) => format!("{bank}[{r}+{}]", fmt_hex(off)),
    }
}

fn bracket_u(parts: &[String], raw: u32) -> String {
    if parts.is_empty() {
        return if raw == 0 {
            "[RZ]".into()
        } else {
            format!("[{raw:#x}]")
        };
    }
    let mut s = parts.join("+");
    if raw != 0 {
        s.push_str(&format!("+{raw:#x}"));
    }
    format!("[{s}]")
}

fn bracket(parts: &[String], raw: u32, bits: u32) -> String {
    if parts.is_empty() {
        return if raw == 0 {
            "[RZ]".into()
        } else {
            format!("[{}]", fmt_hex(i64::from(raw)))
        };
    }
    let off = sext(raw, bits);
    let mut s = parts.join("+");
    if off != 0 {
        s.push('+');
        s.push_str(&fmt_hex(off));
    }
    format!("[{s}]")
}

impl D {
    fn mm(&mut self, s: &'static str) {
        if !s.is_empty() {
            self.m(s);
        }
    }

    fn table(&self, t: &'static [&'static str], lo: u32, len: u32) -> Result<&'static str> {
        match t.get(self.get(lo, len) as usize) {
            Some(s) => Ok(s),
            None => self.err(format!("field {lo}:{len}")),
        }
    }

    fn sym(&mut self, s: String) {
        self.op(Operand::Sym(s));
    }

    fn pred_dst_opt(&mut self) {
        if self.get(81, 3) != 7 {
            let p = self.pred_at(81);
            self.op(p);
        }
    }

    fn pred_src_tail(&mut self) {
        let raw = self.get(64, 4);
        if raw != 0 {
            self.op(Operand::Pred(7 - (raw & 7) as u8, raw & 8 != 0));
        }
    }

    fn data_reg(&self, lo: u32) -> Operand {
        self.reg_plain(lo)
    }

    fn global_addr(&self, ur_lo: u32, wide_bit: u32, off_lo: u32, off_len: u32) -> String {
        let ur = self.bit(91);
        let mut parts = Vec::new();
        let r = self.get(24, 8);
        if r != 255 {
            let suffix = if !ur {
                ""
            } else if self.bit(wide_bit) {
                ".64"
            } else {
                ".U32"
            };
            parts.push(format!("{}{suffix}", rn(r)));
        }
        if ur {
            parts.push(un(self.get(ur_lo, 6)));
        }
        bracket(&parts, self.get(off_lo, off_len), off_len)
    }

    fn mem_addr(&self, ur_lo: u32, global: bool, wide_off: bool) -> Result<String> {
        if !self.bit(91) {
            let mut parts = Vec::new();
            if self.get(24, 8) != 255 {
                parts.push(rn(self.get(24, 8)));
            }
            return Ok(if wide_off {
                bracket(&parts, self.get(32, 32), 32)
            } else {
                bracket(&parts, self.get(40, 24), 24)
            });
        }
        if self.bit(76) {
            if self.bit(101) {
                let wide = if global {
                    if !self.bit(72) || !self.bit(90) {
                        return self.err("descriptor form");
                    }
                    ".64"
                } else {
                    ""
                };
                let mut inner = format!("{}{wide}", rn(self.get(24, 8)));
                let off = sext(self.get(40, 24), 24);
                if off != 0 {
                    inner.push('+');
                    inner.push_str(&fmt_hex(off));
                }
                return Ok(format!("desc[{}][{inner}]", un(self.get(ur_lo, 6))));
            }
            let mut parts = Vec::new();
            if self.get(24, 8) != 255 {
                let suffix = if global && self.bit(90) { ".64" } else { "" };
                parts.push(format!("{}{suffix}", rn(self.get(24, 8))));
            }
            return Ok(bracket(&parts, self.get(40, 24), 24));
        }
        Ok(if global {
            self.global_addr(ur_lo, 90, 40, 24)
        } else {
            self.local_addr(ur_lo, false)
        })
    }

    fn local_addr(&self, ur_lo: u32, stride: bool) -> String {
        let mut parts = Vec::new();
        let r = self.get(24, 8);
        let st = if stride {
            STRIDE[self.get(78, 2) as usize]
        } else {
            ""
        };
        if r != 255 || (!st.is_empty() && self.bit(91)) {
            if st.is_empty() {
                parts.push(rn(r));
            } else {
                parts.push(format!("{}.{st}", rn(r)));
            }
        }
        if self.bit(91) {
            parts.push(un(self.get(ur_lo, 6)));
        }
        bracket(&parts, self.get(40, 24), 24)
    }

    fn atomic_addr(&self, wide_bit: u32) -> String {
        let r = self.get(24, 8);
        let off = self.get(40, 24);
        let mut parts = Vec::new();
        if !self.bit(91) {
            if r != 255 {
                parts.push(rn(r));
            }
            return bracket(&parts, off, 24);
        }
        if self.bit(71) && self.bit(101) {
            let mut inner = format!("{}.64", rn(r));
            let o = sext(off, 24);
            if o != 0 {
                inner.push('+');
                inner.push_str(&fmt_hex(o));
            }
            return format!("desc[{}][{inner}]", un(self.get(64, 6)));
        }
        if r != 255 {
            let suffix = if self.bit(wide_bit) { ".64" } else { ".U32" };
            parts.push(format!("{}{suffix}", rn(r)));
        }
        if !self.bit(71) {
            parts.push(un(self.get(64, 6)));
        }
        bracket(&parts, off, 24)
    }

    fn form_any(&self, a: u32, b: u32) -> Result<()> {
        let f = self.get(0, 12);
        if f == a || f == b {
            Ok(())
        } else {
            self.err(format!("form {f:#x}"))
        }
    }

    fn form_check(&self, with_ur: u32, without_ur: u32) -> Result<()> {
        let f = self.get(0, 12);
        if f == with_ur && self.bit(91) || f == without_ur && !self.bit(91) {
            Ok(())
        } else {
            self.err(format!("form {f:#x}"))
        }
    }
}

fn ldg_like(d: &mut D, name: &'static str, legacy: bool) -> Result<()> {
    d.i.mnemonic = name;
    d.m_if(d.bit(72), "E");
    let ev = d.table(&EVICT, 84, 3)?;
    d.mm(ev);
    let l = d.table(&LTC, 68, 2)?;
    d.mm(l);
    let sz = d.table(&SIZE, 73, 3)?;
    d.mm(sz);
    let ord = d.table(&ORDER_LOAD, 77, 4)?;
    d.mm(ord);
    if !legacy {
        d.pred_dst_opt();
    }
    let dst = d.dst();
    d.op(dst);
    let s = d.mem_addr(32, true, legacy)?;
    d.sym(s);
    d.pred_src_tail();
    Ok(())
}

fn st_global(d: &mut D, name: &'static str, legacy: bool) -> Result<()> {
    d.i.mnemonic = name;
    d.m_if(d.bit(72), "E");
    let ev = d.table(&EVICT, 84, 3)?;
    d.mm(ev);
    let sz = d.table(&SIZE, 73, 3)?;
    d.mm(sz);
    let ord = d.table(&ORDER, 77, 4)?;
    d.mm(ord);
    let s = d.mem_addr(64, true, legacy)?;
    d.sym(s);
    let data = if legacy && !d.bit(91) {
        d.data_reg(64)
    } else {
        d.data_reg(32)
    };
    d.op(data);
    Ok(())
}

fn red(d: &mut D) -> Result<()> {
    d.i.mnemonic = "RED";
    d.m_if(d.bit(72), "E");
    let op = d.table(&ATOM_OP, 87, 3)?;
    d.m(op);
    let ev = d.table(&EVICT, 84, 3)?;
    d.mm(ev);
    let ty = d.table(&ATOM_TYPE, 73, 4)?;
    d.mm(ty);
    let ord = d.table(&ORDER, 77, 4)?;
    d.mm(ord);
    let s = d.atomic_addr(90);
    d.sym(s);
    let data = d.data_reg(32);
    d.op(data);
    Ok(())
}

fn cas_name(d: &mut D) {
    match d.get(87, 2) {
        0 | 2 => d.m("CAS"),
        1 => d.m("CAST"),
        _ => {
            d.m("CAST");
            d.m("SPIN");
        }
    }
}

fn atom_global(d: &mut D, name: &'static str, cas: bool) -> Result<()> {
    d.i.mnemonic = name;
    d.m_if(d.bit(72), "E");
    if cas && name == "ATOM" {
        cas_name(d);
    } else if cas {
        d.m("CAS");
    } else {
        let op = d.table(&ATOM_OP, 87, 4)?;
        d.m(op);
    }
    let ev = d.table(&EVICT, 84, 3)?;
    d.mm(ev);
    let ty = d.table(&ATOM_TYPE, 73, 4)?;
    d.mm(ty);
    let ord = d.table(&ORDER, 77, 4)?;
    d.mm(ord);
    let p = d.pred_at(81);
    d.op(p);
    let dst = d.dst();
    d.op(dst);
    let s = if cas {
        d.atomic_addr(72)
    } else {
        d.atomic_addr(70)
    };
    d.sym(s);
    let a = d.data_reg(32);
    d.op(a);
    if cas {
        let b = d.data_reg(64);
        d.op(b);
    }
    Ok(())
}

/// `ATOMS.POPC.INC.32 Rd, [Ra.stride+URb+imm]`: a shared memory increment
/// with no source register (form 0xf8c).
fn atoms_popc_inc(d: &mut D) -> Result<()> {
    if d.get(87, 5) != 0x1a {
        return d.err("atoms popc op");
    }
    if d.get(73, 2) != 0 {
        return d.err("atoms popc type");
    }
    d.i.mnemonic = "ATOMS";
    d.m("POPC");
    d.m("INC");
    d.m("32");
    let dst = d.dst();
    d.op(dst);
    let r = d.get(24, 8);
    let st = STRIDE[d.get(78, 2) as usize];
    let mut parts = Vec::new();
    if r != 255 || !st.is_empty() {
        parts.push(if st.is_empty() {
            rn(r)
        } else {
            format!("{}.{st}", rn(r))
        });
    }
    parts.push(un(d.get(64, 6)));
    let s = bracket(&parts, d.get(40, 24), 24);
    d.sym(s);
    Ok(())
}

fn attr_index(d: &D) -> Option<Box<Operand>> {
    if d.get(24, 8) == 255 {
        None
    } else {
        Some(Box::new(d.reg_plain(24)))
    }
}

pub(crate) fn decode(d: &mut D, op: u32) -> Result<bool> {
    match op {
        0x121 => {
            d.i.mnemonic = "ALD";
            d.m_if(d.bit(79), "O");
            d.m_if(d.bit(76), "P");
            let s = ASIZE[d.get(74, 2) as usize];
            if !s.is_empty() {
                d.m(s);
            }
            let dst = d.dst();
            d.op(dst);
            let off = d.get(40, 11);
            let index = attr_index(d);
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
            d.m_if(d.bit(76), "P");
            let s = ASIZE[d.get(74, 2) as usize];
            if !s.is_empty() {
                d.m(s);
            }
            let off = d.get(40, 11);
            let index = attr_index(d);
            d.op(Operand::Attr {
                index,
                offset: off,
                patch: false,
            });
            let src = d.reg_at(32);
            d.op(src);
            if d.get(64, 8) != 255 {
                let v = d.reg_plain(64);
                d.op(v);
            }
        }
        0x181 => {
            d.form_any(0x981, 0x381)?;
            ldg_like(d, "LDG", false)?;
        }
        0x180 => {
            d.form_any(0x980, 0x380)?;
            ldg_like(d, "LD", true)?;
        }
        0x186 => {
            d.form_any(0x986, 0x386)?;
            st_global(d, "STG", false)?;
        }
        0x185 => {
            d.form_any(0x985, 0x385)?;
            st_global(d, "ST", true)?;
        }
        0x18e => {
            d.form_any(0x98e, 0x38e)?;
            red(d)?;
        }
        0x1a8 => {
            d.form_any(0x9a8, 0x3a8)?;
            atom_global(d, "ATOMG", false)?;
        }
        0x1a9 => {
            if d.get(0, 12) != 0x3a9 {
                return d.err("form");
            }
            atom_global(d, "ATOMG", true)?;
        }
        0x18a => {
            d.form_any(0x98a, 0x38a)?;
            atom_global(d, "ATOM", false)?;
        }
        0x18b => {
            if d.get(0, 12) != 0x38b {
                return d.err("form");
            }
            atom_global(d, "ATOM", true)?;
        }
        0x18c | 0x18d => {
            let cas = op == 0x18d;
            let f = d.get(0, 12);
            if f == 0xf8c {
                atoms_popc_inc(d)?;
                return Ok(true);
            }
            let ur = f == 0x98c;
            if !(ur || f == 0x38c && !cas || f == 0x38d && cas) {
                return d.err("form");
            }
            d.i.mnemonic = "ATOMS";
            if cas {
                cas_name(d);
            } else {
                let o = d.table(&ATOM_OP, 87, 4)?;
                d.m(o);
            }
            let ty = ["", "S32", "64"];
            let Some(ty) = ty.get(d.get(73, 2) as usize) else {
                return d.err("atoms type");
            };
            d.mm(ty);
            let dst = d.dst();
            d.op(dst);
            let mut parts = Vec::new();
            let r = d.get(24, 8);
            let st = STRIDE[d.get(78, 2) as usize];
            if r != 255 || (!st.is_empty() && ur) {
                parts.push(if st.is_empty() {
                    rn(r)
                } else {
                    format!("{}.{st}", rn(r))
                });
            }
            if ur {
                parts.push(un(d.get(64, 6)));
            }
            let s = bracket(&parts, d.get(40, 24), 24);
            d.sym(s);
            let a = d.reg_plain(32);
            d.op(a);
            if cas {
                let b = d.reg_plain(64);
                d.op(b);
            }
        }
        0x183 | 0x184 => {
            if d.get(0, 12) != op | 0x800 {
                return d.err("form");
            }
            let shared = op == 0x184;
            d.i.mnemonic = if shared { "LDS" } else { "LDL" };
            if !shared {
                let ev = d.table(&EVICT, 84, 3)?;
                d.mm(ev);
            }
            let sz = d.table(&SIZE, 73, 3)?;
            d.mm(sz);
            let dst = d.dst();
            d.op(dst);
            let s = if shared {
                d.local_addr(32, true)
            } else {
                d.mem_addr(32, false, false)?
            };
            d.sym(s);
        }
        0x187 | 0x188 => {
            let shared = op == 0x188;
            d.form_check(op | 0x800, op | 0x200)?;
            d.i.mnemonic = if shared { "STS" } else { "STL" };
            if !shared {
                let ev = d.table(&EVICT, 84, 3)?;
                d.mm(ev);
            }
            let sz = d.table(&SIZE, 73, 3)?;
            d.mm(sz);
            let s = if shared {
                d.local_addr(64, true)
            } else {
                d.mem_addr(64, false, false)?
            };
            d.sym(s);
            let data = d.data_reg(32);
            d.op(data);
        }
        0x0b9 | 0x0bb => {
            d.i.mnemonic = "ULDC";
            let sz = d.table(&SIZE, 73, 3)?;
            d.mm(sz);
            let dst = d.udst();
            d.op(dst);
            let (bank, idx) = if op == 0xb9 {
                let b = if d.bit(91) {
                    format!("cx[{}]", un(d.get(24, 6)))
                } else {
                    format!("c[{:#x}]", d.get(54, 5))
                };
                (b, None)
            } else {
                (format!("c[{:#x}]", d.get(54, 5)), Some(un(d.get(24, 6))))
            };
            let s = if op == 0xb9 && d.bit(91) {
                format!("{bank}[{:#x}]", d.get(38, 16))
            } else {
                cbuf_text(&bank, idx, d.get(38, 16))
            };
            d.sym(s);
        }
        0x182 => {
            d.i.mnemonic = "LDC";
            let sz = d.table(&SIZE, 73, 3)?;
            d.mm(sz);
            let md = ["", "IL", "IS", "ISL"][d.get(78, 2) as usize];
            d.mm(md);
            let dst = d.dst();
            d.op(dst);
            let f = d.get(0, 12);
            let (bank, ridx) = match f {
                0xb82 if !d.bit(91) => (format!("c[{:#x}]", d.get(54, 5)), d.get(24, 8)),
                0x582 if d.bit(91) => (format!("cx[{}]", un(d.get(24, 6))), d.get(64, 8)),
                _ => return d.err("form"),
            };
            let off = d.get(38, 16);
            let idx = if ridx != 255 || off == 0 {
                Some(rn(ridx))
            } else {
                None
            };
            let s = cbuf_text(&bank, idx, off);
            d.sym(s);
        }
        0x189 => {
            d.i.mnemonic = "SHFL";
            let m = ["IDX", "UP", "DOWN", "BFLY"][d.get(58, 2) as usize];
            d.m(m);
            let f = d.get(0, 12);
            if !matches!(f, 0x389 | 0x589 | 0x989 | 0xf89) {
                return d.err("form");
            }
            let p = d.pred_at(81);
            d.op(p);
            let dst = d.dst();
            d.op(dst);
            let a = d.reg_plain(24);
            d.op(a);
            if matches!(f, 0x389 | 0x589) {
                let l = d.reg_plain(32);
                d.op(l);
            } else {
                d.op(Operand::Hex(u64::from(d.get(53, 5))));
            }
            if matches!(f, 0x389 | 0x989) {
                let c = d.reg_plain(64);
                d.op(c);
            } else {
                d.op(Operand::Hex(u64::from(d.get(40, 13))));
            }
        }
        0x126 => {
            let f = d.get(0, 12);
            if !matches!(f, 0x326 | 0x526 | 0x926 | 0xb26) {
                return d.err("form");
            }
            d.i.mnemonic = "IPA";
            let fr = ["PASS", "CONSTANT", "STATE"];
            let Some(fr) = fr.get(d.get(78, 2) as usize) else {
                return d.err("ipa freq");
            };
            d.m(fr);
            let loc = ["", "CENTROID", "OFFSET"];
            let Some(loc) = loc.get(d.get(76, 2) as usize) else {
                return d.err("ipa loc");
            };
            d.mm(loc);
            let dst = d.dst();
            d.op(dst);
            d.pred_dst_opt();
            let index = if f != 0x326 && d.bit(91) {
                Some(Box::new(Operand::UReg(
                    d.get(24, 6) as u8,
                    RegMods::default(),
                )))
            } else {
                None
            };
            d.op(Operand::Attr {
                index,
                offset: d.get(64, 8) << 2,
                patch: false,
            });
            if d.get(76, 2) == 2 {
                match f {
                    0x926 => d.op(Operand::Hex(u64::from(d.get(32, 32)))),
                    0xb26 => {
                        let b = format!("c[{:#x}]", d.get(54, 5));
                        let s = cbuf_text(&b, None, d.get(38, 16));
                        d.sym(s);
                    }
                    _ => {
                        let o = d.reg_plain(32);
                        d.op(o);
                    }
                }
            }
        }
        0x123 | 0x127 => {
            let wr = op == 0x127;
            if d.get(0, 12) != op | 0x800 {
                return d.err("form");
            }
            d.i.mnemonic = if wr { "ISBEWR" } else { "ISBERD" };
            d.m_if(d.bit(79), "O");
            let cls = ["", "PATCH", "PRIM", "ATTR"][d.get(76, 2) as usize];
            d.mm(cls);
            d.m_if(d.bit(78), "SKEW");
            let sz = ["", "U16", "32"];
            let Some(sz) = sz.get(d.get(74, 2) as usize) else {
                return d.err("isbe size");
            };
            d.mm(sz);
            let a = {
                let mut parts = Vec::new();
                if d.get(24, 8) != 255 {
                    parts.push(rn(d.get(24, 8)));
                }
                bracket_u(&parts, d.get(40, 16))
            };
            if wr {
                d.sym(a);
                let r = d.reg_plain(32);
                d.op(r);
            } else {
                let r = d.dst();
                d.op(r);
                d.sym(a);
            }
        }
        0x192 => {
            d.i.mnemonic = "MEMBAR";
            let t = ["SC", "ALL", "", "MMIO"][d.get(79, 2) as usize];
            d.mm(t);
            let sc = ["CTA", "SM", "GPU", "SYS", "", "VC"];
            match sc.get(d.get(76, 3) as usize) {
                Some(s) if !s.is_empty() => d.m(s),
                _ => return d.err("membar scope"),
            }
        }
        0x1ab => d.i.mnemonic = "ERRBAR",
        0x18f..=0x191 => {
            d.i.mnemonic = match op {
                0x18f => "CCTL",
                0x190 => "CCTLL",
                _ => "CCTLT",
            };
            if op == 0x18f {
                d.m_if(d.bit(72), "E");
                let cls = ["", "U", "C", "I"];
                let Some(cls) = cls.get(d.get(78, 3) as usize) else {
                    return d.err("cctl class");
                };
                d.mm(cls);
            }
            let names = [
                "PF1", "PF2", "WB", "IV", "IVALL", "RS", "IVALLP", "", "", "PML2", "DML2", "RML2",
            ];
            let idx = if op == 0x191 {
                match d.get(85, 2) {
                    2 => 4,
                    _ => return d.err("cctlt op"),
                }
            } else {
                d.get(87, 4)
            };
            let name = match names.get(idx as usize) {
                Some(s) if !s.is_empty() => *s,
                _ => return d.err("cctl op"),
            };
            d.m(name);
            if name != "IVALL" && name != "IVALLP" {
                let mut parts = Vec::new();
                if d.get(24, 8) != 255 {
                    parts.push(rn(d.get(24, 8)));
                }
                let s = if op == 0x18f {
                    bracket(&parts, d.get(32, 32), 32)
                } else {
                    bracket(&parts, d.get(40, 24), 24)
                };
                d.sym(s);
            }
        }
        0x1aa => {
            if d.form() != 1 {
                return d.err("qspc form");
            }
            d.i.mnemonic = "QSPC";
            d.m_if(d.bit(72), "E");
            match d.get(74, 2) {
                1 => d.m("S"),
                0 => d.m("G"),
                _ => return d.err("qspc space"),
            }
            let p = d.pred_at(81);
            d.op(p);
            let dst = d.dst();
            d.op(dst);
            let off = sext(d.get(40, 24), 24);
            let r = rn(d.get(24, 8));
            let s = if off == 0 {
                format!("[{r}]")
            } else {
                format!("[{r}+{}]", fmt_hex(off))
            };
            d.sym(s);
        }
        0x1ae => {
            if !matches!(d.form(), 7) {
                return d.err("ldgsts form");
            }
            d.i.mnemonic = "LDGSTS";
            d.m("E");
            d.m_if(!d.bit(81), "BYPASS");
            let ev = d.table(&EVICT, 84, 3)?;
            d.mm(ev);
            d.m_if(d.bit(72), "LTC128B");
            let sz = d.table(&LDGSTS_SIZE, 73, 3)?;
            d.mm(sz);
            let ord = d.table(&ORDER_LOAD, 77, 4)?;
            d.mm(ord);
            d.m_if(d.bit(82), "ZFILL");
            let dst = format!(
                "[{}{}]",
                rn(d.get(16, 8)),
                match sext(d.get(44, 20), 20) {
                    0 => String::new(),
                    v => format!("+{}", fmt_hex(v)),
                }
            );
            d.sym(dst);
            let off = sext(d.get(32, 12), 12);
            let mut src = format!("[{}.64", rn(d.get(24, 8)));
            if !d.bit(76) {
                src.push_str(&format!("+{}", un(d.get(64, 6))));
            }
            if off != 0 {
                src.push_str(&format!("+{}", fmt_hex(off)));
            }
            src.push(']');
            d.sym(src);
            if d.get(87, 3) != 7 || d.bit(90) {
                let p = d.pred_src(87, 90);
                d.op(p);
            }
        }
        0x1af => d.i.mnemonic = "LDGDEPBAR",
        0x1c0 => {
            d.i.mnemonic = "GETLMEMBASE";
            let dst = d.dst();
            d.op(dst);
        }
        0x1c1 => {
            d.i.mnemonic = "SETLMEMBASE";
            let a = d.reg_plain(24);
            d.op(a);
        }
        0x120 => {
            d.i.mnemonic = "AL2P";
            d.m_if(d.bit(79), "O");
            let s = ASIZE[d.get(74, 2) as usize];
            d.mm(s);
            let dst = d.dst();
            d.op(dst);
            let a = d.reg_plain(24);
            d.op(a);
            d.op(Operand::Int(sext(d.get(40, 11), 11)));
        }
        0x124 => {
            d.i.mnemonic = "OUT";
            let t = ["FINAL", "EMIT", "CUT", "EMIT_THEN_CUT"][d.get(78, 2) as usize];
            d.m(t);
            let fin = d.get(78, 2) == 0;
            if !fin || d.get(16, 8) != 255 {
                let dst = d.dst();
                d.op(dst);
            }
            let a = d.reg_plain(24);
            d.op(a);
            let b = match d.get(0, 12) {
                0x324 => d.reg_plain(32),
                0x924 => Operand::Int(i64::from(d.get(32, 32) as i32)),
                0xd24 => d.ureg_at(32),
                _ => return d.err("form"),
            };
            if !(fin && d.get(32, 8) == 255 && d.get(0, 12) == 0x324) {
                d.op(b);
            }
        }
        0x125 => {
            d.i.mnemonic = "PIXLD";
            let t = [
                "",
                "COVMASK",
                "CENTROID_OFFSET",
                "MY_INDEX",
                "INNER_COVERAGE",
            ];
            let Some(t) = t.get(d.get(78, 3) as usize) else {
                return d.err("pixld");
            };
            d.mm(t);
            let dst = d.dst();
            d.op(dst);
            d.pred_dst_opt();
        }
        0x1ad => {
            d.i.mnemonic = "LDTRAM";
            d.m(if d.bit(72) { "C" } else { "AB" });
            let dst = d.dst();
            d.op(dst);
            let index = if d.bit(91) && d.get(24, 6) != 63 {
                Some(Box::new(Operand::UReg(
                    d.get(24, 6) as u8,
                    RegMods::default(),
                )))
            } else {
                None
            };
            d.op(Operand::Attr {
                index,
                offset: d.get(64, 8) << 2,
                patch: false,
            });
        }
        _ => return Ok(false),
    }
    Ok(true)
}

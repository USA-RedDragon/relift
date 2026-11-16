//! Bit manipulation and predicate logic: LOP3, PLOP3, PRMT, SGXT, BMSK,
//! FLO, POPC, BREV, P2R and R2P, with their uniform-datapath twins.

use crate::spirv::{glsl, op};
use crate::translate::{Ctx, Res, bad};
use crate::{Inst, Operand, Swz};

pub(super) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "LOP3" | "ULOP3" => lop3(c, i),
        "PLOP3" | "UPLOP3" => plop3(c, i),
        "PRMT" | "UPRMT" => prmt(c, i),
        "SGXT" | "USGXT" => sgxt(c, i),
        "BMSK" | "UBMSK" => bmsk(c, i),
        "FLO" | "UFLO" => flo(c, i),
        "POPC" | "UPOPC" => popc(c, i),
        "BREV" | "UBREV" => brev(c, i),
        "P2R" => p2r(c, i),
        "R2P" => r2p(c, i),
        "UP2UR" => up2ur(c, i),
        _ => return None,
    })
}

impl Ctx {
    /// The three-input lookup table `lut` over words: result bit `(a, b, c)`
    /// is bit `a * 4 + b * 2 + c` of the table.
    pub(crate) fn lut3(&mut self, a: u32, b: u32, c: u32, lut: u32) -> u32 {
        let lut = lut & 0xff;
        if lut == 0 {
            return self.kc(0);
        }
        if lut == 0xff {
            return self.kc(u32::MAX);
        }
        let inv = lut.count_ones() > 4;
        let l = if inv { !lut & 0xff } else { lut };
        let na = self.inot(a);
        let nb = self.inot(b);
        let nc = self.inot(c);
        let mut acc: Option<u32> = None;
        for m in 0..8u32 {
            if (l >> m) & 1 == 0 {
                continue;
            }
            let ta = if m & 4 != 0 { a } else { na };
            let tb = if m & 2 != 0 { b } else { nb };
            let tc = if m & 1 != 0 { c } else { nc };
            let ab = self.ib(op::BITWISE_AND, ta, tb);
            let t = self.ib(op::BITWISE_AND, ab, tc);
            acc = Some(match acc {
                None => t,
                Some(p) => self.ib(op::BITWISE_OR, p, t),
            });
        }
        let r = acc.unwrap_or_else(|| self.kc(0));
        if inv { self.inot(r) } else { r }
    }

    /// The lookup table over predicates.
    pub(crate) fn plut3(&mut self, a: u32, b: u32, c: u32, lut: u32) -> u32 {
        let t = self.boolt();
        let lut = lut & 0xff;
        let mut acc: Option<u32> = None;
        for m in 0..8u32 {
            if (lut >> m) & 1 == 0 {
                continue;
            }
            let ta = self.not_if(a, m & 4 == 0);
            let tb = self.not_if(b, m & 2 == 0);
            let tc = self.not_if(c, m & 1 == 0);
            let ab = self.m.emit(op::LOGICAL_AND, t, &[ta, tb]);
            let term = self.m.emit(op::LOGICAL_AND, t, &[ab, tc]);
            acc = Some(match acc {
                None => term,
                Some(p) => self.m.emit(op::LOGICAL_OR, t, &[p, term]),
            });
        }
        acc.unwrap_or_else(|| self.m.c_bool(false))
    }

    fn nonzero(&mut self, v: u32) -> u32 {
        let z = self.kc(0);
        self.icmp_op(op::I_NOT_EQUAL, v, z)
    }
}

fn lut_of(o: &Operand) -> Res<u32> {
    match o {
        Operand::Hex(v) => Ok(*v as u32),
        Operand::Int(v) => Ok(*v as u32),
        o => bad(format!("lut {o}")),
    }
}

fn lop3(c: &mut Ctx, i: &Inst) -> Res<()> {
    let n = i.ops.len();
    if n < 6 {
        return bad("LOP3 form");
    }
    let lut = lut_of(&i.ops[n - 2])?;
    let a = c.usrc(&i.ops[n - 5])?;
    let b = c.usrc(&i.ops[n - 4])?;
    let cv = c.usrc(&i.ops[n - 3])?;
    let r = c.lut3(a, b, cv, lut);
    if n == 7 {
        let p = c.nonzero(r);
        c.store_pdst(&i.ops[0], p)?;
    }
    c.store_dst(&i.ops[n - 6], r)
}

fn plop3(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.ops.len() != 7 {
        return bad("PLOP3 form");
    }
    let s0 = c.pred_operand(&i.ops[2])?;
    let s1 = c.pred_operand(&i.ops[3])?;
    let s2 = c.pred_operand(&i.ops[4])?;
    let l0 = lut_of(&i.ops[5])?;
    let l1 = lut_of(&i.ops[6])?;
    let p0 = c.plut3(s0, s1, s2, l0);
    let p1 = c.plut3(s0, s1, s2, l1);
    c.store_pdst(&i.ops[0], p0)?;
    c.store_pdst(&i.ops[1], p1)
}

fn prmt(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.ops.len() != 4 {
        return bad("PRMT form");
    }
    let mode = match i.mods.as_slice() {
        [] => "",
        [m] if matches!(*m, "F4E" | "B4E" | "RC8" | "ECL" | "ECR" | "RC16") => m,
        _ => return bad("PRMT mode"),
    };
    let a = c.usrc(&i.ops[1])?;
    let s = c.usrc(&i.ops[2])?;
    let b = c.usrc(&i.ops[3])?;
    let u = c.u32t();
    let k3 = c.kc(3);
    let k7 = c.kc(7);
    let k8 = c.kc(8);
    let k4 = c.kc(4);
    let kff = c.kc(0xff);
    let kf = c.kc(0xf);
    let cm = c.ib(op::BITWISE_AND, s, k3);
    let mut out: Option<u32> = None;
    for byte in 0..4u32 {
        let ib = c.kc(byte);
        let nib = match mode {
            "" => {
                let sh = c.kc(4 * byte);
                let t = c.ib(op::SHIFT_RIGHT_LOGICAL, s, sh);
                c.ib(op::BITWISE_AND, t, kf)
            }
            "F4E" => c.ib(op::I_ADD, cm, ib),
            "B4E" => {
                let t = c.ib(op::I_SUB, cm, ib);
                c.ib(op::BITWISE_AND, t, k7)
            }
            "RC8" => cm,
            "ECL" => c.ext(u, glsl::U_MAX, &[cm, ib]),
            "ECR" => c.ext(u, glsl::U_MIN, &[cm, ib]),
            _ => {
                let one = c.kc(1);
                let odd = c.ib(op::BITWISE_AND, s, one);
                let two = c.ib(op::I_ADD, odd, odd);
                let half = c.kc(byte & 1);
                c.ib(op::I_ADD, two, half)
            }
        };
        let idx = c.ib(op::BITWISE_AND, nib, k7);
        let from_b = c.icmp_op(op::U_GREATER_THAN_EQUAL, idx, k4);
        let word = c.bpick(from_b, b, a);
        let lane = c.ib(op::BITWISE_AND, idx, k3);
        let sh = c.ib(op::SHIFT_LEFT_LOGICAL, lane, k3);
        let by = c.ib(op::SHIFT_RIGHT_LOGICAL, word, sh);
        let by = c.ib(op::BITWISE_AND, by, kff);
        let sign_bit = c.ib(op::BITWISE_AND, nib, k8);
        let neg = c.nonzero(sign_bit);
        let msb = c.ib(op::SHIFT_RIGHT_LOGICAL, by, k7);
        let rep = c.ib(op::I_MUL, msb, kff);
        let by = c.bpick(neg, rep, by);
        let place = c.kc(8 * byte);
        let placed = c.ib(op::SHIFT_LEFT_LOGICAL, by, place);
        out = Some(match out {
            None => placed,
            Some(p) => c.ib(op::BITWISE_OR, p, placed),
        });
    }
    let r = out.unwrap_or(kff);
    c.store_dst(&i.ops[0], r)
}

fn sgxt(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["U32", "W"])?;
    if i.ops.len() != 3 {
        return bad("SGXT form");
    }
    let signed = !i.has("U32");
    let a = c.usrc(&i.ops[1])?;
    let len = c.usrc(&i.ops[2])?;
    let len = if i.has("W") {
        let k = c.kc(31);
        c.ib(op::BITWISE_AND, len, k)
    } else {
        let k = c.kc(32);
        let u = c.u32t();
        c.ext(u, glsl::U_MIN, &[len, k])
    };
    let zero = c.kc(0);
    let u = c.u32t();
    let opc = if signed {
        op::BIT_FIELD_S_EXTRACT
    } else {
        op::BIT_FIELD_U_EXTRACT
    };
    let r = c.m.emit(opc, u, &[a, zero, len]);
    c.store_dst(&i.ops[0], r)
}

fn bmsk(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["W"])?;
    if i.ops.len() != 3 {
        return bad("BMSK form");
    }
    let pos = c.usrc(&i.ops[1])?;
    let len = c.usrc(&i.ops[2])?;
    let zero = c.kc(0);
    let one = c.kc(1);
    let k32 = c.kc(32);
    let all = c.kc(u32::MAX);
    let (pos, len) = if i.has("W") {
        let k = c.kc(31);
        (c.ib(op::BITWISE_AND, pos, k), c.ib(op::BITWISE_AND, len, k))
    } else {
        (pos, len)
    };
    let small = c.icmp_op(op::U_LESS_THAN, len, k32);
    let sl = c.ib(op::SHIFT_LEFT_LOGICAL, one, len);
    let m = c.ib(op::I_SUB, sl, one);
    let m = c.bpick(small, m, all);
    let pin = c.icmp_op(op::U_LESS_THAN, pos, k32);
    let r = c.ib(op::SHIFT_LEFT_LOGICAL, m, pos);
    let r = c.bpick(pin, r, zero);
    c.store_dst(&i.ops[0], r)
}

fn flo(c: &mut Ctx, i: &Inst) -> Res<()> {
    Ctx::mods_ok(i, &["U32", "SH"])?;
    let n = i.ops.len();
    if !(2..=3).contains(&n) {
        return bad("FLO form");
    }
    let signed = !i.has("U32");
    let v = c.isrc_neg(&i.ops[n - 1])?;
    let u = c.u32t();
    let idx = c.ext(
        u,
        if signed {
            glsl::FIND_S_MSB
        } else {
            glsl::FIND_U_MSB
        },
        &[v],
    );
    let r = if i.has("SH") {
        let k = c.kc(31);
        let none = c.kc(u32::MAX);
        let found = c.icmp_op(op::I_NOT_EQUAL, idx, none);
        let sh = c.ib(op::I_SUB, k, idx);
        c.bpick(found, sh, none)
    } else {
        idx
    };
    if n == 3 {
        let none = c.kc(u32::MAX);
        let found = c.icmp_op(op::I_NOT_EQUAL, idx, none);
        c.store_pdst(&i.ops[1], found)?;
    }
    c.store_dst(&i.ops[0], r)
}

fn popc(c: &mut Ctx, i: &Inst) -> Res<()> {
    let v = c.isrc_neg(&i.ops[1])?;
    let u = c.u32t();
    let r = c.m.emit(op::BIT_COUNT, u, &[v]);
    c.store_dst(&i.ops[0], r)
}

fn brev(c: &mut Ctx, i: &Inst) -> Res<()> {
    let v = c.isrc_neg(&i.ops[1])?;
    let u = c.u32t();
    let r = c.m.emit(op::BIT_REVERSE, u, &[v]);
    c.store_dst(&i.ops[0], r)
}

/// The predicate file as a word: bit `n` is `Pn` (bit 7 is always set).
fn pred_bits(c: &mut Ctx, uni: bool) -> u32 {
    let mut acc = c.kc(0x80);
    for n in 0..7u8 {
        let p = if uni {
            c.load_upred(n, false)
        } else {
            c.load_pred(n, false)
        };
        let bit = c.b2u(p);
        let k = c.kc(u32::from(n));
        let sh = c.ib(op::SHIFT_LEFT_LOGICAL, bit, k);
        acc = c.ib(op::BITWISE_OR, acc, sh);
    }
    acc
}

fn p2r(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.ops.len() != 4 {
        return bad("P2R form");
    }
    let bits = pred_bits(c, false);
    let a = c.usrc(&i.ops[2])?;
    let mask = c.usrc(&i.ops[3])?;
    let nm = c.inot(mask);
    let keep = c.ib(op::BITWISE_AND, a, nm);
    let set = c.ib(op::BITWISE_AND, bits, mask);
    let r = c.ib(op::BITWISE_OR, keep, set);
    c.store_dst(&i.ops[0], r)
}

fn up2ur(c: &mut Ctx, i: &Inst) -> Res<()> {
    if i.ops.len() != 4 {
        return bad("UP2UR form");
    }
    let bits = pred_bits(c, true);
    let a = c.usrc(&i.ops[2])?;
    let mask = c.usrc(&i.ops[3])?;
    let nm = c.inot(mask);
    let keep = c.ib(op::BITWISE_AND, a, nm);
    let set = c.ib(op::BITWISE_AND, bits, mask);
    let r = c.ib(op::BITWISE_OR, keep, set);
    c.store_dst(&i.ops[0], r)
}

fn r2p(c: &mut Ctx, i: &Inst) -> Res<()> {
    let (src, mask) = match i.ops.len() {
        3 => (&i.ops[1], Some(&i.ops[2])),
        2 => (&i.ops[1], None),
        _ => return bad("R2P form"),
    };
    let (mut v, m) = c.raw(src)?;
    if m.neg || m.abs || m.not {
        return bad("R2P source mods");
    }
    match m.swz {
        Swz::None => {}
        Swz::B(k) => {
            let sh = c.kc(8 * u32::from(k));
            v = c.ib(op::SHIFT_RIGHT_LOGICAL, v, sh);
        }
        _ => return bad("R2P byte select"),
    }
    let mask = match mask {
        Some(o) => c.usrc(o)?,
        None => c.kc(0xff),
    };
    for n in 0..7u8 {
        let k = c.kc(1 << n);
        let sel = c.ib(op::BITWISE_AND, mask, k);
        let en = c.nonzero(sel);
        let bit = c.ib(op::BITWISE_AND, v, k);
        let val = c.nonzero(bit);
        let old = c.load_pred(n, false);
        let t = c.boolt();
        let sv = c.select(t, en, val, old);
        c.store_pred(n, sv);
    }
    Ok(())
}

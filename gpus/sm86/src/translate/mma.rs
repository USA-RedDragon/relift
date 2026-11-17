//! Tensor-core matrix multiply-accumulate: HMMA.16816.F16 and
//! HMMA.1688.F16.
//!
//! The fragments are the PTX `mma.m16n8k16` and `mma.m16n8k8` layouts:
//! lane `t` (group `g = t / 4`, thread `q = t % 4`) holds A rows `g` and
//! `g + 8` at columns `2q`, `2q + 1` (and `2q + 8`, `2q + 9`), B column `g`
//! at rows `2q`, `2q + 1` (and `2q + 8`, `2q + 9`), and C and D rows `g` and
//! `g + 8` at columns `2q`, `2q + 1`. Each lane gathers the A rows and B
//! columns it needs from the other lanes of the warp.
//!
//! The arithmetic is the one measured on the hardware. Each group of eight
//! products is one fused step: the products are exact, every product and
//! the accumulator are aligned to a grid 24 bits below the largest exponent
//! among them (the sum of the two input exponents for a product, before
//! normalizing; zero terms do not count) and cut toward zero to that grid,
//! the sum is exact, and it is rounded to half precision to nearest even,
//! a zero result being `+0`. The K = 16 shape is two such steps, the first
//! result rounded to half precision before the second. A NaN input, an
//! infinity times zero or infinities of both signs give `0x7fff`; otherwise
//! an infinite input gives that infinity.

use super::{Ctx, Res, bad};
use crate::spirv::{glsl, op};
use crate::{Inst, Operand};

const FUNCTION_PARAMETER: u16 = 55;
const FUNCTION_CALL: u16 = 57;
const RETURN_VALUE: u16 = 254;
const DOT8_KEY: u32 = 0x4_0000;
const NO_EXP: u32 = -64i32 as u32;

pub(crate) fn inst(c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    Some(match i.mnemonic {
        "HMMA" => hmma(c, i),
        _ => return None,
    })
}

fn reg_of(o: &Operand) -> Res<u8> {
    match o {
        Operand::Reg(n, m) if !m.neg && !m.abs && !m.not => Ok(*n),
        o => bad(format!("HMMA operand {o}")),
    }
}

fn hmma(c: &mut Ctx, i: &Inst) -> Res<()> {
    let mut k16 = false;
    for m in &i.mods {
        match *m {
            "16816" => k16 = true,
            "1688" | "F16" => {}
            m => return bad(format!("HMMA.{m}")),
        }
    }
    if i.ops.len() != 4 {
        return bad("HMMA operands");
    }
    let d = reg_of(&i.ops[0])?;
    let ra = reg_of(&i.ops[1])?;
    let rb = reg_of(&i.ops[2])?;
    let rc = reg_of(&i.ops[3])?;
    let (na, nb) = if k16 { (4, 2) } else { (2, 1) };
    let load = |c: &mut Ctx, r: u8, k: u8| {
        if r == 255 {
            c.m.c_u32(0)
        } else {
            c.load_reg(r + k)
        }
    };
    let a: Vec<u32> = (0..na).map(|k| load(c, ra, k)).collect();
    let b: Vec<u32> = (0..nb).map(|k| load(c, rb, k)).collect();
    let acc: Vec<u32> = (0..2).map(|k| load(c, rc, k)).collect();
    let u = c.u32t();
    let lane = c.lane_id();
    let k2 = c.m.c_u32(2);
    let k3 = c.m.c_u32(3);
    let g = c.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[lane, k2]);
    let q = c.m.emit(op::BITWISE_AND, u, &[lane, k3]);
    let row_lanes: Vec<u32> = (0..4)
        .map(|j| {
            let base = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[g, k2]);
            let kj = c.m.c_u32(j);
            c.m.emit(op::BITWISE_OR, u, &[base, kj])
        })
        .collect();
    let col_lanes: Vec<Vec<u32>> = (0..2)
        .map(|h| {
            let k1 = c.m.c_u32(1);
            let n2 = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[q, k1]);
            let kh = c.m.c_u32(h);
            let n = c.m.emit(op::BITWISE_OR, u, &[n2, kh]);
            let base = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[n, k2]);
            (0..4)
                .map(|j| {
                    let kj = c.m.c_u32(j);
                    c.m.emit(op::BITWISE_OR, u, &[base, kj])
                })
                .collect()
        })
        .collect();
    let arows: Vec<Vec<u32>> = a.iter().map(|&v| c.shuffle_set(v, &row_lanes)).collect();
    let bcols: Vec<Vec<Vec<u32>>> = b
        .iter()
        .map(|&v| col_lanes.iter().map(|ls| c.shuffle_set(v, ls)).collect())
        .collect();
    let dot = c.dot8_fn();
    let k16c = c.m.c_u32(16);
    let mut out = Vec::new();
    for (half_row, &accw) in acc.iter().enumerate() {
        let mut halves = Vec::new();
        for (h, bh) in bcols[0].iter().enumerate() {
            let off = c.m.c_u32(16 * h as u32);
            let mut r = c.m.emit(op::BIT_FIELD_U_EXTRACT, u, &[accw, off, k16c]);
            for step in 0..nb {
                let aw = &arows[half_row + 2 * usize::from(step)];
                let bw = if step == 0 { bh } else { &bcols[1][h] };
                let mut args = vec![dot, r];
                args.extend_from_slice(aw);
                args.extend_from_slice(bw);
                r = c.m.emit(FUNCTION_CALL, u, &args);
            }
            halves.push(r);
        }
        let hi = c.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[halves[1], k16c]);
        out.push(c.m.emit(op::BITWISE_OR, u, &[halves[0], hi]));
    }
    if d != 255 {
        for (k, &v) in out.iter().enumerate() {
            c.store_reg(d + k as u8, v);
        }
    }
    Ok(())
}

impl Ctx {
    /// The helper `dot8(acc, a0..a3, b0..b3)`: one fused step of eight
    /// half-precision products (the halves of the packed words, low first)
    /// added to the half-precision `acc`, returned as half-precision bits.
    fn dot8_fn(&mut self) -> u32 {
        if let Some(&f) = self.builtins.get(&DOT8_KEY) {
            return f;
        }
        let saved = std::mem::take(&mut self.m.body);
        let saved_vars = self.m.suspend_vars();
        let u = self.u32t();
        let fnt = self.m.t_fn_args(u, &[u; 9]);
        let id = self.m.id();
        self.m.emit_void(op::FUNCTION, &[u, id, 0, fnt]);
        let ps: Vec<u32> = (0..9)
            .map(|_| self.m.emit(FUNCTION_PARAMETER, u, &[]))
            .collect();
        let entry = self.m.id();
        self.m.emit_void(op::LABEL, &[entry]);
        let r = self.dot8_body(ps[0], &ps[1..5], &ps[5..9]);
        self.m.emit_void(RETURN_VALUE, &[r]);
        self.m.emit_void(op::FUNCTION_END, &[]);
        let body = std::mem::replace(&mut self.m.body, saved);
        self.m.restore_vars(saved_vars);
        self.funcs.extend(body);
        self.builtins.insert(DOT8_KEY, id);
        id
    }

    fn mu(&mut self, opc: u16, a: u32, b: u32) -> u32 {
        let u = self.u32t();
        self.m.emit(opc, u, &[a, b])
    }

    fn mk(&mut self, opc: u16, a: u32, k: u32) -> u32 {
        let kk = self.m.c_u32(k);
        self.mu(opc, a, kk)
    }

    fn mb(&mut self, opc: u16, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(opc, t, &[a, b])
    }

    fn mbk(&mut self, opc: u16, a: u32, k: u32) -> u32 {
        let kk = self.m.c_u32(k);
        self.mb(opc, a, kk)
    }

    fn msel(&mut self, p: u32, a: u32, b: u32) -> u32 {
        let u = self.u32t();
        self.select(u, p, a, b)
    }

    fn mlog(&mut self, opc: u16, a: u32, b: u32) -> u32 {
        let t = self.boolt();
        self.m.emit(opc, t, &[a, b])
    }

    /// Sign bit, biased exponent (1 for denormals), significand with the
    /// implicit bit, and the NaN, infinity and zero flags of half `h`.
    fn half_parts(&mut self, h: u32) -> [u32; 6] {
        let sign = self.mk(op::SHIFT_RIGHT_LOGICAL, h, 15);
        let e = self.mk(op::SHIFT_RIGHT_LOGICAL, h, 10);
        let e = self.mk(op::BITWISE_AND, e, 31);
        let m = self.mk(op::BITWISE_AND, h, 1023);
        let sub = self.mbk(op::I_EQUAL, e, 0);
        let one = self.m.c_u32(1);
        let ex = self.msel(sub, one, e);
        let im = self.mk(op::BITWISE_OR, m, 1024);
        let sig = self.msel(sub, m, im);
        let special = self.mbk(op::I_EQUAL, e, 31);
        let mz = self.mbk(op::I_EQUAL, m, 0);
        let mnz = self.mbk(op::I_NOT_EQUAL, m, 0);
        let nan = self.mlog(op::LOGICAL_AND, special, mnz);
        let inf = self.mlog(op::LOGICAL_AND, special, mz);
        let mag = self.mk(op::BITWISE_AND, h, 0x7fff);
        let zero = self.mbk(op::I_EQUAL, mag, 0);
        [sign, ex, sig, nan, inf, zero]
    }

    fn dot8_body(&mut self, acc: u32, a: &[u32], b: &[u32]) -> u32 {
        let u = self.u32t();
        let k16 = self.m.c_u32(16);
        let z = self.m.c_u32(0);
        let fls = self.m.c_bool(false);
        let half = |c: &mut Ctx, w: &[u32], k: usize| {
            let off = c.m.c_u32(16 * (k as u32 & 1));
            c.m.emit(op::BIT_FIELD_U_EXTRACT, u, &[w[k / 2], off, k16])
        };
        let mut terms = Vec::new();
        let mut nan = fls;
        let mut pos = fls;
        let mut neg = fls;
        let none = self.m.c_u32(NO_EXP);
        let mut big = none;
        for k in 0..8 {
            let ha = half(self, a, k);
            let hb = half(self, b, k);
            let [sa, ea, ma, na, ia, za] = self.half_parts(ha);
            let [sb, eb, mb, nb, ib, zb] = self.half_parts(hb);
            let s = self.mu(op::BITWISE_XOR, sa, sb);
            let e = self.mu(op::I_ADD, ea, eb);
            let e = self.mk(op::I_SUB, e, 15);
            let m = self.mu(op::I_MUL, ma, mb);
            let n1 = self.mlog(op::LOGICAL_OR, na, nb);
            let n2 = self.mlog(op::LOGICAL_AND, ia, zb);
            let n3 = self.mlog(op::LOGICAL_AND, za, ib);
            let n12 = self.mlog(op::LOGICAL_OR, n1, n2);
            let pn = self.mlog(op::LOGICAL_OR, n12, n3);
            nan = self.mlog(op::LOGICAL_OR, nan, pn);
            let any_inf = self.mlog(op::LOGICAL_OR, ia, ib);
            let t = self.boolt();
            let not_nan = self.m.emit(op::LOGICAL_NOT, t, &[pn]);
            let pi = self.mlog(op::LOGICAL_AND, any_inf, not_nan);
            let sneg = self.mbk(op::I_NOT_EQUAL, s, 0);
            let spos = self.m.emit(op::LOGICAL_NOT, t, &[sneg]);
            let p_pos = self.mlog(op::LOGICAL_AND, pi, spos);
            let p_neg = self.mlog(op::LOGICAL_AND, pi, sneg);
            pos = self.mlog(op::LOGICAL_OR, pos, p_pos);
            neg = self.mlog(op::LOGICAL_OR, neg, p_neg);
            let nz = self.mbk(op::I_NOT_EQUAL, m, 0);
            let ek = self.msel(nz, e, none);
            big = self.ext(u, glsl::S_MAX, &[big, ek]);
            terms.push((sneg, e, m, 4));
        }
        let [sc, ec, mc, ncn, ic, _] = self.half_parts(acc);
        nan = self.mlog(op::LOGICAL_OR, nan, ncn);
        let scn = self.mbk(op::I_NOT_EQUAL, sc, 0);
        let t = self.boolt();
        let scp = self.m.emit(op::LOGICAL_NOT, t, &[scn]);
        let c_pos = self.mlog(op::LOGICAL_AND, ic, scp);
        let c_neg = self.mlog(op::LOGICAL_AND, ic, scn);
        pos = self.mlog(op::LOGICAL_OR, pos, c_pos);
        neg = self.mlog(op::LOGICAL_OR, neg, c_neg);
        let both = self.mlog(op::LOGICAL_AND, pos, neg);
        nan = self.mlog(op::LOGICAL_OR, nan, both);
        let cnz = self.mbk(op::I_NOT_EQUAL, mc, 0);
        let eck = self.msel(cnz, ec, none);
        big = self.ext(u, glsl::S_MAX, &[big, eck]);
        terms.push((scn, ec, mc, 14));
        let mut sum = z;
        for (sneg, e, m, lead) in terms {
            let d = self.mu(op::I_SUB, big, e);
            let left = self.mk(op::I_SUB, d, lead);
            let left = self.m.emit(op::S_NEGATE, u, &[left]);
            let lpos = self.mbk(op::S_GREATER_THAN, left, 0);
            let k31 = self.m.c_u32(31);
            let lsh = self.ext(u, glsl::U_MIN, &[left, k31]);
            let up = self.mu(op::SHIFT_LEFT_LOGICAL, m, lsh);
            let right = self.m.emit(op::S_NEGATE, u, &[left]);
            let far = self.mbk(op::U_GREATER_THAN_EQUAL, right, 32);
            let rsh = self.ext(u, glsl::U_MIN, &[right, k31]);
            let down = self.mu(op::SHIFT_RIGHT_LOGICAL, m, rsh);
            let down = self.msel(far, z, down);
            let v = self.msel(lpos, up, down);
            let nv = self.m.emit(op::S_NEGATE, u, &[v]);
            let v = self.msel(sneg, nv, v);
            sum = self.mu(op::I_ADD, sum, v);
        }
        let sneg = self.mbk(op::S_LESS_THAN, sum, 0);
        let nsum = self.m.emit(op::S_NEGATE, u, &[sum]);
        let mag = self.msel(sneg, nsum, sum);
        let l = self.ext(u, glsl::FIND_U_MSB, &[mag]);
        let ev = self.mu(op::I_ADD, l, big);
        let ev = self.mk(op::I_SUB, ev, 24);
        let k1 = self.m.c_u32(1);
        let eq = self.ext(u, glsl::S_MAX, &[ev, k1]);
        let sh = self.mu(op::I_SUB, eq, big);
        let sh = self.mk(op::I_ADD, sh, 14);
        let shp = self.mbk(op::S_GREATER_THAN, sh, 0);
        let k31 = self.m.c_u32(31);
        let rs = self.ext(u, glsl::U_MIN, &[sh, k31]);
        let ls = self.m.emit(op::S_NEGATE, u, &[sh]);
        let ls = self.ext(u, glsl::U_MIN, &[ls, k31]);
        let qd = self.mu(op::SHIFT_RIGHT_LOGICAL, mag, rs);
        let one_sh = self.mu(op::SHIFT_LEFT_LOGICAL, k1, rs);
        let low = self.mk(op::I_SUB, one_sh, 1);
        let rem = self.mu(op::BITWISE_AND, mag, low);
        let halfv = self.mk(op::SHIFT_RIGHT_LOGICAL, one_sh, 1);
        let gt = self.mb(op::U_GREATER_THAN, rem, halfv);
        let eqh = self.mb(op::I_EQUAL, rem, halfv);
        let odd = self.mk(op::BITWISE_AND, qd, 1);
        let oddb = self.mbk(op::I_NOT_EQUAL, odd, 0);
        let tie = self.mlog(op::LOGICAL_AND, eqh, oddb);
        let rup = self.mlog(op::LOGICAL_OR, gt, tie);
        let inc = self.msel(rup, k1, z);
        let qr = self.mu(op::I_ADD, qd, inc);
        let ql = self.mu(op::SHIFT_LEFT_LOGICAL, mag, ls);
        let q = self.msel(shp, qr, ql);
        let eb = self.mk(op::I_SUB, eq, 1);
        let eb = self.mk(op::SHIFT_LEFT_LOGICAL, eb, 10);
        let bits = self.mu(op::I_ADD, eb, q);
        let k7c = self.m.c_u32(0x7c00);
        let bits = self.ext(u, glsl::U_MIN, &[bits, k7c]);
        let qz = self.mbk(op::I_EQUAL, q, 0);
        let bits = self.msel(qz, z, bits);
        let sbit = self.mk(op::BITWISE_OR, bits, 0x8000);
        let r = self.msel(sneg, sbit, bits);
        let r = self.msel(qz, z, r);
        let pinf = self.m.c_u32(0x7c00);
        let ninf = self.m.c_u32(0xfc00);
        let qnan = self.m.c_u32(0x7fff);
        let r = self.msel(pos, pinf, r);
        let r = self.msel(neg, ninf, r);
        self.msel(nan, qnan, r)
    }
}

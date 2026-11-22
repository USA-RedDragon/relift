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
use crate::translate::host::WarpPath;
use crate::{Inst, Operand};

const FUNCTION_PARAMETER: u16 = 55;
const FUNCTION_CALL: u16 = 57;
const RETURN_VALUE: u16 = 254;
const DOT8_KEY: u32 = 0x4_0000;
const DOT8_FLOAT_KEY: u32 = 0x4_0001;
const HMMA_KEY: u32 = 0x4_0010;
/// The step functions are called, not inlined (`DontInline`): a program
/// has hundreds of HMMA and eight steps each, and inlined they made
/// binaries of megabytes and drivers compile for minutes.
const DOT8_CONTROL: u32 = 2;
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
    let out = if c.hmma_callable() {
        let f = c.hmma_fn(usize::from(na), usize::from(nb));
        let u = c.u32t();
        let v2 = c.m.t_vec(u, 2);
        let mut args = vec![f];
        args.extend_from_slice(&a);
        args.extend_from_slice(&b);
        args.extend_from_slice(&acc);
        let r = c.m.emit(FUNCTION_CALL, v2, &args);
        (0..2)
            .map(|k| c.m.emit(op::COMPOSITE_EXTRACT, u, &[r, k]))
            .collect()
    } else {
        c.hmma_body(&a, &b, &acc)
    };
    if d != 255 {
        for (k, &v) in out.iter().enumerate() {
            c.store_reg(d + k as u8, v);
        }
    }
    Ok(())
}

impl Ctx {
    /// Whether an HMMA can be a call of [`Ctx::hmma_fn`]: on the paths whose
    /// lane exchange is a subgroup shuffle, which reads nothing of the
    /// caller's function.
    fn hmma_callable(&self) -> bool {
        matches!(
            self.iface.host.warp,
            WarpPath::Subgroup | WarpPath::Cluster | WarpPath::Invariant
        ) && !self.stage.is_mesh_pipeline()
    }

    /// The helper `hmma(a0.., b0.., c0, c1)` of one shape (`na` words of A,
    /// `nb` of B): the whole of an HMMA, its lane exchange and its fused
    /// steps, returning D's two words. It is called, not inlined
    /// (`DontInline`): a program of hundreds of HMMA inlined made the host
    /// driver compile for tens of seconds.
    fn hmma_fn(&mut self, na: usize, nb: usize) -> u32 {
        let key = HMMA_KEY + na as u32;
        if let Some(&f) = self.builtins.get(&key) {
            return f;
        }
        let saved = std::mem::take(&mut self.m.body);
        let saved_vars = self.m.suspend_vars();
        let u = self.u32t();
        let v2 = self.m.t_vec(u, 2);
        let params = vec![u; na + nb + 2];
        let fnt = self.m.t_fn_args(v2, &params);
        let id = self.m.id();
        self.m.emit_void(op::FUNCTION, &[v2, id, DOT8_CONTROL, fnt]);
        let ps: Vec<u32> = (0..na + nb + 2)
            .map(|_| self.m.emit(FUNCTION_PARAMETER, u, &[]))
            .collect();
        let entry = self.m.id();
        self.m.emit_void(op::LABEL, &[entry]);
        let out = self.hmma_body(&ps[..na], &ps[na..na + nb], &ps[na + nb..]);
        let r = self.m.emit(op::COMPOSITE_CONSTRUCT, v2, &out);
        self.m.emit_void(RETURN_VALUE, &[r]);
        self.m.emit_void(op::FUNCTION_END, &[]);
        let body = std::mem::replace(&mut self.m.body, saved);
        self.m.restore_vars(saved_vars);
        self.funcs.extend(body);
        self.builtins.insert(key, id);
        id
    }

    /// An HMMA's D words from its A, B and C words: each lane gathers the A
    /// rows and B columns it needs from the other lanes and runs the fused
    /// steps.
    fn hmma_body(&mut self, a: &[u32], b: &[u32], acc: &[u32]) -> Vec<u32> {
        let c = self;
        let u = c.u32t();
        let nb = b.len() as u8;
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
        let dot = if c.host.native_f16 {
            c.dot8_float_fn()
        } else {
            c.dot8_fn()
        };
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
        out
    }
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
        self.m.emit_void(op::FUNCTION, &[u, id, DOT8_CONTROL, fnt]);
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
        let r = self.dot8_round(sum, big);
        let pinf = self.m.c_u32(0x7c00);
        let ninf = self.m.c_u32(0xfc00);
        let qnan = self.m.c_u32(0x7fff);
        let r = self.msel(pos, pinf, r);
        let r = self.msel(neg, ninf, r);
        self.msel(nan, qnan, r)
    }

    /// The exact sum `sum` of a step's terms on the grid of `big` (see the
    /// module documentation) rounded to half precision to nearest even, a
    /// zero being `+0`.
    fn dot8_round(&mut self, sum: u32, big: u32) -> u32 {
        let u = self.u32t();
        let z = self.m.c_u32(0);
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
        self.msel(qz, z, r)
    }

    /// `dot8` as [`Ctx::dot8_fn`] gives it, by single-precision arithmetic
    /// where no input is an infinity or a NaN (see the module
    /// documentation); other steps call the decoded function.
    fn dot8_float_fn(&mut self) -> u32 {
        if let Some(&f) = self.builtins.get(&DOT8_FLOAT_KEY) {
            return f;
        }
        let exact = self.dot8_fn();
        self.use_native_f16();
        let saved = std::mem::take(&mut self.m.body);
        let saved_vars = self.m.suspend_vars();
        let u = self.u32t();
        let fnt = self.m.t_fn_args(u, &[u; 9]);
        let id = self.m.id();
        self.m.emit_void(op::FUNCTION, &[u, id, DOT8_CONTROL, fnt]);
        let ps: Vec<u32> = (0..9)
            .map(|_| self.m.emit(FUNCTION_PARAMETER, u, &[]))
            .collect();
        let entry = self.m.id();
        self.m.emit_void(op::LABEL, &[entry]);
        let mut special = self.m.c_bool(false);
        for (k, &w) in ps.iter().enumerate() {
            for mask in if k == 0 {
                &[0x7c00][..]
            } else {
                &[0x7c00, 0x7c00_0000][..]
            } {
                let e = self.mk(op::BITWISE_AND, w, *mask);
                let s = self.mbk(op::I_EQUAL, e, *mask);
                special = self.mlog(op::LOGICAL_OR, special, s);
            }
        }
        let slow = self.m.id();
        let fast = self.m.id();
        let merge = self.m.id();
        self.m.emit_void(op::SELECTION_MERGE, &[merge, 0]);
        self.m
            .emit_void(op::BRANCH_CONDITIONAL, &[special, slow, fast]);
        self.m.emit_void(op::LABEL, &[slow]);
        let mut args = vec![exact];
        args.extend_from_slice(&ps);
        let rs = self.m.emit(FUNCTION_CALL, u, &args);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[fast]);
        let rf = self.dot8_float_body(ps[0], &ps[1..5], &ps[5..9]);
        self.m.emit_void(op::BRANCH, &[merge]);
        self.m.emit_void(op::LABEL, &[merge]);
        let r = self.m.emit(op::PHI, u, &[rs, slow, rf, fast]);
        self.m.emit_void(RETURN_VALUE, &[r]);
        self.m.emit_void(op::FUNCTION_END, &[]);
        let body = std::mem::replace(&mut self.m.body, saved);
        self.m.restore_vars(saved_vars);
        self.funcs.extend(body);
        self.builtins.insert(DOT8_FLOAT_KEY, id);
        id
    }

    /// The exponent field of each half of packed word `w`, 1 for a zero or
    /// denormal one: low half first.
    fn half_exps(&mut self, w: u32) -> [u32; 2] {
        let one = self.m.c_u32(1);
        let u = self.u32t();
        [10, 26].map(|sh| {
            let e = self.mk(op::SHIFT_RIGHT_LOGICAL, w, sh);
            let e = self.mk(op::BITWISE_AND, e, 31);
            self.ext(u, glsl::U_MAX, &[e, one])
        })
    }

    /// One step of [`Ctx::dot8_fn`] for finite inputs, in single precision.
    fn dot8_float_body(&mut self, acc: u32, a: &[u32], b: &[u32]) -> u32 {
        let u = self.u32t();
        let i = self.i32t();
        let f = self.f32t();
        let v2f = self.m.t_vec(f, 2);
        let hf = self.m.t_f16();
        let v2h = self.m.t_vec(hf, 2);
        let widen = |c: &mut Ctx, w: u32| {
            let h = c.m.emit(op::BITCAST, v2h, &[w]);
            c.m.emit(op::F_CONVERT, v2f, &[h])
        };
        let zero = self.m.c_f32(0);
        let none = self.m.c_u32(NO_EXP);
        let mut big = none;
        let mut prods = Vec::new();
        for k in 0..4 {
            let fa = widen(self, a[k]);
            let fb = widen(self, b[k]);
            let p = self.m.emit(op::F_MUL, v2f, &[fa, fb]);
            self.m.deco(p, 42, &[]);
            let ea = self.half_exps(a[k]);
            let eb = self.half_exps(b[k]);
            for l in 0..2 {
                let pl = self.m.emit(op::COMPOSITE_EXTRACT, f, &[p, l]);
                let e = self.mu(op::I_ADD, ea[l as usize], eb[l as usize]);
                let e = self.mk(op::I_SUB, e, 15);
                let t = self.boolt();
                let nz = self.m.emit(op::F_UNORD_NOT_EQUAL, t, &[pl, zero]);
                let ek = self.msel(nz, e, none);
                big = self.ext(u, glsl::S_MAX, &[big, ek]);
                prods.push(pl);
            }
        }
        let hc = self.m.emit(op::BITCAST, v2h, &[acc]);
        let hc = self.m.emit(op::COMPOSITE_EXTRACT, hf, &[hc, 0]);
        let fc = self.m.emit(op::F_CONVERT, f, &[hc]);
        let [ec, _] = self.half_exps(acc);
        let mag = self.mk(op::BITWISE_AND, acc, 0x7fff);
        let cnz = self.mbk(op::I_NOT_EQUAL, mag, 0);
        let eck = self.msel(cnz, ec, none);
        big = self.ext(u, glsl::S_MAX, &[big, eck]);
        let k = self.m.c_u32(39 + 127);
        let sexp = self.mu(op::I_SUB, k, big);
        let sbits = self.mk(op::SHIFT_LEFT_LOGICAL, sexp, 23);
        let scale = self.m.emit(op::BITCAST, f, &[sbits]);
        prods.push(fc);
        let mut sum = self.m.c_u32(0);
        for t in prods {
            let v = self.m.emit(op::F_MUL, f, &[t, scale]);
            self.m.deco(v, 42, &[]);
            let n = self.m.emit(op::CONVERT_F_TO_S, i, &[v]);
            let n = self.m.emit(op::BITCAST, u, &[n]);
            sum = self.mu(op::I_ADD, sum, n);
        }
        self.dot8_round(sum, big)
    }
}

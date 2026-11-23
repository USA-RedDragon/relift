//! 64-bit integers as pairs of 32-bit words, for a host without
//! `shaderInt64`.
//!
//! The exact double precision model (`translate::fp64`) is 64-bit integer
//! code. [`lower`] rewrites a finished module so it needs no `Int64`: the
//! unsigned 64-bit integer type becomes a two-component vector of 32-bit
//! words (low word first), its constants become constant vectors, and each
//! operation on it becomes the 32-bit operations that give the same bits:
//! adds and subtracts with carry and borrow, the low 64 bits of a product
//! from one extended multiply and two plain ones, shifts by a 32-bit count
//! below 64 from both words, and comparisons from the high words then the
//! low ones. Bitwise operations, selects, loads, stores, calls, parameters
//! and phis keep their form on the vector type. An operation on the type
//! the rewrite does not know is an error, never a module that computes
//! something else.

use std::collections::{HashMap, HashSet};

const OP_CAPABILITY: u16 = 17;
const OP_TYPE_BOOL: u16 = 20;
const OP_TYPE_INT: u16 = 21;
const OP_TYPE_VECTOR: u16 = 23;
const OP_TYPE_STRUCT: u16 = 30;
const OP_CONSTANT: u16 = 43;
const OP_CONSTANT_COMPOSITE: u16 = 44;
const OP_FUNCTION: u16 = 54;
const OP_COPY_OBJECT: u16 = 83;
const OP_COMPOSITE_CONSTRUCT: u16 = 80;
const OP_COMPOSITE_EXTRACT: u16 = 81;
const OP_U_CONVERT: u16 = 113;
const OP_BITCAST: u16 = 124;
const OP_S_NEGATE: u16 = 126;
const OP_I_ADD: u16 = 128;
const OP_I_SUB: u16 = 130;
const OP_I_MUL: u16 = 132;
const OP_I_ADD_CARRY: u16 = 149;
const OP_I_SUB_BORROW: u16 = 150;
const OP_U_MUL_EXTENDED: u16 = 151;
const OP_LOGICAL_OR: u16 = 166;
const OP_LOGICAL_AND: u16 = 167;
const OP_SELECT: u16 = 169;
const OP_I_EQUAL: u16 = 170;
const OP_I_NOT_EQUAL: u16 = 171;
const OP_U_GREATER_THAN: u16 = 172;
const OP_S_GREATER_THAN: u16 = 173;
const OP_U_LESS_THAN: u16 = 176;
const OP_S_LESS_THAN: u16 = 177;
const OP_SHIFT_RIGHT_LOGICAL: u16 = 194;
const OP_SHIFT_LEFT_LOGICAL: u16 = 196;
const OP_BITWISE_AND: u16 = 199;
const CAP_INT64: u32 = 11;

/// Operations whose form carries over to the vector type unchanged.
const KEPT: &[u16] = &[
    1,   // OpUndef
    54,  // OpFunction
    55,  // OpFunctionParameter
    57,  // OpFunctionCall
    59,  // OpVariable
    61,  // OpLoad
    83,  // OpCopyObject
    169, // OpSelect (a scalar condition picks whole vectors, SPIR-V 1.4)
    197, // OpBitwiseOr
    198, // OpBitwiseXor
    199, // OpBitwiseAnd
    200, // OpNot
    245, // OpPhi
];

/// Operations whose operands are all ids after the result type and id, so
/// a 64-bit operand among them can be found.
fn all_id_operands(op: u16) -> bool {
    (109..=201).contains(&op)
}

fn inst(out: &mut Vec<u32>, op: u16, args: &[u32]) {
    out.push(((args.len() as u32 + 1) << 16) | u32::from(op));
    out.extend_from_slice(args);
}

/// The module being rewritten: new ids and the 32-bit constants made.
struct Ids {
    bound: u32,
    u32_consts: HashMap<u32, u32>,
    new_consts: Vec<u32>,
    u: u32,
}

impl Ids {
    fn id(&mut self) -> u32 {
        let i = self.bound;
        self.bound += 1;
        i
    }

    fn c(&mut self, v: u32) -> u32 {
        if let Some(&i) = self.u32_consts.get(&v) {
            return i;
        }
        let i = self.id();
        inst(&mut self.new_consts, OP_CONSTANT, &[self.u, i, v]);
        self.u32_consts.insert(v, i);
        i
    }
}

/// One instruction: opcode and operands.
type Inst<'a> = (u16, &'a [u32]);

fn split(words: &[u32]) -> Result<Vec<Inst<'_>>, String> {
    let mut out = Vec::new();
    let mut k = 5;
    while k < words.len() {
        let n = (words[k] >> 16) as usize;
        if n == 0 || k + n > words.len() {
            return Err(format!("a malformed instruction at word {k}"));
        }
        out.push(((words[k] & 0xffff) as u16, &words[k + 1..k + n]));
        k += n;
    }
    Ok(out)
}

/// `words` with every unsigned 64-bit integer as two 32-bit words (see the
/// module documentation); a module without the 64-bit type comes back as
/// it is.
///
/// # Errors
///
/// The module has a signed 64-bit type, an operation on the 64-bit type the
/// rewrite does not know, or no 32-bit unsigned type declared before the
/// 64-bit one.
pub fn lower(words: &[u32]) -> Result<Vec<u32>, String> {
    if words.len() < 5 {
        return Err("no SPIR-V header".into());
    }
    let insts = split(words)?;
    let mut type_ids = HashSet::new();
    let (mut t64, mut u, mut b, mut v2) = (None, None, None, None);
    let mut u_at = usize::MAX;
    for (k, &(op, a)) in insts.iter().enumerate() {
        if (19..=39).contains(&op) {
            type_ids.insert(a[0]);
        }
        match (op, a) {
            (OP_TYPE_INT, [id, 64, 0]) => t64 = t64.or(Some((*id, k))),
            (OP_TYPE_INT, [_, 64, 1]) => return Err("a signed 64-bit integer type".into()),
            (OP_TYPE_INT, [id, 32, 0]) if u.is_none() => {
                u = Some(*id);
                u_at = k;
            }
            (OP_TYPE_BOOL, [id]) => b = b.or(Some(*id)),
            _ => {}
        }
    }
    let Some((t, t_at)) = t64 else {
        return Ok(words.to_vec());
    };
    let u = u.ok_or("no 32-bit unsigned type")?;
    if u_at > t_at {
        return Err("the 32-bit type is declared after the 64-bit one".into());
    }
    for &(op, a) in &insts {
        if let (OP_TYPE_VECTOR, [id, c, 2]) = (op, a)
            && *c == u
        {
            v2 = Some(*id);
        }
    }
    let mut ids = Ids {
        bound: words[3],
        u32_consts: HashMap::new(),
        new_consts: Vec::new(),
        u,
    };
    let mut extra_types = Vec::new();
    let b = match b {
        Some(b) => b,
        None => {
            let i = ids.id();
            inst(&mut extra_types, OP_TYPE_BOOL, &[i]);
            i
        }
    };
    let pair = ids.id();
    inst(&mut extra_types, OP_TYPE_STRUCT, &[pair, u, u]);
    let mut rtype: HashMap<u32, u32> = HashMap::new();
    for &(op, a) in &insts {
        if a.len() >= 2 && type_ids.contains(&a[0]) && !(19..=39).contains(&op) {
            rtype.insert(a[1], a[0]);
        }
    }
    let is64 = |id: u32| rtype.get(&id) == Some(&t);

    let mut out: Vec<u32> = words[..5].to_vec();
    let mut types_done = false;
    for (k, &(op, a)) in insts.iter().enumerate() {
        if op == OP_CAPABILITY && a == [CAP_INT64] {
            continue;
        }
        if k == t_at {
            if v2.is_none() {
                inst(&mut out, OP_TYPE_VECTOR, &[t, u, 2]);
            }
            out.extend(std::mem::take(&mut extra_types));
            continue;
        }
        if let (OP_CONSTANT, [ty, id, v]) = (op, a)
            && *ty == u
        {
            // Only a constant declared before can stand in a composite.
            ids.u32_consts.entry(*v).or_insert(*id);
        }
        if op == OP_CONSTANT && a.len() == 4 && a[0] == t {
            let (lo, hi) = (ids.c(a[2]), ids.c(a[3]));
            out.append(&mut ids.new_consts);
            inst(&mut out, OP_CONSTANT_COMPOSITE, &[t, a[1], lo, hi]);
            continue;
        }
        if op == OP_FUNCTION && !types_done {
            types_done = true;
            for k in [0, 1, 31, 32] {
                ids.c(k);
            }
            out.append(&mut ids.new_consts);
        }
        if !types_done {
            inst(&mut out, op, a);
            continue;
        }
        let ext = |ids: &mut Ids, out: &mut Vec<u32>, v: u32| {
            let (lo, hi) = (ids.id(), ids.id());
            inst(out, OP_COMPOSITE_EXTRACT, &[u, lo, v, 0]);
            inst(out, OP_COMPOSITE_EXTRACT, &[u, hi, v, 1]);
            (lo, hi)
        };
        let ext1 = |ids: &mut Ids, out: &mut Vec<u32>, v: u32, i: u32| {
            let r = ids.id();
            inst(out, OP_COMPOSITE_EXTRACT, &[u, r, v, i]);
            r
        };
        let bin = |ids: &mut Ids, out: &mut Vec<u32>, op: u16, ty: u32, x: u32, y: u32| {
            let r = ids.id();
            inst(out, op, &[ty, r, x, y]);
            r
        };
        let with_carry = |ids: &mut Ids, out: &mut Vec<u32>, op: u16, x: u32, y: u32| {
            let s = ids.id();
            inst(out, op, &[pair, s, x, y]);
            (ext1(ids, out, s, 0), ext1(ids, out, s, 1))
        };
        if a.len() >= 2 && a[0] == t {
            let r = a[1];
            match op {
                _ if KEPT.contains(&op) => inst(&mut out, op, a),
                OP_I_ADD | OP_I_SUB => {
                    let (al, ah) = ext(&mut ids, &mut out, a[2]);
                    let (bl, bh) = ext(&mut ids, &mut out, a[3]);
                    let wide = if op == OP_I_ADD {
                        OP_I_ADD_CARRY
                    } else {
                        OP_I_SUB_BORROW
                    };
                    let (lo, c) = with_carry(&mut ids, &mut out, wide, al, bl);
                    let h = bin(&mut ids, &mut out, op, u, ah, bh);
                    let hi = bin(&mut ids, &mut out, op, u, h, c);
                    inst(&mut out, OP_COMPOSITE_CONSTRUCT, &[t, r, lo, hi]);
                }
                OP_S_NEGATE => {
                    let (al, ah) = ext(&mut ids, &mut out, a[2]);
                    let z = ids.c(0);
                    let (lo, c) = with_carry(&mut ids, &mut out, OP_I_SUB_BORROW, z, al);
                    let h = bin(&mut ids, &mut out, OP_I_SUB, u, z, ah);
                    let hi = bin(&mut ids, &mut out, OP_I_SUB, u, h, c);
                    inst(&mut out, OP_COMPOSITE_CONSTRUCT, &[t, r, lo, hi]);
                }
                OP_I_MUL => {
                    let (al, ah) = ext(&mut ids, &mut out, a[2]);
                    let (bl, bh) = ext(&mut ids, &mut out, a[3]);
                    let (lo, h) = with_carry(&mut ids, &mut out, OP_U_MUL_EXTENDED, al, bl);
                    let x = bin(&mut ids, &mut out, OP_I_MUL, u, al, bh);
                    let y = bin(&mut ids, &mut out, OP_I_MUL, u, ah, bl);
                    let h = bin(&mut ids, &mut out, OP_I_ADD, u, h, x);
                    let hi = bin(&mut ids, &mut out, OP_I_ADD, u, h, y);
                    inst(&mut out, OP_COMPOSITE_CONSTRUCT, &[t, r, lo, hi]);
                }
                OP_SHIFT_LEFT_LOGICAL | OP_SHIFT_RIGHT_LOGICAL => {
                    let (lo, hi) = ext(&mut ids, &mut out, a[2]);
                    let n = if is64(a[3]) {
                        ext1(&mut ids, &mut out, a[3], 0)
                    } else {
                        a[3]
                    };
                    let (k31, k32, k1, k0) = (ids.c(31), ids.c(32), ids.c(1), ids.c(0));
                    let m = bin(&mut ids, &mut out, OP_BITWISE_AND, u, n, k31);
                    let rest = bin(&mut ids, &mut out, OP_I_SUB, u, k31, m);
                    let small = bin(&mut ids, &mut out, OP_U_LESS_THAN, b, n, k32);
                    let (near, far) = if op == OP_SHIFT_LEFT_LOGICAL {
                        (lo, hi)
                    } else {
                        (hi, lo)
                    };
                    let opp = if op == OP_SHIFT_LEFT_LOGICAL {
                        OP_SHIFT_RIGHT_LOGICAL
                    } else {
                        OP_SHIFT_LEFT_LOGICAL
                    };
                    // The word the bits move out of, shifted by m; the word they move into gets its
                    // own bits shifted by m and the other's top (or bottom) m bits.
                    let near_s = bin(&mut ids, &mut out, op, u, near, m);
                    let far_s = bin(&mut ids, &mut out, op, u, far, m);
                    let spill = bin(&mut ids, &mut out, opp, u, near, k1);
                    let spill = bin(&mut ids, &mut out, opp, u, spill, rest);
                    let far_s = bin(&mut ids, &mut out, 197, u, far_s, spill);
                    let sel = |ids: &mut Ids, out: &mut Vec<u32>, x: u32, y: u32| {
                        let r = ids.id();
                        inst(out, OP_SELECT, &[u, r, small, x, y]);
                        r
                    };
                    let near_r = sel(&mut ids, &mut out, near_s, k0);
                    let far_r = sel(&mut ids, &mut out, far_s, near_s);
                    let (lo_r, hi_r) = if op == OP_SHIFT_LEFT_LOGICAL {
                        (near_r, far_r)
                    } else {
                        (far_r, near_r)
                    };
                    inst(&mut out, OP_COMPOSITE_CONSTRUCT, &[t, r, lo_r, hi_r]);
                }
                OP_U_CONVERT => {
                    if rtype.get(&a[2]) != Some(&u) {
                        return Err(
                            "a conversion to a 64-bit integer from a type other than 32-bit".into(),
                        );
                    }
                    let z = ids.c(0);
                    inst(&mut out, OP_COMPOSITE_CONSTRUCT, &[t, r, a[2], z]);
                }
                OP_BITCAST if is64(a[2]) => {
                    inst(&mut out, OP_COPY_OBJECT, &[t, r, a[2]]);
                }
                _ => return Err(format!("operation {op} on a 64-bit integer")),
            }
            continue;
        }
        let uses64 = all_id_operands(op) && a.len() > 2 && a[2..].iter().any(|&x| is64(x));
        if !uses64 {
            inst(&mut out, op, a);
            continue;
        }
        let r = a[1];
        match op {
            OP_U_CONVERT if a[0] == u => {
                inst(&mut out, OP_COMPOSITE_EXTRACT, &[u, r, a[2], 0]);
            }
            OP_I_EQUAL | OP_I_NOT_EQUAL => {
                let (al, ah) = ext(&mut ids, &mut out, a[2]);
                let (bl, bh) = ext(&mut ids, &mut out, a[3]);
                let l = bin(&mut ids, &mut out, op, b, al, bl);
                let h = bin(&mut ids, &mut out, op, b, ah, bh);
                let join = if op == OP_I_EQUAL {
                    OP_LOGICAL_AND
                } else {
                    OP_LOGICAL_OR
                };
                inst(&mut out, join, &[b, r, l, h]);
            }
            172..=179 => {
                // The high words decide unless they are equal; then the low words, unsigned.
                let (strict_hi, low) = match op {
                    172 => (OP_U_GREATER_THAN, 172),
                    173 => (OP_S_GREATER_THAN, 172),
                    174 => (OP_U_GREATER_THAN, 174),
                    175 => (OP_S_GREATER_THAN, 174),
                    176 => (OP_U_LESS_THAN, 176),
                    177 => (OP_S_LESS_THAN, 176),
                    178 => (OP_U_LESS_THAN, 178),
                    _ => (OP_S_LESS_THAN, 178),
                };
                let (al, ah) = ext(&mut ids, &mut out, a[2]);
                let (bl, bh) = ext(&mut ids, &mut out, a[3]);
                let hs = bin(&mut ids, &mut out, strict_hi, b, ah, bh);
                let he = bin(&mut ids, &mut out, OP_I_EQUAL, b, ah, bh);
                let lc = bin(&mut ids, &mut out, low, b, al, bl);
                let tie = bin(&mut ids, &mut out, OP_LOGICAL_AND, b, he, lc);
                inst(&mut out, OP_LOGICAL_OR, &[b, r, hs, tie]);
            }
            _ => return Err(format!("operation {op} reading a 64-bit integer")),
        }
    }
    out[3] = ids.bound;
    match v2 {
        Some(v) => rename(&out, t, v),
        None => Ok(out),
    }
}

/// `words` with the type `from` named `to` wherever a type is named (the
/// module already declares the vector the 64-bit type becomes, and two
/// declarations of one vector type are not allowed); names and decorations
/// of `from` go.
fn rename(words: &[u32], from: u32, to: u32) -> Result<Vec<u32>, String> {
    const OP_NAME: u16 = 5;
    const OP_DECORATE: u16 = 71;
    let mut out = words[..5].to_vec();
    for (op, a) in split(words)? {
        if matches!(op, OP_NAME | OP_DECORATE) && a.first() == Some(&from) {
            continue;
        }
        let mut a = a.to_vec();
        let swap = |x: &mut u32| {
            if *x == from {
                *x = to;
            }
        };
        match op {
            28 | 29 => swap(&mut a[1]),
            30 | 33 => a[1..].iter_mut().for_each(swap),
            32 => swap(&mut a[2]),
            19..=39 => {}
            _ => {
                if let Some(x) = a.first_mut() {
                    swap(x);
                }
            }
        }
        inst(&mut out, op, &a);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn module(body: &[u32]) -> Vec<u32> {
        let mut w = vec![0x0723_0203, 0x0001_0500, 0, 100, 0];
        inst(&mut w, OP_CAPABILITY, &[1]);
        inst(&mut w, OP_CAPABILITY, &[CAP_INT64]);
        inst(&mut w, OP_TYPE_INT, &[1, 32, 0]);
        inst(&mut w, OP_TYPE_INT, &[2, 64, 0]);
        inst(&mut w, OP_CONSTANT, &[2, 3, 5, 7]);
        w.extend_from_slice(body);
        w
    }

    #[test]
    fn the_type_becomes_a_pair_and_int64_goes() {
        let mut f = Vec::new();
        inst(&mut f, OP_FUNCTION, &[2, 10, 0, 11]);
        inst(&mut f, OP_I_ADD, &[2, 12, 3, 3]);
        let w = lower(&module(&f)).unwrap();
        let insts = split(&w).unwrap();
        assert!(
            !insts
                .iter()
                .any(|&(op, a)| op == OP_CAPABILITY && a == [CAP_INT64])
        );
        assert!(
            insts
                .iter()
                .any(|&(op, a)| op == OP_TYPE_VECTOR && a == [2, 1, 2])
        );
        assert!(
            insts
                .iter()
                .any(|&(op, a)| op == OP_CONSTANT_COMPOSITE && a[..2] == [2, 3])
        );
        assert!(insts.iter().any(|&(op, _)| op == OP_I_ADD_CARRY));
        assert!(!insts.iter().any(|&(op, a)| op == OP_I_ADD && a[0] == 2));
        assert!(w[3] > 100);
    }

    #[test]
    fn an_unknown_operation_is_an_error_not_a_module() {
        let mut f = Vec::new();
        inst(&mut f, OP_FUNCTION, &[2, 10, 0, 11]);
        inst(&mut f, 134, &[2, 12, 3, 3]); // OpUDiv
        assert!(lower(&module(&f)).unwrap_err().contains("134"));
    }

    #[test]
    fn a_module_without_64_bit_integers_is_left_alone() {
        let mut w = vec![0x0723_0203, 0x0001_0500, 0, 3, 0];
        inst(&mut w, OP_TYPE_INT, &[1, 32, 0]);
        assert_eq!(lower(&w).unwrap(), w);
    }
}

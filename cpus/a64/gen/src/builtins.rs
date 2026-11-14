//! The Sail library primitives, over staged values.

use crate::eval::{Gen, R, fail};
use crate::ops::{BOp, Cmp, IOp};
use crate::value::{K, Kind, V, mask};

impl Gen<'_> {
    fn cw(&self, v: &V) -> R<u32> {
        let i = self.cint(v)?;
        if !(0..=4096).contains(&i) {
            return fail(format!("width {i}"));
        }
        Ok(i as u32)
    }

    fn wid(&self, v: &V) -> R<u32> {
        v.width()
            .ok_or_else(|| crate::eval::err(format!("not bits: {v:?}")))
    }

    /// Runs the builtin `name`, or gives `None` when there is no such builtin.
    pub fn builtin(&mut self, name: &str, a: &[V], hint: Option<u32>) -> R<Option<V>> {
        let arg = |i: usize| -> R<&V> {
            a.get(i)
                .ok_or_else(|| crate::eval::err(format!("{name}: missing argument {i}")))
        };
        let v = match name {
            "add_atom" | "add_int" => self.iop(IOp::Add, arg(0)?, arg(1)?)?,
            "sub_atom" | "sub_int" => self.iop(IOp::Sub, arg(0)?, arg(1)?)?,
            "sub_nat" => {
                let d = self.iop(IOp::Sub, arg(0)?, arg(1)?)?;
                self.iop(IOp::Max, &d, &V::Int(0))?
            }
            "mult_atom" | "mult_int" => self.iop(IOp::Mul, arg(0)?, arg(1)?)?,
            "negate_atom" | "negate_int" | "negate" => self.iop(IOp::Sub, &V::Int(0), arg(0)?)?,
            "tdiv_int" => self.iop(IOp::TDiv, arg(0)?, arg(1)?)?,
            "_tmod_int" | "_tmod_int_positive" | "tmod_int" => {
                self.iop(IOp::TMod, arg(0)?, arg(1)?)?
            }
            "fdiv_int" | "ediv_nat" | "ediv_int" => self.iop(IOp::FDiv, arg(0)?, arg(1)?)?,
            "fmod_int" | "emod_nat" | "emod_int" => self.iop(IOp::FMod, arg(0)?, arg(1)?)?,
            "max_int" => self.iop(IOp::Max, arg(0)?, arg(1)?)?,
            "min_int" => self.iop(IOp::Min, arg(0)?, arg(1)?)?,
            "abs_int_atom" | "abs_int_plain" | "abs_int" => {
                let n = self.iop(IOp::Sub, &V::Int(0), arg(0)?)?;
                self.iop(IOp::Max, arg(0)?, &n)?
            }
            "eq_int" | "eq_bits" | "eq_bit" | "eq_bool" | "eq_any" | "eq_anything"
            | "eq_string" => self.cmp(Cmp::Eq, arg(0)?, arg(1)?)?,
            "neq_int" | "neq_bits" | "neq_bool" | "neq_any" | "neq_anything" => {
                self.cmp(Cmp::Ne, arg(0)?, arg(1)?)?
            }
            "lt_int" => self.cmp(Cmp::Lt, arg(0)?, arg(1)?)?,
            "lteq_int" => self.cmp(Cmp::Le, arg(0)?, arg(1)?)?,
            "gt_int" => self.cmp(Cmp::Gt, arg(0)?, arg(1)?)?,
            "gteq_int" => self.cmp(Cmp::Ge, arg(0)?, arg(1)?)?,
            "eq_unit" => V::Bool(true),
            "not_bool" => self.not(arg(0)?)?,
            "and_bool" | "and_bool_no_flow" => self.and(arg(0)?, arg(1)?)?,
            "or_bool" => self.or(arg(0)?, arg(1)?)?,
            "add_bits" => self.bits_op(BOp::Add, arg(0)?, arg(1)?)?,
            "sub_bits" => self.bits_op(BOp::Sub, arg(0)?, arg(1)?)?,
            "mult_bits" => self.bits_op(BOp::Mul, arg(0)?, arg(1)?)?,
            "and_vec" => self.bits_op(BOp::And, arg(0)?, arg(1)?)?,
            "or_vec" => self.bits_op(BOp::Or, arg(0)?, arg(1)?)?,
            "xor_vec" => self.bits_op(BOp::Xor, arg(0)?, arg(1)?)?,
            "not_vec" => self.bits_not(arg(0)?)?,
            "add_bits_int" | "sub_bits_int" | "sub_vec_int" => {
                let x = arg(0)?.clone();
                let w = self.wid(&x)?;
                let y = self.int_bits(arg(1)?, 0, w)?;
                let op = if name == "add_bits_int" {
                    BOp::Add
                } else {
                    BOp::Sub
                };
                self.bits_op(op, &x, &y)?
            }
            "sail_shiftleft" | "shiftl" | "shift_bits_left" => {
                self.shift(arg(0)?, arg(1)?, "shl")?
            }
            "sail_shiftright" | "shiftr" | "shift_bits_right" => {
                self.shift(arg(0)?, arg(1)?, "lshr")?
            }
            "sail_arith_shiftright" | "arith_shiftr" | "shift_bits_right_arith" => {
                self.shift(arg(0)?, arg(1)?, "ashr")?
            }
            "sail_zero_extend" | "zero_extend" | "ZeroExtend0" => {
                let n = self.cw(arg(1)?)?;
                self.extend(arg(0)?, n, false)?
            }
            "sail_sign_extend" | "sign_extend" | "SignExtend0" => {
                let n = self.cw(arg(1)?)?;
                self.extend(arg(0)?, n, true)?
            }
            "sail_zeros" | "zeros" => {
                let n = self.cw(arg(0)?)?;
                V::Bits(n, 0)
            }
            "sail_ones" | "ones" => {
                let n = self.cw(arg(0)?)?;
                V::Bits(n, mask(n))
            }
            "slice" => {
                let n = self.cw(arg(2)?)?;
                self.extract(arg(0)?, arg(1)?, n)?
            }
            "subrange_bits" | "vector_subrange" => {
                let (hi, lo) = (self.cint(arg(1)?)?, self.cint(arg(2)?)?);
                self.extract(arg(0)?, &V::Int(lo), (hi - lo + 1) as u32)?
            }
            "bitvector_access" | "vector_access" | "plain_vector_access" => {
                self.bit(arg(0)?, arg(1)?)?
            }
            "bitvector_update" | "vector_update" => {
                let i = self.cint(arg(1)?)? as u32;
                self.set_slice(arg(0)?, i, i, arg(2)?)?
            }
            "update_subrange_bits" | "vector_update_subrange" => {
                let (hi, lo) = (self.cint(arg(1)?)? as u32, self.cint(arg(2)?)? as u32);
                self.set_slice(arg(0)?, hi, lo, arg(3)?)?
            }
            "bitvector_concat" | "append_64" | "op@" | "append" => self.concat(arg(0)?, arg(1)?)?,
            "bitvector_length" | "vector_length" | "length" => {
                V::Int(i128::from(self.wid(arg(0)?)?))
            }
            "replicate_bits" => {
                let n = self.cw(arg(1)?)?;
                self.replicate(arg(0)?, n)?
            }
            "get_slice_int" => {
                let (n, lo) = (self.cw(arg(0)?)?, self.cw(arg(2)?)?);
                self.int_bits(arg(1)?, lo, n)?
            }
            "set_slice_bits" => {
                let m = self.cw(arg(1)?)?;
                let s = self.cw(arg(3)?)?;
                self.set_slice(arg(2)?, s + m - 1, s, arg(4)?)?
            }
            "unsigned" | "UInt0" | "_builtin_unsigned" | "sail_unsigned" => {
                self.int_of(arg(0)?, false)?
            }
            "signed" | "SInt0" | "sail_signed" => self.int_of(arg(0)?, true)?,
            "count_leading_zeros" => {
                let x = self.defd(arg(0)?);
                let w = self.wid(&x)?;
                match &x {
                    V::Bits(_, v) => V::Int(i128::from(w - (128 - v.leading_zeros()))),
                    V::S(_, e, d) => self.st(K::Int, format!("rt::clz({e}, {w})"), *d),
                    V::D(_, n) => {
                        let c = self.dy(K::Bits(w), format!("b.un(UnOp::Clz, {n})"));
                        self.int_of(&c, false)?
                    }
                    _ => return fail("clz"),
                }
            }
            "truncate" => {
                let n = self.cw(arg(1)?)?;
                self.extract(arg(0)?, &V::Int(0), n)?
            }
            "truncateLSB" => {
                let n = self.cw(arg(1)?)?;
                let w = self.wid(arg(0)?)?;
                self.extract(arg(0)?, &V::Int(i128::from(w - n)), n)?
            }
            "pow2" | "_builtin_pow2" => match arg(0)? {
                V::Int(n) if (0..127).contains(n) => V::Int(1i128 << n),
                V::S(K::Int, e, d) => self.st(K::Int, format!("rt::pow2({e})"), *d),
                _ => return fail("pow2"),
            },
            "shl_int" | "_shl8" | "_shl32" | "_shl1" | "_shl_int" | "_shl_int_general" => {
                match (arg(0)?, arg(1)?) {
                    (V::Int(x), V::Int(n)) => V::Int(x << n),
                    (x, n) if !x.is_dyn() && !n.is_dyn() => {
                        let (xs, ns) = (self.sx(x)?, self.sx(n)?);
                        let d = x.deps() | n.deps();
                        self.st(K::Int, format!("rt::shl_int({xs}, {ns})"), d)
                    }
                    (x, n) => {
                        let (x, n) = (self.dx(x)?, self.dx(n)?);
                        self.dy(K::Int, format!("rt::shl_int_ir(b, {x}, {n})"))
                    }
                }
            }
            "shr_int" | "_shr32" | "_shr_int" | "_shr_int_general" => match (arg(0)?, arg(1)?) {
                (V::Int(x), V::Int(n)) => V::Int(x >> n),
                (x, n) if !x.is_dyn() && !n.is_dyn() => {
                    let (xs, ns) = (self.sx(x)?, self.sx(n)?);
                    let d = x.deps() | n.deps();
                    self.st(K::Int, format!("rt::shr_int({xs}, {ns})"), d)
                }
                (x, n) => {
                    let (x, n) = (self.dx(x)?, self.dx(n)?);
                    self.dy(K::Int, format!("b.bin(BinOp::AShr, {x}, {n})"))
                }
            },
            "concat_str" | "append_str" => V::Str("".into()),
            "dec_str" | "hex_str" | "DecStr" | "HexStr" | "string_of_bits" | "string_of_int" => {
                V::Str("".into())
            }
            "print_endline"
            | "prerr_endline"
            | "print_int"
            | "prerr_int"
            | "print_bits"
            | "prerr_bits"
            | "print"
            | "prerr"
            | "sail_branch_announce"
            | "sail_barrier"
            | "Barrier_ISB" => V::Unit,
            "__monomorphize" => arg(0)?.clone(),
            "undefined_bitvector" | "__UNKNOWN_bits" => {
                let n = self.cw(arg(0)?)?;
                V::Bits(n, 0)
            }
            "undefined_bool" | "__UNKNOWN_boolean" => V::Bool(false),
            "undefined_int" | "__UNKNOWN_integer" | "undefined_nat" => V::Int(0),
            "undefined_bit" => V::Bits(1, 0),
            _ => {
                let _ = hint;
                let _ = Kind::Int;
                return Ok(None);
            }
        };
        Ok(Some(v))
    }
}

//! 64-bit guest addresses. On a host with `shaderInt64` an address is a
//! 64-bit integer; without it, a vector of two 32-bit words (low, high)
//! with the carries done by hand, and a physical storage buffer pointer
//! comes from the pair through `OpBitcast` (as
//! `GL_EXT_buffer_reference_uvec2` does). Nothing here declares a 64-bit
//! type on a host without `shaderInt64`.

use super::super::Ctx;
use crate::spirv::op;

const CAP_INT64: u32 = 11;

impl Ctx {
    /// The type of a guest address.
    pub(crate) fn addr_t(&mut self) -> u32 {
        if self.host.int64 {
            self.cap(CAP_INT64);
            self.m.t_u64()
        } else {
            let u = self.u32t();
            self.m.t_vec(u, 2)
        }
    }

    /// The address with words `lo` and `hi`.
    pub(crate) fn addr_pair(&mut self, lo: u32, hi: u32) -> u32 {
        let u = self.u32t();
        let v2 = self.m.t_vec(u, 2);
        let pair = self.m.emit(op::COMPOSITE_CONSTRUCT, v2, &[lo, hi]);
        if self.host.int64 {
            let t = self.addr_t();
            self.m.emit(op::BITCAST, t, &[pair])
        } else {
            pair
        }
    }

    /// The (low, high) words of an address.
    pub(crate) fn addr_words(&mut self, a: u32) -> (u32, u32) {
        let u = self.u32t();
        let pair = if self.host.int64 {
            let v2 = self.m.t_vec(u, 2);
            self.m.emit(op::BITCAST, v2, &[a])
        } else {
            a
        };
        let lo = self.m.emit(op::COMPOSITE_EXTRACT, u, &[pair, 0]);
        let hi = self.m.emit(op::COMPOSITE_EXTRACT, u, &[pair, 1]);
        (lo, hi)
    }

    /// The low word of an address.
    pub(crate) fn addr_low(&mut self, a: u32) -> u32 {
        let u = self.u32t();
        if self.host.int64 {
            self.m.emit(op::U_CONVERT, u, &[a])
        } else {
            self.m.emit(op::COMPOSITE_EXTRACT, u, &[a, 0])
        }
    }

    pub(crate) fn addr_const(&mut self, v: u64) -> u32 {
        if self.host.int64 {
            self.cap(CAP_INT64);
            self.m.c_u64(v)
        } else {
            let lo = self.m.c_u32(v as u32);
            let hi = self.m.c_u32((v >> 32) as u32);
            let u = self.u32t();
            let v2 = self.m.t_vec(u, 2);
            self.m.c_composite(v2, &[lo, hi])
        }
    }

    /// The 32-bit `lo` zero-extended to an address.
    pub(crate) fn addr_zext(&mut self, lo: u32) -> u32 {
        if self.host.int64 {
            let t = self.addr_t();
            self.m.emit(op::U_CONVERT, t, &[lo])
        } else {
            let z = self.m.c_u32(0);
            self.addr_pair(lo, z)
        }
    }

    /// `a + b` modulo 2^64.
    pub(crate) fn addr_add(&mut self, a: u32, b: u32) -> u32 {
        if self.host.int64 {
            let t = self.addr_t();
            return self.m.emit(op::I_ADD, t, &[a, b]);
        }
        let (alo, ahi) = self.addr_words(a);
        let (blo, bhi) = self.addr_words(b);
        let (lo, carry) = self.addc(alo, blo);
        let u = self.u32t();
        let hi = self.m.emit(op::I_ADD, u, &[ahi, bhi]);
        let hi = self.m.emit(op::I_ADD, u, &[hi, carry]);
        self.addr_pair(lo, hi)
    }

    /// `a + k` modulo 2^64.
    pub(crate) fn addr_add_const(&mut self, a: u32, k: u64) -> u32 {
        if k == 0 {
            return a;
        }
        let kv = self.addr_const(k);
        self.addr_add(a, kv)
    }

    /// `a` with its low `bits` bits cleared.
    pub(crate) fn addr_align(&mut self, a: u32, bits: u32) -> u32 {
        let mask = !((1u64 << bits) - 1);
        if self.host.int64 {
            let t = self.addr_t();
            let m = self.addr_const(mask);
            return self.m.emit(op::BITWISE_AND, t, &[a, m]);
        }
        let (lo, hi) = self.addr_words(a);
        let u = self.u32t();
        let m = self.m.c_u32(mask as u32);
        let lo = self.m.emit(op::BITWISE_AND, u, &[lo, m]);
        self.addr_pair(lo, hi)
    }

    /// The low word of `a >> k` for a constant `k` in `1..32`.
    pub(crate) fn addr_shr_low(&mut self, a: u32, k: u32) -> u32 {
        let (lo, hi) = self.addr_words(a);
        let u = self.u32t();
        let kv = self.m.c_u32(k);
        let back = self.m.c_u32(32 - k);
        let l = self.m.emit(op::SHIFT_RIGHT_LOGICAL, u, &[lo, kv]);
        let h = self.m.emit(op::SHIFT_LEFT_LOGICAL, u, &[hi, back]);
        self.m.emit(op::BITWISE_OR, u, &[l, h])
    }
}

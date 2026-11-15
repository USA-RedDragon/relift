//! The alignment check of ordered, exclusive and atomic accesses, the one
//! part of `AArch64_UnalignedAccessFaults` that applies at EL0 with
//! alignment checking off: such an access that is not naturally aligned
//! faults, except that with `FEAT_LSE2` an ordered or atomic one faults
//! only when it crosses a 16-byte boundary. Exclusives always need natural
//! alignment.

use relift_ir::{BinOp, Builder, CmpOp, Value};

use super::SLOW_ALIGN;
use super::rt::St;
use crate::decode::Feature;

/// Leaves the block for [`SLOW_ALIGN`] when the `size`-byte access at
/// `addr` faults for alignment.
pub fn check(b: &mut Builder, st: &St, addr: Value, size: u32, exclusive: bool) {
    if size <= 1 {
        return;
    }
    let w = b.width(addr);
    let zero = b.konst(w, 0);
    let fault = if exclusive || !st.has(Feature::LSE2) {
        let m = b.konst(w, u128::from(size - 1));
        let low = b.bin(BinOp::And, addr, m);
        b.cmp(CmpOp::Ne, low, zero)
    } else {
        let m = b.konst(w, 15);
        let low = b.bin(BinOp::And, addr, m);
        let n = b.konst(w, u128::from(size));
        let end = b.bin(BinOp::Add, low, n);
        let sixteen = b.konst(w, 16);
        b.cmp(CmpOp::Ult, sixteen, end)
    };
    b.guard(fault, SLOW_ALIGN);
}

/// Leaves the block for [`SLOW_ALIGN`] when `fault` holds: an alignment
/// check the instruction itself makes (LDXP with 64-bit registers).
pub fn fault(b: &mut Builder, fault: Value) {
    b.guard(fault, SLOW_ALIGN);
}

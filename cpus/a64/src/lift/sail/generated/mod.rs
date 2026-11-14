//! The generated templates, one file per instruction class, and the lookup
//! across them.

mod branch;
mod excl;
mod fp;
mod int;
mod ldst;
mod simd;
mod system;

use super::GenFn;
use crate::decode::Encoding;

/// The generated template of `enc`, if there is one.
pub(super) fn template(enc: Encoding) -> Option<GenFn> {
    int::template(enc)
        .or_else(|| ldst::template(enc))
        .or_else(|| excl::template(enc))
        .or_else(|| branch::template(enc))
        .or_else(|| system::template(enc))
        .or_else(|| simd::template(enc))
        .or_else(|| fp::template(enc))
}

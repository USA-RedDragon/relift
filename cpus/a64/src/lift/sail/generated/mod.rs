//! The generated templates, one file per instruction class, and the lookup
//! across them.

mod branch;
mod excl;
mod fp;
mod int;
mod ldst;
mod simd_elem;
mod simd_ldst_multi;
mod simd_ldst_multi_post;
mod simd_ldst_single;
mod simd_misc;
mod simd_other;
mod simd_same;
mod simd_shift;
mod system;

use super::GenFn;
use crate::decode::Encoding;

/// The class files, in lookup order.
const CLASSES: [fn(Encoding) -> Option<GenFn>; 14] = [
    int::template,
    ldst::template,
    excl::template,
    branch::template,
    system::template,
    simd_ldst_multi::template,
    simd_ldst_multi_post::template,
    simd_ldst_single::template,
    simd_same::template,
    simd_shift::template,
    simd_misc::template,
    simd_elem::template,
    simd_other::template,
    fp::template,
];

/// The generated template of `enc`, if there is one.
pub(super) fn template(enc: Encoding) -> Option<GenFn> {
    CLASSES.iter().find_map(|t| t(enc))
}

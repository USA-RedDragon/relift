//! The generator's output (`cpus/mips/gen`), one file per template class.

mod branch;
mod cop1;
mod int;
mod ldst;
mod system;

use super::GenFn;
use crate::decode::Encoding;

/// The template of `enc`.
pub(crate) fn template(enc: Encoding) -> Option<GenFn> {
    int::template(enc)
        .or_else(|| ldst::template(enc))
        .or_else(|| branch::template(enc))
        .or_else(|| system::template(enc))
        .or_else(|| cop1::template(enc))
}

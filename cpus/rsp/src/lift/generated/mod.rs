//! The generator's output (`cpus/rsp/gen`), one file per template class.

mod branch;
mod int;
mod ldst;
mod system;
mod vector;
mod vmem;

use super::GenFn;
use crate::decode::Encoding;

/// The template of `enc`.
pub(crate) fn template(enc: Encoding) -> Option<GenFn> {
    int::template(enc)
        .or_else(|| ldst::template(enc))
        .or_else(|| branch::template(enc))
        .or_else(|| system::template(enc))
        .or_else(|| vector::template(enc))
        .or_else(|| vmem::template(enc))
}

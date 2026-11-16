//! The generator's output (`cpus/mips/gen`), one file per template class.

mod int;

use super::GenFn;
use crate::r3000a::decode::Encoding;

/// The template of `enc`.
pub(crate) fn template(enc: Encoding) -> Option<GenFn> {
    int::template(enc)
}

//! The generator's output (`cpus/arm32/gen`), one file per instruction set.

mod a32;
mod t16;

use super::GenFn;
use crate::decode::Encoding;

/// The template of `enc`.
pub(crate) fn template(enc: Encoding) -> Option<GenFn> {
    a32::template(enc).or_else(|| t16::template(enc))
}

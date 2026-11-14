//! Placeholder written by `cpus/sm83/gen/regen.sh` before it regenerates, so
//! the crate builds while the generator runs.

use super::rt::St;

/// No opcode has a template.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Encoding {}

/// No base template.
pub(super) fn base(_code: u8) -> Option<super::GenFn> {
    None
}

/// No CB template.
pub(super) fn cb(_code: u8) -> Option<super::GenFn> {
    None
}

const _: Option<fn(&mut St)> = None;

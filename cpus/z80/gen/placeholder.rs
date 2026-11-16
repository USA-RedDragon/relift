//! Placeholder written by `cpus/z80/gen/regen.sh` before it regenerates, so
//! the crate builds while the generator runs.

use super::rt::St;

/// No template.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Encoding {}

/// No template.
pub(super) fn template(_group: u8, _code: u8) -> Option<super::GenFn> {
    None
}

const _: Option<fn(&mut St)> = None;

//! The generator's output (`cpus/arm32/gen`), one file per instruction set
//! and profile.

mod a32;
mod a32_v5te;
mod t16;
mod t16_v5te;

use super::GenFn;
use crate::decode::{Encoding, Profile};

/// The template of `enc` on `profile`.
pub(crate) fn template(enc: Encoding, profile: Profile) -> Option<GenFn> {
    match profile {
        Profile::V4t => a32::template(enc).or_else(|| t16::template(enc)),
        Profile::V5te => a32_v5te::template(enc).or_else(|| t16_v5te::template(enc)),
    }
}

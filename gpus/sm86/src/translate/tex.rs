//! Textures and surfaces.

use super::{Ctx, Res, bad};
use crate::Inst;

pub(crate) fn inst(_c: &mut Ctx, i: &Inst) -> Option<Res<()>> {
    if matches!(i.mnemonic, "TEX" | "TLD" | "TLD4" | "TXQ" | "TMML" | "TXD") {
        return Some(bad(format!("texture {}", i.mnemonic)));
    }
    None
}

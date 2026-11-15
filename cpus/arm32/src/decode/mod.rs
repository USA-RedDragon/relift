//! Decoding: the A32 and Thumb clause tables of sail-arm and the ARMv4T
//! overlay, in the Sail decoder's order, generated into
//! `decode/generated.rs`.
//! A word goes to the first clause whose fixed bits match; a clause outside
//! ARMv4T (or no clause) makes it undefined. The condition field of an A32
//! word is not part of the match: the lifter tests it.

mod generated;

pub use generated::Encoding;

/// The A32 decode group of a word, as in Sail's `__DecodeA32`; the
/// condition field 1111 (never, on ARMv4T) decodes as 1110.
fn a32_group(w: u32) -> &'static [(u32, u32, Option<Encoding>, &'static str)] {
    let op = (w >> 25) & 0b111;
    match op >> 1 {
        0b00 => generated::A32_DATAPROMISC,
        0b10 => generated::A32_BRANCHBLOCK,
        0b11 => generated::A32_SYSASIMDFP,
        _ if op == 0b010 => generated::A32_LOADSTOREIMMLIT,
        _ if w >> 4 & 1 == 0 => generated::A32_LOADSTOREREG,
        _ => generated::A32_MEDIA,
    }
}

fn first(tab: &[(u32, u32, Option<Encoding>, &'static str)], w: u32) -> Option<Encoding> {
    tab.iter()
        .find(|(m, v, _, _)| w & m == *v)
        .and_then(|e| e.2)
}

/// The ARMv4T encoding of the A32 word `w`, or `None` when it is undefined.
pub fn decode_a32(w: u32) -> Option<Encoding> {
    let w = (w & 0x0fff_ffff) | 0xe000_0000;
    first(a32_group(w), w)
}

/// The name of the sail-arm clause that takes the word, ignoring the
/// ARMv4T profile (for diagnostics).
pub fn sail_clause(w: u32, thumb: bool) -> &'static str {
    let (tab, w) = if thumb {
        (generated::T16, w & 0xffff)
    } else {
        let w = (w & 0x0fff_ffff) | 0xe000_0000;
        (a32_group(w), w)
    };
    tab.iter()
        .find(|(m, v, _, _)| w & m == *v)
        .map_or("none", |e| e.3)
}

/// The ARMv4T encoding of the Thumb halfword `h`, or `None`.
pub fn decode_t16(h: u16) -> Option<Encoding> {
    first(generated::T16, u32::from(h))
}

/// Every encoding with its fixed bits, whether it is Thumb, and its Sail
/// decode group.
pub fn forms() -> &'static [(Encoding, u32, u32, bool, &'static str)] {
    generated::FORMS
}

impl Encoding {
    /// The encoding's name: the sail-arm decode function's, or the overlay's.
    pub fn name(self) -> &'static str {
        generated::NAMES[self as usize]
    }
}

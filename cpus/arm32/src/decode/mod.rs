//! Decoding: the A32 and Thumb clause tables of sail-arm and an overlay per
//! profile (ARMv4T `armv4t.sail`, ARMv5TE `armv5te.sail`), in the Sail
//! decoder's order, generated into `decode/generated.rs`.
//! A word goes to the first clause whose fixed bits match; a clause outside
//! the profile (or no clause) makes it undefined. The condition field of an
//! A32 word is not part of the match: the lifter tests it.

mod generated;

pub use generated::Encoding;

/// The architecture a CPU implements.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum Profile {
    /// ARMv4T: the ARM7TDMI of the Game Boy Advance and the NDS ARM7.
    #[default]
    V4t,
    /// ARMv5TE: the ARM946E-S of the NDS ARM9.
    V5te,
}

type Table = &'static [(u32, u32, Option<Encoding>, &'static str)];

/// A profile's decode groups.
struct Groups {
    data_pro_misc: Table,
    load_store_imm_lit: Table,
    load_store_reg: Table,
    media: Table,
    branch_block: Table,
    sys_asimd_fp: Table,
    /// The words with the condition field 1111 (ARMv5).
    unconditional: Option<Table>,
    t16: Table,
    forms: &'static [(Encoding, u32, u32, bool, &'static str)],
}

static V4T: Groups = Groups {
    data_pro_misc: generated::A32_DATAPROMISC,
    load_store_imm_lit: generated::A32_LOADSTOREIMMLIT,
    load_store_reg: generated::A32_LOADSTOREREG,
    media: generated::A32_MEDIA,
    branch_block: generated::A32_BRANCHBLOCK,
    sys_asimd_fp: generated::A32_SYSASIMDFP,
    unconditional: None,
    t16: generated::T16,
    forms: generated::FORMS,
};

static V5TE: Groups = Groups {
    data_pro_misc: generated::V5TE_A32_DATAPROMISC,
    load_store_imm_lit: generated::V5TE_A32_LOADSTOREIMMLIT,
    load_store_reg: generated::V5TE_A32_LOADSTOREREG,
    media: generated::V5TE_A32_MEDIA,
    branch_block: generated::V5TE_A32_BRANCHBLOCK,
    sys_asimd_fp: generated::V5TE_A32_SYSASIMDFP,
    unconditional: Some(generated::V5TE_A32_UNCONDITIONAL),
    t16: generated::V5TE_T16,
    forms: generated::V5TE_FORMS,
};

fn groups(p: Profile) -> &'static Groups {
    match p {
        Profile::V4t => &V4T,
        Profile::V5te => &V5TE,
    }
}

/// The A32 decode group of a word, as in Sail's `__DecodeA32`.
fn a32_group(g: &Groups, w: u32) -> Table {
    let op = (w >> 25) & 0b111;
    match op >> 1 {
        0b00 => g.data_pro_misc,
        0b10 => g.branch_block,
        0b11 => g.sys_asimd_fp,
        _ if op == 0b010 => g.load_store_imm_lit,
        _ if w >> 4 & 1 == 0 => g.load_store_reg,
        _ => g.media,
    }
}

fn first(tab: Table, w: u32) -> Option<Encoding> {
    tab.iter()
        .find(|(m, v, _, _)| w & m == *v)
        .and_then(|e| e.2)
}

/// ARMv5: a word with the condition field 1111 goes to the unconditional
/// group (bit 27 clear) or to the clauses of its group that fix the
/// condition field to 1111 (BLX immediate).
fn decode_nv(g: &Groups, uncond: Table, w: u32) -> Option<Encoding> {
    if w >> 27 & 1 == 0 {
        return first(uncond, w);
    }
    a32_group(g, w)
        .iter()
        .find(|(m, v, _, _)| m >> 28 == 0xf && w & m == *v)
        .and_then(|e| e.2)
}

/// The encoding of the A32 word `w` on `profile`, or `None` when it is
/// undefined. On ARMv4T the condition field 1111 (never) decodes as 1110.
pub fn decode_a32_for(w: u32, profile: Profile) -> Option<Encoding> {
    let g = groups(profile);
    if let (Some(u), 0xf) = (g.unconditional, w >> 28) {
        return decode_nv(g, u, w);
    }
    let w = (w & 0x0fff_ffff) | 0xe000_0000;
    first(a32_group(g, w), w)
}

/// The ARMv4T encoding of the A32 word `w`, or `None` when it is undefined.
pub fn decode_a32(w: u32) -> Option<Encoding> {
    decode_a32_for(w, Profile::V4t)
}

/// The name of the sail-arm clause that takes the word, ignoring the
/// ARMv4T profile (for diagnostics).
pub fn sail_clause(w: u32, thumb: bool) -> &'static str {
    let (tab, w) = if thumb {
        (V4T.t16, w & 0xffff)
    } else {
        let w = (w & 0x0fff_ffff) | 0xe000_0000;
        (a32_group(&V4T, w), w)
    };
    tab.iter()
        .find(|(m, v, _, _)| w & m == *v)
        .map_or("none", |e| e.3)
}

/// The encoding of the Thumb halfword `h` on `profile`, or `None`.
pub fn decode_t16_for(h: u16, profile: Profile) -> Option<Encoding> {
    first(groups(profile).t16, u32::from(h))
}

/// The ARMv4T encoding of the Thumb halfword `h`, or `None`.
pub fn decode_t16(h: u16) -> Option<Encoding> {
    decode_t16_for(h, Profile::V4t)
}

/// Every ARMv4T encoding with its fixed bits, whether it is Thumb, and its
/// Sail decode group.
pub fn forms() -> &'static [(Encoding, u32, u32, bool, &'static str)] {
    V4T.forms
}

/// Every encoding of `profile`, as [`forms`].
pub fn forms_for(profile: Profile) -> &'static [(Encoding, u32, u32, bool, &'static str)] {
    groups(profile).forms
}

impl Encoding {
    /// The encoding's name: the sail-arm decode function's, or the overlay's.
    pub fn name(self) -> &'static str {
        generated::NAMES[self as usize]
    }
}

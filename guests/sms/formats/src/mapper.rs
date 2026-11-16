//! A static first guess at the cartridge mapper.

use crate::codemasters::CodemastersHeader;

/// The mapper a cartridge uses.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MapperKind {
    /// No bank writes: the ROM sits in `$0000-$BFFF` as it is.
    None,
    /// The Sega mapper, registers at `$FFFC-$FFFF`.
    Sega,
    /// The Codemasters mapper, registers at `$0000`, `$4000` and `$8000`.
    Codemasters,
    /// The Korean mapper, one register at `$A000`.
    Korea,
}

impl MapperKind {
    /// A short lowercase name, stable for tool output.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            MapperKind::None => "none",
            MapperKind::Sega => "sega",
            MapperKind::Codemasters => "codemasters",
            MapperKind::Korea => "korea",
        }
    }
}

/// The mapper chosen, the evidence and the reason.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MapperDecision {
    /// The chosen mapper.
    pub kind: MapperKind,
    /// A sentence saying why.
    pub reason: String,
    /// Stores `ld (nn),a` to `$FFFC-$FFFF` found anywhere in the image.
    pub sega_stores: usize,
    /// Stores to `$0000`, `$4000` or `$8000`.
    pub codemasters_stores: usize,
    /// Stores to `$A000`.
    pub korea_stores: usize,
}

/// Count the `ld (nn),a` stores (`$32 nn nn`) of each mapper's registers. A
/// byte scan of the whole image, so data that looks like a store counts too;
/// discovery refines the guess.
#[must_use]
pub fn scan_stores(rom: &[u8]) -> (usize, usize, usize) {
    let (mut sega, mut cm, mut ko) = (0, 0, 0);
    for w in rom.windows(3) {
        if w[0] != 0x32 {
            continue;
        }
        match u16::from_le_bytes([w[1], w[2]]) {
            0xFFFC..=0xFFFF => sega += 1,
            0x0000 | 0x4000 | 0x8000 => cm += 1,
            0xA000 => ko += 1,
            _ => {}
        }
    }
    (sega, cm, ko)
}

/// Choose the mapper of `rom`: Codemasters when its header checksum
/// matches, else by the stores found.
#[must_use]
pub fn detect(rom: &[u8], codemasters: Option<&CodemastersHeader>) -> MapperDecision {
    let (sega_stores, codemasters_stores, korea_stores) = scan_stores(rom);
    let (kind, reason) = if codemasters.is_some_and(|c| c.checksum_ok) {
        (
            MapperKind::Codemasters,
            "the Codemasters header checksum at $7FE6 matches".to_owned(),
        )
    } else if sega_stores > 0 {
        (
            MapperKind::Sega,
            format!("{sega_stores} stores to $FFFC-$FFFF"),
        )
    } else if codemasters_stores > 0 && rom.len() > 0x8000 {
        (
            MapperKind::Codemasters,
            format!("{codemasters_stores} stores to $0000/$4000/$8000 and no Sega stores"),
        )
    } else if korea_stores > 0 && rom.len() > 0x8000 {
        (
            MapperKind::Korea,
            format!("{korea_stores} stores to $A000 and no Sega stores"),
        )
    } else if rom.len() <= 0xC000 {
        (
            MapperKind::None,
            "no bank stores and at most 48 KiB: the power-on mapping covers the ROM".to_owned(),
        )
    } else {
        (
            MapperKind::Sega,
            "no bank stores seen, over 48 KiB: the Sega mapper is assumed".to_owned(),
        )
    };
    MapperDecision {
        kind,
        reason,
        sega_stores,
        codemasters_stores,
        korea_stores,
    }
}

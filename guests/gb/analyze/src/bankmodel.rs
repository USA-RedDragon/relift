//! Which writes select the ROM bank, and which bank a register pair names.

use relift_gb_formats::MapperKind;

/// A ROM bank register.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BankReg {
    /// The low bank register.
    Lo,
    /// The upper bank register (MBC1 bits 5-6, MBC5 bit 8).
    Hi,
}

/// Why a bank number does not name a bank of the image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OutOfRange(pub u16);

/// The ROM banking of one mapper.
#[derive(Clone, Copy, Debug)]
pub struct BankModel {
    kind: Option<MapperKind>,
    banks: usize,
}

impl BankModel {
    /// The model for `kind` with `banks` ROM banks, or `None` for a mapper
    /// whose ROM banking this analysis does not model.
    #[must_use]
    pub fn new(kind: MapperKind, banks: usize) -> Option<BankModel> {
        match kind {
            MapperKind::None
            | MapperKind::Mbc1
            | MapperKind::Mbc1M
            | MapperKind::Mbc2
            | MapperKind::Mbc3
            | MapperKind::Mbc5 => Some(BankModel {
                kind: Some(kind),
                banks,
            }),
            _ => None,
        }
    }

    /// The model of a cartridge whose ROM banking is not modelled: the
    /// bank in the window is never known, so only the fixed bank's code is
    /// found.
    #[must_use]
    pub fn opaque(banks: usize) -> BankModel {
        BankModel { kind: None, banks }
    }

    /// The mapper kind, or `None` when its banking is not modelled.
    #[must_use]
    pub fn kind(&self) -> Option<MapperKind> {
        self.kind
    }

    /// The register a write to `addr` selects, if any.
    #[must_use]
    pub fn classify(&self, addr: u16) -> Option<BankReg> {
        let kind = self.kind?;
        match kind {
            MapperKind::None => None,
            MapperKind::Mbc1 | MapperKind::Mbc1M => match addr {
                0x2000..=0x3FFF => Some(BankReg::Lo),
                0x4000..=0x5FFF => Some(BankReg::Hi),
                _ => None,
            },
            MapperKind::Mbc2 => (addr < 0x4000 && addr & 0x100 != 0).then_some(BankReg::Lo),
            MapperKind::Mbc3 => (0x2000..=0x3FFF).contains(&addr).then_some(BankReg::Lo),
            MapperKind::Mbc5 => match addr {
                0x2000..=0x2FFF => Some(BankReg::Lo),
                0x3000..=0x3FFF => Some(BankReg::Hi),
                _ => None,
            },
            _ => None,
        }
    }

    /// The register content after writing `value` to `reg`.
    #[must_use]
    pub fn register_value(&self, reg: BankReg, value: u8) -> u16 {
        let v = u16::from(value);
        let Some(kind) = self.kind else {
            return 0;
        };
        match (kind, reg) {
            (MapperKind::Mbc1, BankReg::Lo) => v & 0x1F,
            (MapperKind::Mbc1M | MapperKind::Mbc2, BankReg::Lo) => v & 0x0F,
            (MapperKind::Mbc3, BankReg::Lo) => v & 0x7F,
            (MapperKind::Mbc5, BankReg::Lo) => v,
            (MapperKind::Mbc1 | MapperKind::Mbc1M, BankReg::Hi) => v & 3,
            (MapperKind::Mbc5, BankReg::Hi) => v & 1,
            _ => 0,
        }
    }

    /// Whether the upper register can change the bank for this image, so a
    /// bank selected through the low register alone is not known without it.
    #[must_use]
    pub fn hi_matters(&self) -> bool {
        match self.kind {
            Some(MapperKind::Mbc1) => self.banks > 32,
            Some(MapperKind::Mbc1M) => self.banks > 16,
            Some(MapperKind::Mbc5) => self.banks > 256,
            _ => false,
        }
    }

    /// The bank for the register contents, taking an unknown upper register
    /// as zero when it cannot matter.
    ///
    /// # Errors
    ///
    /// The bank is not in the image.
    pub fn resolve(&self, lo: Option<u16>, hi: Option<u8>) -> Option<Result<u16, OutOfRange>> {
        if self.kind? == MapperKind::None {
            return Some(Ok(1));
        }
        let lo = lo?;
        match hi {
            Some(h) => Some(self.effective(lo, h)),
            None if !self.hi_matters() => Some(self.effective(lo, 0)),
            None => None,
        }
    }

    /// The register contents after reset.
    #[must_use]
    pub fn reset(&self) -> (u16, u8) {
        (1, 0)
    }

    /// The registers that select `bank`.
    #[must_use]
    pub fn split(&self, bank: u16) -> (u16, u8) {
        let hi = |shift: u32| u8::try_from(bank >> shift).unwrap_or(0);
        match self.kind {
            Some(MapperKind::Mbc1) => (bank & 0x1F, hi(5)),
            Some(MapperKind::Mbc1M) => (bank & 0x0F, hi(4)),
            Some(MapperKind::Mbc5) => (bank & 0xFF, hi(8)),
            _ => (bank, 0),
        }
    }

    /// The bank mapped at `$4000-$7FFF` for the register contents `lo` and
    /// `hi`: the zero-to-one remap, the field widths and the wrap of a
    /// number above the image size.
    ///
    /// # Errors
    ///
    /// The bank is not in the image.
    pub fn effective(&self, lo: u16, hi: u8) -> Result<u16, OutOfRange> {
        let Some(kind) = self.kind else {
            return Err(OutOfRange(lo));
        };
        let low = match kind {
            MapperKind::Mbc5 => lo,
            _ if lo == 0 => 1,
            _ => lo,
        };
        let h = u16::from(hi);
        let raw = match kind {
            MapperKind::None | MapperKind::Mbc2 | MapperKind::Mbc3 => low,
            MapperKind::Mbc1 => (h << 5) | low,
            MapperKind::Mbc1M => (h << 4) | low,
            _ => (h << 8) | low,
        };
        if kind == MapperKind::None {
            return Ok(1);
        }
        let mask = self.banks.next_power_of_two().saturating_sub(1);
        let bank = usize::from(raw) & mask;
        if bank < self.banks {
            Ok(u16::try_from(bank).unwrap_or(u16::MAX))
        } else {
            Err(OutOfRange(raw))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mbc1_remaps_zero_and_joins_the_upper_bits() {
        let m = BankModel::new(MapperKind::Mbc1, 128).expect("modelled");
        assert_eq!(m.effective(0, 0), Ok(1));
        assert_eq!(m.effective(0, 1), Ok(0x21));
        assert_eq!(m.effective(0x1F, 3), Ok(0x7F));
        assert_eq!(m.register_value(BankReg::Lo, 0xE5), 5);
    }

    #[test]
    fn mbc5_allows_bank_zero_and_nine_bits() {
        let m = BankModel::new(MapperKind::Mbc5, 512).expect("modelled");
        assert_eq!(m.effective(0, 0), Ok(0));
        assert_eq!(m.effective(0x23, 1), Ok(0x123));
        assert_eq!(m.classify(0x3000), Some(BankReg::Hi));
        assert_eq!(m.classify(0x2100), Some(BankReg::Lo));
        assert_eq!(m.split(0x123), (0x23, 1));
    }

    #[test]
    fn small_images_wrap_the_bank_number() {
        let m = BankModel::new(MapperKind::Mbc3, 8).expect("modelled");
        assert_eq!(m.effective(0x0A, 0), Ok(2));
        let odd = BankModel::new(MapperKind::Mbc5, 72).expect("modelled");
        assert_eq!(odd.effective(100, 0), Err(OutOfRange(100)));
    }

    #[test]
    fn mbc2_selects_on_address_bit_eight() {
        let m = BankModel::new(MapperKind::Mbc2, 16).expect("modelled");
        assert_eq!(m.classify(0x2100), Some(BankReg::Lo));
        assert_eq!(m.classify(0x2000), None);
    }

    #[test]
    fn an_unbanked_cartridge_always_maps_bank_one() {
        let m = BankModel::new(MapperKind::None, 2).expect("modelled");
        assert_eq!(m.resolve(None, None), Some(Ok(1)));
    }

    #[test]
    fn unmodelled_mappers_are_refused() {
        assert!(BankModel::new(MapperKind::Huc3, 64).is_none());
    }

    #[test]
    fn an_opaque_model_never_knows_the_window_bank() {
        let m = BankModel::opaque(64);
        assert_eq!(m.resolve(Some(1), Some(0)), None);
        assert!(m.effective(1, 0).is_err());
        assert_eq!(m.classify(0x2000), None);
    }
}

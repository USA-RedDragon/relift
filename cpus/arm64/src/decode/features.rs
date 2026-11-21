//! Architecture feature sets: what a guest's CPU implements.
//!
//! A guest names its CPU's features in its descriptor; the decoder and the
//! lifter take them so that an encoding the CPU lacks is unallocated there
//! (it lifts to an undefined-instruction exception, as on that hardware) and
//! a template whose Arm semantics branch on a feature takes the branch the
//! CPU would. Nothing in relift picks a level globally.

use super::generated::{self, FEATURE_COUNT, FEATURE_NAMES, FEATURES};

pub use generated::Feature;

const WORDS: usize = FEATURE_COUNT.div_ceil(64);

/// A set of architecture features.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Features {
    bits: [u64; WORDS],
}

impl Feature {
    /// The number of features.
    pub const COUNT: usize = FEATURE_COUNT;

    /// Every feature, in name order.
    pub fn all() -> impl Iterator<Item = Feature> {
        FEATURES.iter().copied()
    }

    /// The AARCHMRS name, for example `FEAT_LSE`.
    pub fn name(self) -> &'static str {
        FEATURE_NAMES[self as usize]
    }

    /// The feature named `name` (`FEAT_LSE` or `LSE`).
    pub fn from_name(name: &str) -> Option<Feature> {
        let n = name.strip_prefix("FEAT_").unwrap_or(name);
        FEATURES
            .iter()
            .copied()
            .find(|f| &f.name()["FEAT_".len()..] == n)
    }
}

impl Features {
    /// No features at all.
    pub const NONE: Features = Features { bits: [0; WORDS] };

    /// Armv8.0-A with floating point and Advanced SIMD and no optional
    /// extension.
    pub const ARMV8_0: Features = Features::NONE.with(Feature::FP).with(Feature::AdvSIMD);

    /// The Cortex-A57 (the Nintendo Switch CPU): Armv8.0-A with the
    /// Cryptographic Extension (AES, PMULL, SHA1, SHA256) and CRC32.
    pub const CORTEX_A57: Features = Features::ARMV8_0
        .with(Feature::AES)
        .with(Feature::PMULL)
        .with(Feature::SHA1)
        .with(Feature::SHA256)
        .with(Feature::CRC32);

    /// The Cortex-A78C (the Nintendo Switch 2 CPU), from its ID registers
    /// (Technical Reference Manual r0p2): Armv8.2-A with the Cryptographic
    /// Extension, CRC32, LSE, RDM, FP16, DotProd, FlagM, LRCPC2, DPB, RAS,
    /// LOR, SSBS, CSV2 and CSV3, LSE2 (`ID_AA64MMFR2_EL1.AT` 1, one of the
    /// Armv8.4-A features the core has: an unaligned atomic or ordered
    /// access inside one 16-byte granule does not fault), and pointer
    /// authentication with QARMA5 and PAuth2 but without EPAC, FPAC or BTI
    /// (`ID_AA64ISAR1_EL1.APA` 3, `GPA` 1).
    pub const CORTEX_A78C: Features = Features::CORTEX_A57
        .with(Feature::LSE)
        .with(Feature::RDM)
        .with(Feature::FP16)
        .with(Feature::DotProd)
        .with(Feature::FlagM)
        .with(Feature::LRCPC)
        .with(Feature::LRCPC2)
        .with(Feature::DPB)
        .with(Feature::RAS)
        .with(Feature::LOR)
        .with(Feature::SSBS)
        .with(Feature::CSV2)
        .with(Feature::CSV3)
        .with(Feature::PAuth)
        .with(Feature::PAuth2)
        .with(Feature::PACQARMA5)
        .with(Feature::LSE2);

    /// Every feature the specification names; what [`super::decode`] uses.
    pub const ALL: Features = {
        let mut s = Features::NONE;
        let mut i = 0;
        while i < FEATURE_COUNT {
            s = s.with(FEATURES[i]);
            i += 1;
        }
        s
    };

    /// This set plus `f`.
    pub const fn with(self, f: Feature) -> Features {
        let mut bits = self.bits;
        let i = f as usize;
        bits[i / 64] |= 1 << (i % 64);
        Features { bits }
    }

    /// This set without `f`.
    pub const fn without(self, f: Feature) -> Features {
        let mut bits = self.bits;
        let i = f as usize;
        bits[i / 64] &= !(1 << (i % 64));
        Features { bits }
    }

    /// Whether the set holds `f`.
    pub const fn has(&self, f: Feature) -> bool {
        let i = f as usize;
        self.bits[i / 64] >> (i % 64) & 1 == 1
    }

    /// Whether the set holds every feature of `other`.
    pub fn contains(&self, other: &Features) -> bool {
        self.bits
            .iter()
            .zip(other.bits.iter())
            .all(|(a, b)| a & b == *b)
    }

    /// The features in the set, in name order.
    pub fn iter(&self) -> impl Iterator<Item = Feature> + '_ {
        Feature::all().filter(|f| self.has(*f))
    }

    /// Parses a comma-separated list: a base (`armv8.0`, `cortex-a57`, `cortex-a78c`,
    /// `all`, `none`) and features (`FEAT_LSE` or `LSE`), each optionally
    /// preceded by `-` to remove it.
    pub fn parse(text: &str) -> Result<Features, String> {
        let mut s = Features::NONE;
        for item in text.split(',').map(str::trim).filter(|t| !t.is_empty()) {
            let (minus, name) = match item.strip_prefix('-') {
                Some(n) => (true, n),
                None => (false, item),
            };
            let base = match name.to_ascii_lowercase().as_str() {
                "armv8.0" => Some(Features::ARMV8_0),
                "cortex-a57" => Some(Features::CORTEX_A57),
                "cortex-a78c" => Some(Features::CORTEX_A78C),
                "all" => Some(Features::ALL),
                "none" => Some(Features::NONE),
                _ => None,
            };
            let add = match base {
                Some(b) => b,
                None => match Feature::from_name(name) {
                    Some(f) => Features::NONE.with(f),
                    None => return Err(format!("unknown feature {name}")),
                },
            };
            for (w, a) in s.bits.iter_mut().zip(add.bits) {
                if minus {
                    *w &= !a;
                } else {
                    *w |= a;
                }
            }
        }
        Ok(s)
    }
}

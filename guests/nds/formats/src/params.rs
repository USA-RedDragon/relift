//! The SDK's module parameters of a static binary: the autoload list, the
//! bss range, the compressed static end and the SDK version.
//!
//! Layout from the start of the parameters: autoload list start and end,
//! autoload data start, bss start and end, compressed static end (nonzero
//! when the static is BLZ-compressed), SDK version, then the two marker
//! words.

use crate::bytes::u32_at;

/// Offset of the compressed static end inside the parameters.
pub const COMPRESSED_END: usize = 0x14;
/// Offset of the SDK version inside the parameters.
pub const SDK_VERSION: usize = 0x18;
/// Offset of the marker words inside the parameters.
pub const MARKERS: usize = 0x1C;
/// Size of the parameters with their markers.
pub const SIZE: usize = 0x24;

const MARK_A: u32 = 0xDEC0_0621;
const MARK_B: u32 = 0x2106_C0DE;
/// The ARM9 footer after the binary in the image starts with this word.
pub const FOOTER_MARK: u32 = MARK_A;
/// Size of the ARM9 footer.
pub const FOOTER_LEN: usize = 12;

/// How the parameters were found.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Source {
    /// The 12-byte footer after the ARM9 binary.
    Footer,
    /// The marker words, in the order `0xDEC00621`, `0x2106C0DE`.
    Markers,
    /// The marker words, in the order `0x2106C0DE`, `0xDEC00621`.
    MarkersSwapped,
    /// A pointer in the start code's literal pool, checked against the
    /// autoload list (binaries without markers).
    LiteralPool,
}

/// The module parameters of a static binary.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ModuleParams {
    /// Offset of the parameters in the binary.
    pub offset: usize,
    /// How they were found.
    pub source: Source,
    /// Address of the autoload list start.
    pub autoload_start: u32,
    /// Address of the autoload list end.
    pub autoload_end: u32,
    /// Address of the first autoload block's data.
    pub autoload_data: u32,
    /// Address of the bss start.
    pub bss_start: u32,
    /// Address of the bss end.
    pub bss_end: u32,
    /// Address of the end of the compressed static; zero when the static
    /// is not compressed (or the parameters were found without markers).
    pub compressed_end: u32,
    /// The SDK version word, when inside the binary.
    pub sdk_version: Option<u32>,
}

fn at(raw: &[u8], offset: usize, source: Source) -> Option<ModuleParams> {
    let w = |i: usize| u32_at(raw, offset.checked_add(i)?);
    Some(ModuleParams {
        offset,
        source,
        autoload_start: w(0)?,
        autoload_end: w(4)?,
        autoload_data: w(8)?,
        bss_start: w(12)?,
        bss_end: w(16)?,
        compressed_end: w(COMPRESSED_END)?,
        sdk_version: w(SDK_VERSION),
    })
}

fn find_pair(raw: &[u8], a: u32, b: u32) -> Option<usize> {
    let mut pat = [0u8; 8];
    pat[..4].copy_from_slice(&a.to_le_bytes());
    pat[4..].copy_from_slice(&b.to_le_bytes());
    raw.windows(8).position(|w| w == pat)
}

fn literal_pool(raw: &[u8], ram: u32) -> Option<ModuleParams> {
    for i in (0..raw.len().min(0x400).saturating_sub(3)).step_by(4) {
        let Some(p0) = u32_at(raw, i)
            .map(|w| w.wrapping_sub(ram) as usize)
            .filter(|p| *p <= raw.len().saturating_sub(0x14))
        else {
            continue;
        };
        let rel = |j: usize| u32_at(raw, p0 + 4 * j).map(|w| w.wrapping_sub(ram) as usize);
        let (Some(ls), Some(le), Some(src)) = (rel(0), rel(1), rel(2)) else {
            continue;
        };
        if !(0 < ls && ls < le && le <= raw.len() && (le - ls).is_multiple_of(12)) {
            continue;
        }
        if !(0 < src && src <= ls) {
            continue;
        }
        let total: usize = (ls..le)
            .step_by(12)
            .map(|a| u32_at(raw, a + 4).unwrap_or(0) as usize)
            .sum();
        if src + total == ls {
            return at(raw, p0, Source::LiteralPool).map(|p| ModuleParams {
                compressed_end: 0,
                ..p
            });
        }
    }
    None
}

/// Find the parameters of the static binary `raw` loaded at `ram`. `footer`
/// is the 12 bytes after an ARM9 binary in the image. Tries, in order, the
/// footer, the two marker orders and, for binaries without markers, the
/// literal pool.
#[must_use]
pub fn find(raw: &[u8], ram: u32, footer: Option<&[u8]>) -> Option<ModuleParams> {
    if let Some(f) = footer
        && u32_at(f, 0) == Some(FOOTER_MARK)
        && let Some(off) = u32_at(f, 4).map(|o| o as usize)
        && off.saturating_add(SIZE) <= raw.len()
    {
        return at(raw, off, Source::Footer);
    }
    for (a, b, source) in [
        (MARK_A, MARK_B, Source::Markers),
        (MARK_B, MARK_A, Source::MarkersSwapped),
    ] {
        if let Some(k) = find_pair(raw, a, b) {
            return k.checked_sub(MARKERS).and_then(|o| at(raw, o, source));
        }
    }
    literal_pool(raw, ram)
}

//! The character set each fallback font covers: its region's national
//! standard, decoded through encoding_rs's WHATWG tables, plus printable
//! ASCII in every font.

use std::ops::RangeInclusive;

/// A national character set a fallback font is cut to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Charset {
    /// JIS X 0208 and JIS X 0213 (both planes), with Latin-1 and Latin
    /// Extended-A for the European languages the standard font serves (Noto
    /// Sans CJK JP has Latin-1 and Cyrillic, and only part of Extended-A).
    Japanese,
    /// GB 2312.
    Gb2312,
    /// Big5: its symbols and both levels of hanzi.
    Big5,
    /// KS X 1001: symbols, hangul and hanja.
    KsX1001,
    /// No characters beyond printable ASCII.
    Empty,
}

const ASCII: RangeInclusive<u32> = 0x20..=0x7e;
const LATIN: RangeInclusive<u32> = 0xa0..=0x17f;
const EUC_TRAIL: RangeInclusive<u8> = 0xa1..=0xfe;

/// The characters of every two-byte code of `enc` with a lead byte in
/// `leads` and a trail byte in `trails` that decodes to one character.
fn decode(
    enc: &'static encoding_rs::Encoding,
    leads: impl Iterator<Item = u8>,
    trails: &[RangeInclusive<u8>],
    out: &mut Vec<u32>,
) {
    for lead in leads {
        for trail in trails.iter().cloned().flatten() {
            let bytes = [lead, trail];
            let (text, bad) = enc.decode_without_bom_handling(&bytes);
            let mut chars = text.chars();
            if let (false, Some(c), None) = (bad, chars.next(), chars.next()) {
                out.push(u32::from(c));
            }
        }
    }
}

/// The code points x0213.org's JIS X 0213:2004 table maps to: the second
/// field of each line, `U+XXXX` or a sequence `U+XXXX+XXXX`.
fn jis_x_0213(table: &str) -> Vec<u32> {
    table
        .lines()
        .filter(|l| !l.starts_with('#'))
        .filter_map(|l| l.split('\t').nth(1)?.strip_prefix("U+"))
        .flat_map(|seq| seq.split('+'))
        .filter_map(|h| u32::from_str_radix(h, 16).ok())
        .collect()
}

impl Charset {
    /// The sorted code points of the set. `jis_x_0213` is x0213.org's
    /// table ([`crate::JIS_X_0213`]), read by [`Charset::Japanese`] only.
    #[must_use]
    pub fn code_points(self, jis_x_0213_table: &str) -> Vec<u32> {
        let mut v: Vec<u32> = ASCII.collect();
        match self {
            Charset::Japanese => {
                v.extend(LATIN);
                decode(
                    encoding_rs::EUC_JP,
                    (0xa1..=0xa8).chain(0xb0..=0xf4),
                    &[EUC_TRAIL],
                    &mut v,
                );
                v.extend(jis_x_0213(jis_x_0213_table));
            }
            Charset::Gb2312 => decode(
                encoding_rs::GBK,
                (0xa1..=0xa9).chain(0xb0..=0xf7),
                &[EUC_TRAIL],
                &mut v,
            ),
            Charset::Big5 => decode(
                encoding_rs::BIG5,
                0xa1..=0xf9,
                &[0x40..=0x7e, EUC_TRAIL],
                &mut v,
            ),
            Charset::KsX1001 => decode(encoding_rs::EUC_KR, 0xa1..=0xfe, &[EUC_TRAIL], &mut v),
            Charset::Empty => {}
        }
        v.sort_unstable();
        v.dedup();
        v
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn has(set: &[u32], c: char) -> bool {
        set.binary_search(&u32::from(c)).is_ok()
    }

    #[test]
    fn each_set_holds_its_standard_and_not_its_neighbors() {
        let table = "## a comment\n3-2121\tU+3000\t# IDEOGRAPHIC SPACE\n3-2477\tU+304B+309A\t# ka with handakuten\n4-2121\tU+20089\t# plane 2\n3-2D21\t\t# unmapped\n";
        let jp = Charset::Japanese.code_points(table);
        assert!(has(&jp, '漢') && has(&jp, 'あ') && has(&jp, 'é') && has(&jp, 'Ł'));
        assert!(has(&jp, '\u{309a}') && has(&jp, '\u{20089}'));
        let sc = Charset::Gb2312.code_points("");
        assert!(has(&sc, '汉') && !has(&sc, '漢'));
        assert_eq!(
            sc.iter()
                .filter(|&&c| (0x4e00..=0x9fff).contains(&c))
                .count(),
            6763
        );
        let tc = Charset::Big5.code_points("");
        assert!(has(&tc, '漢') && !has(&tc, '汉'));
        let kr = Charset::KsX1001.code_points("");
        assert!(has(&kr, '한') && has(&kr, '漢'));
        assert_eq!(
            kr.iter()
                .filter(|&&c| (0xac00..=0xd7a3).contains(&c))
                .count(),
            2350
        );
        assert_eq!(
            kr.iter()
                .filter(|&&c| (0x4e00..=0x9fff).contains(&c) || (0xf900..=0xfaff).contains(&c))
                .count(),
            4888
        );
        assert_eq!(Charset::Empty.code_points(""), ASCII.collect::<Vec<_>>());
    }
}

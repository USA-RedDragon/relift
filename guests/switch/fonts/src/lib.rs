//! The open fallback for the console's shared fonts.
//!
//! When the player has not imported their console's system fonts, relift
//! builds stand-ins at build time from upstream Noto Sans CJK variable fonts
//! (SIL Open Font License): each font type gets the characters of its
//! region's standard character set ([`Charset`]), subset with the fontations
//! subsetter (skera) and instanced at regular weight into a static TrueType
//! font, small enough that every type fits pl:u's shared memory together.
//! relift ships no font data: the sources are fetched by their pinned hash
//! ([`SOURCES`]) and the fonts made from them on the player's machine.

mod charset;
mod instance;

pub use charset::Charset;
pub use instance::{Error, instance};

/// An upstream file the fallback is made from, fetched at build time and
/// accepted only with its pinned SHA-256.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Source {
    /// The file's name in relift's cache.
    pub name: &'static str,
    /// Where it is fetched from.
    pub url: &'static str,
    /// Its SHA-256, in hex.
    pub sha256: &'static str,
}

/// The release of Noto Sans CJK the fallback is made from.
pub const NOTO_TAG: &str = "Sans2.004";

macro_rules! noto {
    ($file:literal) => {
        concat!(
            "https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/Sans/Variable/TTF/Subset/",
            $file
        )
    };
}

/// Noto Sans CJK JP, the source of the standard font.
pub const NOTO_JP: Source = Source {
    name: "NotoSansJP-VF.ttf",
    url: noto!("NotoSansJP-VF.ttf"),
    sha256: "f4b373b226668ee33a6e54b02823dcd2d1209f17159f777421ae8c2275160369",
};

/// Noto Sans CJK SC, the source of the Chinese Simplified fonts.
pub const NOTO_SC: Source = Source {
    name: "NotoSansSC-VF.ttf",
    url: noto!("NotoSansSC-VF.ttf"),
    sha256: "d68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964",
};

/// Noto Sans CJK TC, the source of the Chinese Traditional font.
pub const NOTO_TC: Source = Source {
    name: "NotoSansTC-VF.ttf",
    url: noto!("NotoSansTC-VF.ttf"),
    sha256: "ac091cc8cd19e848202afc8fe6d3809b4526c8fdbdb4be82da20c4f785949591",
};

/// Noto Sans CJK KR, the source of the Korean font.
pub const NOTO_KR: Source = Source {
    name: "NotoSansKR-VF.ttf",
    url: noto!("NotoSansKR-VF.ttf"),
    sha256: "9e1d729e7e2b36f9ef439da102f8c134c10aabe46f1c843bf0aca5c043b86f76",
};

/// The JIS X 0213:2004 to Unicode table of x0213.org, the source of the
/// standard font's JIS X 0213 characters (encoding_rs knows JIS X 0208 only).
pub const JIS_X_0213: Source = Source {
    name: "jisx0213-2004-std.txt",
    url: "https://x0213.org/codetable/jisx0213-2004-std.txt",
    sha256: "388ae7822cbf0aecff09b8466dd630ea343673d01a3c7df9d61947ad1ad5b2dd",
};

/// Every file the fallback needs.
pub const SOURCES: [Source; 5] = [NOTO_JP, NOTO_SC, NOTO_TC, NOTO_KR, JIS_X_0213];

/// The weight the fallback is instanced at: regular, as the console's fonts are.
pub const WEIGHT: f32 = 400.0;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_source_is_pinned_by_a_sha256() {
        for s in SOURCES {
            assert!(
                s.sha256.len() == 64 && s.sha256.bytes().all(|b| b.is_ascii_hexdigit()),
                "{}",
                s.name
            );
            assert!(s.url.ends_with(s.name), "{}", s.name);
        }
        for s in [NOTO_JP, NOTO_SC, NOTO_TC, NOTO_KR] {
            assert!(s.url.contains(&format!("/noto-cjk/{NOTO_TAG}/")));
        }
    }
}

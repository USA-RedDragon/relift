//! Reading a Switch title's control NACP: its identity and its display
//! version.

use relift_info::Identity;

use crate::identity::{Malformed, text};

/// The languages of the NACP's title entries, in their order.
const NACP_LANGUAGES: [&str; 16] = [
    "AmericanEnglish",
    "BritishEnglish",
    "Japanese",
    "French",
    "German",
    "LatinAmericanSpanish",
    "Spanish",
    "Italian",
    "Dutch",
    "CanadianFrench",
    "Portuguese",
    "Russian",
    "Korean",
    "TraditionalChinese",
    "SimplifiedChinese",
    "BrazilianPortuguese",
];
const NACP_ENTRY: usize = 0x300;
const NACP_NAME: usize = 0x200;
/// Where the display version field starts.
pub const NACP_VERSION: usize = 0x3060;
/// The NACP's fixed size.
pub const NACP_SIZE: usize = 0x3070;

/// The identity of a Switch title from its control NACP: names and
/// publishers per language (empty entries skipped), the display version,
/// and the icon (JPEG bytes) when given.
///
/// # Errors
///
/// The NACP is shorter than its fixed size.
pub fn identity(nacp: &[u8], title_id: u64, icon: Option<&[u8]>) -> Result<Identity, Malformed> {
    if nacp.len() < NACP_SIZE {
        return Err(Malformed(format!(
            "a NACP of {} bytes; at least {NACP_SIZE} are needed",
            nacp.len()
        )));
    }
    let mut id = Identity {
        platform: "switch".into(),
        title_id: Some(format!("{title_id:016x}")),
        ..Identity::default()
    };
    for (i, lang) in NACP_LANGUAGES.iter().enumerate() {
        let e = &nacp[i * NACP_ENTRY..(i + 1) * NACP_ENTRY];
        let name = text(&e[..NACP_NAME]);
        if name.is_empty() {
            continue;
        }
        id.names.insert((*lang).into(), name);
        let publisher = text(&e[NACP_NAME..]);
        if !publisher.is_empty() {
            id.publishers.insert((*lang).into(), publisher);
        }
    }
    let version = text(&nacp[NACP_VERSION..NACP_VERSION + 16]);
    id.version = (!version.is_empty()).then_some(version);
    id.default_language = id.first_language();
    id.icon = icon.and_then(crate::icon::from_image);
    Ok(id)
}

/// The display version field of a NACP, or `None` when the NACP is too short
/// to hold it or the field is empty.
#[must_use]
pub fn display_version(nacp: &[u8]) -> Option<String> {
    let field = nacp.get(NACP_VERSION..NACP_VERSION + 16)?;
    if nacp.len() < NACP_SIZE {
        return None;
    }
    let end = field.iter().position(|&c| c == 0).unwrap_or(field.len());
    let v = String::from_utf8_lossy(&field[..end]).into_owned();
    (!v.is_empty()).then_some(v)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn nacp() -> Vec<u8> {
        let mut n = vec![0u8; 0x4000];
        n[..5].copy_from_slice(b"Synth");
        n[0x200..0x205].copy_from_slice(b"Maker");
        let j = 2 * NACP_ENTRY;
        n[j..j + 9].copy_from_slice("ゲーム".as_bytes());
        n[NACP_VERSION..NACP_VERSION + 5].copy_from_slice(b"1.0.2");
        n
    }

    #[test]
    fn a_nacp_gives_names_publishers_version_and_icon() {
        let jpeg: &[u8] = include_bytes!("../../tests/icon.jpg");
        let id = identity(&nacp(), 0x0100_0000_0000_1234, Some(jpeg)).unwrap();
        assert_eq!(id.platform, "switch");
        assert_eq!(id.title_id.as_deref(), Some("0100000000001234"));
        assert_eq!(id.names["AmericanEnglish"], "Synth");
        assert_eq!(id.names["Japanese"], "ゲーム");
        assert_eq!(id.names.len(), 2);
        assert_eq!(id.publishers["AmericanEnglish"], "Maker");
        assert_eq!(id.version.as_deref(), Some("1.0.2"));
        let icon = id.icon.unwrap();
        assert_eq!(
            (icon.format.as_str(), icon.width, icon.height),
            ("png", 16, 8)
        );
        assert_eq!(id.default_language.as_deref(), Some("AmericanEnglish"));
        assert!(
            identity(&nacp(), 1, Some(b"not a jpeg"))
                .unwrap()
                .icon
                .is_none()
        );
        assert!(identity(&[0; 16], 0, None).is_err());
    }
}

//! The `identity` and `save` sections from what a guest carries: a Switch
//! control NACP, a Game Boy header or a GBA header, and the identity of a
//! patched ROM.

use std::collections::BTreeMap;

use relift_gb_formats::{CgbFlag, Header};
use relift_info::{Identity, Patch, Save};

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
const NACP_VERSION: usize = 0x3060;
const NACP_SIZE: usize = 0x3070;

/// Why a header did not give an identity.
#[derive(Debug, PartialEq, Eq)]
pub struct Malformed(pub String);

impl std::fmt::Display for Malformed {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Malformed {}

fn text(b: &[u8]) -> String {
    let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..end]).trim().to_string()
}

/// Standard base64 with padding.
#[must_use]
pub fn base64(data: &[u8]) -> String {
    const A: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for c in data.chunks(3) {
        let n = (u32::from(c[0]) << 16)
            | (u32::from(*c.get(1).unwrap_or(&0)) << 8)
            | u32::from(*c.get(2).unwrap_or(&0));
        for i in 0..4 {
            if i <= c.len() {
                out.push(char::from(A[((n >> (18 - 6 * i)) & 63) as usize]));
            } else {
                out.push('=');
            }
        }
    }
    out
}

/// The identity of a Switch title from its control NACP: names and
/// publishers per language (empty entries skipped), the display version,
/// and the icon (JPEG bytes) when given.
///
/// # Errors
///
/// The NACP is shorter than its fixed size.
pub fn from_nacp(nacp: &[u8], title_id: u64, icon: Option<&[u8]>) -> Result<Identity, Malformed> {
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
    id.icon = icon.and_then(crate::icon::from_jpeg);
    Ok(id)
}

/// The identity and save of a Game Boy or Game Boy Color cartridge.
///
/// # Errors
///
/// The header does not decode.
pub fn from_gb(rom: &[u8]) -> Result<(Identity, Save), Malformed> {
    let h = Header::parse(rom).map_err(|e| Malformed(format!("a Game Boy header: {e}")))?;
    let platform = if h.cgb == CgbFlag::DmgOnly {
        "gb"
    } else {
        "gbc"
    };
    let mut names = BTreeMap::new();
    names.insert("default".to_string(), h.title.clone());
    let f = h.mapper.features;
    let save = if f.battery && f.ram && h.layout.ram_bytes > 0 {
        Save {
            kind: "raw".into(),
            size: Some(h.layout.ram_bytes as u64),
            location: Some("launcher".into()),
            ..Save::default()
        }
    } else {
        Save {
            kind: "none".into(),
            ..Save::default()
        }
    };
    Ok((
        Identity {
            platform: platform.into(),
            title_id: h.manufacturer.clone(),
            names,
            version: Some(h.version.to_string()),
            ..Identity::default()
        },
        save,
    ))
}

const GBA_HEADER: usize = 0xC0;

const GBA_SAVE_TAGS: [(&[u8], &str, u64); 6] = [
    (b"EEPROM_V", "EEPROM", 0x2000),
    (b"SRAM_V", "SRAM", 0x8000),
    (b"FLASH_V", "Flash 64K", 0x10000),
    (b"FLASH512_V", "Flash 64K", 0x10000),
    (b"FLASH1M_V", "Flash 128K", 0x20000),
    (b"SRAM_F_V", "SRAM", 0x8000),
];

/// The identity and save of a GBA cartridge: the title, game code, maker
/// code and version of the header, and the save library tag the ROM carries
/// (EEPROM defaults to 8 KiB here: the size is only known from the game's
/// accesses).
///
/// # Errors
///
/// The image is shorter than a GBA header.
pub fn from_gba(rom: &[u8]) -> Result<(Identity, Save), Malformed> {
    if rom.len() < GBA_HEADER {
        return Err(Malformed(format!(
            "a GBA image of {} bytes; the header needs {GBA_HEADER}",
            rom.len()
        )));
    }
    let title = text(&rom[0xA0..0xAC]);
    let code = text(&rom[0xAC..0xB0]);
    let mut names = BTreeMap::new();
    names.insert("default".to_string(), title);
    let id = Identity {
        platform: "gba".into(),
        title_id: (!code.is_empty()).then_some(code),
        names,
        version: Some(rom[0xBC].to_string()),
        ..Identity::default()
    };
    let mut found: Option<(usize, &str, u64)> = None;
    for (tag, chip, size) in GBA_SAVE_TAGS {
        if let Some(at) = rom.windows(tag.len()).position(|w| w == tag)
            && found.is_none_or(|(a, _, _)| at < a)
        {
            found = Some((at, chip, size));
        }
    }
    let save = match found {
        Some((_, chip, size)) => Save {
            kind: "raw".into(),
            size: Some(size),
            chip: Some(chip.into()),
            location: Some("launcher".into()),
            ..Save::default()
        },
        None => Save {
            kind: "none".into(),
            ..Save::default()
        },
    };
    Ok((id, save))
}

/// The identity of a Nintendo DS cartridge: the title, game code and
/// version of its header. Saves are not read yet, so the save is `none`.
///
/// # Errors
///
/// The image is shorter than the header's first 0x20 bytes.
pub fn from_nds(rom: &[u8]) -> Result<(Identity, Save), Malformed> {
    if rom.len() < 0x20 {
        return Err(Malformed(format!(
            "a DS image of {} bytes; the header needs 0x20",
            rom.len()
        )));
    }
    let code = text(&rom[0x0C..0x10]);
    let mut names = BTreeMap::new();
    names.insert("default".to_string(), text(&rom[..0x0C]));
    let id = Identity {
        platform: "nds".into(),
        title_id: (!code.is_empty()).then_some(code),
        names,
        version: Some(rom[0x1E].to_string()),
        ..Identity::default()
    };
    let save = Save {
        kind: "none".into(),
        ..Save::default()
    };
    Ok((id, save))
}

/// The identity of a patched ROM: its own `patched` identity plus the
/// original as `base`, the patch, and `base_mismatch` when the base the
/// patch declared (`declared_base`, a hash) is not the base's `actual_base`
/// hash.
#[must_use]
pub fn patched(
    base: Identity,
    mut patched: Identity,
    patch: Patch,
    declared_base: Option<&str>,
    actual_base: &str,
) -> Identity {
    patched.base_mismatch = declared_base.is_some_and(|d| !d.eq_ignore_ascii_case(actual_base));
    patched.base = Some(Box::new(base));
    patched.patch = Some(patch);
    patched
}

#[cfg(test)]
mod tests {
    use super::*;
    use relift_gb_formats::synth::{Spec, rom};

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
        let jpeg: &[u8] = include_bytes!("../tests/icon.jpg");
        let id = from_nacp(&nacp(), 0x0100_0000_0000_1234, Some(jpeg)).unwrap();
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
            from_nacp(&nacp(), 1, Some(b"not a jpeg"))
                .unwrap()
                .icon
                .is_none()
        );
        assert!(from_nacp(&[0; 16], 0, None).is_err());
    }

    #[test]
    fn base64_pads() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
    }

    #[test]
    fn a_gb_header_gives_platform_title_and_battery_save() {
        let (id, save) = from_gb(&rom(&Spec {
            title: "SYNTHGB".into(),
            cart_type: 0x03,
            rom_size: 1,
            ram_size: 2,
            ..Spec::default()
        }))
        .unwrap();
        assert_eq!(
            (id.platform.as_str(), id.names["default"].as_str()),
            ("gb", "SYNTHGB")
        );
        assert_eq!((save.kind.as_str(), save.size), ("raw", Some(0x2000)));
        let (id, save) = from_gb(&rom(&Spec {
            cgb: 0x80,
            ..Spec::default()
        }))
        .unwrap();
        assert_eq!(id.platform, "gbc");
        assert_eq!(save.kind, "none");
        assert!(from_gb(&[0; 8]).is_err());
    }

    fn gba(tag: &[u8]) -> Vec<u8> {
        let mut r = vec![0u8; 0x400];
        r[0xA0..0xA7].copy_from_slice(b"SYNTHGA");
        r[0xAC..0xB0].copy_from_slice(b"ABCE");
        r[0xBC] = 3;
        r[0x200..0x200 + tag.len()].copy_from_slice(tag);
        r
    }

    #[test]
    fn a_gba_header_gives_code_version_and_save_chip() {
        let (id, save) = from_gba(&gba(b"EEPROM_V124")).unwrap();
        assert_eq!(id.platform, "gba");
        assert_eq!(id.title_id.as_deref(), Some("ABCE"));
        assert_eq!(id.names["default"], "SYNTHGA");
        assert_eq!(id.version.as_deref(), Some("3"));
        assert_eq!(save.chip.as_deref(), Some("EEPROM"));
        let (_, save) = from_gba(&gba(b"FLASH1M_V103")).unwrap();
        assert_eq!(
            (save.chip.as_deref(), save.size),
            (Some("Flash 128K"), Some(0x20000))
        );
        let (_, save) = from_gba(&gba(b"")).unwrap();
        assert_eq!(save.kind, "none");
        assert!(from_gba(&[0; 0x40]).is_err());
    }

    fn patch() -> Patch {
        Patch {
            name: "hack".into(),
            kind: "ips".into(),
            hash: "ab".into(),
        }
    }

    #[test]
    fn a_patched_rom_has_its_own_identity_plus_base_and_a_mismatch_flag() {
        let base = from_gba(&gba(b"")).unwrap().0;
        let mut own = base.clone();
        own.names.insert("default".into(), "SYNTHGA HACK".into());
        let ok = patched(base.clone(), own.clone(), patch(), Some("AA"), "aa");
        assert!(!ok.base_mismatch);
        assert_eq!(ok.names["default"], "SYNTHGA HACK");
        assert_eq!(ok.base.as_deref(), Some(&base));
        assert_eq!(ok.patch, Some(patch()));
        let bad = patched(base.clone(), own.clone(), patch(), Some("bb"), "aa");
        assert!(bad.base_mismatch);
        let undeclared = patched(base, own, patch(), None, "aa");
        assert!(!undeclared.base_mismatch);
    }
}

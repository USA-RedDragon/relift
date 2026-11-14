//! ROM hacks: a handheld ROM patched with an IPS, UPS or BPS patch is a
//! game of its own, with its own identity and a `base` link to the
//! original.
//!
//! The patch is applied before analysis. When its declared base does not
//! match the ROM (the CRC-32 a UPS or BPS carries, or the SHA-256 the game
//! repo names for an IPS), relift applies it anyway with a warning and
//! records `base_mismatch`.

use relift_info::{Identity, Patch};

use crate::import::BoxError;

/// A patched ROM.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RomHack {
    /// The patched ROM.
    pub data: Vec<u8>,
    /// Its identity, with `base`, `patch` and `base_mismatch` set.
    pub identity: Identity,
    /// What to warn about.
    pub warnings: Vec<String>,
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// The identity a Game Boy or Game Boy Color ROM's header gives.
///
/// # Errors
///
/// The header does not decode.
pub fn gb_identity(rom: &[u8]) -> Result<Identity, BoxError> {
    let h = relift_gb_formats::Header::parse(rom).map_err(|e| format!("ROM header: {e}"))?;
    let platform = if h.cgb_byte & 0x80 != 0 { "gbc" } else { "gb" };
    let mut id = Identity {
        platform: platform.into(),
        title_id: h.manufacturer.clone(),
        version: Some(h.version.to_string()),
        ..Identity::default()
    };
    id.names.insert("default".into(), h.title);
    Ok(id)
}

/// Applies the patch `patch` (named `name`) to `base`, with `identity`
/// giving a ROM's identity. `expected_base` is the SHA-256 (hex) of the ROM
/// the patch is for, when the game repo names one.
///
/// # Errors
///
/// The patch is not IPS, UPS or BPS, is damaged, or its output does not
/// match the checksum it declares; or the patched ROM has no identity.
pub fn apply(
    base: &[u8],
    name: &str,
    patch: &[u8],
    expected_base: Option<&str>,
    identity: &dyn Fn(&[u8]) -> Result<Identity, BoxError>,
) -> Result<RomHack, BoxError> {
    let kind = relift_patch::Kind::detect(patch)
        .ok_or_else(|| format!("{name}: not an IPS, UPS or BPS patch"))?;
    if kind == relift_patch::Kind::Ips32 {
        return Err(format!("{name}: an IPS32 patch is for exefs modules, not a ROM").into());
    }
    let p = relift_patch::apply(base, patch).map_err(|e| format!("{name}: {e}"))?;
    let base_sha = hex(&relift_switch_formats::sha256(base));
    let mut warnings = Vec::new();
    let mut mismatch = p.base_mismatch;
    if p.base_mismatch {
        warnings.push(format!(
            "{name} is for a ROM with CRC-32 {:08x}, this one has {:08x}; applied anyway",
            p.declared_base.unwrap_or(0),
            relift_patch::crc32(base)
        ));
    }
    if let Some(want) = expected_base
        && !want.eq_ignore_ascii_case(&base_sha)
    {
        mismatch = true;
        warnings.push(format!(
            "{name} is for a ROM with SHA-256 {want}, this one has {base_sha}; applied anyway"
        ));
    }
    let mut base_id = identity(base)?;
    base_id.modules.clear();
    let mut id = identity(&p.data)?;
    id.patch = Some(Patch {
        name: name.into(),
        kind: kind.name().into(),
        hash: hex(&relift_switch_formats::sha256(patch)),
    });
    id.base = Some(Box::new(base_id));
    id.base_mismatch = mismatch;
    Ok(RomHack {
        data: p.data,
        identity: id,
        warnings,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rom(title: &[u8]) -> Vec<u8> {
        let mut r = vec![0u8; 0x8000];
        r[0x134..0x134 + title.len()].copy_from_slice(title);
        r[0x147] = 0;
        r[0x148] = 0;
        r
    }

    fn ips(at: u32, bytes: &[u8]) -> Vec<u8> {
        let mut p = b"PATCH".to_vec();
        p.extend_from_slice(&at.to_be_bytes()[1..]);
        p.extend_from_slice(&(bytes.len() as u16).to_be_bytes());
        p.extend_from_slice(bytes);
        p.extend_from_slice(b"EOF");
        p
    }

    #[test]
    fn a_rom_hack_has_its_own_identity_and_a_base_link() {
        let base = rom(b"BASEGAME");
        let p = ips(0x134, b"HACKGAME");
        let sha = hex(&relift_switch_formats::sha256(&base));
        let h = apply(&base, "hack.ips", &p, Some(&sha), &gb_identity).expect("applies");
        assert_eq!(h.identity.names["default"], "HACKGAME");
        let b = h.identity.base.as_ref().expect("base");
        assert_eq!(b.names["default"], "BASEGAME");
        let patch = h.identity.patch.as_ref().expect("patch");
        assert_eq!(
            (patch.name.as_str(), patch.kind.as_str()),
            ("hack.ips", "ips")
        );
        assert!(!h.identity.base_mismatch);
        assert!(h.warnings.is_empty());
    }

    #[test]
    fn a_base_mismatch_applies_with_a_warning() {
        let base = rom(b"OTHER");
        let p = ips(0x134, b"HACK");
        let h = apply(&base, "hack.ips", &p, Some(&"00".repeat(32)), &gb_identity)
            .expect("applies anyway");
        assert!(h.identity.base_mismatch);
        assert_eq!(h.warnings.len(), 1);
        assert!(
            h.warnings[0].contains("applied anyway"),
            "{}",
            h.warnings[0]
        );
        assert_eq!(h.identity.names["default"], "HACKR");
    }

    #[test]
    fn an_ips32_is_not_a_rom_patch() {
        let mut p = b"IPS32".to_vec();
        p.extend_from_slice(b"EEOF");
        assert!(apply(&rom(b"A"), "x.ips", &p, None, &gb_identity).is_err());
    }
}

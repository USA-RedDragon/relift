//! The display version a Switch program reads from the application control
//! property (`nn::oe::GetDisplayVersion`): where `relift build` finds it and
//! the check that it fits the control data's 16-byte field.
//!
//! The sources, first that has one wins:
//!
//! 1. the NACP the import kept (`control.nacp`, field at 0x3060);
//! 2. the version the import kept from the package's meta block
//!    (`display_version.txt`, written when the package has the version but no
//!    NACP);
//! 3. `display_version` in the `[game]` table of `relift.toml`, for dumps whose
//!    control data holds neither.
//!
//! With none of them the program is built without a version and the runtime
//! answers the call with an empty field.

use std::path::Path;

use crate::identity::{NACP_SIZE, NACP_VERSION};
use crate::import::BoxError;

/// The file of the import that holds a display version read from a package's
/// meta block.
pub const IMPORT_FILE: &str = "display_version.txt";

/// The longest display version: the field holds 16 bytes, the last a NUL.
pub const MAX_LEN: usize = 15;

/// Checks `v` is a display version the field can hold: 1 to [`MAX_LEN`]
/// printable ASCII characters.
///
/// # Errors
///
/// `v` is empty, too long, or has a character outside printable ASCII.
pub fn check(v: &str) -> Result<(), String> {
    if v.is_empty() || v.len() > MAX_LEN {
        return Err(format!(
            "a display version of {} bytes; it takes 1 to {MAX_LEN}",
            v.len()
        ));
    }
    if !v.bytes().all(|b| (0x20..0x7f).contains(&b)) {
        return Err("a display version with a character outside printable ASCII".into());
    }
    Ok(())
}

/// The display version field of a NACP, or `None` when the NACP is too short
/// to hold it or the field is empty.
#[must_use]
pub fn from_nacp(nacp: &[u8]) -> Option<String> {
    let field = nacp.get(NACP_VERSION..NACP_VERSION + 16)?;
    if nacp.len() < NACP_SIZE {
        return None;
    }
    let end = field.iter().position(|&c| c == 0).unwrap_or(field.len());
    let v = String::from_utf8_lossy(&field[..end]).into_owned();
    (!v.is_empty()).then_some(v)
}

/// The display version of the program the import and `relift.toml` describe,
/// or `None` when no source has one.
///
/// # Errors
///
/// A source holds a version the field cannot, or a file cannot be read.
pub fn resolve(import: &Path, game: &toml::Table) -> Result<Option<String>, BoxError> {
    let read = |name: &str| match std::fs::read(import.join(name)) {
        Ok(b) => Ok(Some(b)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(format!("{}: {e}", import.join(name).display())),
    };
    if let Some(v) = read("control.nacp")?.as_deref().and_then(from_nacp) {
        check(&v).map_err(|e| format!("{}: {e}", import.join("control.nacp").display()))?;
        return Ok(Some(v));
    }
    if let Some(b) = read(IMPORT_FILE)? {
        let v = String::from_utf8_lossy(&b).trim().to_string();
        check(&v).map_err(|e| format!("{}: {e}", import.join(IMPORT_FILE).display()))?;
        return Ok(Some(v));
    }
    let Some(v) = game.get("game").and_then(|g| g.get("display_version")) else {
        return Ok(None);
    };
    let v = v
        .as_str()
        .ok_or("relift.toml: game.display_version is not a string")?;
    check(v).map_err(|e| format!("relift.toml: game.display_version: {e}"))?;
    Ok(Some(v.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn nacp(version: &[u8]) -> Vec<u8> {
        let mut n = vec![0u8; 0x4000];
        n[NACP_VERSION..NACP_VERSION + version.len()].copy_from_slice(version);
        n
    }

    fn table(s: &str) -> toml::Table {
        s.parse().unwrap()
    }

    fn dir(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("relift-dv-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn the_nacp_field_is_read_to_its_terminator() {
        assert_eq!(from_nacp(&nacp(b"4.5.6")).as_deref(), Some("4.5.6"));
        assert_eq!(from_nacp(&nacp(b"")), None);
        assert_eq!(from_nacp(&[0; 16]), None);
    }

    #[test]
    fn the_nacp_beats_the_package_version_and_the_toml() {
        let d = dir("nacp");
        relift_cache::write_if_changed(&d.join("control.nacp"), &nacp(b"4.5.6")).unwrap();
        relift_cache::write_if_changed(&d.join(IMPORT_FILE), b"7.8.9\n").unwrap();
        let g = table("[game]\ndisplay_version = \"1.1.1\"\n");
        assert_eq!(resolve(&d, &g).unwrap().as_deref(), Some("4.5.6"));
        std::fs::remove_file(d.join("control.nacp")).unwrap();
        assert_eq!(resolve(&d, &g).unwrap().as_deref(), Some("7.8.9"));
        std::fs::remove_file(d.join(IMPORT_FILE)).unwrap();
        assert_eq!(resolve(&d, &g).unwrap().as_deref(), Some("1.1.1"));
        assert_eq!(resolve(&d, &table("[game]\n")).unwrap(), None);
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn a_version_the_field_cannot_hold_is_refused() {
        let d = dir("bad");
        let long = table("[game]\ndisplay_version = \"1.2.3.4.5.6.7.8.9\"\n");
        assert!(resolve(&d, &long).is_err());
        let wide = table("[game]\ndisplay_version = \"1.0é\"\n");
        assert!(resolve(&d, &wide).is_err());
        let num = table("[game]\ndisplay_version = 1\n");
        assert!(resolve(&d, &num).is_err());
        let _ = std::fs::remove_dir_all(&d);
    }
}

//! Code mods: build-time patches to the imported code (design 006).
//!
//! [`user`] reads the mods `relift build --mods` names (Atmosphere and
//! Ryujinx layouts: exefs replacements and `exefs_patches` IPS32 and
//! `.pchtxt` patches matched by build id) and applies them in order before
//! analysis. [`bundled`] reads the mods `game.toml` declares, which are
//! built in and switched at start. [`conflicts`] refuses the ambiguous
//! cases. [`rom`] patches a handheld ROM into a ROM hack with its own
//! identity.

pub mod bundled;
pub mod cache;
pub mod conflicts;
pub mod rom;
pub mod stage;
pub mod user;
pub mod variants;

use std::collections::BTreeMap;
use std::ops::Range;
use std::path::Path;

use relift_switch_formats::pchtxt;

use crate::import::BoxError;

/// The IPS32 offset of image byte 0: patch offsets count from the start of
/// the NSO header.
pub const NSO_HEADER: u64 = 0x100;

/// One patch file for an exefs module.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExefsPatch {
    /// The file, relative to the mod, for messages.
    pub file: String,
    /// The build id the file is for (its stem, or the `.pchtxt`'s `@nsobid`).
    pub build_id: String,
    /// The writes, at IPS32 offsets.
    pub records: Vec<pchtxt::Record>,
    /// `ips32`, `ips` or `pchtxt`.
    pub kind: &'static str,
}

impl ExefsPatch {
    /// Reads one patch file: an IPS or IPS32 named for its build id, or a
    /// `.pchtxt`.
    ///
    /// # Errors
    ///
    /// The file does not parse.
    pub fn read(path: &Path, rel: &str) -> Result<ExefsPatch, BoxError> {
        let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let stem = path
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        if path.extension().is_some_and(|x| x == "pchtxt") {
            let text = String::from_utf8_lossy(&bytes);
            let p = pchtxt::parse(&text).map_err(|e| format!("{rel}: {e}"))?;
            return Ok(ExefsPatch {
                file: rel.into(),
                build_id: p.build_id,
                records: p.records,
                kind: "pchtxt",
            });
        }
        if stem.is_empty() || !stem.chars().all(|c| c.is_ascii_hexdigit()) {
            return Err(format!(
                "{rel}: an IPS patch is named for the build id of its module (<build id>.ips)"
            )
            .into());
        }
        let (records, kind) = ips_records(&bytes).map_err(|e| format!("{rel}: {e}"))?;
        Ok(ExefsPatch {
            file: rel.into(),
            build_id: stem.to_ascii_lowercase(),
            records,
            kind,
        })
    }

    /// Whether the patch is for the module with build id `id`.
    pub fn matches(&self, id: &[u8; 32]) -> bool {
        pchtxt::build_id_matches(&self.build_id, id)
    }

    /// Writes the patch into `image`; returns the image ranges written.
    ///
    /// # Errors
    ///
    /// A write lies before the image or past its end.
    pub fn apply(&self, image: &mut [u8]) -> Result<Vec<Range<u64>>, String> {
        let mut out = Vec::new();
        for r in &self.records {
            let len = r.bytes.len() as u64;
            let start = r.offset.checked_sub(NSO_HEADER).ok_or_else(|| {
                format!(
                    "{}: a write at {:#x} lies in the NSO header",
                    self.file, r.offset
                )
            })?;
            let end = start + len;
            let (s, e) = (
                usize::try_from(start).unwrap_or(usize::MAX),
                usize::try_from(end).unwrap_or(usize::MAX),
            );
            let dst = image.get_mut(s..e).ok_or_else(|| {
                format!(
                    "{}: a write at image offset {start:#x} runs past the module's end",
                    self.file
                )
            })?;
            dst.copy_from_slice(&r.bytes);
            out.push(start..end);
        }
        Ok(relift_patch::merge(out))
    }
}

/// The records of an IPS or IPS32 patch, at the offsets the patch gives.
fn ips_records(patch: &[u8]) -> Result<(Vec<pchtxt::Record>, &'static str), BoxError> {
    let (wide, kind) = match relift_patch::Kind::detect(patch) {
        Some(relift_patch::Kind::Ips32) => (true, "ips32"),
        Some(relift_patch::Kind::Ips) => (false, "ips"),
        _ => return Err("not an IPS or IPS32 patch".into()),
    };
    let off_bytes = if wide { 4 } else { 3 };
    let eof: &[u8] = if wide { b"EEOF" } else { b"EOF" };
    let mut at = 5usize;
    let mut out = Vec::new();
    let be = |b: &[u8]| b.iter().fold(0u64, |v, &x| (v << 8) | u64::from(x));
    loop {
        if patch.get(at..at + eof.len()) == Some(eof) {
            break;
        }
        let head = patch
            .get(at..at + off_bytes + 2)
            .ok_or(relift_patch::Error::Truncated)?;
        let offset = be(&head[..off_bytes]);
        let size = usize::try_from(be(&head[off_bytes..])).unwrap_or(0);
        at += off_bytes + 2;
        let bytes = if size == 0 {
            let rle = patch
                .get(at..at + 3)
                .ok_or(relift_patch::Error::Truncated)?;
            at += 3;
            vec![rle[2]; usize::try_from(be(&rle[..2])).unwrap_or(0)]
        } else {
            let b = patch
                .get(at..at + size)
                .ok_or(relift_patch::Error::Truncated)?;
            at += size;
            b.to_vec()
        };
        out.push(pchtxt::Record { offset, bytes });
    }
    Ok((out, kind))
}

/// SHA-256 over the files of a mod: each relative path, a NUL, its bytes.
pub fn hash_files(files: &BTreeMap<String, Vec<u8>>) -> String {
    let mut all = Vec::new();
    for (k, v) in files {
        all.extend_from_slice(k.as_bytes());
        all.push(0);
        all.extend_from_slice(&(v.len() as u64).to_le_bytes());
        all.extend_from_slice(v);
    }
    relift_switch_formats::sha256(&all)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// Every regular file under `dir`, by path relative to it with `/`.
pub(crate) fn walk(dir: &Path) -> Result<Vec<String>, BoxError> {
    let mut out = Vec::new();
    let mut todo = vec![String::new()];
    while let Some(rel) = todo.pop() {
        let d = dir.join(&rel);
        let mut entries: Vec<_> = std::fs::read_dir(&d)
            .map_err(|e| format!("{}: {e}", d.display()))?
            .filter_map(Result::ok)
            .collect();
        entries.sort_by_key(std::fs::DirEntry::file_name);
        for e in entries {
            let name = e.file_name().to_string_lossy().into_owned();
            let r = if rel.is_empty() {
                name
            } else {
                format!("{rel}/{name}")
            };
            let t = e.file_type()?;
            if t.is_dir() {
                todo.push(r);
            } else if t.is_file() {
                out.push(r);
            }
        }
    }
    out.sort();
    Ok(out)
}

/// The self-description's `mods` section and the bundled mods' bool
/// settings, as the build recorded them.
pub fn info_section(
    user: &[user::UserMod],
    bundled: &[bundled::Bundled],
) -> (relift_info::Mods, Vec<relift_info::Setting>) {
    let mods = relift_info::Mods {
        code: user
            .iter()
            .map(|m| relift_info::CodeMod {
                name: m.name.clone(),
                hash: m.hash.clone(),
            })
            .collect(),
        bundled: bundled.iter().map(bundled::Bundled::key).collect(),
        ..relift_info::Mods::default()
    };
    let settings = bundled
        .iter()
        .map(|b| relift_info::Setting {
            key: b.key(),
            label: b.label.clone(),
            description: b.description.clone(),
            apply: relift_info::Apply::Start,
            default: serde_json::Value::Bool(b.default),
            kind: relift_info::SettingKind::Bool,
        })
        .collect();
    (mods, settings)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ips32_records_keep_their_offsets_and_runs() {
        let mut p = b"IPS32".to_vec();
        p.extend_from_slice(&[0, 0, 1, 4, 0, 2, 0xaa, 0xbb]);
        p.extend_from_slice(&[0, 0, 1, 8, 0, 0, 0, 3, 0x11]);
        p.extend_from_slice(b"EEOF");
        let (r, kind) = ips_records(&p).expect("parses");
        assert_eq!(kind, "ips32");
        assert_eq!(r[0].offset, 0x104);
        assert_eq!(r[1].bytes, [0x11; 3]);
        let patch = ExefsPatch {
            file: "x.ips".into(),
            build_id: "00".into(),
            records: r,
            kind,
        };
        let mut image = vec![0u8; 16];
        assert_eq!(patch.apply(&mut image).expect("fits"), vec![4..6, 8..11]);
        assert_eq!(&image[4..11], &[0xaa, 0xbb, 0, 0, 0x11, 0x11, 0x11]);
        let mut small = vec![0u8; 6];
        assert!(patch.apply(&mut small).is_err());
    }

    #[test]
    fn a_write_into_the_header_is_refused() {
        let patch = ExefsPatch {
            file: "x.ips".into(),
            build_id: "00".into(),
            records: vec![pchtxt::Record {
                offset: 0x10,
                bytes: vec![1],
            }],
            kind: "ips32",
        };
        assert!(patch.apply(&mut [0u8; 4]).is_err());
    }
}

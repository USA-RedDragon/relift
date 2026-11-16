//! The game data manifest of a mobile app: every file of game data the app
//! is given, with its size and SHA-256, so the app can tell at start whether
//! the data it was given is complete (`runtime/core/include/relift/datacheck.h`),
//! and `relift push` can check what it copied.
//!
//! The data is one plain RomFS image per romfs (crate::gamedata): `romfs.img`
//! for the game, `dlc/<title id>/<index>/romfs.img` for each DLC. One line per
//! image: `<size> <sha256 hex> <name>`, sorted by name. Hashes are kept in
//! `<work>/data-digests.tsv` by the source's size and modification time, so a
//! rebuild reads the data again only after it changed.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::Path;
#[cfg(test)]
use std::path::PathBuf;

use crate::import::BoxError;

/// The file the app carries the manifest in.
pub const FILE: &str = "data-manifest.txt";

/// One file the manifest lists.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    /// `romfs.img` or `dlc/<title id>/<index>/romfs.img`.
    pub name: String,
    pub size: u64,
    pub sha256: String,
}

/// The entries for the romfs and DLC of `import`, each as its plain image,
/// hashing only what changed since `work`'s cache.
///
/// # Errors
///
/// A romfs cannot be read, or the cache cannot be written.
pub fn entries(import: &Path, work: &Path) -> Result<Vec<Entry>, BoxError> {
    let cache = work.join("data-digests.tsv");
    let mut out = Vec::new();
    for u in crate::gamedata::units(import)? {
        let (size, sha256) = crate::gamedata::image_digest(&u, &cache)?;
        out.push(Entry {
            name: u.image_name(),
            size,
            sha256,
        });
    }
    out.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(out)
}

/// The manifest text of `entries`.
#[must_use]
pub fn text(entries: &[Entry]) -> String {
    let mut s = String::from("# relift game data manifest: size sha256 path\n");
    for e in entries {
        let _ = writeln!(s, "{} {} {}", e.size, e.sha256, e.name);
    }
    s
}

/// Parses manifest text.
///
/// # Errors
///
/// A line that is not `<size> <64 hex digits> <path>`.
pub fn parse(text: &str) -> Result<Vec<Entry>, BoxError> {
    let mut out = Vec::new();
    for (i, l) in text.lines().enumerate() {
        if l.is_empty() || l.starts_with('#') {
            continue;
        }
        let mut f = l.splitn(3, ' ');
        let (Some(size), Some(sha256), Some(name)) = (f.next(), f.next(), f.next()) else {
            return Err(format!("data manifest line {}: not `size sha256 path`", i + 1).into());
        };
        let size = size
            .parse()
            .map_err(|_| format!("data manifest line {}: bad size", i + 1))?;
        if sha256.len() != 64 || !sha256.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(format!("data manifest line {}: bad SHA-256", i + 1).into());
        }
        out.push(Entry {
            name: name.to_string(),
            size,
            sha256: sha256.to_string(),
        });
    }
    Ok(out)
}

/// The entries whose size differs from `have` (name to size) or that
/// `have` lacks, in manifest order.
#[must_use]
pub fn mismatches<'a>(
    entries: &'a [Entry],
    have: &BTreeMap<String, u64>,
) -> Vec<(&'a Entry, Option<u64>)> {
    entries
        .iter()
        .filter_map(|e| {
            let got = have.get(&e.name).copied();
            (got != Some(e.size)).then_some((e, got))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use sha2::Digest;

    fn tmp(tag: &str) -> PathBuf {
        let d = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("target")
            .join(format!("relift-datamanifest-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn lists_one_image_per_romfs_with_size_and_hash_and_round_trips() {
        let d = tmp("list");
        let import = d.join("import");
        std::fs::create_dir_all(import.join("romfs/a")).unwrap();
        std::fs::create_dir_all(import.join("dlc/0100/0/romfs")).unwrap();
        relift_cache::write_if_changed(&import.join("romfs/a/x.bin"), b"abc").unwrap();
        relift_cache::write_if_changed(&import.join("romfs/top"), b"").unwrap();
        relift_cache::write_if_changed(&import.join("dlc/0100/0/romfs/d"), b"dd").unwrap();
        let e = entries(&import, &d.join("work")).unwrap();
        let names: Vec<&str> = e.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(names, ["dlc/0100/0/romfs.img", "romfs.img"]);
        let out = d.join("out");
        crate::gamedata::write_images(&import, &out, false).unwrap();
        let img = std::fs::read(out.join("romfs.img")).unwrap();
        assert_eq!(e[1].size, img.len() as u64);
        let want: String = sha2::Sha256::digest(&img)
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect();
        assert_eq!(e[1].sha256, want);
        assert_eq!(parse(&text(&e)).unwrap(), e);
        let again = entries(&import, &d.join("work")).unwrap();
        assert_eq!(again, e);
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn mismatches_name_missing_and_short_files() {
        let e = parse(&format!(
            "3 {h} romfs/a\n2 {h} romfs/b\n1 {h} romfs/c\n",
            h = "0".repeat(64)
        ))
        .unwrap();
        let have: BTreeMap<String, u64> =
            [("romfs/a".to_string(), 3), ("romfs/b".to_string(), 1)].into();
        let m = mismatches(&e, &have);
        assert_eq!(m.len(), 2);
        assert_eq!((m[0].0.name.as_str(), m[0].1), ("romfs/b", Some(1)));
        assert_eq!((m[1].0.name.as_str(), m[1].1), ("romfs/c", None));
    }

    #[test]
    fn a_malformed_line_is_an_error() {
        assert!(parse("3 abc romfs/a\n").is_err());
        assert!(parse("x 0000 romfs/a\n").is_err());
    }
}

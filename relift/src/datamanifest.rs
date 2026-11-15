//! The game data manifest of a mobile app: every file of the imported romfs
//! and DLC with its size and SHA-256, so the app can tell at start whether
//! the data it was given is complete (`runtime/core/include/relift/datacheck.h`),
//! and `relift push` can check what it copied.
//!
//! One line per file: `<size> <sha256 hex> <root>/<path>`, root `romfs` or
//! `dlc`, sorted by path. Hashes are kept in `<work>/data-hashes.tsv` by
//! path, size and modification time, so a rebuild hashes only what changed.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use sha2::{Digest, Sha256};

use crate::import::BoxError;

/// The file the app carries the manifest in.
pub const FILE: &str = "data-manifest.txt";

/// One file the manifest lists.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    /// `romfs/<path>` or `dlc/<path>`, with `/` separators.
    pub name: String,
    pub size: u64,
    pub sha256: String,
}

fn files(root: &Path, prefix: &str, out: &mut Vec<(String, PathBuf)>) -> std::io::Result<()> {
    let mut stack = vec![root.to_path_buf()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d)? {
            let e = e?;
            let t = e.file_type()?;
            let p = e.path();
            if t.is_dir() {
                stack.push(p);
            } else if t.is_file() {
                let rel = p.strip_prefix(root).unwrap_or(&p);
                let rel: Vec<String> = rel
                    .components()
                    .map(|c| c.as_os_str().to_string_lossy().into_owned())
                    .collect();
                out.push((format!("{prefix}/{}", rel.join("/")), p));
            }
        }
    }
    Ok(())
}

fn hash_file(p: &Path) -> std::io::Result<String> {
    let mut f = std::fs::File::open(p)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    let mut s = String::with_capacity(64);
    for b in h.finalize() {
        let _ = write!(s, "{b:02x}");
    }
    Ok(s)
}

fn stamp(p: &Path) -> std::io::Result<(u64, u128)> {
    let m = std::fs::metadata(p)?;
    let t = m
        .modified()?
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos());
    Ok((m.len(), t))
}

/// The entries for the romfs and DLC folders of `import` (each optional),
/// hashing only files whose size or time changed since `work`'s cache.
///
/// # Errors
///
/// A folder or file cannot be read, or the cache cannot be written.
pub fn entries(import: &Path, work: &Path) -> Result<Vec<Entry>, BoxError> {
    let mut list = Vec::new();
    for root in ["romfs", "dlc"] {
        let d = import.join(root);
        if d.is_dir() {
            files(&d, root, &mut list)?;
        }
    }
    list.sort();
    let cache_path = work.join("data-hashes.tsv");
    let mut cache: BTreeMap<String, (u64, u128, String)> = BTreeMap::new();
    if let Ok(text) = std::fs::read_to_string(&cache_path) {
        for l in text.lines() {
            let f: Vec<&str> = l.splitn(4, '\t').collect();
            if let [size, time, hash, name] = f[..]
                && let (Ok(size), Ok(time)) = (size.parse(), time.parse())
            {
                cache.insert(name.to_string(), (size, time, hash.to_string()));
            }
        }
    }
    let mut out = Vec::with_capacity(list.len());
    let mut kept = String::new();
    for (name, path) in list {
        let (size, time) = stamp(&path)?;
        let sha256 = match cache.get(&name) {
            Some((s, t, h)) if *s == size && *t == time => h.clone(),
            _ => hash_file(&path)?,
        };
        let _ = writeln!(kept, "{size}\t{time}\t{sha256}\t{name}");
        out.push(Entry { name, size, sha256 });
    }
    std::fs::create_dir_all(work)?;
    relift_cache::write_if_changed(&cache_path, kept.as_bytes())?;
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
    fn lists_romfs_and_dlc_with_sizes_and_hashes_and_round_trips() {
        let d = tmp("list");
        let import = d.join("import");
        std::fs::create_dir_all(import.join("romfs/a")).unwrap();
        std::fs::create_dir_all(import.join("dlc/0100/0/romfs")).unwrap();
        relift_cache::write_if_changed(&import.join("romfs/a/x.bin"), b"abc").unwrap();
        relift_cache::write_if_changed(&import.join("romfs/top"), b"").unwrap();
        relift_cache::write_if_changed(&import.join("dlc/0100/0/romfs/d"), b"dd").unwrap();
        let e = entries(&import, &d.join("work")).unwrap();
        let names: Vec<&str> = e.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(names, ["dlc/0100/0/romfs/d", "romfs/a/x.bin", "romfs/top"]);
        assert_eq!(e[1].size, 3);
        assert_eq!(
            e[1].sha256,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
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

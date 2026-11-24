//! The game data of a Switch import as relift's commands use it: the
//! game's romfs and each DLC's, each held as a RomFS map (`romfs.map`, read
//! in place in the `.nup`), a plain image (`romfs.img`) or, from an older
//! import, an extracted `romfs/` tree (see
//! `relift_switch_formats::romfs_image`).
//!
//! Desktop runs read the import as it is. What leaves the machine (an app
//! bundle, a phone) always gets plain images: `romfs.img` and
//! `dlc/<title id>/<index>/romfs.img`, one file per romfs whatever the
//! number of files in it, which every host opens with positioned reads and
//! which a map could not be (it names files on this machine).

use std::collections::HashMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use relift_switch_formats::Sha256Writer;
use relift_switch_formats::romfs::Romfs;
use relift_switch_formats::romfs_image::{self, Found, IMAGE_NAME};
use relift_switch_formats::storage::{Shared, Storage};
use sha2::{Digest, Sha256};

use crate::import::BoxError;

/// One romfs of an import: the game's, or one DLC's.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Unit {
    /// Where it goes beside the others: `""` for the game's romfs,
    /// `dlc/<title id>/<index>` for a DLC.
    pub rel: String,
    /// The form it is in.
    pub found: Found,
}

impl Unit {
    /// The plain image's name beside the others: `romfs.img` or
    /// `dlc/<title id>/<index>/romfs.img`.
    #[must_use]
    pub fn image_name(&self) -> String {
        if self.rel.is_empty() {
            IMAGE_NAME.to_string()
        } else {
            format!("{}/{IMAGE_NAME}", self.rel)
        }
    }

    /// Open it as one image.
    ///
    /// # Errors
    ///
    /// It cannot be read, is malformed, or a file a map reads changed.
    pub fn open(&self) -> Result<Shared, BoxError> {
        romfs_image::open(self.found.path())
            .map_err(|e| format!("{}: {e}", self.found.path().display()).into())
    }
}

/// The game's romfs in `import`, if it has one.
#[must_use]
pub fn romfs(import: &Path) -> Option<Found> {
    romfs_image::find(import)
}

/// The DLC folder of `import`, if it has one.
#[must_use]
pub fn dlc(import: &Path) -> Option<PathBuf> {
    Some(import.join("dlc")).filter(|d| d.is_dir())
}

/// The folder of the other programs' romfs of `import`, when the dump held
/// several programs (`programs/<title id>`).
#[must_use]
pub fn programs(import: &Path) -> Option<PathBuf> {
    Some(import.join("programs")).filter(|d| d.is_dir())
}

/// Every romfs of `import`: the game's first, then each DLC's sorted by
/// title id and index.
///
/// # Errors
///
/// The DLC folder cannot be read.
pub fn units(import: &Path) -> std::io::Result<Vec<Unit>> {
    let mut out = Vec::new();
    if let Some(found) = romfs(import) {
        out.push(Unit {
            rel: String::new(),
            found,
        });
    }
    let Some(d) = dlc(import) else {
        return Ok(out);
    };
    let mut dlcs = Vec::new();
    for id in std::fs::read_dir(&d)? {
        let id = id?;
        if !id.file_type()?.is_dir() {
            continue;
        }
        for idx in std::fs::read_dir(id.path())? {
            let idx = idx?;
            if let Some(found) = romfs_image::find(&idx.path()) {
                dlcs.push(Unit {
                    rel: format!(
                        "dlc/{}/{}",
                        id.file_name().to_string_lossy(),
                        idx.file_name().to_string_lossy()
                    ),
                    found,
                });
            }
        }
    }
    dlcs.sort_by(|a, b| a.rel.cmp(&b.rel));
    out.extend(dlcs);
    Ok(out)
}

/// The first host file a romfs map of `import` (the game's or a DLC's) reads
/// from that does not exist, as the map and the file. An import made from a
/// dump keeps its romfs data in the file the map names, so a missing one means
/// the program would run without its data.
///
/// # Errors
///
/// A map cannot be read or is malformed.
pub fn missing_source(import: &Path) -> Result<Option<(PathBuf, PathBuf)>, BoxError> {
    for u in units(import)? {
        let Found::Map(map) = &u.found else {
            continue;
        };
        let parsed = romfs_image::RomfsMap::parse(&std::fs::read(map)?)
            .map_err(|e| format!("{}: {e}", map.display()))?;
        let dir = map.parent().unwrap_or(Path::new("."));
        for s in &parsed.sources {
            let file = dir.join(&s.path);
            if !file.exists() {
                return Ok(Some((map.clone(), file)));
            }
        }
    }
    Ok(None)
}

/// Point a run of the program at `import`'s data: `RELIFT_ROMFS` at the
/// romfs as it is, `RELIFT_DLC` at the DLC folder, `RELIFT_PROGRAMS` at the
/// other programs' romfs, `RELIFT_FONTS` at the shared fonts decoded from
/// the player's system data.
pub fn set_env(cmd: &mut std::process::Command, import: &Path) {
    if let Some(f) = crate::switch::fonts::dir(import) {
        cmd.env("RELIFT_FONTS", f);
    }
    if let Some(r) = romfs(import) {
        cmd.env("RELIFT_ROMFS", r.path());
    }
    if let Some(d) = dlc(import) {
        cmd.env("RELIFT_DLC", d);
    }
    if let Some(p) = programs(import) {
        cmd.env("RELIFT_PROGRAMS", p);
    }
}

/// Write every romfs of `import` as plain images under `dest`
/// ([`Unit::image_name`]), streaming and only where they changed (a plain
/// image in the import is cloned on APFS when `clone` is set and copied
/// otherwise). Returns whether there is a game romfs and whether there is
/// DLC.
///
/// # Errors
///
/// A romfs cannot be read or an image cannot be written.
pub fn write_images(import: &Path, dest: &Path, clone: bool) -> Result<(bool, bool), BoxError> {
    let units = units(import)?;
    for u in &units {
        let to = dest.join(u.image_name());
        if let Some(d) = to.parent() {
            std::fs::create_dir_all(d)?;
        }
        if clone && matches!(u.found, Found::Image(_)) {
            let _ = std::fs::remove_file(&to);
            let ok = relift_proc::command("cp")
                .arg("-c")
                .arg(u.found.path())
                .arg(&to)
                .status()
                .is_ok_and(|s| s.success());
            if ok {
                continue;
            }
        }
        romfs_image::flatten(u.found.path(), &to, &mut |_| {})
            .map_err(|e| format!("{}: {e}", u.found.path().display()))?;
    }
    Ok((
        units.iter().any(|u| u.rel.is_empty()),
        units.iter().any(|u| !u.rel.is_empty()),
    ))
}

/// What a cache line keys on: the size and modification time of the file
/// (or the folder of a tree) a romfs is read from.
fn stamp(p: &Path) -> std::io::Result<(u64, u128)> {
    let m = std::fs::metadata(p)?;
    let t = m
        .modified()?
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos());
    Ok((m.len(), t))
}

/// Results kept by source path and stamp, one `<kind>\t<size>\t<time>\t<value>\t<path>` line each.
struct Cache {
    path: PathBuf,
    lines: HashMap<(String, String), (u64, u128, String)>,
}

impl Cache {
    fn load(path: &Path) -> Cache {
        let mut lines = HashMap::new();
        if let Ok(text) = std::fs::read_to_string(path) {
            for l in text.lines() {
                let f: Vec<&str> = l.splitn(5, '\t').collect();
                if let [kind, size, time, value, p] = f[..]
                    && let (Ok(size), Ok(time)) = (size.parse(), time.parse())
                {
                    lines.insert(
                        (kind.to_string(), p.to_string()),
                        (size, time, value.to_string()),
                    );
                }
            }
        }
        Cache {
            path: path.to_path_buf(),
            lines,
        }
    }

    fn get(&self, kind: &str, src: &Found) -> Option<String> {
        if matches!(src, Found::Tree(_)) {
            return None;
        }
        let (size, time) = stamp(src.path()).ok()?;
        let key = (kind.to_string(), src.path().display().to_string());
        self.lines
            .get(&key)
            .filter(|(s, t, _)| *s == size && *t == time)
            .map(|(_, _, v)| v.clone())
    }

    fn put(&mut self, kind: &str, src: &Found, value: &str) -> std::io::Result<()> {
        if matches!(src, Found::Tree(_)) {
            return Ok(());
        }
        let (size, time) = stamp(src.path())?;
        self.lines.insert(
            (kind.to_string(), src.path().display().to_string()),
            (size, time, value.to_string()),
        );
        let mut keys: Vec<_> = self.lines.keys().cloned().collect();
        keys.sort();
        let mut text = String::new();
        for k in keys {
            let (s, t, v) = &self.lines[&k];
            let _ = writeln!(text, "{}\t{s}\t{t}\t{v}\t{}", k.0, k.1);
        }
        if let Some(d) = self.path.parent() {
            std::fs::create_dir_all(d)?;
        }
        relift_cache::write_if_changed(&self.path, text.as_bytes())?;
        Ok(())
    }
}

fn hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        let _ = write!(s, "{x:02x}");
    }
    s
}

/// SHA-256 of the whole image `s`, read in large chunks.
fn image_sha256(s: &dyn Storage) -> Result<String, BoxError> {
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 4 << 20];
    let mut off = 0u64;
    while off < s.size() {
        let n = usize::try_from((buf.len() as u64).min(s.size() - off))?;
        s.read_at(off, &mut buf[..n])?;
        h.update(&buf[..n]);
        off += n as u64;
    }
    Ok(hex(&h.finalize()))
}

/// The size and SHA-256 of `unit`'s plain image, kept in `cache` (a file)
/// by the source's stamp, so a rebuild reads the data only after it
/// changed.
///
/// # Errors
///
/// The romfs cannot be read, or the cache cannot be written.
pub fn image_digest(unit: &Unit, cache: &Path) -> Result<(u64, String), BoxError> {
    let mut c = Cache::load(cache);
    if let Some(v) = c.get("image", &unit.found)
        && let Some((size, sha)) = v.split_once(' ')
        && let Ok(size) = size.parse()
    {
        return Ok((size, sha.to_string()));
    }
    let s = unit.open()?;
    let sha = image_sha256(&*s)?;
    c.put("image", &unit.found, &format!("{} {sha}", s.size()))?;
    Ok((s.size(), sha))
}

/// The tree hash of a romfs that is not an extracted tree: SHA-256 over
/// every file's path, a zero byte and the file's SHA-256, sorted by path, as
/// `relift info` defines it for a tree. Kept in `cache` by the source's
/// stamp.
///
/// # Errors
///
/// The romfs cannot be read, or the cache cannot be written.
pub fn tree_hash(found: &Found, cache: &Path) -> Result<String, BoxError> {
    let mut c = Cache::load(cache);
    if let Some(v) = c.get("tree", found) {
        return Ok(v);
    }
    let unit = Unit {
        rel: String::new(),
        found: found.clone(),
    };
    let r = Romfs::open(unit.open()?)?;
    let mut files: Vec<_> = r.files().iter().collect();
    files.sort_by(|a, b| a.path.cmp(&b.path));
    let next = std::sync::atomic::AtomicUsize::new(0);
    let hashes = std::sync::Mutex::new(vec![[0u8; 32]; files.len()]);
    let failed = std::sync::Mutex::new(None);
    relift_proc::workers::run(relift_proc::workers::jobs().min(files.len().max(1)), || {
        loop {
            let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let Some(f) = files.get(i) else { break };
            let mut h = Sha256Writer::default();
            match r.copy_file(f, &mut h) {
                Ok(()) => {
                    hashes.lock().expect("lock")[i] = h.finish();
                }
                Err(e) => {
                    *failed.lock().expect("lock") = Some(e);
                    break;
                }
            }
        }
    });
    if let Some(e) = failed.into_inner().expect("lock") {
        return Err(e.into());
    }
    let hashes = hashes.into_inner().expect("lock");
    let mut all = Sha256::new();
    for (f, h) in files.iter().zip(&hashes) {
        all.update(f.path.as_bytes());
        all.update([0]);
        all.update(h);
    }
    let v = hex(&all.finalize());
    c.put("tree", found, &v)?;
    Ok(v)
}

/// The first `n` bytes of every file of `romfs` (fewer for a shorter
/// file), with its path, for scans by content.
///
/// # Errors
///
/// The romfs cannot be read.
pub fn heads(unit: &Unit, n: usize) -> Result<Vec<(String, Vec<u8>)>, BoxError> {
    let s = unit.open()?;
    let r = Romfs::open(s.clone())?;
    let mut out = Vec::with_capacity(r.files().len());
    for f in r.files() {
        let len = usize::try_from(f.size.min(n as u64))?;
        let mut b = vec![0u8; len];
        s.read_at(f.offset, &mut b)?;
        out.push((f.path.clone(), b));
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("relift-gamedata-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).expect("mkdir");
        d
    }

    fn put(p: &Path, b: &[u8]) {
        std::fs::create_dir_all(p.parent().expect("parent")).expect("mkdir");
        relift_cache::write_if_changed(p, b).expect("write");
    }

    #[test]
    fn a_map_whose_source_file_is_gone_is_named() {
        let d = scratch("missing-source");
        let import = d.join("import");
        let map = romfs_image::RomfsMap {
            size: 0,
            meta: Vec::new(),
            sources: vec![romfs_image::MapSource {
                path: "data.blob".into(),
                size: 4,
                check_offset: 0,
                check: [0; 32],
            }],
            pieces: Vec::new(),
        };
        put(&import.join("romfs.map"), &map.to_bytes());
        assert_eq!(
            missing_source(&import).expect("check"),
            Some((import.join("romfs.map"), import.join("data.blob")))
        );
        put(&import.join("data.blob"), b"data");
        assert_eq!(missing_source(&import).expect("check"), None);
        std::fs::remove_dir_all(&d).expect("clean");
    }

    #[test]
    fn units_find_every_form_and_images_equal_the_trees() {
        let d = scratch("units");
        let import = d.join("import");
        put(&import.join("romfs/a/x.bin"), b"abc");
        put(&import.join("romfs/top"), b"");
        put(&import.join("dlc/0100000000001001/0/romfs/d"), b"dd");
        let tree_units = units(&import).expect("units");
        assert_eq!(
            tree_units
                .iter()
                .map(|u| u.image_name())
                .collect::<Vec<_>>(),
            ["romfs.img", "dlc/0100000000001001/0/romfs.img"]
        );
        let out = d.join("out");
        assert_eq!(
            write_images(&import, &out, false).expect("images"),
            (true, true)
        );
        let flat = import.with_file_name("flat");
        put(
            &flat.join("romfs.img"),
            &std::fs::read(out.join("romfs.img")).expect("read"),
        );
        let img_unit = &units(&flat).expect("units")[0];
        assert!(matches!(img_unit.found, Found::Image(_)));
        let cache = d.join("cache.tsv");
        let a = image_digest(&tree_units[0], &cache).expect("digest");
        let b = image_digest(img_unit, &cache).expect("digest");
        assert_eq!(a, b);
        assert_eq!(image_digest(img_unit, &cache).expect("cached"), b);
        assert!(
            std::fs::read_to_string(&cache)
                .expect("cache")
                .contains("image\t")
        );
        assert_eq!(
            tree_hash(&img_unit.found, &cache).expect("hash"),
            crate::info::tree_hash(&import.join("romfs")).expect("tree hash")
        );
        let heads = heads(img_unit, 2).expect("heads");
        assert_eq!(
            heads,
            [
                ("a/x.bin".to_string(), b"ab".to_vec()),
                ("top".to_string(), Vec::new())
            ]
        );
        let mut cmd = relift_proc::command("true");
        set_env(&mut cmd, &flat);
        let envs: Vec<_> = cmd.get_envs().collect();
        assert_eq!(envs.len(), 1);
        assert_eq!(envs[0].1, Some(flat.join("romfs.img").as_os_str()));
        std::fs::create_dir_all(flat.join("programs/0400000000000000/romfs")).expect("mkdir");
        let mut cmd = relift_proc::command("true");
        set_env(&mut cmd, &flat);
        let programs: Vec<_> = cmd
            .get_envs()
            .filter(|(k, _)| *k == "RELIFT_PROGRAMS")
            .collect();
        assert_eq!(
            programs,
            [(
                "RELIFT_PROGRAMS".as_ref(),
                Some(flat.join("programs").as_os_str())
            )]
        );
        let _ = std::fs::remove_dir_all(&d);
    }
}

//! Where the files to scan come from: a directory tree, or the packages of
//! a NUP over any [`Storage`].

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use relift_switch_formats::nup::{Codec, Nup, NupEntry};
use relift_switch_formats::storage::Shared;

use crate::error::{Error, Result, malformed};
use crate::sha256_hex;

/// Files at most this large are hashed whole for their identity in a
/// directory; larger ones by size and modification time.
const HASH_LIMIT: u64 = 32 << 20;
/// Files at most this large are read whole to sniff their content.
pub const SNIFF_WHOLE: u64 = 1 << 20;
/// How many bytes at the start of a file the content sniff looks at.
pub const SNIFF_HEAD: usize = 0x20;

/// One file a source offers.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SourceFile {
    /// The package index (0 for a directory).
    pub pkg: usize,
    /// The `/`-separated path from the package root.
    pub path: String,
    /// The size in bytes (uncompressed for a NUP entry).
    pub size: u64,
}

impl SourceFile {
    /// A label for reports: the path, with the package for a NUP.
    pub fn label(&self, packages: bool) -> String {
        if packages {
            format!("pkg{}:{}", self.pkg, self.path)
        } else {
            self.path.clone()
        }
    }
}

/// A set of files that can be listed and read.
pub trait Source: Send + Sync {
    /// Every file, in a stable order.
    ///
    /// # Errors
    ///
    /// The listing cannot be read.
    fn files(&self) -> Result<Vec<SourceFile>>;

    /// Whether files carry a package index worth showing.
    fn has_packages(&self) -> bool;

    /// A string that changes when the content of `f` changes.
    ///
    /// # Errors
    ///
    /// The file cannot be read.
    fn identity(&self, f: &SourceFile) -> Result<String>;

    /// The whole file.
    ///
    /// # Errors
    ///
    /// The file cannot be read or decoded.
    fn read(&self, f: &SourceFile) -> Result<Vec<u8>>;

    /// The first [`SNIFF_HEAD`] bytes, when that is cheap to give: `None`
    /// when it would take decoding a large file.
    ///
    /// # Errors
    ///
    /// The file cannot be read.
    fn head(&self, f: &SourceFile) -> Result<Option<Vec<u8>>>;

    /// The byte ranges `(offset, length)` of the file, each as given.
    ///
    /// # Errors
    ///
    /// The file cannot be read, or a range is past its end.
    fn read_ranges(&self, f: &SourceFile, ranges: &[(u64, u64)]) -> Result<Vec<Vec<u8>>>;

    /// Whether [`Source::read_ranges`] reads only the ranges (the file is
    /// stored as it is), not the whole file.
    fn ranges_are_cheap(&self, f: &SourceFile) -> bool;
}

fn slice_ranges(whole: &[u8], ranges: &[(u64, u64)]) -> Result<Vec<Vec<u8>>> {
    ranges
        .iter()
        .map(|&(o, n)| {
            let (Ok(o), Ok(n)) = (usize::try_from(o), usize::try_from(n)) else {
                return malformed("a range does not fit in memory");
            };
            match o.checked_add(n).and_then(|e| whole.get(o..e)) {
                Some(s) => Ok(s.to_vec()),
                None => malformed("a range runs past the end of the file"),
            }
        })
        .collect()
}

/// A directory tree: an extracted `exefs`/`romfs`, or a folder holding both.
pub struct DirSource {
    root: PathBuf,
}

impl DirSource {
    /// A source over the files under `root`.
    pub fn new(root: &Path) -> DirSource {
        DirSource {
            root: root.to_path_buf(),
        }
    }

    fn path(&self, f: &SourceFile) -> PathBuf {
        self.root.join(&f.path)
    }
}

fn walk(dir: &Path, rel: &str, out: &mut Vec<SourceFile>) -> Result<()> {
    let mut names: Vec<_> = std::fs::read_dir(dir)?.collect::<std::io::Result<Vec<_>>>()?;
    names.sort_by_key(std::fs::DirEntry::file_name);
    for e in names {
        let name = e.file_name().to_string_lossy().into_owned();
        let path = if rel.is_empty() {
            name
        } else {
            format!("{rel}/{name}")
        };
        let ty = e.file_type()?;
        if ty.is_dir() {
            walk(&e.path(), &path, out)?;
        } else if ty.is_file() {
            out.push(SourceFile {
                pkg: 0,
                path,
                size: e.metadata()?.len(),
            });
        }
    }
    Ok(())
}

fn disk_identity(p: &Path, size: u64) -> Result<String> {
    if size <= HASH_LIMIT {
        return Ok(sha256_hex(&std::fs::read(p)?));
    }
    let mtime = std::fs::metadata(p)?
        .modified()?
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos());
    Ok(format!("stat:{size}:{mtime}"))
}

fn disk_ranges(p: &Path, ranges: &[(u64, u64)]) -> Result<Vec<Vec<u8>>> {
    let mut file = File::open(p)?;
    let mut out = Vec::new();
    for &(o, n) in ranges {
        let Ok(n) = usize::try_from(n) else {
            return malformed("a range does not fit in memory");
        };
        file.seek(SeekFrom::Start(o))?;
        let mut b = vec![0u8; n];
        file.read_exact(&mut b)?;
        out.push(b);
    }
    Ok(out)
}

impl Source for DirSource {
    fn files(&self) -> Result<Vec<SourceFile>> {
        let mut out = Vec::new();
        walk(&self.root, "", &mut out)?;
        Ok(out)
    }

    fn has_packages(&self) -> bool {
        false
    }

    fn identity(&self, f: &SourceFile) -> Result<String> {
        disk_identity(&self.path(f), f.size)
    }

    fn read(&self, f: &SourceFile) -> Result<Vec<u8>> {
        Ok(std::fs::read(self.path(f))?)
    }

    fn head(&self, f: &SourceFile) -> Result<Option<Vec<u8>>> {
        let mut v = Vec::new();
        File::open(self.path(f))?
            .take(SNIFF_HEAD as u64)
            .read_to_end(&mut v)?;
        Ok(Some(v))
    }

    fn read_ranges(&self, f: &SourceFile, ranges: &[(u64, u64)]) -> Result<Vec<Vec<u8>>> {
        disk_ranges(&self.path(f), ranges)
    }

    fn ranges_are_cheap(&self, _: &SourceFile) -> bool {
        true
    }
}

/// The files of every package of a NUP.
pub struct NupSource {
    nup: Nup,
    storage: Shared,
    entries: Vec<(SourceFile, NupEntry)>,
}

impl NupSource {
    /// Open the NUP in `storage` and list its files.
    ///
    /// # Errors
    ///
    /// The storage is not a valid NUP.
    pub fn new(storage: Shared) -> Result<NupSource> {
        let nup = Nup::open(storage.clone())?;
        let mut entries = Vec::new();
        for (i, p) in nup.packages().iter().enumerate() {
            for (path, e) in &p.files {
                entries.push((
                    SourceFile {
                        pkg: i,
                        path: path.clone(),
                        size: e.size,
                    },
                    e.clone(),
                ));
            }
        }
        entries.sort_by(|a, b| (a.0.pkg, &a.0.path).cmp(&(b.0.pkg, &b.0.path)));
        Ok(NupSource {
            nup,
            storage,
            entries,
        })
    }

    fn entry(&self, f: &SourceFile) -> Result<&NupEntry> {
        match self
            .entries
            .binary_search_by(|(s, _)| (s.pkg, &s.path).cmp(&(f.pkg, &f.path)))
        {
            Ok(i) => Ok(&self.entries[i].1),
            Err(_) => malformed(format!("no file {} in package {}", f.path, f.pkg)),
        }
    }
}

impl Source for NupSource {
    fn files(&self) -> Result<Vec<SourceFile>> {
        Ok(self.entries.iter().map(|(s, _)| s.clone()).collect())
    }

    fn has_packages(&self) -> bool {
        true
    }

    fn identity(&self, f: &SourceFile) -> Result<String> {
        Ok(relift_switch_formats::hex(&self.entry(f)?.sha256))
    }

    fn read(&self, f: &SourceFile) -> Result<Vec<u8>> {
        Ok(self.nup.read(self.entry(f)?)?)
    }

    fn head(&self, f: &SourceFile) -> Result<Option<Vec<u8>>> {
        let e = self.entry(f)?;
        if e.codec == Codec::Raw {
            let n = e.size.min(SNIFF_HEAD as u64);
            let mut b = vec![0u8; usize::try_from(n).unwrap_or(0)];
            self.storage.read_at(e.extent_offset, &mut b)?;
            return Ok(Some(b));
        }
        if e.size <= SNIFF_WHOLE {
            let mut b = self.nup.read(e)?;
            b.truncate(SNIFF_HEAD);
            return Ok(Some(b));
        }
        Ok(None)
    }

    fn read_ranges(&self, f: &SourceFile, ranges: &[(u64, u64)]) -> Result<Vec<Vec<u8>>> {
        let e = self.entry(f)?;
        if e.codec != Codec::Raw {
            return slice_ranges(&self.nup.read(e)?, ranges);
        }
        let mut out = Vec::new();
        for &(o, n) in ranges {
            if o.checked_add(n).is_none_or(|end| end > e.size) {
                return Err(Error::Format(
                    "a range runs past the end of the file".into(),
                ));
            }
            let mut b = vec![0u8; usize::try_from(n).unwrap_or(0)];
            self.storage.read_at(e.extent_offset + o, &mut b)?;
            out.push(b);
        }
        Ok(out)
    }

    fn ranges_are_cheap(&self, f: &SourceFile) -> bool {
        self.entry(f).is_ok_and(|e| e.codec == Codec::Raw)
    }
}

/// Where one file of an [`ImportSource`] is read from.
enum Place {
    /// A file of the import folder.
    Disk(PathBuf),
    /// `offset..offset + size` of a romfs image (`unit` of the source).
    Image(usize, u64),
}

/// An import folder as a build sees it: the files of the folder itself
/// (`exefs/`, `modules/`), the game's romfs as `romfs/<path>`, each DLC's
/// as `dlc/<title id>/<index>/romfs/<path>` and each other program's
/// below `programs/<title id>/`, whether a romfs is kept as a map, a plain
/// image or an extracted tree.
pub struct ImportSource {
    files: Vec<(SourceFile, Place)>,
    units: Vec<(Shared, String)>,
}

fn skip_name(name: &str) -> bool {
    matches!(
        name,
        "romfs" | "dlc" | "programs" | "romfs.map" | "romfs.img" | "romfs.blob"
    )
}

fn walk_top(dir: &Path, rel: &str, top: bool, out: &mut Vec<(SourceFile, Place)>) -> Result<()> {
    let mut names: Vec<_> = std::fs::read_dir(dir)?.collect::<std::io::Result<Vec<_>>>()?;
    names.sort_by_key(std::fs::DirEntry::file_name);
    for e in names {
        let name = e.file_name().to_string_lossy().into_owned();
        if top && skip_name(&name) {
            continue;
        }
        let path = format!("{rel}{name}");
        let ty = e.file_type()?;
        if ty.is_dir() {
            walk_top(&e.path(), &format!("{path}/"), false, out)?;
        } else if ty.is_file() {
            out.push((
                SourceFile {
                    pkg: 0,
                    path,
                    size: e.metadata()?.len(),
                },
                Place::Disk(e.path()),
            ));
        }
    }
    Ok(())
}

impl ImportSource {
    /// A source over the import folder `root`.
    ///
    /// # Errors
    ///
    /// The folder or a romfs in it cannot be read.
    pub fn new(root: &Path) -> Result<ImportSource> {
        let mut s = ImportSource {
            files: Vec::new(),
            units: Vec::new(),
        };
        s.add(root, "")?;
        s.files.sort_by(|a, b| a.0.path.cmp(&b.0.path));
        Ok(s)
    }

    fn add(&mut self, dir: &Path, rel: &str) -> Result<()> {
        walk_top(dir, rel, true, &mut self.files)?;
        self.add_romfs(dir, rel)?;
        let dlc = dir.join("dlc");
        if dlc.is_dir() {
            let mut ids = sorted_dirs(&dlc)?;
            ids.sort();
            for id in ids {
                for idx in sorted_dirs(&id)? {
                    let sub = format!("{rel}dlc/{}/{}/", file_name(&id), file_name(&idx));
                    self.add_romfs(&idx, &sub)?;
                }
            }
        }
        let programs = dir.join("programs");
        if programs.is_dir() {
            for p in sorted_dirs(&programs)? {
                self.add(&p, &format!("{rel}programs/{}/", file_name(&p)))?;
            }
        }
        Ok(())
    }

    fn add_romfs(&mut self, dir: &Path, rel: &str) -> Result<()> {
        let Some(found) = relift_switch_formats::romfs_image::find(dir) else {
            return Ok(());
        };
        let storage = relift_switch_formats::romfs_image::open(found.path())?;
        let romfs = relift_switch_formats::romfs::Romfs::open(storage.clone())?;
        let id = match &found {
            relift_switch_formats::romfs_image::Found::Map(p) => sha256_hex(&std::fs::read(p)?),
            f => {
                let m = std::fs::metadata(f.path())?;
                let t = m
                    .modified()?
                    .duration_since(std::time::UNIX_EPOCH)
                    .map_or(0, |d| d.as_nanos());
                format!("stat:{}:{t}", m.len())
            }
        };
        let unit = self.units.len();
        self.units.push((storage, id));
        for f in romfs.files() {
            self.files.push((
                SourceFile {
                    pkg: 0,
                    path: format!("{rel}romfs/{}", f.path),
                    size: f.size,
                },
                Place::Image(unit, f.offset),
            ));
        }
        Ok(())
    }

    fn place(&self, f: &SourceFile) -> Result<&Place> {
        match self
            .files
            .binary_search_by(|(s, _)| s.path.as_str().cmp(&f.path))
        {
            Ok(i) => Ok(&self.files[i].1),
            Err(_) => malformed(format!("no file {} in the import", f.path)),
        }
    }
}

fn file_name(p: &Path) -> String {
    p.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
}

fn sorted_dirs(dir: &Path) -> Result<Vec<PathBuf>> {
    let mut v = Vec::new();
    for e in std::fs::read_dir(dir)? {
        let e = e?;
        if e.file_type()?.is_dir() {
            v.push(e.path());
        }
    }
    v.sort();
    Ok(v)
}

impl Source for ImportSource {
    fn files(&self) -> Result<Vec<SourceFile>> {
        Ok(self.files.iter().map(|(s, _)| s.clone()).collect())
    }

    fn has_packages(&self) -> bool {
        false
    }

    fn identity(&self, f: &SourceFile) -> Result<String> {
        match self.place(f)? {
            Place::Disk(p) => disk_identity(p, f.size),
            Place::Image(u, off) => Ok(format!("{}:{off}:{}", self.units[*u].1, f.size)),
        }
    }

    fn read(&self, f: &SourceFile) -> Result<Vec<u8>> {
        match self.place(f)? {
            Place::Disk(p) => Ok(std::fs::read(p)?),
            Place::Image(u, off) => Ok(relift_switch_formats::storage::read_vec(
                &*self.units[*u].0,
                *off,
                f.size,
            )?),
        }
    }

    fn head(&self, f: &SourceFile) -> Result<Option<Vec<u8>>> {
        let n = f.size.min(SNIFF_HEAD as u64);
        Ok(Some(self.read_ranges(f, &[(0, n)])?.remove(0)))
    }

    fn read_ranges(&self, f: &SourceFile, ranges: &[(u64, u64)]) -> Result<Vec<Vec<u8>>> {
        match self.place(f)? {
            Place::Disk(p) => disk_ranges(p, ranges),
            Place::Image(u, off) => ranges
                .iter()
                .map(|&(o, n)| {
                    if o.checked_add(n).is_none_or(|e| e > f.size) {
                        return malformed("a range runs past the end of the file");
                    }
                    Ok(relift_switch_formats::storage::read_vec(
                        &*self.units[*u].0,
                        off + o,
                        n,
                    )?)
                })
                .collect(),
        }
    }

    fn ranges_are_cheap(&self, _: &SourceFile) -> bool {
        true
    }
}

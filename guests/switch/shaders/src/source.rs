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
        let p = self.path(f);
        if f.size <= HASH_LIMIT {
            return Ok(sha256_hex(&std::fs::read(p)?));
        }
        let mtime = std::fs::metadata(&p)?
            .modified()?
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.as_nanos());
        Ok(format!("stat:{}:{mtime}", f.size))
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
        let mut file = File::open(self.path(f))?;
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

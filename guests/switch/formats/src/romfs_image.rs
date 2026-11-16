//! Whole RomFS images without an extracted tree: the byte-exact image of a
//! file list, served from where the files already are (an extracted tree, a
//! `.nup`, a plain image file), and the RomFS map that records such an image
//! on disk in one small file.
//!
//! The layout is the one the runtime's synthesizer builds from an extracted
//! tree (`runtime/guests/switch/romfs_image.c`): directories in breadth-first
//! order, the entries of each directory sorted by name, file data 16-byte
//! aligned in file order and the data area 0x200-byte aligned. So a tree, the
//! image built here from its files and the image a map describes are the
//! same bytes.
//!
//! A RomFS map (`romfs.map`) is a little-endian file:
//!
//! ```text
//! 0x00  "RLRFMAP1"
//! 0x08  u32 source count, u32 0
//! 0x10  u64 image size
//! 0x18  u64 metadata size (the image's bytes before its data area)
//! 0x20  u64 piece count
//! 0x28  u64 sources offset, u64 pieces offset, u64 metadata offset
//! source:  u64 size, u64 check offset, [32] check bytes, u32 path length,
//!          u32 0, then the UTF-8 path padded to 8 bytes; a relative path is
//!          relative to the map's folder
//! piece:   u64 image offset, u64 size, u32 source, u32 0, u64 source offset
//! ```
//!
//! Pieces are sorted and do not overlap; image bytes between them read as
//! zeros. A source is checked when it is opened: its size, and the 32 bytes
//! at the check offset (a `.nup`'s table SHA-256, a blob's id).
//!
//! A blob (`romfs.blob`) holds the decompressed bytes of the zstd files of a
//! `.nup`, which the runtime cannot read in place: `"RLRFBLOB"`, a 32-byte
//! id (SHA-256 over the files' SHA-256 values in image order), zero padding
//! to 0x40, then the files at 16-byte aligned offsets.

use std::collections::HashMap;
use std::fs::File;
use std::io::Write;
use std::path::{Component, Path, PathBuf};
use std::sync::Arc;

use crate::bytes::{array, sha256, u32_at, u64_at};
use crate::error::{Error, Result, malformed};
use crate::nup::{Codec, Nup, NupEntry};
use crate::storage::{FileStorage, Shared, Storage, check_range, read_vec};

const NONE: u32 = u32::MAX;
const HEADER_SIZE: u64 = 0x50;
const DATA_ALIGN: u64 = 0x200;
const FILE_ALIGN: u64 = 0x10;
const DIR_REC: usize = 0x18;
const FILE_REC: usize = 0x20;

/// The magic of a RomFS map.
pub const MAP_MAGIC: [u8; 8] = *b"RLRFMAP1";
/// The magic of a blob.
pub const BLOB_MAGIC: [u8; 8] = *b"RLRFBLOB";
/// Where a blob's data starts.
pub const BLOB_HEADER: u64 = 0x40;
const MAP_HEADER: usize = 0x40;
const SOURCE_REC: usize = 0x40;
const PIECE_REC: usize = 0x20;

/// The image a file list makes: its metadata and where each file's data is.
#[derive(Clone, Debug)]
pub struct Layout {
    /// The image's bytes before the data area: header, hash tables and
    /// metadata, zero padded.
    pub meta: Vec<u8>,
    /// For each input file, the absolute offset of its data in the image.
    pub offsets: Vec<u64>,
    /// The input file indices in image order.
    pub order: Vec<usize>,
    /// The image size.
    pub size: u64,
}

fn align_up(v: u64, a: u64) -> u64 {
    v.div_ceil(a) * a
}

fn hash_count(n: usize) -> u32 {
    let mut n = u32::try_from(n).unwrap_or(u32::MAX);
    if n < 3 {
        return 3;
    }
    if n < 19 {
        return n | 1;
    }
    while [2, 3, 5, 7, 11, 13, 17].iter().any(|p| n % p == 0) {
        n += 1;
    }
    n
}

fn name_hash(parent: u32, name: &[u8], count: u32) -> u32 {
    let mut h = parent ^ 123_456_789;
    for &b in name {
        h = h.rotate_right(5) ^ u32::from(b);
    }
    h % count
}

fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

fn get32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

struct Node {
    name: String,
    dirs: Vec<usize>,
    files: Vec<usize>,
}

fn check_part(p: &str) -> Result<()> {
    if p.is_empty() || p == "." || p == ".." || p.contains(['\\', '\0']) {
        return malformed(format!("romfs: unsafe name {p:?}"));
    }
    Ok(())
}

/// Lay out the image of `files` (`/`-separated path, size) and `dirs`
/// (directories that exist even when empty, `/`-separated).
///
/// # Errors
///
/// A path is unsafe, a name is both a file and a directory or appears twice,
/// or the tables outgrow the format's 32-bit offsets.
pub fn layout(files: &[(String, u64)], dirs: &[String]) -> Result<Layout> {
    let mut nodes = vec![Node {
        name: String::new(),
        dirs: Vec::new(),
        files: Vec::new(),
    }];
    let mut by_path: HashMap<String, usize> = HashMap::new();
    by_path.insert(String::new(), 0);
    let mut ensure = |nodes: &mut Vec<Node>, path: &str| -> Result<usize> {
        let mut cur = 0usize;
        let mut at = String::new();
        for part in path.split('/') {
            check_part(part)?;
            if !at.is_empty() {
                at.push('/');
            }
            at.push_str(part);
            cur = match by_path.get(&at) {
                Some(&n) => n,
                None => {
                    nodes.push(Node {
                        name: part.to_string(),
                        dirs: Vec::new(),
                        files: Vec::new(),
                    });
                    let n = nodes.len() - 1;
                    nodes[cur].dirs.push(n);
                    by_path.insert(at.clone(), n);
                    n
                }
            };
        }
        Ok(cur)
    };
    for d in dirs {
        if !d.is_empty() {
            ensure(&mut nodes, d)?;
        }
    }
    let mut file_names = Vec::with_capacity(files.len());
    for (i, (path, _)) in files.iter().enumerate() {
        let (parent, name) = match path.rsplit_once('/') {
            Some((p, n)) => (ensure(&mut nodes, p)?, n),
            None => (0, path.as_str()),
        };
        check_part(name)?;
        nodes[parent].files.push(i);
        file_names.push(name);
    }
    for n in &mut nodes {
        let mut names: Vec<&str> = n.files.iter().map(|&f| file_names[f]).collect();
        names.sort_unstable();
        if names.windows(2).any(|w| w[0] == w[1]) {
            return malformed(format!("romfs: a file appears twice in {:?}", n.name));
        }
    }
    let node_names: Vec<String> = nodes.iter().map(|n| n.name.clone()).collect();
    for n in &mut nodes {
        n.dirs.sort_by(|a, b| node_names[*a].cmp(&node_names[*b]));
        n.files.sort_by(|a, b| file_names[*a].cmp(file_names[*b]));
        let mut i = 0;
        for &d in &n.dirs {
            while i < n.files.len() && file_names[n.files[i]] < node_names[d].as_str() {
                i += 1;
            }
            if i < n.files.len() && file_names[n.files[i]] == node_names[d] {
                return malformed(format!(
                    "romfs: {:?} is both a file and a directory",
                    node_names[d]
                ));
            }
        }
    }
    let mut bfs = vec![0usize];
    let mut i = 0;
    while i < bfs.len() {
        let kids = nodes[bfs[i]].dirs.clone();
        bfs.extend(kids);
        i += 1;
    }
    let mut index = vec![0usize; nodes.len()];
    for (k, &n) in bfs.iter().enumerate() {
        index[n] = k;
    }
    let order: Vec<usize> = bfs
        .iter()
        .flat_map(|&n| nodes[n].files.iter().copied())
        .collect();
    let mut dir_meta = vec![0u32; bfs.len()];
    let mut dm = 0u64;
    for (k, &n) in bfs.iter().enumerate() {
        dir_meta[k] = u32::try_from(dm).map_err(|_| too_big())?;
        dm += (DIR_REC as u64) + align_up(nodes[n].name.len() as u64, 4);
    }
    let mut file_meta = vec![0u32; files.len()];
    let mut offsets = vec![0u64; files.len()];
    let (mut fm, mut cur) = (0u64, 0u64);
    for &f in &order {
        file_meta[f] = u32::try_from(fm).map_err(|_| too_big())?;
        fm += (FILE_REC as u64) + align_up(file_names[f].len() as u64, 4);
        cur = align_up(cur, FILE_ALIGN);
        offsets[f] = cur;
        cur += files[f].1;
    }
    if dm > i32::MAX as u64 || fm > i32::MAX as u64 {
        return Err(too_big());
    }
    let (dcount, fcount) = (hash_count(bfs.len()), hash_count(files.len()));
    let dh_off = HEADER_SIZE;
    let dm_off = dh_off + 4 * u64::from(dcount);
    let fh_off = dm_off + dm;
    let fm_off = fh_off + 4 * u64::from(fcount);
    let data_off = align_up(fm_off + fm, DATA_ALIGN);
    let mut m = vec![0u8; usize::try_from(data_off).map_err(|_| too_big())?];
    for (k, v) in [
        HEADER_SIZE,
        dh_off,
        4 * u64::from(dcount),
        dm_off,
        dm,
        fh_off,
        4 * u64::from(fcount),
        fm_off,
        fm,
        data_off,
    ]
    .into_iter()
    .enumerate()
    {
        put64(&mut m, k * 8, v);
    }
    let (dh, dmo, fh, fmo) = (
        dh_off as usize,
        dm_off as usize,
        fh_off as usize,
        fm_off as usize,
    );
    m[dh..dmo].fill(0xFF);
    m[fh..fmo].fill(0xFF);
    let mut parent_of = vec![0usize; bfs.len()];
    for (k, &n) in bfs.iter().enumerate() {
        for &c in &nodes[n].dirs {
            parent_of[index[c]] = k;
        }
    }
    for (k, &n) in bfs.iter().enumerate() {
        let node = &nodes[n];
        let p = parent_of[k];
        let rec = dmo + dir_meta[k] as usize;
        let siblings = &nodes[bfs[p]].dirs;
        let sib = if k == 0 {
            NONE
        } else {
            let pos = siblings.iter().position(|&s| s == n).unwrap_or(0);
            siblings.get(pos + 1).map_or(NONE, |&s| dir_meta[index[s]])
        };
        let parent_meta = if k == 0 { 0 } else { dir_meta[p] };
        let bucket = name_hash(parent_meta, node.name.as_bytes(), dcount) as usize;
        put32(&mut m, rec, parent_meta);
        put32(&mut m, rec + 4, sib);
        put32(
            &mut m,
            rec + 8,
            node.dirs.first().map_or(NONE, |&c| dir_meta[index[c]]),
        );
        put32(
            &mut m,
            rec + 0xC,
            node.files.first().map_or(NONE, |&f| file_meta[f]),
        );
        put32(&mut m, rec + 0x14, node.name.len() as u32);
        m[rec + DIR_REC..rec + DIR_REC + node.name.len()].copy_from_slice(node.name.as_bytes());
        let slot = dh + 4 * bucket;
        let prev = get32(&m, slot);
        put32(&mut m, rec + 0x10, prev);
        put32(&mut m, slot, dir_meta[k]);
    }
    for (k, &n) in bfs.iter().enumerate() {
        let node = &nodes[n];
        for (j, &f) in node.files.iter().enumerate() {
            let name = file_names[f].as_bytes();
            let rec = fmo + file_meta[f] as usize;
            let sib = node.files.get(j + 1).map_or(NONE, |&s| file_meta[s]);
            let slot = fh + 4 * name_hash(dir_meta[k], name, fcount) as usize;
            let prev = get32(&m, slot);
            put32(&mut m, rec, dir_meta[k]);
            put32(&mut m, rec + 4, sib);
            put64(&mut m, rec + 8, offsets[f]);
            put64(&mut m, rec + 0x10, files[f].1);
            put32(&mut m, rec + 0x18, prev);
            put32(&mut m, rec + 0x1C, name.len() as u32);
            m[rec + FILE_REC..rec + FILE_REC + name.len()].copy_from_slice(name);
            put32(&mut m, slot, file_meta[f]);
        }
    }
    for o in &mut offsets {
        *o += data_off;
    }
    Ok(Layout {
        meta: m,
        offsets,
        order,
        size: data_off + cur,
    })
}

fn too_big() -> Error {
    Error::Format("romfs: the tables outgrow the format".into())
}

/// Where the bytes of one file of an image come from.
#[derive(Clone, Debug)]
pub enum PieceSource {
    /// A whole host file.
    Path(PathBuf),
    /// `offset..` of backing storage `index`.
    At(usize, u64),
}

/// One file's data in an image.
#[derive(Clone, Debug)]
pub struct Piece {
    /// The offset in the image.
    pub offset: u64,
    /// The size.
    pub size: u64,
    /// Where the bytes are.
    pub source: PieceSource,
}

/// A RomFS image read from its parts. It is a [`Storage`], so
/// [`crate::romfs::Romfs::open`] lists and reads its files.
pub struct Image {
    meta: Vec<u8>,
    size: u64,
    pieces: Vec<Piece>,
    backing: Vec<Shared>,
}

impl Image {
    /// An image of `meta` (the bytes before the data area), `pieces` and
    /// the storages [`PieceSource::At`] names.
    ///
    /// # Errors
    ///
    /// Pieces overlap, run past `size` or into `meta`, or name a missing
    /// backing storage or a range past its end.
    pub fn new(
        meta: Vec<u8>,
        size: u64,
        mut pieces: Vec<Piece>,
        backing: Vec<Shared>,
    ) -> Result<Image> {
        pieces.retain(|p| p.size > 0);
        pieces.sort_by_key(|p| p.offset);
        let mut end = meta.len() as u64;
        for p in &pieces {
            let e = p.offset.checked_add(p.size);
            if p.offset < end || e.is_none_or(|e| e > size) {
                return malformed("romfs image: pieces overlap or run past the image");
            }
            if let PieceSource::At(i, at) = p.source {
                let Some(b) = backing.get(i) else {
                    return malformed("romfs image: a piece names a missing source");
                };
                if at.checked_add(p.size).is_none_or(|e| e > b.size()) {
                    return malformed("romfs image: a piece runs past its source");
                }
            }
            end = p.offset + p.size;
        }
        if (meta.len() as u64) > size {
            return malformed("romfs image: metadata past the image");
        }
        Ok(Image {
            meta,
            size,
            pieces,
            backing,
        })
    }

    /// The image of the extracted tree at `root`, read in place.
    ///
    /// # Errors
    ///
    /// The tree cannot be read, a name is not UTF-8, or [`layout`] fails.
    pub fn from_tree(root: &Path) -> Result<Image> {
        let TreeList { files, dirs } = walk(root)?;
        let l = layout(&files, &dirs)?;
        let pieces = files
            .iter()
            .enumerate()
            .map(|(i, (p, n))| Piece {
                offset: l.offsets[i],
                size: *n,
                source: PieceSource::Path(root.join(p)),
            })
            .collect();
        Image::new(l.meta, l.size, pieces, Vec::new())
    }

    /// How many files the image's file table lists (empty files too).
    pub fn file_count(&self) -> usize {
        let at = |o: usize| {
            self.meta
                .get(o..o + 8)
                .map_or(0, |b| u64::from_le_bytes(b.try_into().unwrap_or([0; 8])))
        };
        let (start, len) = (at(0x38) as usize, at(0x40) as usize);
        let Some(t) = self.meta.get(start..start.saturating_add(len)) else {
            return 0;
        };
        let (mut o, mut n) = (0usize, 0usize);
        while o + FILE_REC <= t.len() {
            o += FILE_REC + align_up(u64::from(get32(t, o + 0x1C)), 4) as usize;
            n += 1;
        }
        n
    }

    /// The metadata bytes.
    pub fn meta(&self) -> &[u8] {
        &self.meta
    }

    /// The data pieces in image order.
    pub fn pieces(&self) -> &[Piece] {
        &self.pieces
    }

    /// Stream the whole image into `out`, telling `on` about every chunk
    /// (bytes) and every file finished (`None`).
    ///
    /// # Errors
    ///
    /// A read or write fails.
    pub fn write_to(&self, out: &mut dyn Write, on: &mut dyn FnMut(Option<u64>)) -> Result<()> {
        let mut buf = vec![0u8; 1 << 20];
        let mut off = 0u64;
        let ends = self
            .pieces
            .iter()
            .map(|p| (p.offset + p.size, true))
            .chain(std::iter::once((self.size, false)));
        for (end, file) in ends {
            while off < end {
                let n = (buf.len() as u64).min(end - off) as usize;
                self.read_at(off, &mut buf[..n])?;
                out.write_all(&buf[..n])?;
                on(Some(n as u64));
                off += n as u64;
            }
            if file {
                on(None);
            }
        }
        Ok(())
    }
}

fn read_path(path: &Path, at: u64, buf: &mut [u8]) -> Result<()> {
    let f = File::open(path).map_err(|e| {
        Error::Io(std::io::Error::new(
            e.kind(),
            format!("{}: {e}", path.display()),
        ))
    })?;
    crate::storage::read_exact_at(&f, at, buf).map_err(|e| {
        Error::Io(std::io::Error::new(
            e.kind(),
            format!("{}: {e}", path.display()),
        ))
    })
}

impl Storage for Image {
    fn size(&self) -> u64 {
        self.size
    }

    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()> {
        check_range(offset, buf.len(), self.size, "romfs image")?;
        let mut off = offset;
        let mut done = 0usize;
        while done < buf.len() {
            let left = buf.len() - done;
            let out = &mut buf[done..];
            if off < self.meta.len() as u64 {
                let n = left.min(self.meta.len() - off as usize);
                out[..n].copy_from_slice(&self.meta[off as usize..off as usize + n]);
                done += n;
                off += n as u64;
                continue;
            }
            let i = self.pieces.partition_point(|p| p.offset + p.size <= off);
            let n = match self.pieces.get(i) {
                Some(p) if p.offset <= off => {
                    let n = left.min((p.offset + p.size - off) as usize);
                    let rel = off - p.offset;
                    match &p.source {
                        PieceSource::Path(path) => read_path(path, rel, &mut out[..n])?,
                        PieceSource::At(b, at) => {
                            self.backing[*b].read_at(at + rel, &mut out[..n])?
                        }
                    }
                    n
                }
                Some(p) => {
                    let n = left.min((p.offset - off) as usize);
                    out[..n].fill(0);
                    n
                }
                None => {
                    out.fill(0);
                    left
                }
            };
            done += n;
            off += n as u64;
        }
        Ok(())
    }
}

/// The files and directories of an extracted tree.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TreeList {
    /// Every file, `/`-separated from the root, with its size, sorted.
    pub files: Vec<(String, u64)>,
    /// Every directory below the root, `/`-separated, sorted.
    pub dirs: Vec<String>,
}

/// The files and directories of the extracted tree at `root`.
///
/// # Errors
///
/// A directory cannot be read, a name is not UTF-8, or an entry is neither a
/// file nor a directory.
pub fn walk(root: &Path) -> Result<TreeList> {
    let (mut files, mut dirs) = (Vec::new(), Vec::new());
    let mut stack = vec![String::new()];
    while let Some(rel) = stack.pop() {
        let dir = if rel.is_empty() {
            root.to_path_buf()
        } else {
            root.join(&rel)
        };
        let rd = std::fs::read_dir(&dir).map_err(|e| {
            Error::Io(std::io::Error::new(
                e.kind(),
                format!("{}: {e}", dir.display()),
            ))
        })?;
        for e in rd {
            let e = e?;
            let name = e.file_name().into_string().map_err(|n| {
                Error::Format(format!("{}: a name is not UTF-8: {n:?}", dir.display()))
            })?;
            let path = if rel.is_empty() {
                name
            } else {
                format!("{rel}/{name}")
            };
            let meta = std::fs::metadata(e.path())?;
            if meta.is_dir() {
                dirs.push(path.clone());
                stack.push(path);
            } else if meta.is_file() {
                files.push((path, meta.len()));
            }
        }
    }
    files.sort();
    dirs.sort();
    Ok(TreeList { files, dirs })
}

/// One host file a map reads from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MapSource {
    /// The path, relative to the map's folder unless absolute.
    pub path: String,
    /// The file's size.
    pub size: u64,
    /// Where the check bytes are in the file.
    pub check_offset: u64,
    /// The 32 bytes the file must hold at `check_offset`.
    pub check: [u8; 32],
}

/// One piece of a map.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MapPiece {
    /// The offset in the image.
    pub offset: u64,
    /// The size.
    pub size: u64,
    /// The source index.
    pub source: u32,
    /// The offset in the source.
    pub at: u64,
}

/// A RomFS map: an image described by its metadata and where its file data
/// already is.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RomfsMap {
    /// The image size.
    pub size: u64,
    /// The image's bytes before the data area.
    pub meta: Vec<u8>,
    /// The host files the pieces read.
    pub sources: Vec<MapSource>,
    /// The pieces, sorted by offset.
    pub pieces: Vec<MapPiece>,
}

impl RomfsMap {
    /// The map as bytes.
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut src = Vec::new();
        for s in &self.sources {
            let mut r = vec![0u8; SOURCE_REC];
            put64(&mut r, 0, s.size);
            put64(&mut r, 8, s.check_offset);
            r[0x10..0x30].copy_from_slice(&s.check);
            put32(&mut r, 0x30, s.path.len() as u32);
            r.extend_from_slice(s.path.as_bytes());
            r.resize(align_up(r.len() as u64, 8) as usize, 0);
            src.extend_from_slice(&r);
        }
        let sources_at = MAP_HEADER as u64;
        let pieces_at = sources_at + src.len() as u64;
        let meta_at = pieces_at + (self.pieces.len() * PIECE_REC) as u64;
        let mut out = vec![0u8; MAP_HEADER];
        out[..8].copy_from_slice(&MAP_MAGIC);
        put32(&mut out, 8, self.sources.len() as u32);
        put64(&mut out, 0x10, self.size);
        put64(&mut out, 0x18, self.meta.len() as u64);
        put64(&mut out, 0x20, self.pieces.len() as u64);
        put64(&mut out, 0x28, sources_at);
        put64(&mut out, 0x30, pieces_at);
        put64(&mut out, 0x38, meta_at);
        out.extend_from_slice(&src);
        for p in &self.pieces {
            let mut r = [0u8; PIECE_REC];
            put64(&mut r, 0, p.offset);
            put64(&mut r, 8, p.size);
            put32(&mut r, 0x10, p.source);
            put64(&mut r, 0x18, p.at);
            out.extend_from_slice(&r);
        }
        out.extend_from_slice(&self.meta);
        out
    }

    /// Parse a map.
    ///
    /// # Errors
    ///
    /// The bytes are not a well-formed map.
    pub fn parse(b: &[u8]) -> Result<RomfsMap> {
        let w = "romfs map";
        if b.get(..8) != Some(&MAP_MAGIC[..]) {
            return malformed("romfs map: bad magic");
        }
        let nsrc = u32_at(b, 8, w)? as usize;
        let size = u64_at(b, 0x10, w)?;
        let meta_size = u64_at(b, 0x18, w)?;
        let npieces = u64_at(b, 0x20, w)?;
        let (sources_at, pieces_at, meta_at) = (
            u64_at(b, 0x28, w)?,
            u64_at(b, 0x30, w)?,
            u64_at(b, 0x38, w)?,
        );
        let at = |v: u64| {
            usize::try_from(v).map_err(|_| Error::Format("romfs map: offset too large".into()))
        };
        let mut sources = Vec::new();
        let mut o = at(sources_at)?;
        for _ in 0..nsrc {
            let len = u32_at(b, o + 0x30, w)? as usize;
            let path = b.get(o + SOURCE_REC..o + SOURCE_REC + len).ok_or_else(|| {
                Error::Format("romfs map: a source path runs past the end".into())
            })?;
            sources.push(MapSource {
                path: String::from_utf8(path.to_vec())
                    .map_err(|_| Error::Format("romfs map: a source path is not UTF-8".into()))?,
                size: u64_at(b, o, w)?,
                check_offset: u64_at(b, o + 8, w)?,
                check: array(b, o + 0x10, w)?,
            });
            o += SOURCE_REC + align_up(len as u64, 8) as usize;
        }
        let n = at(npieces)?;
        let base = at(pieces_at)?;
        if n.checked_mul(PIECE_REC)
            .and_then(|k| k.checked_add(base))
            .is_none_or(|e| e > b.len())
        {
            return malformed("romfs map: pieces run past the end");
        }
        let mut pieces = Vec::with_capacity(n);
        for i in 0..n {
            let r = base + i * PIECE_REC;
            let source = u32_at(b, r + 0x10, w)?;
            if source as usize >= sources.len() {
                return malformed("romfs map: a piece names a missing source");
            }
            pieces.push(MapPiece {
                offset: u64_at(b, r, w)?,
                size: u64_at(b, r + 8, w)?,
                source,
                at: u64_at(b, r + 0x18, w)?,
            });
        }
        let m = at(meta_at)?;
        let meta = b
            .get(m..m + at(meta_size)?)
            .ok_or_else(|| Error::Format("romfs map: metadata runs past the end".into()))?
            .to_vec();
        Ok(RomfsMap {
            size,
            meta,
            sources,
            pieces,
        })
    }

    /// Open the sources (relative paths from `dir`), check them, and serve
    /// the image.
    ///
    /// # Errors
    ///
    /// A source is missing, has another size or other check bytes, or the
    /// pieces are malformed.
    pub fn open(&self, dir: &Path) -> Result<Image> {
        let mut backing: Vec<Shared> = Vec::new();
        for s in &self.sources {
            let path = dir.join(&s.path);
            let f = FileStorage::open(&path)?;
            if f.size() != s.size {
                return malformed(format!(
                    "{}: is {} bytes, the romfs map expects {}",
                    path.display(),
                    f.size(),
                    s.size
                ));
            }
            if read_vec(&f, s.check_offset, 32)? != s.check {
                return malformed(format!(
                    "{}: not the file the romfs map was made from",
                    path.display()
                ));
            }
            backing.push(Arc::new(f));
        }
        let pieces = self
            .pieces
            .iter()
            .map(|p| Piece {
                offset: p.offset,
                size: p.size,
                source: PieceSource::At(p.source as usize, p.at),
            })
            .collect();
        Image::new(self.meta.clone(), self.size, pieces, backing)
    }
}

/// What [`find`] found for a romfs in a folder.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Found {
    /// `romfs.map`.
    Map(PathBuf),
    /// `romfs.img`.
    Image(PathBuf),
    /// An extracted `romfs/` tree.
    Tree(PathBuf),
}

impl Found {
    /// The path.
    pub fn path(&self) -> &Path {
        match self {
            Found::Map(p) | Found::Image(p) | Found::Tree(p) => p,
        }
    }
}

/// The name of a map in an import folder.
pub const MAP_NAME: &str = "romfs.map";
/// The name of a plain image in an import folder or on a device.
pub const IMAGE_NAME: &str = "romfs.img";
/// The name of a blob in an import folder.
pub const BLOB_NAME: &str = "romfs.blob";

/// The romfs of folder `dir` (an import, or a DLC `<id>/<index>` folder):
/// `romfs.map`, else `romfs.img`, else a `romfs/` tree.
pub fn find(dir: &Path) -> Option<Found> {
    let map = dir.join(MAP_NAME);
    if map.is_file() {
        return Some(Found::Map(map));
    }
    let img = dir.join(IMAGE_NAME);
    if img.is_file() {
        return Some(Found::Image(img));
    }
    let tree = dir.join("romfs");
    tree.is_dir().then_some(Found::Tree(tree))
}

/// Open a romfs given as an extracted tree, a map or a plain image.
///
/// # Errors
///
/// The path cannot be read or is malformed.
pub fn open(path: &Path) -> Result<Shared> {
    if path.is_dir() {
        return Ok(Arc::new(Image::from_tree(path)?));
    }
    let f = FileStorage::open(path)?;
    if f.size() >= 8 && read_vec(&f, 0, 8)? == MAP_MAGIC {
        let map = RomfsMap::parse(&std::fs::read(path)?)?;
        let dir = path.parent().unwrap_or(Path::new("."));
        return Ok(Arc::new(map.open(dir)?));
    }
    Ok(Arc::new(f))
}

/// Write the romfs at `src` (as [`open`] takes it) to `dst` as one plain
/// image, through relift-cache's streaming write-if-changed writer, telling
/// `on` about every chunk. Returns whether `dst` was rewritten.
///
/// # Errors
///
/// `src` cannot be opened or read, or the write fails.
pub fn flatten(src: &Path, dst: &Path, on: &mut dyn FnMut(u64)) -> Result<bool> {
    let s = open(src)?;
    if let Some(d) = dst.parent() {
        std::fs::create_dir_all(d)?;
    }
    let mut w = relift_cache::StreamWriter::new(dst)?;
    let mut buf = vec![0u8; 1 << 20];
    let mut off = 0u64;
    while off < s.size() {
        let n = (buf.len() as u64).min(s.size() - off) as usize;
        s.read_at(off, &mut buf[..n])?;
        w.write_all(&buf[..n])?;
        on(n as u64);
        off += n as u64;
    }
    Ok(w.finish()?)
}

/// Remove the romfs forms in folder `dir` other than `keep` (a file name of
/// [`MAP_NAME`], [`IMAGE_NAME`] or `romfs`), with the blob unless the map is
/// kept, so a folder never holds two romfs forms that disagree.
///
/// # Errors
///
/// A removal fails.
pub fn remove_others(dir: &Path, keep: &str) -> Result<()> {
    for name in [MAP_NAME, IMAGE_NAME, BLOB_NAME, "romfs"] {
        if name == keep || (name == BLOB_NAME && keep == MAP_NAME) {
            continue;
        }
        let p = dir.join(name);
        match std::fs::symlink_metadata(&p) {
            Ok(m) if m.is_dir() => std::fs::remove_dir_all(&p)?,
            Ok(_) => std::fs::remove_file(&p)?,
            Err(_) => {}
        }
    }
    Ok(())
}

/// What [`write_nup`] did.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NupWrite {
    /// Files whose data the map reads in place from the `.nup`.
    pub in_place: usize,
    /// Files decompressed into the blob.
    pub blob_files: usize,
    /// The blob's size, 0 without one.
    pub blob_bytes: u64,
    /// Files rewritten (map and blob).
    pub written: usize,
}

/// Write the map of the RomFS `files` and `dirs` of the `.nup` at
/// `nup_path` (opened as `nup`) into `dir` as [`MAP_NAME`], with a
/// [`BLOB_NAME`] for its zstd files when it has any. Every file is checked
/// against its SHA-256 on the way (in place files in parallel), so a map
/// only ever describes verified bytes. `on` hears about every checked or
/// decompressed chunk and file.
///
/// # Errors
///
/// A read, hash check or write fails, or [`layout`] fails.
pub fn write_nup(
    nup: &Nup,
    nup_path: &Path,
    files: &[(String, NupEntry)],
    dirs: &[String],
    dir: &Path,
    on: &mut dyn FnMut(Option<u64>),
) -> Result<NupWrite> {
    std::fs::create_dir_all(dir)?;
    let list: Vec<(String, u64)> = files.iter().map(|(p, e)| (p.clone(), e.size)).collect();
    let l = layout(&list, dirs)?;
    let abs = std::path::absolute(nup_path)?;
    let nup_src = MapSource {
        path: path_string(&abs)?,
        size: nup.superblock().file_size,
        check_offset: 0x18,
        check: nup.superblock().toc_sha256,
    };
    let zstd: Vec<usize> = l
        .order
        .iter()
        .copied()
        .filter(|&i| files[i].1.codec == Codec::Zstd && files[i].1.size > 0)
        .collect();
    let mut id_input = Vec::with_capacity(zstd.len() * 32);
    for &i in &zstd {
        id_input.extend_from_slice(&files[i].1.sha256);
    }
    let blob_id = sha256(&id_input);
    let mut blob_at: HashMap<usize, u64> = HashMap::new();
    let mut cur = BLOB_HEADER;
    for &i in &zstd {
        cur = align_up(cur, FILE_ALIGN);
        blob_at.insert(i, cur);
        cur += files[i].1.size;
    }
    let blob_size = if zstd.is_empty() { 0 } else { cur };
    let mut out = NupWrite::default();
    verify_in_place(nup, files, on)?;
    let blob_path = dir.join(BLOB_NAME);
    if zstd.is_empty() {
        if blob_path.exists() {
            std::fs::remove_file(&blob_path)?;
        }
    } else if !blob_matches(&blob_path, blob_size, &blob_id) {
        write_blob(nup, files, &zstd, &blob_at, &blob_id, &blob_path, on)?;
        out.written += 1;
    }
    let mut sources = vec![nup_src];
    if !zstd.is_empty() {
        sources.push(MapSource {
            path: BLOB_NAME.to_string(),
            size: blob_size,
            check_offset: 8,
            check: blob_id,
        });
    }
    let mut pieces = Vec::new();
    for &i in &l.order {
        let e = &files[i].1;
        if e.size == 0 {
            continue;
        }
        let (source, at) = match blob_at.get(&i) {
            Some(&a) => (1, a),
            None => (0, e.extent_offset),
        };
        pieces.push(MapPiece {
            offset: l.offsets[i],
            size: e.size,
            source,
            at,
        });
        if source == 0 {
            out.in_place += 1;
        } else {
            out.blob_files += 1;
        }
    }
    let map = RomfsMap {
        size: l.size,
        meta: l.meta,
        sources,
        pieces,
    };
    if relift_cache::write_if_changed(&dir.join(MAP_NAME), &map.to_bytes())? {
        out.written += 1;
    }
    out.blob_bytes = blob_size;
    Ok(out)
}

fn path_string(p: &Path) -> Result<String> {
    if !p
        .components()
        .any(|c| matches!(c, Component::RootDir | Component::Prefix(_)))
    {
        return malformed(format!("{}: not an absolute path", p.display()));
    }
    p.to_str()
        .map(str::to_string)
        .ok_or_else(|| Error::Format(format!("{}: the path is not UTF-8", p.display())))
}

fn blob_matches(path: &Path, size: u64, id: &[u8; 32]) -> bool {
    let Ok(f) = FileStorage::open(path) else {
        return false;
    };
    f.size() == size
        && read_vec(&f, 0, BLOB_HEADER).is_ok_and(|h| h[..8] == BLOB_MAGIC && h[8..40] == id[..])
}

fn verify_in_place(
    nup: &Nup,
    files: &[(String, NupEntry)],
    on: &mut dyn FnMut(Option<u64>),
) -> Result<()> {
    use std::sync::Mutex;
    use std::sync::atomic::{AtomicUsize, Ordering};
    let raw: Vec<&NupEntry> = files
        .iter()
        .map(|(_, e)| e)
        .filter(|e| e.codec == Codec::Raw)
        .collect();
    let mut sorted = raw;
    sorted.sort_by_key(|e| e.extent_offset);
    let next = AtomicUsize::new(0);
    let events: Mutex<Vec<Option<u64>>> = Mutex::new(Vec::new());
    let first_err: Mutex<Option<Error>> = Mutex::new(None);
    let threads = std::thread::available_parallelism().map_or(4, |n| n.get().min(16));
    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| {
                let mut sink = std::io::sink();
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some(e) = sorted.get(i) else {
                        break;
                    };
                    if let Err(x) = nup.copy(e, &mut sink) {
                        let mut f = first_err
                            .lock()
                            .unwrap_or_else(std::sync::PoisonError::into_inner);
                        f.get_or_insert(x);
                        break;
                    }
                    let mut ev = events
                        .lock()
                        .unwrap_or_else(std::sync::PoisonError::into_inner);
                    ev.push(Some(e.size));
                    ev.push(None);
                }
            });
        }
        loop {
            let done = next.load(Ordering::Relaxed) >= sorted.len()
                || first_err.lock().map(|e| e.is_some()).unwrap_or(true);
            let drained: Vec<Option<u64>> = std::mem::take(
                &mut *events
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner),
            );
            for ev in drained {
                on(ev);
            }
            if done {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
    });
    for ev in std::mem::take(
        &mut *events
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner),
    ) {
        on(ev);
    }
    match first_err
        .into_inner()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
    {
        Some(e) => Err(e),
        None => Ok(()),
    }
}

fn write_blob(
    nup: &Nup,
    files: &[(String, NupEntry)],
    zstd: &[usize],
    blob_at: &HashMap<usize, u64>,
    id: &[u8; 32],
    path: &Path,
    on: &mut dyn FnMut(Option<u64>),
) -> Result<()> {
    struct Counting<'a, W: Write> {
        inner: W,
        at: u64,
        on: &'a mut dyn FnMut(Option<u64>),
    }
    impl<W: Write> Write for Counting<'_, W> {
        fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
            let n = self.inner.write(b)?;
            self.at += n as u64;
            (self.on)(Some(n as u64));
            Ok(n)
        }
        fn flush(&mut self) -> std::io::Result<()> {
            self.inner.flush()
        }
    }
    let mut w = Counting {
        inner: relift_cache::StreamWriter::new(path)?,
        at: 0,
        on,
    };
    let mut head = [0u8; BLOB_HEADER as usize];
    head[..8].copy_from_slice(&BLOB_MAGIC);
    head[8..40].copy_from_slice(id);
    w.inner.write_all(&head)?;
    w.at = BLOB_HEADER;
    let zeros = [0u8; FILE_ALIGN as usize];
    for &i in zstd {
        let pad = (blob_at[&i] - w.at) as usize;
        w.inner.write_all(&zeros[..pad])?;
        w.at += pad as u64;
        nup.copy(&files[i].1, &mut w)?;
        (w.on)(None);
    }
    w.inner.finish()?;
    Ok(())
}

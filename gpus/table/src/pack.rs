//! The NVN programs a table is translated from, in a folder or in one file
//! (the program pack of a Switch 2 game).
//!
//! A build keeps them as `programs/<key>.bin` with the scan's JSON files
//! beside (see [`Programs::dir`] and [`SECTIONS`]). A package carries them
//! as one pack file ([`PACK`]), all little-endian:
//!
//! * the magic `RLSPP003` (8 bytes), the program count and the section
//!   count (u32 each);
//! * per section: the length of its name, the name (UTF-8), the length of
//!   its bytes (u32 each but the name) and its bytes; a reader keeps the
//!   sections it does not know and gives them by name ([`Programs::section`]),
//!   so a new section needs no new version;
//! * per program, sorted by key: the key (32 bytes, the SHA-256 of the
//!   program's bytes), the offset of its zstd frame from the start of the
//!   file (u64), the frame's length and the program's length in bytes (u32
//!   each);
//! * the frames, each at a 4-byte boundary.
//!
//! Older packs still read: `RLSPP001` (a 16-byte head: magic, program count,
//! pipelines length; the entries, then the pipelines JSON) and `RLSPP002`
//! (a 20-byte head that adds the kernels length; the kernels JSON follows
//! the pipelines JSON).

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::{BoxError, hex, hex_key, sha256};

/// The pack's file name in a package's shader folder.
pub const PACK: &str = "programs.rlsp";

/// The scan's file of pipelines, in a build's shader folder.
pub const MESHES: &str = "meshes.json";

/// The scan's file of CUDA kernels (name, program key and static shared
/// bytes of each), in a build's shader folder.
pub const KERNELS: &str = "kernels.json";

/// The scan's file of transform feedback layouts (each program's GLSLC
/// transform feedback section), in a build's shader folder.
pub const XFB: &str = "xfb.json";

/// The sections a build's folder holds as files: section name, file name.
pub const SECTIONS: [(&str, &str); 3] = [("meshes", MESHES), ("kernels", KERNELS), ("xfb", XFB)];

const MAGIC: &[u8; 8] = b"RLSPP003";
const MAGIC_2: &[u8; 8] = b"RLSPP002";
const MAGIC_1: &[u8; 8] = b"RLSPP001";
const ENTRY: usize = 48;

/// Where the programs are.
enum Store {
    /// `programs/<key>.bin` files in a folder.
    Dir(PathBuf),
    /// A pack's bytes and, by key order, each frame's offset, frame length
    /// and program length.
    Pack(Vec<u8>, Vec<(usize, usize, usize)>),
}

/// The programs a table is translated from, and their pipelines.
pub struct Programs {
    keys: Vec<[u8; 32]>,
    sections: BTreeMap<String, Vec<u8>>,
    store: Store,
}

fn u32_at(b: &[u8], at: usize) -> Option<usize> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?) as usize)
}

impl Programs {
    /// The programs of a build's shader folder `dir`: `dir`/programs and
    /// the files of [`SECTIONS`], any of which may be missing.
    ///
    /// # Errors
    ///
    /// The programs folder exists but cannot be read.
    pub fn dir(dir: &Path) -> Result<Programs, BoxError> {
        let mut keys = Vec::new();
        if let Ok(rd) = std::fs::read_dir(dir.join("programs")) {
            for e in rd {
                if let Some(key) = e?.file_name().to_str().and_then(hex_key) {
                    keys.push(key);
                }
            }
        }
        keys.sort_unstable();
        let mut sections = BTreeMap::new();
        for (name, file) in SECTIONS {
            if let Ok(b) = std::fs::read(dir.join(file)) {
                sections.insert(name.to_owned(), b);
            }
        }
        Ok(Programs {
            keys,
            sections,
            store: Store::Dir(dir.join("programs")),
        })
    }

    /// No programs, and no pipelines.
    #[must_use]
    pub fn none() -> Programs {
        Programs {
            keys: Vec::new(),
            sections: BTreeMap::new(),
            store: Store::Pack(Vec::new(), Vec::new()),
        }
    }

    /// The programs of a pack's bytes.
    ///
    /// # Errors
    ///
    /// The bytes are not a pack, or an entry points past them.
    pub fn pack(bytes: Vec<u8>) -> Result<Programs, BoxError> {
        let short = || -> BoxError { "the program pack is cut short".into() };
        let magic = bytes.get(..8).ok_or_else(short)?;
        let n = u32_at(&bytes, 8).ok_or_else(short)?;
        let mut sections = BTreeMap::new();
        let mut take = |name: &str, at: usize, len: usize| -> Result<(), BoxError> {
            let b = bytes
                .get(at..at.checked_add(len).ok_or_else(short)?)
                .ok_or_else(short)?;
            sections.insert(name.to_owned(), b.to_vec());
            Ok(())
        };
        let entries = if magic == MAGIC {
            let count = u32_at(&bytes, 12).ok_or_else(short)?;
            let mut p = 16;
            for _ in 0..count {
                let nlen = u32_at(&bytes, p).ok_or_else(short)?;
                let name = bytes.get(p + 4..p + 4 + nlen).ok_or_else(short)?;
                let name =
                    std::str::from_utf8(name).map_err(|_| "a pack section name is not UTF-8")?;
                let len = u32_at(&bytes, p + 4 + nlen).ok_or_else(short)?;
                take(name, p + 8 + nlen, len)?;
                p += 8 + nlen + len;
            }
            p
        } else if magic == MAGIC_2 || magic == MAGIC_1 {
            let head = if magic == MAGIC_2 { 20 } else { 16 };
            let meshes = u32_at(&bytes, 12).ok_or_else(short)?;
            let kernels = if magic == MAGIC_2 {
                u32_at(&bytes, 16).ok_or_else(short)?
            } else {
                0
            };
            let at = head + ENTRY * n;
            take("meshes", at, meshes)?;
            if magic == MAGIC_2 {
                take("kernels", at + meshes, kernels)?;
            }
            head
        } else {
            return Err("not a program pack (no RLSPP003, RLSPP002 or RLSPP001 magic)".into());
        };
        let mut keys = Vec::with_capacity(n);
        let mut at = Vec::with_capacity(n);
        for i in 0..n {
            let e = entries + ENTRY * i;
            let entry = bytes.get(e..e + ENTRY).ok_or_else(short)?;
            let key: [u8; 32] = entry[..32].try_into().expect("32 bytes");
            let off = u64::from_le_bytes(entry[32..40].try_into().expect("8 bytes"));
            let off = usize::try_from(off).map_err(|_| short())?;
            let frame = u32_at(entry, 40).ok_or_else(short)?;
            let len = u32_at(entry, 44).ok_or_else(short)?;
            if off.checked_add(frame).is_none_or(|end| end > bytes.len()) {
                return Err(short());
            }
            keys.push(key);
            at.push((off, frame, len));
        }
        if keys.windows(2).any(|w| w[0] >= w[1]) {
            return Err("the program pack's keys are not sorted".into());
        }
        Ok(Programs {
            keys,
            sections,
            store: Store::Pack(bytes, at),
        })
    }

    /// The programs at `path`: a pack file, or a build's shader folder.
    ///
    /// # Errors
    ///
    /// As [`Programs::dir`] and [`Programs::pack`].
    pub fn open(path: &Path) -> Result<Programs, BoxError> {
        if path.is_file() {
            Programs::pack(std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?)
        } else {
            Programs::dir(path)
        }
    }

    /// The programs' keys, sorted.
    #[must_use]
    pub fn keys(&self) -> &[[u8; 32]] {
        &self.keys
    }

    /// The bytes of section `name`, empty when there is none.
    #[must_use]
    pub fn section(&self, name: &str) -> &[u8] {
        self.sections.get(name).map_or(&[], Vec::as_slice)
    }

    /// The scan's pipelines JSON (`meshes.json`), empty when there is none.
    #[must_use]
    pub fn meshes(&self) -> &[u8] {
        self.section("meshes")
    }

    /// The scan's kernels JSON (`kernels.json`), empty when there is none.
    #[must_use]
    pub fn kernels(&self) -> &[u8] {
        self.section("kernels")
    }

    /// The scan's transform feedback JSON (`xfb.json`), empty when there is
    /// none.
    #[must_use]
    pub fn xfb(&self) -> &[u8] {
        self.section("xfb")
    }

    /// The bytes of program `key`, or none when it is not here or its
    /// bytes do not hash to its key.
    ///
    /// # Errors
    ///
    /// Its file cannot be read, or its frame does not decompress.
    pub fn code(&self, key: &[u8; 32]) -> Result<Option<Vec<u8>>, BoxError> {
        let code = match &self.store {
            Store::Dir(dir) => match std::fs::read(dir.join(format!("{}.bin", hex(key)))) {
                Ok(c) => c,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
                Err(e) => return Err(e.into()),
            },
            Store::Pack(bytes, at) => {
                let Ok(i) = self.keys.binary_search(key) else {
                    return Ok(None);
                };
                let (off, frame, len) = at[i];
                zstd::bulk::decompress(&bytes[off..off + frame], len)?
            }
        };
        Ok((sha256(&code) == *key).then_some(code))
    }

    /// These programs as a pack, each compressed at zstd `level`.
    ///
    /// # Errors
    ///
    /// A program cannot be read or compressed.
    pub fn to_pack(&self, level: i32) -> Result<Vec<u8>, BoxError> {
        let mut frames = Vec::with_capacity(self.keys.len());
        for k in &self.keys {
            if let Some(code) = self.code(k)? {
                let f = zstd::bulk::compress(&code, level)?;
                frames.push((*k, f, code.len()));
            }
        }
        let mut out = Vec::new();
        out.extend(MAGIC);
        out.extend((frames.len() as u32).to_le_bytes());
        out.extend((self.sections.len() as u32).to_le_bytes());
        for (name, b) in &self.sections {
            out.extend((name.len() as u32).to_le_bytes());
            out.extend(name.as_bytes());
            out.extend((b.len() as u32).to_le_bytes());
            out.extend(b);
        }
        let mut at = (out.len() + ENTRY * frames.len()).next_multiple_of(4) as u64;
        for (k, f, len) in &frames {
            out.extend(k);
            out.extend(at.to_le_bytes());
            out.extend((f.len() as u32).to_le_bytes());
            out.extend((*len as u32).to_le_bytes());
            at += (f.len() as u64).next_multiple_of(4);
        }
        for (_, f, _) in &frames {
            out.resize(out.len().next_multiple_of(4), 0);
            out.extend(f);
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_pack_holds_what_the_folder_holds() {
        let d = crate::tests::scratch("pack");
        std::fs::create_dir_all(d.join("programs")).unwrap();
        let progs: [&[u8]; 3] = [b"one", b"second program", b"x"];
        for p in progs {
            relift_cache::write_if_changed(
                &d.join("programs").join(format!("{}.bin", hex(&sha256(p)))),
                p,
            )
            .unwrap();
        }
        relift_cache::write_if_changed(&d.join("programs").join("bad.bin"), b"no key").unwrap();
        relift_cache::write_if_changed(&d.join(MESHES), b"[]").unwrap();
        relift_cache::write_if_changed(&d.join(XFB), b"[{}]").unwrap();
        let dir = Programs::dir(&d).unwrap();
        assert_eq!(dir.keys().len(), 3);
        let pack = Programs::pack(dir.to_pack(3).unwrap()).unwrap();
        assert_eq!(pack.keys(), dir.keys());
        assert_eq!(pack.meshes(), b"[]");
        assert_eq!(pack.xfb(), b"[{}]");
        assert_eq!(pack.kernels(), b"");
        for k in dir.keys() {
            assert_eq!(pack.code(k).unwrap(), dir.code(k).unwrap());
            assert!(pack.code(k).unwrap().is_some());
        }
        assert_eq!(pack.code(&[0; 32]).unwrap(), None);
        let mut cut = dir.to_pack(3).unwrap();
        cut.truncate(cut.len() - 2);
        assert!(Programs::pack(cut).is_err());
        assert!(Programs::pack(b"RLSPV002".to_vec()).is_err());
        let mut old = b"RLSPP001".to_vec();
        old.extend(0u32.to_le_bytes());
        old.extend(2u32.to_le_bytes());
        old.extend(b"[]");
        let old = Programs::pack(old).unwrap();
        assert_eq!(
            (old.keys().len(), old.meshes(), old.kernels()),
            (0, &b"[]"[..], &b""[..])
        );
        let mut two = b"RLSPP002".to_vec();
        two.extend(0u32.to_le_bytes());
        two.extend(2u32.to_le_bytes());
        two.extend(4u32.to_le_bytes());
        two.extend(b"[][{}]");
        let two = Programs::pack(two).unwrap();
        assert_eq!((two.meshes(), two.kernels()), (&b"[]"[..], &b"[{}]"[..]));
        let mut unknown = b"RLSPP003".to_vec();
        unknown.extend(0u32.to_le_bytes());
        unknown.extend(1u32.to_le_bytes());
        unknown.extend(4u32.to_le_bytes());
        unknown.extend(b"next");
        unknown.extend(1u32.to_le_bytes());
        unknown.extend(b"x");
        let unknown = Programs::pack(unknown).unwrap();
        assert_eq!(
            (unknown.section("next"), unknown.meshes()),
            (&b"x"[..], &b""[..])
        );
        std::fs::remove_dir_all(&d).unwrap();
    }
}
